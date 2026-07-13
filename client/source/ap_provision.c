/**
 * @file ap_provision.c
 * @ingroup pc_client
 * @brief Interactive utility: store Wi-Fi credentials encrypted at rest, and hand them
 *        to the ESP32 over its temporary provisioning AP.
 *
 * The ESP32 cannot join the user's Wi-Fi until it has been told the SSID and password,
 * but it has no keyboard and no screen. It solves this by bringing up its own AP; this
 * utility connects to that AP, delivers the credentials over an encrypted TCP session,
 * and then puts the machine's Wi-Fi back the way it found it.
 *
 * ### Two independent secrets, and why
 * These are routinely confused, and confusing them defeats the design:
 * - The @b user @b key protects the credentials @b at @b rest on this machine. It is
 *   prompted for, used to derive an AES key, and then forgotten -- it is NEVER written to
 *   disk. Losing it means the credential file is unrecoverable, which is the intended
 *   property.
 * - The @b pairing @b code protects the credentials @b in @b transit to the ESP. It is
 *   passed on the command line (@c --pc=), and both sides derive the same session key from
 *   it plus a compiled-in master secret. It is not the user key and cannot substitute for
 *   it.
 *
 * ### Credential file (@c ~/.ap_prov/ap_prov.creds, mode 0600)
 * @verbatim
 *   [ encrypted_creds_hdr_t ][ AES-256-GCM ciphertext ]
 *     version, salt, nonce,     SSID \0 password \0 bssid_set \0 [BSSID]
 *     tag, ciphertext_len
 * @endverbatim
 * The salt and nonce are random per write, so re-encrypting the same credentials with the
 * same key produces different bytes. The GCM tag is what makes a wrong user key fail
 * loudly at @c EVP_DecryptFinal_ex rather than silently yielding garbage plaintext.
 *
 * ### Provisioning sequence (@c --pc=)
 * -# Load and decrypt the stored credentials with the user key.
 * -# Derive the session key from the pairing code.
 * -# Build the credentials packet, encrypted under the session key.
 * -# Save the current Wi-Fi connection, so it can be restored afterwards.
 * -# Rescan, find the ESP's AP, and join it.
 * -# Open TCP to the ESP and send the packet, retrying on a transient verdict.
 * -# Restore the original Wi-Fi connection, whatever the outcome.
 *
 * ### CLI
 * Exactly one option verb, plus any number of logging flags:
 * @verbatim
 *   ap_prov --set_creds | --clear_creds | --reset_key | --pc=<code> | --help
 *   ap_prov --pc=123456 -v            (logging flags may accompany the verb)
 * @endverbatim
 * ::main separates the two using ::log_is_log_arg, so the "exactly one option" rule means
 * exactly one @e non-logging argument. See ::main.
 *
 * ### Logging vs prompting
 * Diagnostics go through ::PRINT_MSG (see log.h) and honour @c -v / @c -q. Interactive
 * prompts and the @c --help text do NOT -- they are @c printf directly, because they are
 * user interface, not diagnostics: they must appear unconditionally, with no timestamp,
 * no severity tag, and no suppression under @c -q.
 *
 * @warning No key material or plaintext credential is ever logged, at any level. Not the
 *          user key, not the derived AES key, not the salt/nonce/tag, not the decrypted
 *          SSID or password. @c LOG_LEVEL=debug is something an operator will set
 *          casually, so "only at DEBUG" is not a safe place to put a secret.
 */

#define		LOG_TAG			"ap_prov"
#define		LOG_IMPLEMENTATION

#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <openssl/rand.h>

#include "common.h"
#include "log.h"

/**
 * @name CLI option verbs
 * @{
 */
#define		CLEAR_CREDS		"--clear_creds"	/**< Truncate the credential file. */
#define		SET_CREDS		"--set_creds"	/**< Prompt for and store new credentials. */
#define		RESET_KEY		"--reset_key"	/**< Re-encrypt existing credentials under a new user key. */
#define		PAIRING_CODE		"--pc="		/**< Provision the ESP; the code follows the '='. */
#define		HELP			"--help"	/**< Print usage. */
/** @} */

/**
 * @name Credential store
 * @{
 */
#define		CRED_DIR		".ap_prov"	/**< Directory under @c $HOME. */
#define		CRED_DIR_PERMS		0700		/**< Owner-only: nobody else may even list it. */
#define		CRED_FILE		"ap_prov.creds"	/**< Encrypted credential blob. */
#define		CRED_FILE_PERMS		0600		/**< Owner-only read/write. */
#define		MAX_CRED_REREAD		3		/**< Re-prompt attempts before giving up on bad input. */
/** @} */

/**
 * @name Credential field sizes
 * @brief Each is the usable length plus room for a terminator and the trailing newline
 *        that @c fgets leaves behind.
 * @{
 */
#define		SSID_SIZE		34	/**< 32 usable. */
#define		PSD_SIZE		66	/**< 64 usable. */
#define		BSSID_SET_SIZE		1	/**< Single '0' or '1' flag character. */
#define		BSSID_SIZE		19	/**< 17 usable (AA:BB:CC:DD:EE:FF). */
#define		USER_KEY_SIZE		34	/**< 32 usable. */
#define		MAX_CRED_BLOB_SIZE	512	/**< Sanity ceiling on ciphertext read from disk. */
/** @} */

/** @brief Total plaintext credential buffer: SSID, password, bssid_set flag, BSSID. */
#define		CRED_BUFF_SIZE		(SSID_SIZE + PSD_SIZE + BSSID_SET_SIZE + BSSID_SIZE)

/**
 * @brief Result codes, banded by sign so callers can distinguish "retry" from "give up".
 *
 * The sign is the contract, and it is the whole reason this enum exists rather than bare
 * ints:
 * - @b positive -- a soft, control-flow outcome. Not a failure. The operation can sensibly
 *   be retried or reported.
 * - @b zero (@c AP_OK) -- success.
 * - @b negative -- a hard failure.
 *
 * @warning Callers MUST test errors with @c "< 0", never with @c "if(retval)". A plain
 *          truth test treats the positive warnings as failures, which turns a retryable
 *          timeout into a fatal error.
 */
typedef enum {
	/* ---- positive band: soft / control-flow outcomes ---- */
	AP_WARN_RETRY			= 1,	/**< Generic "try the operation again". */
	AP_WARN_RECV_TIMEOUT		= 2,	/**< No ACK yet; caller may resend or reconnect. */
	AP_WARN_REJECTED_PKT		= 3,	/**< ESP reported CORRUPT or INVALID packet; resend. */

	AP_OK				= 0,	/**< Success. */

	/* ---- input / CLI (1xx) ---- */
	AP_ERR_BAD_ARGS			= -100,
	AP_ERR_PAIRING_FORMAT		= -101,
	AP_ERR_BSSID_FORMAT		= -102,
	AP_ERR_KEY_MISMATCH		= -104,

	/* ---- filesystem / creds store (2xx) ---- */
	AP_ERR_NO_HOME			= -200,
	AP_ERR_MKDIR			= -201,
	AP_ERR_OPEN			= -202,
	AP_ERR_FTRUNCATE		= -203,
	AP_ERR_CRED_FILE_EMPTY		= -204,
	AP_ERR_CRED_FILE_CORRUPT	= -205,

	/* ---- crypto (3xx) ---- */
	AP_ERR_PBKDF2			= -300,
	AP_ERR_RAND			= -301,
	AP_ERR_CIPHER_CTX		= -302,
	AP_ERR_ENCRYPT			= -303,
	AP_ERR_DECRYPT			= -304,	/**< Includes a GCM tag mismatch, i.e. the wrong user key. */
	AP_ERR_MALLOC			= -305,
	AP_ERR_REJECTED_CRYPTO		= -306,	/**< ESP reported BAD_CRYPTO: wrong pairing code. */
	AP_ERR_REJECTED_FORMAT		= -307,	/**< ESP reported BAD_FORMAT: malformed SSID/password. */

	/* ---- terminal / IO (4xx) ---- */
	AP_ERR_TERMIOS			= -400,
	AP_ERR_READ_INPUT		= -401,
	AP_ERR_INPUT_TOO_LONG		= -402,

	/* ---- network / provisioning transport (5xx) ---- */
	AP_ERR_SOCKET			= -500,
	AP_ERR_INET_PTON		= -501,
	AP_ERR_CONNECT			= -502,
	AP_ERR_SEND			= -503,
	AP_ERR_RECV			= -504,
	AP_ERR_RECV_TIMEOUT		= -505,	/**< Hard timeout; retries already exhausted in the callee. */
	AP_ERR_FILE_IO			= -506,
	AP_ERR_NMCLI_SAVE		= -507,
	AP_ERR_NMCLI_CONNECT		= -508,
	AP_ERR_NMCLI_RESTORE		= -509,
	AP_ERR_NMCLI_SCAN		= -510,
	AP_ERR_NMCLI_AP_NOT_FOUND	= -511,

	/* ---- provisioning outcome (6xx): hard, terminal ---- */
	AP_ERR_PROV_RETRIES_EXHAUSTED	= -603,	/**< RESEND_COUNTER_MAX hit with no verdict. */

	/* ---- fatal ---- */
	AP_ERR_FATAL			= -7

} ap_err_t;

/**
 * @name Local (at-rest) crypto parameters
 * @brief Aliased from common.h so the on-disk format and the wire format stay visibly
 *        distinct even though they currently share sizes.
 * @{
 */
#define		LOCAL_SALT_SIZE		SALT_SIZE
#define		LOCAL_NONCE_SIZE	NONCE_SIZE
#define		LOCAL_TAG_SIZE		TAG_SIZE
#define		LOCAL_PBKDF2_ITERS	PBKDF2_ITERS
#define		ENC_VERSION		1	/**< On-disk format version; a mismatch means "corrupt". */
/** @} */

/**
 * @brief Header prefixed to the encrypted credential blob on disk.
 *
 * Everything needed to reverse the encryption, except the secret itself. The salt drives
 * PBKDF2 (so the same user key yields a different AES key per file), the nonce is the GCM
 * IV, and the tag is the authentication tag that makes a wrong key fail loudly instead of
 * producing garbage.
 *
 * @note @c __attribute__((packed)) because this is written to disk verbatim: the compiler
 *       must not insert padding between fields, or the layout would depend on the build.
 */
typedef struct {
	uint8_t		version;			/**< ::ENC_VERSION at time of writing. */
	uint8_t		salt[LOCAL_SALT_SIZE];		/**< Random per write; PBKDF2 salt. */
	uint8_t		nonce[LOCAL_NONCE_SIZE];	/**< Random per write; GCM IV. */
	uint8_t		tag[LOCAL_TAG_SIZE];		/**< GCM authentication tag. */
	uint32_t	ciphertext_len;			/**< Bytes of ciphertext following this header. */
} __attribute__((packed)) encrypted_creds_hdr_t;

