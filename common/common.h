#ifndef COMMON_H
#define COMMON_H

/**
 * @file common.h
 * @ingroup shared
 * @brief The wire protocol. Compiled into BOTH the PC daemon and the ESP firmware.
 *
 * This header is the contract between the two halves of the project, and it is the reason they
 * cannot silently disagree about the format: the same struct definitions are compiled into both
 * binaries, so a change to one side is a compile error on the other rather than a packet the
 * receiver quietly misreads.
 *
 * ### The three conversations
 * -# @b Discovery @b (UDP, port ::ESP_COMM_PORT_UDP). The PC broadcasts a ::discovery_req_t; the
 *    ESP answers with a ::discovery_ack_t naming the TCP port it is listening on. This is how the
 *    PC finds a board whose DHCP address it cannot know in advance.
 * -# @b Orientation @b (TCP). The ESP sends ::orient_cmd_t whenever the board is turned; the PC
 *    applies the rotation and replies with ::orient_ack_t.
 * -# @b Provisioning @b (TCP, port ::ESP_TCP_SERVER_PORT_AP, over the ESP's own AP). The PC sends
 *    ::prov_creds_t -- the Wi-Fi credentials, encrypted -- and the ESP replies with ::prov_ack_t.
 *    This only happens once, before the board has ever been on the network.
 *
 * ### Packet layout
 * Every packet is a ::proto_hdr_t followed by exactly one member of ::msg_payload_u:
 * @verbatim
 *   +--------+--------+--------+--------+------------------------+
 *   | magic  | msg_id | length | chksum |        payload         |
 *   | 0xA5A5 |        |        |        |  (one of msg_payload_u)|
 *   +--------+--------+--------+--------+------------------------+
 * @endverbatim
 * @c msg_id selects which union member the payload actually is. Reading the wrong one is not a
 * crash -- it is a silent misinterpretation of the same bytes -- so the header must be trusted
 * before the payload is touched.
 *
 * @warning Every struct here is @c __attribute__((packed)). That is not an optimisation: these
 *          bytes go on the wire, and the PC (x86-64) and the ESP32 (xtensa) have different natural
 *          alignments. Without @c packed the compiler would insert padding differently on each
 *          side, and the two would disagree about where every field after the first one begins.
 *
 * @warning Any change to a struct here, to ::msg_id_t, or to the crypto constants must be flashed
 *          to the ESP and rebuilt on the PC TOGETHER. A version skew does not fail loudly: the
 *          packets still arrive, ::proto_verify_chksum still passes, and the receiver reads garbage
 *          out of correctly-checksummed bytes.
 */

#include <stdint.h>
#include <stdbool.h>

/**
 * @name Network endpoints
 * @{
 */
/** @brief UDP port the ESP listens on for discovery broadcasts. */
#define ESP_COMM_PORT_UDP       5000

/** @brief TCP port the ESP's provisioning server listens on, while its own AP is up. */
#define ESP_TCP_SERVER_PORT_AP  8000
/** @} */

/**
 * @brief Compiled-in salt for the provisioning session-key derivation.
 *
 * Both sides derive the transport key from the pairing code plus this constant, so it never crosses
 * the wire. It is not a secret in any meaningful sense -- it is in both binaries -- but it does mean
 * a pairing code alone is not enough to talk to the board.
 *
 * @warning The PC and the ESP must derive the key over the same bytes, INCLUDING the same length.
 *          A mismatch does not fail loudly: it silently yields a different key, and every
 *          provisioning attempt is then rejected as ::PROV_STATUS_BAD_CRYPTO with no clue as to why.
 */
#define MASTER_SECRET_COMM      "secret_esp32_pc"

/**
 * @name The ESP's provisioning access point
 * @brief Brought up only when the board has no working credentials, and torn down once it has.
 * @{
 */
#define ESP_AP_WIFI_SSID        "esp_wroom_32_ap"   /**< SSID of the temporary provisioning AP. */
#define ESP_AP_WIFI_PSD         "myesp@32"          /**< Its passphrase. */
#define ESP_AP_IP               "192.168.4.1"       /**< ESP-IDF's default gateway address for a SoftAP. */
#define ESP_AP_MAX_CONN         1                   /**< One client. Only the provisioning tool should ever connect. */
#define ESP_AP_AUTHMODE         WIFI_AUTH_WPA2_PSK  /**< Set explicitly: some Linux clients will not associate otherwise. */
/** @} */