/**
 * @brief The Wi-Fi connection that was active before provisioning started.
 *
 * Captured so it can be restored afterwards. Joining the ESP's AP necessarily drops the
 * user's own network, and leaving them disconnected because provisioning failed would be
 * unacceptable -- so restoration happens on every exit path, success or failure.
 *
 * The UUID is preferred over the SSID when restoring: it names one specific saved
 * connection profile, whereas an SSID can be ambiguous across profiles.
 */
typedef struct {
	char	ifname[32];	/**< Wireless interface (e.g. "wlo1"). */
	char	ssid[64];	/**< Connection name as nmcli reports it. */
	char	uuid[64];	/**< NetworkManager connection UUID; empty if it could not be read. */
} wifi_state_t;

/** @brief Saved pre-provisioning Wi-Fi state; populated by ::__save_current_wifi__. */
wifi_state_t wfs = {0};

/**
 * @name Retry ceilings
 * @{
 */
#define		CRED_RECV_COUNTER_MAX	50	/**< Consecutive recv timeouts before reporting one. */
#define		RESEND_COUNTER_MAX	50	/**< Packet resends within one TCP session. */
#define		PROVISION_COUNTER_MAX	10	/**< Full provisioning attempts (new session each time). */
/** @} */

/**
 * @brief Ensure @c ~/.ap_prov/ and the credential file exist, and return the file's path.
 *
 * Creates the directory 0700 and the file 0600, so no other user can read the encrypted
 * blob (the encryption is the real defence, but there is no reason to also hand out the
 * ciphertext). Both are created idempotently: an existing directory is not an error, and
 * @c O_CREAT leaves an existing file untouched -- this runs on every invocation, including
 * ones that only mean to read.
 *
 * @param[out] file_path Receives the full path to the credential file; must be at least
 *                       @c PATH_MAX bytes.
 * @return ::AP_OK, or ::AP_ERR_NO_HOME / ::AP_ERR_MKDIR / ::AP_ERR_OPEN.
 */
static ap_err_t check_cred_dir_and_file(char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "check_cred_dir_and_file: entry");

	char path[PATH_MAX] = {0};
	const char* home = getenv("HOME");

	if(home == NULL){
		PRINT_MSG(LOG_LVL_ERROR, "check_cred_dir_and_file: HOME environment variable not set");
		PRINT_MSG(LOG_LVL_DEBUG, "check_cred_dir_and_file: exit (AP_ERR_NO_HOME)");
		return AP_ERR_NO_HOME;
	}

	snprintf(path, sizeof(char)*PATH_MAX, "%s/%s", home, CRED_DIR);

	if(mkdir(path, CRED_DIR_PERMS) < 0){
		if(errno != EEXIST){
			PRINT_MSG(LOG_LVL_ERROR, "check_cred_dir_and_file: mkdir '%s' failed, errno %d (%s)",
				  path, errno, strerror(errno));
			PRINT_MSG(LOG_LVL_DEBUG, "check_cred_dir_and_file: exit (AP_ERR_MKDIR)");
			return AP_ERR_MKDIR;
		}
	}

	int curr_len = strnlen(path, sizeof(char)*PATH_MAX);
	snprintf(path + curr_len, PATH_MAX - curr_len, "/%s", CRED_FILE);

	int file_fd = -1;

	file_fd = open(path, O_WRONLY | O_CREAT, CRED_FILE_PERMS);
	if(file_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "check_cred_dir_and_file: open '%s' failed, errno %d (%s)",
			  path, errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "check_cred_dir_and_file: exit (AP_ERR_OPEN)");
		return AP_ERR_OPEN;
	}

	close(file_fd);
	memcpy(file_path, path, strnlen(path, sizeof(char)*PATH_MAX) + 1);

	PRINT_MSG(LOG_LVL_DEBUG, "check_cred_dir_and_file: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Prompt for a line of non-sensitive input, echoed to the terminal.
 *
 * Used for the SSID and the BSSID, which are not secret and are easier to get right when
 * visible.
 *
 * An over-long line is rejected rather than silently truncated. @c fgets fills the buffer
 * and leaves the remainder sitting in the libc buffer, so the absence of a @c '\\n' is the
 * signal that more was typed than fits; the rest of the line is then drained with
 * @c getchar so it cannot be misread as the answer to the @e next prompt. The user gets
 * ::MAX_CRED_REREAD attempts.
 *
 * @note The trailing @c '\\n' is deliberately left in @p dest. Callers rely on it as a
 *       field separator when packing several credentials into one buffer, and convert it
 *       to @c '\\0' later.
 *
 * @param prompt    Printed before reading; not logged, since it is UI.
 * @param dest      Destination buffer.
 * @param dest_size Size of @p dest, including room for the terminator.
 * @return ::AP_OK, ::AP_ERR_READ_INPUT on a read failure, or ::AP_ERR_INPUT_TOO_LONG if
 *         every attempt was over-long.
 */
static ap_err_t _read_unprotected_string_(char* prompt, char* dest, ssize_t dest_size){
	uint8_t counter = MAX_CRED_REREAD;

	while(counter--){
		printf("%s", prompt);
		fflush(stdout);

		if(!fgets(dest, dest_size, stdin)){
			PRINT_MSG(LOG_LVL_ERROR, "_read_unprotected_string_: fgets failed, errno %d (%s)",
				  errno, strerror(errno));
			return AP_ERR_READ_INPUT;
		}

		if(!strchr(dest, '\n')){
			PRINT_MSG(LOG_LVL_WARN, "_read_unprotected_string_: input exceeds %zd bytes, retrying",
				  dest_size - 2);

			int c;
			while((c = getchar()) != '\n' && c != EOF){
			}
			continue;
		}else{
			return AP_OK;
		}
	}

	PRINT_MSG(LOG_LVL_ERROR, "_read_unprotected_string_: all %d attempts were over-long", MAX_CRED_REREAD);
	return AP_ERR_INPUT_TOO_LONG;
}

/**
 * @brief Prompt for a line of sensitive input with terminal echo disabled.
 *
 * Used for the password and the user key. Echo is turned off via @c termios so the secret
 * does not appear on screen and cannot be read from the scrollback or over a shoulder.
 *
 * The old terminal settings are restored on @e every exit path, including the failure
 * ones. Leaving a terminal with echo disabled would strand the user in an apparently dead
 * shell, so restoration cannot be skipped just because something went wrong.
 *
 * Over-long input is handled exactly as in ::_read_unprotected_string_.
 *
 * @note As there, the trailing @c '\\n' is left in @p dest on purpose.
 *
 * @param prompt    Printed before reading; not logged, since it is UI.
 * @param dest      Destination buffer.
 * @param dest_size Size of @p dest, including room for the terminator.
 * @return ::AP_OK, ::AP_ERR_TERMIOS if the terminal could not be reconfigured,
 *         ::AP_ERR_READ_INPUT on a read failure, or ::AP_ERR_INPUT_TOO_LONG.
 */
static ap_err_t _read_protected_string_(char* prompt, char* dest, ssize_t dest_size){
	struct termios oldt;
	struct termios newt;

	if(tcgetattr(STDIN_FILENO, &oldt) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: tcgetattr failed, errno %d (%s)",
			  errno, strerror(errno));
		return AP_ERR_TERMIOS;
	}

	newt = oldt;
	newt.c_lflag &= ~(ECHO);

	if(tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: tcsetattr failed, errno %d (%s)",
			  errno, strerror(errno));
		return AP_ERR_TERMIOS;
	}

	uint8_t counter = MAX_CRED_REREAD;

	while(counter--){
		printf("%s", prompt);
		fflush(stdout);

		if(!fgets(dest, dest_size, stdin)){
			PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: fgets failed, errno %d (%s)",
				  errno, strerror(errno));

			if(tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0){
				PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: tcsetattr restore failed, errno %d (%s)",
					  errno, strerror(errno));
				return AP_ERR_TERMIOS;
			}

			return AP_ERR_READ_INPUT;
		}

		if(!strchr(dest, '\n')){
			PRINT_MSG(LOG_LVL_WARN, "_read_protected_string_: input exceeds %zd bytes, retrying",
				  dest_size - 2);

			int c;
			while((c = getchar()) != '\n' && c != EOF){
			}
			continue;
		}else{
			if(tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0){
				PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: tcsetattr restore failed, errno %d (%s)",
					  errno, strerror(errno));
				return AP_ERR_TERMIOS;
			}

			printf("\n");
			fflush(stdout);
			return AP_OK;
		}
	}

	if(tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: tcsetattr restore failed, errno %d (%s)",
			  errno, strerror(errno));
		return AP_ERR_TERMIOS;
	}

	PRINT_MSG(LOG_LVL_ERROR, "_read_protected_string_: all %d attempts were over-long", MAX_CRED_REREAD);
	return AP_ERR_INPUT_TOO_LONG;
}

/**
 * @brief Truncate the credential file to zero length.
 *
 * The file itself is kept (rather than unlinked) so its 0600 mode and location survive;
 * only the contents go. This is what @c --clear_creds does, and it is also used before
 * rewriting the file with fresh credentials.
 *
 * @param file_path Path from ::check_cred_dir_and_file.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS, ::AP_ERR_OPEN, or ::AP_ERR_FTRUNCATE.
 */
static ap_err_t _reset_file_(char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "_reset_file_: entry");

	if(!file_path){
		PRINT_MSG(LOG_LVL_ERROR, "_reset_file_: file path is NULL");
		PRINT_MSG(LOG_LVL_DEBUG, "_reset_file_: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	int cred_fd = open(file_path, O_WRONLY);
	if(cred_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_reset_file_: open failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "_reset_file_: exit (AP_ERR_OPEN)");
		return AP_ERR_OPEN;
	}

	if(ftruncate(cred_fd, 0)){
		PRINT_MSG(LOG_LVL_ERROR, "_reset_file_: ftruncate failed, errno %d (%s)", errno, strerror(errno));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "_reset_file_: exit (AP_ERR_FTRUNCATE)");
		return AP_ERR_FTRUNCATE;
	}

	close(cred_fd);
	PRINT_MSG(LOG_LVL_DEBUG, "_reset_file_: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Read exactly @p sz bytes from @p fd, or fail.
 *
 * A single @c read may return fewer bytes than asked for even on a regular file, so a
 * short read is not an error -- it just means loop again. Only @c 0 (unexpected EOF, i.e.
 * a truncated credential file) or a negative return is a genuine failure.
 *
 * @param fd  Open file descriptor.
 * @param buf Destination buffer, at least @p sz bytes.
 * @param sz  Exact number of bytes to read.
 * @return ::AP_OK, or ::AP_ERR_FILE_IO.
 */
static ap_err_t __read_full__(int fd, void* buf, size_t sz){
	size_t total = 0;

	while(total < sz){
		ssize_t rd = read(fd, (uint8_t *)buf + total, sz - total);

		if(rd <= 0){
			PRINT_MSG(LOG_LVL_ERROR, "__read_full__: read failed at %zu/%zu bytes, errno %d (%s)",
				  total, sz, errno, strerror(errno));
			return AP_ERR_FILE_IO;
		}

		total += rd;
	}

	return AP_OK;
}

/**
 * @brief Write exactly @p sz bytes to @p fd, or fail.
 *
 * The mirror of ::__read_full__: a short write is normal and simply means loop again.
 *
 * @param fd  Open file descriptor.
 * @param buf Source buffer.
 * @param sz  Exact number of bytes to write.
 * @return ::AP_OK, or ::AP_ERR_FILE_IO.
 */
static ap_err_t __write_full__(int fd, const void* buf, size_t sz){
	size_t total = 0;

	while(total < sz){
		ssize_t wr = write(fd, (const uint8_t *)buf + total, sz - total);

		if(wr <= 0){
			PRINT_MSG(LOG_LVL_ERROR, "__write_full__: write failed at %zu/%zu bytes, errno %d (%s)",
				  total, sz, errno, strerror(errno));
			return AP_ERR_FILE_IO;
		}

		total += wr;
	}

	return AP_OK;
}

/**
 * @brief Derive the at-rest AES key from the user key and the file's salt.
 *
 * PBKDF2-HMAC-SHA256 with ::LOCAL_PBKDF2_ITERS iterations. The iteration count is the
 * point: it makes each guess of the user key expensive, so an attacker holding the
 * credential file cannot cheaply brute-force short keys. The per-file random salt means
 * the same user key produces a different AES key in every file, defeating precomputed
 * tables.
 *
 * @warning The derived key is secret. It is never logged, and the caller cleanses it from
 *          memory once finished.
 *
 * @param secret      The user key, NUL-terminated.
 * @param salt        Salt from the file header.
 * @param[out] aes_key Receives the derived key.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS, or ::AP_ERR_PBKDF2.
 */
static ap_err_t __derive_local_aes_key__(const char *secret, const uint8_t salt[LOCAL_SALT_SIZE], uint8_t aes_key[AES_KEY_SIZE]){
	if(!secret || !aes_key){
		PRINT_MSG(LOG_LVL_ERROR, "__derive_local_aes_key__: invalid argument");
		return AP_ERR_BAD_ARGS;
	}

	if(PKCS5_PBKDF2_HMAC(secret, strlen(secret), salt, LOCAL_SALT_SIZE, LOCAL_PBKDF2_ITERS, EVP_sha256(), AES_KEY_SIZE, aes_key) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__derive_local_aes_key__: PKCS5_PBKDF2_HMAC failed");
		return AP_ERR_PBKDF2;
	}

	return AP_OK;
}

/**
 * @brief Encrypt the credential buffer under the user key and write it to the file.
 *
 * Generates a fresh salt and nonce, derives the AES key, encrypts with AES-256-GCM, and
 * writes the header followed by the ciphertext.
 *
 * The salt and nonce are regenerated on every write, never reused. Reusing a GCM nonce
 * with the same key is catastrophic -- it leaks the XOR of the two plaintexts and can
 * expose the authentication subkey -- so they are drawn from @c RAND_bytes each time,
 * which is also why re-storing identical credentials produces a completely different file.
 *
 * The tag is retrieved only after @c EVP_EncryptFinal_ex, since GCM does not finalise it
 * until the plaintext is exhausted.
 *
 * @note @p cred_fd must already be positioned at offset 0 on a truncated file; the caller
 *       arranges that. This function appends wherever the descriptor happens to point.
 *
 * @param cred_fd  Open, writable credential file.
 * @param cred_buf Plaintext credentials.
 * @param cred_len Length of @p cred_buf.
 * @param user_key The user key, NUL-terminated.
 * @return ::AP_OK, or one of ::AP_ERR_BAD_ARGS, ::AP_ERR_MALLOC, ::AP_ERR_RAND,
 *         ::AP_ERR_PBKDF2, ::AP_ERR_CIPHER_CTX, ::AP_ERR_ENCRYPT, ::AP_ERR_FILE_IO.
 */
static ap_err_t _encrypt_and_store_creds_(int cred_fd, const void *cred_buf, size_t cred_len, const char *user_key){
	PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: entry");

	encrypted_creds_hdr_t hdr = {0};
	EVP_CIPHER_CTX *ctx = NULL;
	uint8_t aes_key[AES_KEY_SIZE] = {0};
	uint8_t *ciphertext = NULL;

	int outlen = 0;
	int finallen = 0;

	if(!cred_buf || !user_key){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: invalid argument");
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	ciphertext = malloc(cred_len);

	if(!ciphertext){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: malloc of %zu bytes failed", cred_len);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_MALLOC)");
		return AP_ERR_MALLOC;
	}

	hdr.version = ENC_VERSION;

	if(RAND_bytes(hdr.salt, LOCAL_SALT_SIZE) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: RAND_bytes failed for salt");
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_RAND)");
		return AP_ERR_RAND;
	}

	if(RAND_bytes(hdr.nonce, LOCAL_NONCE_SIZE) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: RAND_bytes failed for nonce");
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_RAND)");
		return AP_ERR_RAND;
	}

	if(__derive_local_aes_key__(user_key, hdr.salt, aes_key) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: key derivation failed");
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_PBKDF2)");
		return AP_ERR_PBKDF2;
	}

	ctx = EVP_CIPHER_CTX_new();

	if(!ctx){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_CIPHER_CTX_new failed");
		OPENSSL_cleanse(aes_key, sizeof(aes_key));
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_CIPHER_CTX)");
		return AP_ERR_CIPHER_CTX;
	}

	if(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_EncryptInit_ex failed");
		goto err;
	}

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, LOCAL_NONCE_SIZE, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_CIPHER_CTX_ctrl SET_IVLEN failed");
		goto err;
	}

	if(EVP_EncryptInit_ex(ctx, NULL, NULL, aes_key, hdr.nonce) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_EncryptInit_ex (key/nonce) failed");
		goto err;
	}

	if(EVP_EncryptUpdate(ctx, ciphertext, &outlen, cred_buf, cred_len) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_EncryptUpdate failed");
		goto err;
	}

	if(EVP_EncryptFinal_ex(ctx, ciphertext + outlen, &finallen) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_EncryptFinal_ex failed");
		goto err;
	}

	outlen += finallen;

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, LOCAL_TAG_SIZE, hdr.tag) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: EVP_CIPHER_CTX_ctrl GET_TAG failed");
		goto err;
	}

	hdr.ciphertext_len = outlen;

	if(__write_full__(cred_fd, &hdr, sizeof(hdr)) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: writing header failed");
		EVP_CIPHER_CTX_free(ctx);
		OPENSSL_cleanse(aes_key, sizeof(aes_key));
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	if(__write_full__(cred_fd, ciphertext, hdr.ciphertext_len) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: writing ciphertext failed");
		EVP_CIPHER_CTX_free(ctx);
		OPENSSL_cleanse(aes_key, sizeof(aes_key));
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(aes_key, sizeof(aes_key));
	free(ciphertext);

	PRINT_MSG(LOG_LVL_INFO, "_encrypt_and_store_creds_: credentials encrypted and stored (%u bytes)",
		  hdr.ciphertext_len);
	PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_OK)");
	return AP_OK;

err:
	PRINT_MSG(LOG_LVL_ERROR, "_encrypt_and_store_creds_: encryption failed");

	if(ctx){
		EVP_CIPHER_CTX_free(ctx);
	}

	OPENSSL_cleanse(aes_key, sizeof(aes_key));
	free(ciphertext);

	PRINT_MSG(LOG_LVL_DEBUG, "_encrypt_and_store_creds_: exit (AP_ERR_ENCRYPT)");
	return AP_ERR_ENCRYPT;
}

/**
 * @brief Load the credential file and decrypt it under the user key.
 *
 * Reads the header, sanity-checks it, derives the AES key from the stored salt, and
 * decrypts with AES-256-GCM.
 *
 * The header checks exist because the file is attacker-reachable and a corrupt or hostile
 * one must not be trusted: a version mismatch means the format is not what this build
 * understands, a @c ciphertext_len larger than the caller's buffer would overflow it, and
 * a zero or absurdly large length is nonsense. All are reported as
 * ::AP_ERR_CRED_FILE_CORRUPT.
 *
 * A @b wrong @b user @b key surfaces at @c EVP_DecryptFinal_ex, not before: GCM verifies
 * the authentication tag there, so decryption fails cleanly rather than handing back
 * plausible-looking garbage. That is the property that makes a wrong key safe to get
 * wrong.
 *
 * @warning The plaintext written to @p plain_buf is the user's Wi-Fi password. It is never
 *          logged, at any level, and the caller should not log it either.
 *
 * @param cred_fd       Open, readable credential file, positioned at offset 0.
 * @param user_key      The user key, NUL-terminated.
 * @param[out] plain_buf Receives the decrypted credentials.
 * @param plain_buf_sz  Size of @p plain_buf.
 * @param[out] plain_len Receives the decrypted length.
 * @return ::AP_OK, or one of ::AP_ERR_BAD_ARGS, ::AP_ERR_FILE_IO,
 *         ::AP_ERR_CRED_FILE_CORRUPT, ::AP_ERR_MALLOC, ::AP_ERR_PBKDF2,
 *         ::AP_ERR_CIPHER_CTX, ::AP_ERR_DECRYPT (which includes a wrong user key).
 */