/**
 * @name Crypto parameters
 * @brief Shared by the at-rest encryption on the PC and the in-transit encryption to the ESP.
 *
 * AES-256-GCM throughout. GCM is chosen for the authentication tag: it makes a wrong key fail
 * loudly at the decrypt step rather than yielding plausible-looking garbage.
 * @{
 */
#define		AES_KEY_SIZE    32      /**< AES-256. */
#define		SALT_SIZE		16      /**< PBKDF2 salt. Random per encryption, never reused. */
#define		NONCE_SIZE		12      /**< GCM IV. Random per encryption -- see the warning below. */
#define		TAG_SIZE		16      /**< GCM authentication tag. */
#define		PBKDF2_ITERS	10000   /**< Iteration count. Deliberately slow: it is what makes brute-forcing a short key expensive. */
/** @} */

/**
 * @brief Length of the pairing code shown on the ESP's LCD.
 *
 * @note This is NOT the user key. The pairing code protects the credentials in TRANSIT to the ESP;
 *       the user key protects them AT REST on the PC and is never stored anywhere. Confusing the two
 *       defeats the design.
 */
#define PAIRING_CODE_LEN             6

/**
 * @brief Message identifier. Selects which member of ::msg_payload_u the payload is.
 *
 * @warning This is the ONLY thing distinguishing one payload from another -- the union members
 *          overlap in memory, and reading the wrong one does not fail, it simply reinterprets the
 *          same bytes. Validate the header before touching the payload.
 */
typedef enum {
	/* ---- Discovery (UDP) ---- */
	MSG_DISCOVERY_REQ = 1,  /**< PC -> ESP. Broadcast: "are you out there?" */
	MSG_DISCOVERY_ACK,      /**< ESP -> PC. "Yes, and here is my TCP port." */

	/* ---- Orientation (TCP) ---- */
	MSG_ORIENT_CMD,         /**< ESP -> PC. "I have been turned; rotate the display." */
	MSG_ORIENT_ACK,         /**< PC -> ESP. "Applied." */

	/* ---- Provisioning (TCP, over the ESP's own AP) ---- */
	MSG_PROV_CREDS,         /**< PC -> ESP. Encrypted Wi-Fi credentials. */
	MSG_PROV_ACK            /**< ESP -> PC. Accepted, or why not. */

} msg_id_t;

/**
 * @brief Sentinel that opens every packet.
 *
 * A framing check, not a security one. It costs two bytes and catches the case where the stream has
 * desynchronised -- a partial read, a stale buffer -- before anything downstream tries to interpret
 * the misaligned bytes as a header.
 */
#define MAGIC_BYTES         0xA5A5

/**
 * @brief Header prefixed to every packet, in both directions.
 *
 * ::proto_verify_chksum must pass before the payload is read. Not because the network is hostile --
 * it is a local Wi-Fi link -- but because a truncated or interleaved read produces bytes that are
 * structurally valid and semantically nonsense, and the checksum is the only thing that tells them
 * apart from a real packet.
 */
typedef struct __attribute__((packed)) {
	uint16_t magic;   /**< ::MAGIC_BYTES. Framing sentinel; see above. */
	uint16_t msg_id;  /**< One of ::msg_id_t. Selects the payload type. */
	uint16_t length;  /**< Payload length in bytes, excluding this header. */
	uint16_t chksum;  /**< Computed by ::proto_generate_chksum over the whole packet. */
} proto_hdr_t;

/**
 * @name Discovery
 * @{
 */
/** @brief The name the ESP answers to. Broadcast by the PC, matched by the ESP. */
#define NAME        "esp_wroom_32?"

/** @brief Length of ::NAME without its terminator -- the terminator is not sent. */
#define NAME_SIZE   (sizeof(NAME) - 1)

/**
 * @brief Discovery broadcast. PC -> ESP, over UDP.
 *
 * The PC has no idea what address the ESP was given by DHCP, so it shouts on the broadcast address
 * and waits to see who answers.
 *
 * @note Not NUL-terminated. ::NAME_SIZE is @c sizeof-1 precisely so the terminator is not sent: it
 *       would be a wasted byte, and the receiver compares a known length rather than scanning for
 *       one.
 */
typedef struct __attribute__((packed)) {
	char name[NAME_SIZE];  /**< ::NAME, without its NUL. */
} discovery_req_t;

/**
 * @brief Discovery reply. ESP -> PC, over UDP.
 *
 * Carries the TCP port rather than assuming a fixed one, so the firmware can move it without the PC
 * needing to be rebuilt. The PC then opens a TCP session to the source address this arrived from,
 * on this port.
 */
typedef struct __attribute__((packed)) {
	uint16_t tcp_port;  /**< Port the ESP's orientation server is listening on. */
} discovery_ack_t;
/** @} */