static ap_err_t _load_and_decrypt_creds_(int cred_fd, const char* user_key, void* plain_buf, size_t plain_buf_sz, size_t* plain_len){
	PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: entry");

	uint8_t aes_key[AES_KEY_SIZE] = {0};
	uint8_t *ciphertext = NULL;
	EVP_CIPHER_CTX *ctx = NULL;

	int out_len = 0;
	int final_len = 0;

	encrypted_creds_hdr_t hdr;

	if(!user_key || !plain_buf || !plain_len){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: invalid argument");
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	if(__read_full__(cred_fd, &hdr, sizeof(hdr)) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: reading header failed");
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	if(hdr.version != ENC_VERSION){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: corrupt credential file (header version %d, expected %d)",
			  hdr.version, ENC_VERSION);
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: use --set_creds to reset the credentials");
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_CRED_FILE_CORRUPT)");
		return AP_ERR_CRED_FILE_CORRUPT;
	}

	if(hdr.ciphertext_len > plain_buf_sz){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: corrupt credential file (ciphertext %u exceeds buffer %zu)",
			  hdr.ciphertext_len, plain_buf_sz);
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_CRED_FILE_CORRUPT)");
		return AP_ERR_CRED_FILE_CORRUPT;
	}

	if(hdr.ciphertext_len == 0){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: corrupt credential file (no ciphertext)");
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: use --set_creds to reset the credentials");
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_CRED_FILE_CORRUPT)");
		return AP_ERR_CRED_FILE_CORRUPT;
	}

	if(hdr.ciphertext_len > MAX_CRED_BLOB_SIZE){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: corrupt credential file (ciphertext %u exceeds max %d)",
			  hdr.ciphertext_len, MAX_CRED_BLOB_SIZE);
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: use --set_creds to reset the credentials");
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_CRED_FILE_CORRUPT)");
		return AP_ERR_CRED_FILE_CORRUPT;
	}

	ciphertext = malloc(hdr.ciphertext_len);

	if(!ciphertext){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: malloc of %u bytes failed", hdr.ciphertext_len);
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_MALLOC)");
		return AP_ERR_MALLOC;
	}

	if(__read_full__(cred_fd, ciphertext, hdr.ciphertext_len) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: reading ciphertext failed");
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	if(__derive_local_aes_key__(user_key, hdr.salt, aes_key) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: key derivation failed");
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_PBKDF2)");
		return AP_ERR_PBKDF2;
	}

	ctx = EVP_CIPHER_CTX_new();

	if(!ctx){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_CIPHER_CTX_new failed");
		OPENSSL_cleanse(aes_key, sizeof(aes_key));
		free(ciphertext);
		PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_CIPHER_CTX)");
		return AP_ERR_CIPHER_CTX;
	}

	if(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_DecryptInit_ex failed");
		goto cleanup;
	}

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, LOCAL_NONCE_SIZE, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_CIPHER_CTX_ctrl SET_IVLEN failed");
		goto cleanup;
	}

	if(EVP_DecryptInit_ex(ctx, NULL, NULL, aes_key, hdr.nonce) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_DecryptInit_ex (key/nonce) failed");
		goto cleanup;
	}

	if(EVP_DecryptUpdate(ctx, plain_buf, &out_len, ciphertext, hdr.ciphertext_len) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_DecryptUpdate failed");
		goto cleanup;
	}

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, LOCAL_TAG_SIZE, hdr.tag) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_CIPHER_CTX_ctrl SET_TAG failed");
		goto cleanup;
	}

	if(EVP_DecryptFinal_ex(ctx, (uint8_t *)plain_buf + out_len, &final_len) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: EVP_DecryptFinal_ex failed (wrong key, or the file is corrupt)");
		goto cleanup;
	}

	out_len += final_len;
	*plain_len = out_len;

	EVP_CIPHER_CTX_free(ctx);
	free(ciphertext);
	OPENSSL_cleanse(aes_key, sizeof(aes_key));

	PRINT_MSG(LOG_LVL_INFO, "_load_and_decrypt_creds_: credentials decrypted (%d bytes)", out_len);
	PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_OK)");
	return AP_OK;

cleanup:
	PRINT_MSG(LOG_LVL_ERROR, "_load_and_decrypt_creds_: decryption failed");

	if(ctx){
		EVP_CIPHER_CTX_free(ctx);
	}

	if(ciphertext){
		free(ciphertext);
	}

	OPENSSL_cleanse(aes_key, sizeof(aes_key));

	PRINT_MSG(LOG_LVL_DEBUG, "_load_and_decrypt_creds_: exit (AP_ERR_DECRYPT)");
	return AP_ERR_DECRYPT;
}

/**
 * @brief Derive the transport session key from the pairing code.
 *
 * PBKDF2-HMAC-SHA256 over the pairing code, salted with the compiled-in
 * @c MASTER_SECRET_COMM. The ESP performs the identical derivation, so both ends arrive at
 * the same key without it ever crossing the wire -- the pairing code is the only thing the
 * user carries between them, and it is short enough to read off a screen.
 *
 * @warning This derivation must match the firmware's byte for byte, including the salt
 *          length. A mismatch does not fail loudly: it silently produces a different key,
 *          and every provisioning attempt is then rejected as @c BAD_CRYPTO with no
 *          indication of why.
 *
 * @param pairing_code  The code from @c --pc=, NUL-terminated.
 * @param[out] aes_key  Receives the derived session key.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS, or ::AP_ERR_PBKDF2.
 */
static ap_err_t __derive_session_key__(const char* pairing_code, uint8_t aes_key[AES_KEY_SIZE]){
	if(!pairing_code || !aes_key){
		PRINT_MSG(LOG_LVL_ERROR, "__derive_session_key__: invalid argument");
		return AP_ERR_BAD_ARGS;
	}

	if(PKCS5_PBKDF2_HMAC(pairing_code, strlen(pairing_code), (const uint8_t*)MASTER_SECRET_COMM, sizeof(MASTER_SECRET_COMM) - 1, PBKDF2_ITERS, EVP_sha256(), 32, aes_key) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__derive_session_key__: PKCS5_PBKDF2_HMAC failed");
		return AP_ERR_PBKDF2;
	}

	return AP_OK;
}

/**
 * @brief Build the provisioning packet: credentials encrypted under the session key.
 *
 * Generates a fresh salt and nonce into the packet, encrypts the credentials struct with
 * AES-256-GCM, attaches the GCM tag, fills the protocol header, and computes the checksum.
 *
 * The checksum is computed last, after every other header field is final, because it
 * covers them. Computing it earlier would checksum a half-built header.
 *
 * The salt travels in the packet even though the session key is derived from the pairing
 * code rather than from it -- the ESP needs the nonce to decrypt, and the pair is carried
 * together.
 *
 * @param aes_key Session key from ::__derive_session_key__.
 * @param creds   Credentials to encrypt.
 * @param[out] pkt Receives the completed packet.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS, ::AP_ERR_RAND, ::AP_ERR_CIPHER_CTX, or
 *         ::AP_ERR_ENCRYPT.
 */
static ap_err_t __build_prov_packet__(const uint8_t aes_key[AES_KEY_SIZE], const _sta_credentials_t* creds, proto_packet_t* pkt){
	PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: entry");

	EVP_CIPHER_CTX *ctx = NULL;

	int outlen = 0;
	int finallen = 0;

	if(!aes_key || !creds || !pkt){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: invalid argument");
		PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	if(RAND_bytes(pkt->payload.prov_creds.salt, SALT_SIZE) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: RAND_bytes failed for salt");
		PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_ERR_RAND)");
		return AP_ERR_RAND;
	}

	if(RAND_bytes(pkt->payload.prov_creds.nonce, NONCE_SIZE) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: RAND_bytes failed for nonce");
		PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_ERR_RAND)");
		return AP_ERR_RAND;
	}

	ctx = EVP_CIPHER_CTX_new();

	if(!ctx){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_CIPHER_CTX_new failed");
		PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_ERR_CIPHER_CTX)");
		return AP_ERR_CIPHER_CTX;
	}

	if(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_EncryptInit_ex failed");
		goto err;
	}

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, NULL) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_CIPHER_CTX_ctrl SET_IVLEN failed");
		goto err;
	}

	if(EVP_EncryptInit_ex(ctx, NULL, NULL, aes_key, pkt->payload.prov_creds.nonce) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_EncryptInit_ex (key/nonce) failed");
		goto err;
	}

	if(EVP_EncryptUpdate(ctx, pkt->payload.prov_creds.ciphertext, &outlen, (const uint8_t *)creds, sizeof(*creds)) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_EncryptUpdate failed");
		goto err;
	}

	if(EVP_EncryptFinal_ex(ctx, pkt->payload.prov_creds.ciphertext + outlen, &finallen) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_EncryptFinal_ex failed");
		goto err;
	}

	if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, pkt->payload.prov_creds.tag) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: EVP_CIPHER_CTX_ctrl GET_TAG failed");
		goto err;
	}

	pkt->header.magic  = MAGIC_BYTES;
	pkt->header.msg_id = MSG_PROV_CREDS;
	pkt->header.length = sizeof(prov_creds_t);

	pkt->header.chksum = proto_generate_chksum(pkt);

	EVP_CIPHER_CTX_free(ctx);

	PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_OK)");
	return AP_OK;

err:
	PRINT_MSG(LOG_LVL_ERROR, "__build_prov_packet__: unable to build the credential packet");

	if(ctx){
		EVP_CIPHER_CTX_free(ctx);
	}

	PRINT_MSG(LOG_LVL_DEBUG, "__build_prov_packet__: exit (AP_ERR_ENCRYPT)");
	return AP_ERR_ENCRYPT;
}

/**
 * @brief Record the currently active Wi-Fi connection so it can be restored later.
 *
 * Asks nmcli for the connected wireless device, then looks up that connection's UUID.
 *
 * Both are captured because restoration prefers the UUID: it names one specific saved
 * profile unambiguously, whereas reconnecting by SSID can pick a different profile (or
 * fail outright) when several exist for the same network. The SSID is kept as the fallback
 * for when the UUID lookup fails.
 *
 * Provisioning must not start without this. Joining the ESP's AP tears down whatever the
 * user was connected to, and with no record of it there is nothing to put back.
 *
 * @return ::AP_OK if at least the interface and SSID were captured; ::AP_ERR_NMCLI_SAVE if
 *         no connected wireless device was found.
 */
static ap_err_t __save_current_wifi__(void){
	PRINT_MSG(LOG_LVL_DEBUG, "__save_current_wifi__: entry");

	FILE *fp = NULL;
	char line[256];

	fp = popen("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device", "r");

	if(!fp){
		PRINT_MSG(LOG_LVL_ERROR, "__save_current_wifi__: popen failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__save_current_wifi__: exit (AP_ERR_NMCLI_SAVE)");
		return AP_ERR_NMCLI_SAVE;
	}

	while(fgets(line, sizeof(line), fp)){
		char dev[32];
		char type[32];
		char state[32];
		char conn[64];

		if(sscanf(line, "%31[^:]:%31[^:]:%31[^:]:%63[^\n]", dev, type, state, conn) != 4){
			continue;
		}

		if(strcmp(type, "wifi")){
			continue;
		}

		if(strcmp(state, "connected")){
			continue;
		}

		strncpy(wfs.ifname, dev, sizeof(wfs.ifname)-1);
		strncpy(wfs.ssid, conn, sizeof(wfs.ssid)-1);
		break;
	}

	pclose(fp);

	if(wfs.ifname[0] == '\0'){
		PRINT_MSG(LOG_LVL_ERROR, "__save_current_wifi__: no connected wifi device found");
		PRINT_MSG(LOG_LVL_DEBUG, "__save_current_wifi__: exit (AP_ERR_NMCLI_SAVE)");
		return AP_ERR_NMCLI_SAVE;
	}

	snprintf(line, sizeof(line), "nmcli -t -f NAME,UUID connection show " "--active | grep '^%s:'", wfs.ssid);

	fp = popen(line, "r");

	if(!fp){
		PRINT_MSG(LOG_LVL_ERROR, "__save_current_wifi__: popen failed for uuid lookup, errno %d (%s)",
			  errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__save_current_wifi__: exit (AP_ERR_NMCLI_SAVE)");
		return AP_ERR_NMCLI_SAVE;
	}

	if(fgets(line, sizeof(line), fp)){
		char name[64];
		char uuid[64];

		if(sscanf(line, "%63[^:]:%63s", name, uuid) == 2){
			strncpy(wfs.uuid, uuid, sizeof(wfs.uuid)-1);
		}
	}

	pclose(fp);

	if(wfs.uuid[0] == '\0'){
		PRINT_MSG(LOG_LVL_WARN, "__save_current_wifi__: no uuid for '%s', will restore by ssid", wfs.ssid);
	}

	PRINT_MSG(LOG_LVL_INFO, "__save_current_wifi__: saved current wifi state (%s on %s)", wfs.ssid, wfs.ifname);
	PRINT_MSG(LOG_LVL_DEBUG, "__save_current_wifi__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Force a Wi-Fi rescan on @p ifname and wait for the results to land.
 *
 * The ESP's AP has only just come up, so it is very unlikely to be in NetworkManager's
 * cached scan list. Without an explicit rescan the subsequent lookup would search a stale
 * list and conclude the ESP is absent.
 *
 * @note The sleep afterwards is not optional. @c nmcli returns as soon as the rescan is
 *       @e requested, not when results are in, so querying immediately would race the
 *       scan and read the same stale list this call exists to refresh.
 *
 * @param ifname Wireless interface to scan on.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS, or ::AP_ERR_NMCLI_SCAN.
 */
static ap_err_t __force_wifi_scan__(const char *ifname){
	PRINT_MSG(LOG_LVL_DEBUG, "__force_wifi_scan__: entry (ifname %s)", ifname ? ifname : "(null)");

	FILE *fp;
	char cmd[128];

	if(!ifname || !*ifname){
		PRINT_MSG(LOG_LVL_ERROR, "__force_wifi_scan__: invalid interface name");
		PRINT_MSG(LOG_LVL_DEBUG, "__force_wifi_scan__: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	snprintf(cmd, sizeof(cmd), "nmcli device wifi rescan ifname %s 2>/dev/null", ifname);

	fp = popen(cmd, "r");

	if(!fp){
		PRINT_MSG(LOG_LVL_ERROR, "__force_wifi_scan__: popen failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__force_wifi_scan__: exit (AP_ERR_NMCLI_SCAN)");
		return AP_ERR_NMCLI_SCAN;
	}

	pclose(fp);

	usleep(1500 * 1000);

	PRINT_MSG(LOG_LVL_DEBUG, "__force_wifi_scan__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Check whether @p target_ssid appears in the scan results for @p ifname.
 *
 * Run after ::__force_wifi_scan__, so the list is fresh. Confirming the AP is actually
 * visible before attempting to join gives a precise failure ("the ESP is not broadcasting")
 * instead of a vague nmcli connect error.
 *
 * @param ifname      Wireless interface whose scan list is queried.
 * @param target_ssid SSID to look for.
 * @return ::AP_OK if present; ::AP_ERR_NMCLI_AP_NOT_FOUND if absent; ::AP_ERR_NMCLI_SCAN if
 *         the list could not be read.
 */
static ap_err_t __wifi_ap_present__(const char *ifname, const char *target_ssid){
	PRINT_MSG(LOG_LVL_DEBUG, "__wifi_ap_present__: entry (looking for %s)", target_ssid);

	FILE *fp;
	char cmd[256];
	char line[256];

	snprintf(cmd, sizeof(cmd), "nmcli -t -f SSID device wifi list ifname %s", ifname);

	fp = popen(cmd, "r");

	if(!fp){
		PRINT_MSG(LOG_LVL_ERROR, "__wifi_ap_present__: popen failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__wifi_ap_present__: exit (AP_ERR_NMCLI_SCAN)");
		return AP_ERR_NMCLI_SCAN;
	}

	while(fgets(line, sizeof(line), fp)){
		line[strcspn(line, "\n")] = '\0';

		if(strcmp(line, target_ssid) == 0){
			pclose(fp);
			PRINT_MSG(LOG_LVL_DEBUG, "__wifi_ap_present__: exit (AP_OK, found)");
			return AP_OK;
		}
	}

	pclose(fp);

	PRINT_MSG(LOG_LVL_ERROR, "__wifi_ap_present__: '%s' not found in scan results", target_ssid);
	PRINT_MSG(LOG_LVL_DEBUG, "__wifi_ap_present__: exit (AP_ERR_NMCLI_AP_NOT_FOUND)");
	return AP_ERR_NMCLI_AP_NOT_FOUND;
}

/**
 * @brief Join the ESP's provisioning AP.
 *
 * This necessarily drops the machine's current Wi-Fi connection, which is why
 * ::__save_current_wifi__ must have run first and ::__restore_wifi__ must run afterwards on
 * every path.
 *
 * @param ssid     The ESP's AP SSID.
 * @param password The ESP's AP password.
 * @return ::AP_OK, or ::AP_ERR_NMCLI_CONNECT.
 */
static ap_err_t __connect_to_esp_ap__(const char* ssid, const char* password){
	PRINT_MSG(LOG_LVL_DEBUG, "__connect_to_esp_ap__: entry (ssid %s)", ssid);

	char cmd[256];

	snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect '%s' " "password '%s'", ssid, password);

	if(system(cmd) != 0){
		PRINT_MSG(LOG_LVL_ERROR, "__connect_to_esp_ap__: unable to connect to '%s'", ESP_AP_WIFI_SSID);
		PRINT_MSG(LOG_LVL_DEBUG, "__connect_to_esp_ap__: exit (AP_ERR_NMCLI_CONNECT)");
		return AP_ERR_NMCLI_CONNECT;
	}

	PRINT_MSG(LOG_LVL_INFO, "__connect_to_esp_ap__: connected to '%s'", ssid);
	PRINT_MSG(LOG_LVL_DEBUG, "__connect_to_esp_ap__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Open a TCP connection to the ESP's provisioning server.
 *
 * The ESP's address is fixed and known (@c ESP_AP_IP), since it runs the DHCP server on its
 * own AP -- so unlike the daemon in main.c, no discovery is needed here.
 *
 * @c SO_RCVTIMEO is set so a receive cannot block forever; ::__recv_exact__ counts the
 * resulting timeouts and reports one once its budget is exhausted.
 *
 * @note The fd is returned through @p fd rather than as the return value, so the return
 *       value can remain a pure ::ap_err_t. A function that returns "an fd, or a negative
 *       error" forces every caller to disambiguate two meanings from one integer.
 *
 * @param[out] fd Receives the connected socket on success, or -1 on failure.
 * @return ::AP_OK, ::AP_ERR_SOCKET, ::AP_ERR_INET_PTON, or ::AP_ERR_CONNECT.
 */
static ap_err_t __create_tcp_client__(int* fd){
	PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: entry");

	if(!fd){
		PRINT_MSG(LOG_LVL_ERROR, "__create_tcp_client__: invalid argument");
		PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	*fd = -1;

	int tcp_sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(tcp_sock_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__create_tcp_client__: socket failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_ERR_SOCKET)");
		return AP_ERR_SOCKET;
	}

	struct sockaddr_in server_addr = {0};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(ESP_TCP_SERVER_PORT_AP);

	if(inet_pton(AF_INET, ESP_AP_IP, &server_addr.sin_addr) != 1){
		PRINT_MSG(LOG_LVL_ERROR, "__create_tcp_client__: inet_pton failed for '%s'", ESP_AP_IP);
		close(tcp_sock_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_ERR_INET_PTON)");
		return AP_ERR_INET_PTON;
	}

	struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};

	if(setsockopt(tcp_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__create_tcp_client__: setsockopt SO_RCVTIMEO failed, errno %d (%s)",
			  errno, strerror(errno));
		close(tcp_sock_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_ERR_SOCKET)");
		return AP_ERR_SOCKET;
	}

	if(connect(tcp_sock_fd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__create_tcp_client__: connect to %s:%d failed, errno %d (%s)",
			  ESP_AP_IP, ESP_TCP_SERVER_PORT_AP, errno, strerror(errno));
		close(tcp_sock_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_ERR_CONNECT)");
		return AP_ERR_CONNECT;
	}

	*fd = tcp_sock_fd;

	PRINT_MSG(LOG_LVL_INFO, "__create_tcp_client__: connected to ESP at %s:%d", ESP_AP_IP, ESP_TCP_SERVER_PORT_AP);
	PRINT_MSG(LOG_LVL_DEBUG, "__create_tcp_client__: exit (AP_OK, fd %d)", tcp_sock_fd);
	return AP_OK;
}

/**
 * @brief Receive exactly @p len bytes, or report a timeout or failure.
 *
 * TCP is a byte stream, so one @c recv may return a partial packet; this loops until the
 * whole fixed-size ACK has arrived.
 *
 * With @c SO_RCVTIMEO armed, an idle socket returns @c EAGAIN rather than blocking. A
 * single timeout is not a failure -- it just means the ESP has not answered @e yet -- so
 * they are counted, and only after ::CRED_RECV_COUNTER_MAX consecutive ones is
 * ::AP_WARN_RECV_TIMEOUT reported. The counter resets on any successful read, so a slow
 * trickle of bytes does not accumulate toward a false timeout.
 *
 * @note ::AP_WARN_RECV_TIMEOUT is positive: it is a retry signal, not an error. The caller
 *       must test for failure with @c "< 0" or it will mistake it for one.
 *
 * @param fd  Connected socket.
 * @param buf Destination buffer, at least @p len bytes.
 * @param len Exact number of bytes to read.
 * @return ::AP_OK on success, ::AP_WARN_RECV_TIMEOUT if the ESP stayed silent, or
 *         ::AP_ERR_RECV on a peer close or real socket error.
 */
static ap_err_t __recv_exact__(int fd, void* buf, size_t len){
	uint8_t* p = (uint8_t*)buf;
	size_t got = 0;
	uint8_t cred_recv_counter = 0;

	while(got < len){
		ssize_t n = recv(fd, p + got, len - got, 0);

		if(n > 0){
			got += (size_t)n;
			cred_recv_counter = 0;
			continue;
		}

		if(n == 0){
			PRINT_MSG(LOG_LVL_ERROR, "__recv_exact__: peer closed the connection");
			return AP_ERR_RECV;
		}

		if(errno == EINTR){
			continue;
		}

		if(errno == EAGAIN || errno == EWOULDBLOCK){
			if(++cred_recv_counter >= CRED_RECV_COUNTER_MAX){
				PRINT_MSG(LOG_LVL_WARN, "__recv_exact__: no response after %d attempts", CRED_RECV_COUNTER_MAX);
				return AP_WARN_RECV_TIMEOUT;
			}

			continue;
		}

		PRINT_MSG(LOG_LVL_ERROR, "__recv_exact__: recv failed, errno %d (%s)", errno, strerror(errno));
		return AP_ERR_RECV;
	}

	return AP_OK;
}

/**
 * @brief Reconnect to the Wi-Fi network that was active before provisioning.
 *
 * Restores by UUID when one was captured, since that names one specific saved profile;
 * falls back to the SSID otherwise.
 *
 * This runs on every exit path out of provisioning, successful or not. Joining the ESP's AP
 * disconnected the user from their own network, and leaving them stranded there because
 * provisioning happened to fail would be worse than the original failure.
 *
 * @return ::AP_OK, or ::AP_ERR_NMCLI_RESTORE.
 */
static ap_err_t __restore_wifi__(void){
	PRINT_MSG(LOG_LVL_DEBUG, "__restore_wifi__: entry");

	char cmd[256];

	if(wfs.uuid[0]){
		snprintf(cmd, sizeof(cmd), "nmcli connection up uuid '%s'", wfs.uuid);
	}else{
		snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect '%s'", wfs.ssid);
	}

	if(system(cmd) != 0){
		PRINT_MSG(LOG_LVL_ERROR, "__restore_wifi__: unable to restore connection to '%s'", wfs.ssid);
		PRINT_MSG(LOG_LVL_DEBUG, "__restore_wifi__: exit (AP_ERR_NMCLI_RESTORE)");
		return AP_ERR_NMCLI_RESTORE;
	}

	PRINT_MSG(LOG_LVL_INFO, "__restore_wifi__: restored connection to '%s'", wfs.ssid);
	PRINT_MSG(LOG_LVL_DEBUG, "__restore_wifi__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief One full provisioning attempt: unpack, encrypt, join the ESP, deliver, restore.
 *
 * Unpacks the decrypted credential blob into a @c _sta_credentials_t, encrypts it under the
 * session key derived from @p pairing_code, saves the current Wi-Fi, joins the ESP's AP,
 * sends the packet over TCP, and interprets the ESP's verdict.
 *
 * ### Credential blob layout
 * The blob is a run of NUL-separated fields, walked by length:
 * @verbatim
 *   SSID \0 password \0 bssid_set \0 [ BSSID \0 ]
 * @endverbatim
 * The BSSID is present only when the @c bssid_set flag is @c '1'.
 *
 * ### Verdicts, and which are worth retrying
 * The ESP's answer determines whether another attempt could possibly help:
 * - @c ACCEPTED -- done.
 * - @c BAD_CRYPTO -- the pairing code is wrong. Retrying with the same code cannot succeed,
 *   so this returns immediately rather than burning the retry budget.
 * - @c BAD_FORMAT -- the stored SSID/password are malformed. Same reasoning: terminal.
 * - @c CORRUPT_PKT / @c INVALID_PKT -- the packet was damaged in transit. This one @e is
 *   worth resending, so the counter advances and the loop continues on the same connection.
 * - A receive timeout is likewise retried, not failed.
 *
 * @warning @c wfs must be restored before returning on every path. Each early return here
 *          calls ::__restore_wifi__ for that reason -- an early return that skipped it would
 *          leave the user connected to the ESP's AP with no internet.
 *
 * @warning @p cred_len is accepted but not currently used: the blob is walked with
 *          @c strlen, which trusts it to be NUL-terminated. That holds for anything this
 *          program wrote, since ::__set_creds__ terminates every field, but it means a
 *          truncated or hand-edited file would be walked off the end. Bounding the walk by
 *          @p cred_len would close that.
 *
 * @param cred_buff    Decrypted credential blob.
 * @param cred_len     Length of @p cred_buff.
 * @param pairing_code Pairing code from @c --pc=.
 * @return ::AP_OK on acceptance; ::AP_ERR_REJECTED_CRYPTO / ::AP_ERR_REJECTED_FORMAT for a
 *         terminal rejection; ::AP_ERR_PROV_RETRIES_EXHAUSTED if the budget ran out; or a
 *         transport/crypto error.
 */
static ap_err_t _do_provision_(char* cred_buff, size_t cred_len, char* pairing_code){
	PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: entry");

	uint8_t aes_key[AES_KEY_SIZE] = {0};
	int retval = __derive_session_key__(pairing_code, aes_key);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: session key derivation failed, retval %d", retval);
		OPENSSL_cleanse(aes_key, sizeof(aes_key));
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	_sta_credentials_t sta = {0};
	proto_packet_t pkt = {0};

	size_t next_pos = strlen(cred_buff) + 1;
	strcpy(sta.ssid, cred_buff);
	strcpy(sta.password, cred_buff + next_pos);

	next_pos = strlen(cred_buff + next_pos) + 1;

	if(*(cred_buff + next_pos) == '1'){
		uint32_t b[6] = {0};
		sta.bssid_set = true;
		next_pos += 2;

		if(sscanf(cred_buff + next_pos, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: malformed BSSID in credentials file, use --set_creds to override it");
			OPENSSL_cleanse(aes_key, sizeof(aes_key));
			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_BSSID_FORMAT)");
			return AP_ERR_BSSID_FORMAT;
		}

		for(int i = 0; i < 6; i++){
			sta.bssid[i] = (uint8_t)b[i];
		}
	}else{
		sta.bssid_set = false;
	}

	retval = __build_prov_packet__(aes_key, &sta, &pkt);

	OPENSSL_cleanse(aes_key, sizeof(aes_key));
	OPENSSL_cleanse(&sta, sizeof(sta));

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: building provisioning packet failed, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "_do_provision_: saving current wifi configuration");
	retval = __save_current_wifi__();

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: unable to save current wifi info, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "_do_provision_: scanning for the ESP access point");
	retval = __force_wifi_scan__(wfs.ifname);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: wifi scan on '%s' failed, retval %d", wfs.ifname, retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "_do_provision_: searching for the ESP in the scan results");
	retval = __wifi_ap_present__(wfs.ifname, ESP_AP_WIFI_SSID);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: '%s' access point not found, retval %d", ESP_AP_WIFI_SSID, retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "_do_provision_: connecting to the ESP access point");
	retval = __connect_to_esp_ap__(ESP_AP_WIFI_SSID, ESP_AP_WIFI_PSD);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: unable to connect to '%s', retval %d", ESP_AP_WIFI_SSID, retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "_do_provision_: opening the provisioning session");

	int tcp_fd = -1;
	retval = __create_tcp_client__(&tcp_fd);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: __create_tcp_client__ failed, retval %d", retval);
		__restore_wifi__();
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	int resend_counter = 0;

	while(1){
		if(resend_counter == RESEND_COUNTER_MAX){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: maximum retries exhausted, provisioning failed");
			close(tcp_fd);
			__restore_wifi__();
			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_PROV_RETRIES_EXHAUSTED)");
			return AP_ERR_PROV_RETRIES_EXHAUSTED;
		}

		ssize_t s = send(tcp_fd, &pkt, sizeof(proto_hdr_t) + sizeof(prov_creds_t), 0);

		if(s < 0){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: send failed, errno %d (%s)", errno, strerror(errno));
			close(tcp_fd);

			retval = __restore_wifi__();
			if(retval < 0){
				PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
				return retval;
			}

			PRINT_MSG(LOG_LVL_WARN, "_do_provision_: will retry with a new connection");
			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_SEND)");
			return AP_ERR_SEND;
		}

		proto_packet_t ack = {0};

		int r = __recv_exact__(tcp_fd, &ack, sizeof(proto_hdr_t) + sizeof(prov_ack_t));

		if(r < 0){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: __recv_exact__ failed, retval %d", r);
			close(tcp_fd);

			retval = __restore_wifi__();
			if(retval < 0){
				PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
				return retval;
			}

			PRINT_MSG(LOG_LVL_WARN, "_do_provision_: will retry with a new connection");
			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_RECV)");
			return AP_ERR_RECV;
		}

		if(r == AP_WARN_RECV_TIMEOUT){
			PRINT_MSG(LOG_LVL_WARN, "_do_provision_: no ack from the ESP, resending (attempt %d)", resend_counter + 1);
			resend_counter++;
			continue;
		}

		if(ack.payload.prov_ack.status == PROV_STATUS_ACCEPTED){
			PRINT_MSG(LOG_LVL_INFO, "_do_provision_: credentials accepted, the ESP will now retry with them");
			close(tcp_fd);
			break;
		}else if(ack.payload.prov_ack.status == PROV_STATUS_BAD_CRYPTO){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: invalid pairing code, retry with the correct one");
			close(tcp_fd);

			retval = __restore_wifi__();
			if(retval < 0){
				PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
				return retval;
			}

			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_REJECTED_CRYPTO)");
			return AP_ERR_REJECTED_CRYPTO;
		}else if(ack.payload.prov_ack.status == PROV_STATUS_BAD_FORMAT){
			PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: the ESP rejected the SSID or password as malformed, use --set_creds to replace them");
			close(tcp_fd);

			retval = __restore_wifi__();
			if(retval < 0){
				PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
				return retval;
			}

			PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_ERR_REJECTED_FORMAT)");
			return AP_ERR_REJECTED_FORMAT;
		}else if(ack.payload.prov_ack.status == PROV_STATUS_CORRUPT_PKT || ack.payload.prov_ack.status == PROV_STATUS_INVALID_PKT){
			PRINT_MSG(LOG_LVL_WARN, "_do_provision_: the ESP reported a corrupt packet, resending (attempt %d)",
				  resend_counter + 1);
			resend_counter++;
			continue;
		}else{
			PRINT_MSG(LOG_LVL_WARN, "_do_provision_: unrecognised ack status %d, resending (attempt %d)",
				  ack.payload.prov_ack.status, resend_counter + 1);
			resend_counter++;
			continue;
		}
	}

	retval = __restore_wifi__();

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "_do_provision_: unable to restore the original wifi connection, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_DEBUG, "_do_provision_: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Print the usage text.
 *
 * @note Raw @c printf, not ::PRINT_MSG. This is user interface: the user asked for it, so
 *       it must appear regardless of the log level (including under @c -q), with no
 *       timestamp and no severity tag.
 */
static void __print_help__(void){
	printf(
"AP Provisioning Utility\n"
"======================\n"
"\n"
"Usage:\n"
"    ap_prov <option> [log flags]\n"
"\n"
"Exactly one option must be supplied. Logging flags may accompany it.\n"
"\n"
"Options:\n"
"\n"
"  --set_creds\n"
"      Create or replace stored Wi-Fi credentials.\n"
"\n"
"      The utility will prompt for:\n"
"          - SSID\n"
"          - Password\n"
"          - BSSID usage flag\n"
"          - Optional BSSID/MAC address\n"
"\n"
"      Credentials are encrypted using AES-256-GCM before\n"
"      being written to:\n"
"          ~/.ap_prov/ap_prov.creds\n"
"\n"
"      If no credential file exists, a new secret key is\n"
"      requested and used to protect the credentials.\n"
"\n"
"      If credentials already exist, the current secret key\n"
"      must be provided before they can be replaced.\n"
"\n"
"\n"
"  --clear_creds\n"
"      Remove all stored credentials.\n"
"\n"
"      This truncates the credential file and deletes all\n"
"      encrypted Wi-Fi information.\n"
"\n"
"\n"
"  --reset_key\n"
"      Change the secret key protecting the credential file.\n"
"\n"
"      The current key must be supplied so the existing\n"
"      credentials can be decrypted and re-encrypted using\n"
"      the new key.\n"
"\n"
"\n"
"  --pc=<pairing_code>\n"
"      Start provisioning of the ESP32 using the supplied\n"
"      pairing code.\n"
"\n"
"      The utility will:\n"
"          1. Load encrypted credentials.\n"
"          2. Prompt for the secret key.\n"
"          3. Decrypt the credentials.\n"
"          4. Establish a provisioning session using the\n"
"             pairing code.\n"
"          5. Encrypt credentials for transport.\n"
"          6. Send them to the ESP32.\n"
"\n"
"      Example:\n"
"          ap_prov --pc=123456\n"
"\n"
"\n"
"  --help\n"
"      Display this help text.\n"
"\n"
"Logging flags:\n"
"\n"
"  -v                  Verbose (debug) output.\n"
"  -vv                 Very verbose (trace) output.\n"
"  -q                  Quiet; errors only.\n"
"  --log-level=<lvl>   error | warn | info | debug | verbose\n"
"\n"
"  The LOG_LEVEL environment variable does the same, e.g.\n"
"      LOG_LEVEL=debug ap_prov --pc=123456\n"
"\n"
"Security Notes:\n"
"\n"
"  * Wi-Fi credentials are stored only in encrypted form.\n"
"\n"
"  * The secret key is never stored on disk.\n"
"\n"
"  * Forgetting the secret key makes existing credentials\n"
"    unrecoverable.\n"
"\n"
"  * The pairing code is NOT the secret key.\n"
"\n"
"  * The pairing code is used only to establish a trusted\n"
"    provisioning session with the ESP32.\n"
"\n"
);
}

/**
 * @brief Prompt for a full set of credentials and store them encrypted.
 *
 * Backs @c --set_creds. Prompts for SSID, password, the BSSID flag and (if set) the BSSID,
 * packs them into one NUL-separated blob, then encrypts and writes it.
 *
 * Which key is asked for depends on whether the file already holds anything. An empty file
 * means there is nothing to unlock, so a @e new key is prompted for and confirmed. A
 * populated file means the existing key must be supplied first -- otherwise anyone with
 * access to the terminal could overwrite the credentials without knowing the old key.
 *
 * The individual reads leave a trailing @c '\\n' on each field, which is exactly what makes
 * the blob walkable: the newlines are converted to @c '\\0' in one pass at the end, turning
 * the buffer into a run of NUL-terminated strings.
 *
 * @param file_path Path from ::check_cred_dir_and_file.
 * @return ::AP_OK, or an error from the read, stat, open, or encryption steps.
 */
static ap_err_t __set_creds__(const char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: entry");

	char cred_buff[CRED_BUFF_SIZE] = {0};
	int retval = 0;

	retval = _read_unprotected_string_("SSID (32 characters max) : ", cred_buff, SSID_SIZE);
	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read SSID, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
		return retval;
	}

	retval = _read_protected_string_("Password (64 characters max) : ", cred_buff + strnlen(cred_buff, sizeof(cred_buff)), PSD_SIZE);
	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read password, retval %d", retval);
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
		return retval;
	}

	char bssid_set = 0;
	uint8_t counter = MAX_CRED_REREAD;
	ssize_t len = strnlen(cred_buff, sizeof(cred_buff));

	while(counter--){
		printf("bssid_set (1 or 0 only): ");
		fflush(stdout);

		bssid_set = fgetc(stdin);

		if(!(bssid_set == '1' || bssid_set == '0')){
			PRINT_MSG(LOG_LVL_WARN, "__set_creds__: invalid bssid_set value, retrying");
			continue;
		}else{
			cred_buff[len] = bssid_set;
			cred_buff[len + 1] = '\n';

			if(counter == 1){
				counter++;
			}

			break;
		}
	}

	int ch;
	while((ch = getchar()) != '\n' && ch != EOF){
	}

	if(!counter){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: all attempts for bssid_set were invalid");
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_ERR_READ_INPUT)");
		return AP_ERR_READ_INPUT;
	}

	if(bssid_set == '1'){
		char* bssid = cred_buff + strnlen(cred_buff, sizeof(cred_buff));

		retval = _read_unprotected_string_("MAC (17 characters max e.g AA:BB:CC:DD:EE:FF) : ", bssid, BSSID_SIZE);
		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read MAC address, retval %d", retval);
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
			return retval;
		}

		if(!(bssid[2] == ':' && bssid[5] == ':' && bssid[8] == ':' && bssid[11] == ':' && bssid[14] == ':')){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: MAC address format error");
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_ERR_BSSID_FORMAT)");
			return AP_ERR_BSSID_FORMAT;
		}
	}

	struct stat st;

	if(stat(file_path, &st) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: stat failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	char user_key[USER_KEY_SIZE] = {0};

	if(!st.st_size){
		PRINT_MSG(LOG_LVL_INFO, "__set_creds__: credential file is empty, a new key is needed");

		char user_key_conf[USER_KEY_SIZE] = {0};

		retval = _read_protected_string_("new key (32 characters max, you can use your old key if there was one): ", user_key, sizeof(user_key));
		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read the new key, retval %d", retval);
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			OPENSSL_cleanse(user_key, sizeof(user_key));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
			return retval;
		}

		retval = _read_protected_string_("confirm new key (32 characters max): ", user_key_conf, sizeof(user_key_conf));
		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read the confirmation key, retval %d", retval);
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			OPENSSL_cleanse(user_key, sizeof(user_key));
			OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
			return retval;
		}

		if(strncmp(user_key, user_key_conf, sizeof(user_key))){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: the confirmation key did not match");
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			OPENSSL_cleanse(user_key, sizeof(user_key));
			OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_ERR_KEY_MISMATCH)");
			return AP_ERR_KEY_MISMATCH;
		}

		OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));
	}else{
		retval = _read_protected_string_("key (32 characters max): ", user_key, sizeof(user_key));
		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to read the key, retval %d", retval);
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			OPENSSL_cleanse(user_key, sizeof(user_key));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
			return retval;
		}

		retval = _reset_file_((char*)file_path);
		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: unable to clear the credential file, retval %d", retval);
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			OPENSSL_cleanse(user_key, sizeof(user_key));
			PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
			return retval;
		}
	}

	*strchr(user_key, '\n') = '\0';

	int cred_fd = open(file_path, O_WRONLY);

	if(cred_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: open failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_ERR_OPEN)");
		return AP_ERR_OPEN;
	}

	len = strnlen(cred_buff, sizeof(cred_buff));

	for(uint8_t i = 0; i < len; i++){
		if(cred_buff[i] == '\n'){
			cred_buff[i] = '\0';
		}
	}

	retval = _encrypt_and_store_creds_(cred_fd, cred_buff, len, user_key);

	OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
	OPENSSL_cleanse(user_key, sizeof(user_key));
	close(cred_fd);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__set_creds__: encryption and storage failed, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "__set_creds__: credentials stored");
	PRINT_MSG(LOG_LVL_DEBUG, "__set_creds__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Re-encrypt the stored credentials under a new user key.
 *
 * Backs @c --reset_key. Decrypts with the old key, truncates the file, then re-encrypts the
 * same credentials under a new one.
 *
 * The old key must be supplied because the credentials cannot be re-encrypted without first
 * being decrypted, and only the old key can do that -- which is also why an empty credential
 * file makes this operation meaningless: there is nothing to re-encrypt, and the key itself
 * is never stored, so there is nothing to change.
 *
 * @note The descriptor is rewound with @c lseek after the truncate. @c ftruncate changes the
 *       file's length but not the descriptor's offset, which is still sitting past the old
 *       ciphertext -- writing without the rewind would leave a hole of zero bytes ahead of
 *       the new blob and the file would not parse.
 *
 * @param file_path Path from ::check_cred_dir_and_file.
 * @return ::AP_OK, or an error from the stat, read, decrypt, truncate, or encrypt steps.
 */
static ap_err_t __reset_key__(const char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: entry");

	char user_key[USER_KEY_SIZE] = {0};
	char cred_buff[CRED_BUFF_SIZE] = {0};
	struct stat st;
	int retval = 0;

	if(stat(file_path, &st) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: stat failed, errno %d (%s)", errno, strerror(errno));
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	if(st.st_size == 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: credential file is empty; the key is never stored, so there is nothing to change");
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_CRED_FILE_EMPTY)");
		return AP_ERR_CRED_FILE_EMPTY;
	}

	retval = _read_protected_string_("key (32 characters max): ", user_key, sizeof(user_key));
	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: unable to read the key, retval %d", retval);
		OPENSSL_cleanse(user_key, sizeof(user_key));
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (%d)", retval);
		return retval;
	}

	*strchr(user_key, '\n') = '\0';

	int cred_fd = open(file_path, O_RDWR);

	if(cred_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: open failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_OPEN)");
		return AP_ERR_OPEN;
	}

	size_t cred_len = 0;
	retval = _load_and_decrypt_creds_(cred_fd, user_key, cred_buff, CRED_BUFF_SIZE, &cred_len);

	OPENSSL_cleanse(user_key, sizeof(user_key));

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: loading and decryption failed, retval %d", retval);
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (%d)", retval);
		return retval;
	}

	if(ftruncate(cred_fd, 0)){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: ftruncate failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_FTRUNCATE)");
		return AP_ERR_FTRUNCATE;
	}

	if(lseek(cred_fd, 0, SEEK_SET) < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: lseek failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_FILE_IO)");
		return AP_ERR_FILE_IO;
	}

	char user_key_conf[USER_KEY_SIZE] = {0};
	memset(user_key, 0, USER_KEY_SIZE);

	retval = _read_protected_string_("new key (32 characters max, you can use your old key if there was one): ", user_key, sizeof(user_key));
	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: unable to read the new key, retval %d", retval);
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (%d)", retval);
		return retval;
	}

	retval = _read_protected_string_("confirm new key (32 characters max): ", user_key_conf, sizeof(user_key_conf));
	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: unable to read the confirmation key, retval %d", retval);
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (%d)", retval);
		return retval;
	}

	if(strncmp(user_key, user_key_conf, sizeof(user_key))){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: the confirmation key did not match");
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));
		close(cred_fd);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_ERR_KEY_MISMATCH)");
		return AP_ERR_KEY_MISMATCH;
	}

	OPENSSL_cleanse(user_key_conf, sizeof(user_key_conf));

	*strchr(user_key, '\n') = '\0';

	retval = _encrypt_and_store_creds_(cred_fd, cred_buff, cred_len, user_key);

	OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
	OPENSSL_cleanse(user_key, sizeof(user_key));
	close(cred_fd);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__reset_key__: encryption and storage failed, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (%d)", retval);
		return retval;
	}

	PRINT_MSG(LOG_LVL_INFO, "__reset_key__: credentials re-encrypted under the new key");
	PRINT_MSG(LOG_LVL_DEBUG, "__reset_key__: exit (AP_OK)");
	return AP_OK;
}