/**
 * @brief Display orientation, as the accelerometer sees it.
 *
 * The values ARE the rotation in degrees, so they pass straight to @c xrandr with no lookup table in
 * between -- @c ORIENT_90 means @c "xrandr --rotate left" applies a 90-degree rotation. Changing
 * these numbers changes the wire format.
 */
typedef enum {
	ORIENT_0       = 0,    /**< Upright. @c xrandr @c normal. */
	ORIENT_90      = 90,   /**< Rotated a quarter turn. */
	ORIENT_180     = 180,  /**< Inverted. */
	ORIENT_270     = 270,  /**< Rotated a quarter turn the other way. */
	ORIENT_INVALID = -1    /**< Not a real orientation: the sentinel for "unknown yet" and for a rejected reading. */
} display_orient_t;

/**
 * @brief Sequence stamp carried by every orientation command and its acknowledgement.
 *
 * Two fields, because one is not enough. @c seq_us is monotonic only WITHIN a boot -- the ESP's
 * timer restarts from zero on reset -- so after a reboot its sequence numbers begin again from the
 * bottom, and a PC comparing @c seq_us alone would dismiss every fresh command as stale. The
 * @c boot_epoch, incremented in NVS on each boot, breaks that tie: a command from a NEWER epoch is
 * always newer, whatever its @c seq_us says.
 */
typedef struct __attribute__((packed)) {
	uint64_t seq_us;      /**< @c esp_timer_get_time() at send. Monotonic within one boot only. */
	uint32_t boot_epoch;  /**< Incremented in NVS on every ESP boot. Disambiguates across resets. */
} _cmd_seq_t;

/**
 * @brief Orientation command. ESP -> PC.
 *
 * Sent when the board is turned. The PC applies the rotation and answers with an ::orient_ack_t
 * carrying the same ::_cmd_seq_t, which is how the ESP knows WHICH command was applied rather than
 * merely that something was.
 */
typedef struct __attribute__((packed)) {
	_cmd_seq_t       seq;          /**< Stamp; echoed back in the ack. */
	display_orient_t orientation;  /**< The orientation to apply. */
} orient_cmd_t;

/**
 * @brief Orientation acknowledgement. PC -> ESP.
 *
 * Carries only the sequence stamp: it identifies which command is being acknowledged, and there is
 * nothing else the ESP needs to know.
 */
typedef struct __attribute__((packed)) {
	_cmd_seq_t seq;  /**< Copied verbatim from the ::orient_cmd_t being acknowledged. */
} orient_ack_t;

/**
 * @brief Wi-Fi credentials, in plaintext.
 *
 * @warning This is the PLAINTEXT struct. It never travels in the clear: it is encrypted into
 *          ::prov_creds_t before it leaves the PC, and it exists on the wire only as ciphertext.
 *          On the PC it is @c OPENSSL_cleanse'd the moment it is no longer needed.
 */
typedef struct __attribute__((packed)) {
	char    ssid[32];       /**< NUL-terminated. */
	char    password[64];   /**< NUL-terminated. */
	bool    bssid_set;      /**< Whether @c bssid below is meaningful. */
	uint8_t bssid[6];       /**< Pins the connection to one specific AP. Only read when @c bssid_set. */
	uint8_t security_type;  /**< Reserved for future use. */
	uint8_t reserved;       /**< Padding, and room to grow without changing the struct size. */
} _sta_credentials_t;

/**
 * @brief Encrypted credentials, as they actually go on the wire. PC -> ESP.
 *
 * A ::_sta_credentials_t encrypted with AES-256-GCM under the session key both sides derive from the
 * pairing code and ::MASTER_SECRET_COMM.
 *
 * @note @c ciphertext is exactly @c sizeof(_sta_credentials_t) because GCM is a stream mode: the
 *       ciphertext is the same length as the plaintext, with no block padding. That is why the size
 *       can be fixed at compile time rather than sent.
 *
 * @warning The nonce must be freshly random for EVERY packet. Reusing one under the same key is
 *          catastrophic for GCM specifically -- it leaks the XOR of the two plaintexts and can
 *          expose the authentication subkey, which would let an attacker forge packets rather than
 *          merely read them.
 */
typedef struct __attribute__((packed)) {
	uint8_t salt[SALT_SIZE];    /**< PBKDF2 salt. */
	uint8_t nonce[NONCE_SIZE];  /**< GCM IV. Random per packet, never reused. */
	uint8_t tag[TAG_SIZE];      /**< GCM authentication tag. A wrong key fails HERE, cleanly. */
	uint8_t ciphertext[sizeof(_sta_credentials_t)];  /**< The encrypted credentials. */
} prov_creds_t;