/**
 * @brief Load the credentials and provision the ESP with them.
 *
 * Backs @c --pc=. Extracts and validates the pairing code from the option, prompts for the
 * user key, decrypts the stored credentials, and then attempts provisioning up to
 * ::PROVISION_COUNTER_MAX times.
 *
 * The retry loop only retries what retrying can fix. A rejected pairing code, a malformed
 * BSSID or a rejected credential format will fail identically every time, so those return
 * at once; only transport-level failures go round again with a fresh session.
 *
 * @param opt       The full @c "--pc=<code>" option string.
 * @param file_path Path from ::check_cred_dir_and_file.
 * @return ::AP_OK on success, ::AP_ERR_PAIRING_FORMAT if the code is the wrong length, or a
 *         propagated error from decryption or provisioning.
 */
static ap_err_t __provision__(const char* opt, const char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "__provision__: entry");

	char pairing_code[PAIRING_CODE_LEN + 1] = {0};
	const char* code = opt + sizeof(PAIRING_CODE) - 1;

	if(strnlen(code, PAIRING_CODE_LEN + 1) == PAIRING_CODE_LEN){
		memcpy(pairing_code, code, PAIRING_CODE_LEN);
		pairing_code[PAIRING_CODE_LEN] = '\0';
	}else{
		PRINT_MSG(LOG_LVL_ERROR, "__provision__: pairing code must be exactly %d characters", PAIRING_CODE_LEN);
		PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (AP_ERR_PAIRING_FORMAT)");
		return AP_ERR_PAIRING_FORMAT;
	}

	char user_key[USER_KEY_SIZE] = {0};
	int retval = _read_protected_string_("key (32 characters max): ", user_key, sizeof(user_key));

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__provision__: unable to read the key, retval %d", retval);
		OPENSSL_cleanse(user_key, sizeof(user_key));
		PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (%d)", retval);
		return retval;
	}

	*strchr(user_key, '\n') = '\0';

	char cred_buff[CRED_BUFF_SIZE] = {0};
	int cred_fd = open(file_path, O_RDONLY);

	if(cred_fd < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__provision__: open failed, errno %d (%s)", errno, strerror(errno));
		OPENSSL_cleanse(user_key, sizeof(user_key));
		PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (AP_ERR_OPEN)");
		return AP_ERR_OPEN;
	}

	size_t cred_len = 0;
	retval = _load_and_decrypt_creds_(cred_fd, user_key, cred_buff, CRED_BUFF_SIZE, &cred_len);

	OPENSSL_cleanse(user_key, sizeof(user_key));
	close(cred_fd);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "__provision__: loading and decryption failed, retval %d", retval);
		OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
		PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (%d)", retval);
		return retval;
	}

	int prov = AP_ERR_FATAL;

	for(uint8_t i = 0; i < PROVISION_COUNTER_MAX; i++){
		PRINT_MSG(LOG_LVL_INFO, "__provision__: provisioning attempt %u of %d", i + 1, PROVISION_COUNTER_MAX);

		prov = _do_provision_(cred_buff, cred_len, pairing_code);

		if(prov == AP_ERR_BAD_ARGS || prov == AP_ERR_BSSID_FORMAT || prov == AP_ERR_REJECTED_CRYPTO || prov == AP_ERR_REJECTED_FORMAT){
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (%d, terminal)", prov);
			return prov;
		}else if(prov == AP_OK){
			OPENSSL_cleanse(cred_buff, sizeof(cred_buff));
			PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (AP_OK)");
			return prov;
		}else{
			PRINT_MSG(LOG_LVL_WARN, "__provision__: attempt %u failed (retval %d), retrying", i + 1, prov);
			continue;
		}
	}

	OPENSSL_cleanse(cred_buff, sizeof(cred_buff));

	PRINT_MSG(LOG_LVL_ERROR, "__provision__: all %d provisioning attempts failed", PROVISION_COUNTER_MAX);
	PRINT_MSG(LOG_LVL_DEBUG, "__provision__: exit (%d)", prov);
	return prov;
}

/**
 * @brief Dispatch the single option verb to its handler.
 *
 * Takes the verb itself rather than @c argc / @c argv: ::main has already separated the
 * logging flags from the option and guaranteed there is exactly one of the latter, so there
 * is nothing left here to parse.
 *
 * @param opt       The option verb (e.g. @c "--set_creds", @c "--pc=123456").
 * @param file_path Path from ::check_cred_dir_and_file.
 * @return ::AP_OK, ::AP_ERR_BAD_ARGS for an unrecognised verb, or a propagated error from
 *         the handler.
 */
static ap_err_t process_cli_args(const char* opt, const char* file_path){
	PRINT_MSG(LOG_LVL_DEBUG, "process_cli_args: entry (opt %s)", opt);

	int retval = AP_OK;

	if(!strncmp(opt, CLEAR_CREDS, sizeof(CLEAR_CREDS) - 1)){
		retval = _reset_file_((char*)file_path);

		if(retval < 0){
			PRINT_MSG(LOG_LVL_ERROR, "process_cli_args: unable to clear the credentials, retval %d", retval);
			PRINT_MSG(LOG_LVL_DEBUG, "process_cli_args: exit (%d)", retval);
			return retval;
		}

		PRINT_MSG(LOG_LVL_INFO, "process_cli_args: credentials cleared");
	}else if(!strncmp(opt, SET_CREDS, sizeof(SET_CREDS) - 1)){
		retval = __set_creds__(file_path);
	}else if(!strncmp(opt, RESET_KEY, sizeof(RESET_KEY) - 1)){
		retval = __reset_key__(file_path);
	}else if(!strncmp(opt, PAIRING_CODE, sizeof(PAIRING_CODE) - 1)){
		retval = __provision__(opt, file_path);
	}else if(!strncmp(opt, HELP, sizeof(HELP) - 1)){
		__print_help__();
	}else{
		PRINT_MSG(LOG_LVL_ERROR, "process_cli_args: invalid option '%s', use --help for details", opt);
		PRINT_MSG(LOG_LVL_DEBUG, "process_cli_args: exit (AP_ERR_BAD_ARGS)");
		return AP_ERR_BAD_ARGS;
	}

	PRINT_MSG(LOG_LVL_DEBUG, "process_cli_args: exit (%d)", retval);
	return retval;
}