/**
 * @brief The ESP's verdict on a provisioning attempt.
 *
 * The distinction that matters is between the values worth retrying and the ones that are not:
 *
 * - ::PROV_STATUS_CORRUPT_PKT and ::PROV_STATUS_INVALID_PKT mean the packet was damaged in transit.
 *   Resending is exactly the right response, and the PC does.
 * - ::PROV_STATUS_BAD_CRYPTO means the pairing code is wrong. Resending the identical packet with
 *   the identical wrong code cannot possibly succeed, so the PC gives up immediately rather than
 *   burning its retry budget.
 * - ::PROV_STATUS_BAD_FORMAT means the credentials themselves are malformed. Same reasoning:
 *   terminal.
 *
 * @note Powers of two, so several could in principle be OR'd together. Currently only one is ever
 *       sent.
 */
typedef enum {
	PROV_STATUS_ACCEPTED    = 0x01,  /**< Tag verified, credentials well-formed. The ESP will now retry with them. */
	PROV_STATUS_CORRUPT_PKT = 0x02,  /**< Checksum or framing failed. Retryable: resend. */
	PROV_STATUS_BAD_CRYPTO  = 0x04,  /**< GCM tag verification failed -- the pairing code is wrong. NOT retryable. */
	PROV_STATUS_BAD_FORMAT  = 0x08,  /**< Decrypted cleanly, but the SSID or password is malformed. NOT retryable. */
	PROV_STATUS_INVALID_PKT = 0x10   /**< Header was not what was expected. Retryable: resend. */
} prov_status_t;

/**
 * @brief Provisioning acknowledgement. ESP -> PC.
 */
typedef struct __attribute__((packed)) {
	uint8_t status;  /**< One of ::prov_status_t. */
} prov_ack_t;

/**
 * @brief The payload. Exactly one member is meaningful, chosen by @c msg_id in the header.
 *
 * A union rather than a tagged struct because a packet is only ever one kind of thing, and the union
 * makes the buffer exactly as large as the largest message rather than the sum of all of them.
 *
 * @warning The members OVERLAP in memory. Reading the wrong one does not fail -- it reinterprets the
 *          same bytes as a different type and returns whatever they happen to mean. So @c msg_id
 *          must be validated before the payload is touched, every time.
 */
typedef union {
	discovery_req_t discovery_req;  /**< When @c msg_id is ::MSG_DISCOVERY_REQ. */
	discovery_ack_t discovery_ack;  /**< When @c msg_id is ::MSG_DISCOVERY_ACK. */
	orient_cmd_t    orient_cmd;     /**< When @c msg_id is ::MSG_ORIENT_CMD. */
	orient_ack_t    orient_ack;     /**< When @c msg_id is ::MSG_ORIENT_ACK. */
	prov_creds_t    prov_creds;     /**< When @c msg_id is ::MSG_PROV_CREDS. */
	prov_ack_t      prov_ack;       /**< When @c msg_id is ::MSG_PROV_ACK. */
} msg_payload_u;

/**
 * @brief A complete packet: header, then payload.
 *
 * The single type both sides send and receive. Note that the whole union is allocated even when the
 * payload in use is much smaller -- the @c length field in the header says how many bytes are
 * actually meaningful, and that is what is transmitted, not @c sizeof(proto_packet_t).
 */
typedef struct __attribute__((packed)) {
	proto_hdr_t   header;   /**< Magic, message id, length, checksum. */
	msg_payload_u payload;  /**< The message itself; @c header.msg_id says which. */
} proto_packet_t;

/**
 * @brief Compute the checksum for a packet.
 *
 * @note Must be called LAST, after every other header field is final, because it covers them.
 *       Computing it earlier checksums a half-built header, and the receiver rejects the result.
 *
 * @param pkt The packet, with everything except @c header.chksum already filled in.
 * @return The checksum, to be written into @c pkt->header.chksum.
 */
uint16_t proto_generate_chksum(const proto_packet_t* pkt);

/**
 * @brief Verify a received packet's checksum.
 *
 * @warning Call this BEFORE reading the payload. ::msg_payload_u is a union whose members overlap,
 *          so a corrupt @c msg_id does not produce an error -- it produces a confident
 *          misinterpretation of the bytes as the wrong message type.
 *
 * @param pkt The received packet.
 * @return @c true if the checksum matches; @c false if the packet is corrupt.
 */
bool proto_verify_chksum(const proto_packet_t* pkt);

#endif /* COMMON_H */