/**
 * @brief Entry point: set up logging, isolate the single option verb, and dispatch it.
 *
 * ::log_init runs first, so every subsequent line honours @c -v / @c -q. It consumes the
 * logging flags and ignores everything else.
 *
 * The argv scan that follows enforces the "exactly one option" rule, but measures it in
 * @e non-logging arguments: ::log_is_log_arg identifies the ones that belong to the logging
 * layer and they are skipped. Without that distinction @c "ap_prov --pc=123456 -v" would be
 * rejected as two arguments, and the utility -- the one binary a user actually watches run
 * -- would be the one place @c -v could not be used.
 *
 * @note ::log_is_log_arg is the single source of truth for what a log flag is; ::log_init
 *       gates on the same function, so the two cannot drift apart.
 *
 * @warning Errors are tested with @c "< 0", not with @c "if(retval)". ::ap_err_t reserves
 *          positive values for retryable warnings, and a plain truth test would report one
 *          of those as a failure.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success; 1 on a usage error; 2 if the operation failed.
 */
int main(int argc, char** argv){
	log_init(argc, argv);

	PRINT_MSG(LOG_LVL_DEBUG, "main: entry");

	const char* opt = NULL;

	for(int i = 1; i < argc; i++){
		if(log_is_log_arg(argv[i])){
			continue;
		}

		if(opt){
			PRINT_MSG(LOG_LVL_ERROR, "main: only one option allowed, got '%s' and '%s'", opt, argv[i]);
			PRINT_MSG(LOG_LVL_ERROR, "main: use --help for details");
			PRINT_MSG(LOG_LVL_DEBUG, "main: exit (1)");
			return 1;
		}

		opt = argv[i];
	}

	if(!opt){
		PRINT_MSG(LOG_LVL_ERROR, "main: an option is required, use --help for details");
		PRINT_MSG(LOG_LVL_DEBUG, "main: exit (1)");
		return 1;
	}

	int retval = 0;
	char file_path[PATH_MAX] = {0};

	retval = check_cred_dir_and_file(file_path);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "main: check_cred_dir_and_file failed, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "main: exit (1)");
		return 1;
	}

	retval = process_cli_args(opt, file_path);

	if(retval < 0){
		PRINT_MSG(LOG_LVL_ERROR, "main: process_cli_args failed, retval %d", retval);
		PRINT_MSG(LOG_LVL_DEBUG, "main: exit (2)");
		return 2;
	}

	PRINT_MSG(LOG_LVL_DEBUG, "main: exit (0)");
	return 0;
}
