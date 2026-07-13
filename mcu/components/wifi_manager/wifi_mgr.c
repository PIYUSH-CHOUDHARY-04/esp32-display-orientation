/**
 * @file wifi_mgr.c
 * @ingroup esp_firmware
 * @brief Wi-Fi connection manager: a single queue-driven FreeRTOS task that runs
 *        the whole STA/AP lifecycle, PC discovery, provisioning, and command send.
 *
 * ### Design: one task, one queue, layered error recovery
 * All work happens in ::wifi_task, which blocks on a message queue and dispatches
 * one ::wifi_msg_id_t at a time. Every worker function returns a ::wifi_mgr_err_t,
 * and the dispatcher decides the next message(s) from that result. The event
 * handler (::wifi_event_handler) only translates raw ESP-IDF Wi-Fi/IP events into
 * queue messages; it never touches sockets directly.
 *
 * ### The wifi_mgr_err_t taxonomy — "the error tells you which layer to rebuild"
 * The result codes are grouped so the dispatcher knows the smallest thing it can
 * rebuild:
 * - success: @c WIFI_MGR_OK, @c WIFI_MGR_GOT_CONN, @c WIFI_MGR_CREDS_ACCEPTED,
 *   @c WIFI_MGR_FOUND_PC;
 * - soft/transient -> @c WIFI_MGR_RETRY: re-post the same op, the socket stays open;
 * - peer-level -> @c WIFI_MGR_PEER_GONE: the PC socket is dead, the listener is
 *   fine, so just re-accept;
 * - listener-level -> @c WIFI_MGR_LISTENER_DEAD: rebuild the TCP server;
 * - udp-level -> @c WIFI_MGR_UDP_DEAD: rebuild the UDP socket and rediscover;
 * - state mismatch -> @c WIFI_MGR_BAD_STATE: rebuild the relevant mode;
 * - hard -> @c WIFI_MGR_FATAL: tear everything down and exit the task.
 *
 * ### The RETRY invariant
 * A function returning @c WIFI_MGR_RETRY must leave its socket healthy and open;
 * the dispatcher simply re-posts the same message. Closing a socket on the RETRY
 * path would break this contract, so those paths deliberately do NOT close.
 *
 * ### Logging
 * All logging is developer-facing UART trace via ::PRINT_MSG and carries no
 * end-user meaning. Every function logs @c LOG_LVL_DEBUG on entry and on each exit
 * path, @c LOG_LVL_ERROR at each failure (naming the function and the failing
 * call/reason), @c LOG_LVL_INFO at operational milestones, and @c LOG_LVL_WARN on
 * benign/recoverable conditions.
 */

#define LOG_COMPONENT_WIFI_MGR
#include "wifi_mgr.h"
#include "common.h"
#include "logging.h"

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/gcm.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "nvs.h"

#include "lcd.h"

/**
 * @brief Semantic result of every internal worker function.
 *
 * The dispatcher maps each value to the next action; see the file header for the
 * "which layer to rebuild" grouping and the RETRY invariant.
 */
typedef enum {
    WIFI_MGR_OK = 0,            /**< Generic success, nothing further implied. */
    WIFI_MGR_GOT_CONN,          /**< accept_pc_conn: a PC connected (mode in msg.u.wifi_mode). */
    WIFI_MGR_CREDS_ACCEPTED,    /**< get_router_creds: valid creds stored, go STA. */
    WIFI_MGR_FOUND_PC,          /**< discover_pc: PC discovered, start server. */

    WIFI_MGR_CREDS_NVS_EMPTY,   /**< No STA credentials stored in NVS. */
    WIFI_MGR_CREDS_MALFORMED,   /**< Stored STA credentials are present but invalid. */

    WIFI_MGR_RETRY,             /**< Timed out, no error; re-post the same op (socket stays open). */

    WIFI_MGR_CREDS_REJECTED,    /**< get_router_creds: ACK(reason) sent, socket closed, stay AP + re-accept. */

    WIFI_MGR_PEER_GONE,         /**< PC socket dead/closed; re-accept on the existing listener. */

    WIFI_MGR_LISTENER_DEAD,     /**< TCP listener socket gone; rebuild the TCP server. */

    WIFI_MGR_UDP_DEAD,          /**< UDP socket gone; rebuild UDP + rediscover. */

    WIFI_MGR_BAD_STATE,         /**< is_sta_up/is_ap_up false when required; rebuild that mode. */

    WIFI_MGR_FATAL,             /**< esp_wifi_* / netif / queue failure; tear down and exit. */

    WIFI_MGR_NOT_READY,         /**< send_command: no PC connection; caller caches last_orient. */
} wifi_mgr_err_t;

/**
 * @brief All Wi-Fi manager runtime state (single static instance ::ws).
 *
 * Holds the two netif handles, the three socket fds (sentinel -1 = closed), the
 * current mode and STA/AP up flags, the STA config, the AP pairing code and derived
 * AES session key, the NVS handle, and the boot epoch used for command sequencing.
 */
struct wifi_state {
    esp_netif_t* interface_sta;                  /**< Default STA netif handle. */
    esp_netif_t* interface_ap;                   /**< Default AP netif handle. */
    wifi_mgr_mode_t curr_mode;                   /**< Current mode (NONE/STA/AP). */
    bool is_sta_up;                              /**< True once STA has an IP (set on GOT_IP). */
    int sock_fd_udp;                             /**< UDP discovery socket (-1 = closed). */
    int sock_fd_tcp;                             /**< TCP listener socket (-1 = closed). */
    struct sockaddr_in pc_addr;                  /**< Connected PC address. */
    int pc_sock_fd;                              /**< Accepted PC connection socket (-1 = closed). */
    uint32_t _boot_epoch;                        /**< Per-boot counter for command sequencing. */
    bool is_ap_up;                               /**< True while AP mode is running. */
    wifi_config_t* cfg_ptr;                      /**< Non-NULL when a fresh STA config is staged. */
    wifi_config_t sta_wcfg;                      /**< STA credentials/config. */
    uint8_t pairing_code[PAIRING_CODE_LEN + 1];  /**< AP pairing code (NUL-terminated). */
    uint8_t aes_key[AES_KEY_SIZE];               /**< AES-GCM session key derived from the pairing code. */
    nvs_handle_t nvs;                            /**< Open NVS handle for STA credentials. */
    bool is_creds_write_pending;                 /**< True when accepted creds await a deferred NVS write. */
};

static struct wifi_state ws = {
    .interface_sta = NULL,
    .interface_ap = NULL,
    .curr_mode = WIFI_MGR_MODE_NONE,
    .is_sta_up = false,
    .sock_fd_udp = -1,
    .sock_fd_tcp = -1,
    .pc_addr = {0},
    .pc_sock_fd = -1,
    ._boot_epoch = 0,
    .is_ap_up = false,
    .cfg_ptr = NULL,
    .nvs = 0,
    .is_creds_write_pending = false,
};

/**
 * @brief Internal event ids exchanged over the queue.
 *
 * These are the manager's own events, remapped from raw ESP events by
 * ::wifi_event_handler, so the dispatcher needs no @c event_base — there is no
 * overlap between ids.
 */
typedef enum {
    WIFI_MGR_DRIVER_INIT = 1,   /**< Initialise the Wi-Fi driver. */
    WIFI_MGR_DRIVER_DEINIT,     /**< Deinitialise the driver and exit the task. */
    WIFI_MGR_STA_START,         /**< Start station mode. */
    WIFI_MGR_STA_STOP,          /**< Stop station mode. */
    WIFI_MGR_AP_START,          /**< Start access-point mode. */
    WIFI_MGR_AP_STOP,           /**< Stop access-point mode. */
    WIFI_MGR_CREATE_UDP_SOCKET, /**< Create the UDP discovery socket. */
    WIFI_MGR_DISCOVER_PC,       /**< Listen for a PC discovery broadcast. */
    WIFI_MGR_START_TCP_SERVER,  /**< Bring up the TCP listener. */
    WIFI_MGR_ACCEPT_PC_CONN,    /**< Accept a PC connection. */
    WIFI_MGR_GET_ROUTER_CREDS,  /**< Receive+decrypt provisioning credentials. */
    WIFI_MGR_SEND_COMMAND       /**< Send an orientation command. */
} wifi_msg_id_t;

/**
 * @brief One queue message: an id plus an optional orientation or mode payload.
 */
typedef struct {
    wifi_msg_id_t id;              /**< Which event this message carries. */
    union {
        display_orient_t orient;   /**< Orientation, for SEND_COMMAND. */
        wifi_mgr_mode_t wifi_mode; /**< Mode tag, for TCP-server/accept messages. */
    } u;
} wifi_msg_t;

/**
 * @brief Forward declaration: raw ESP event -> internal queue message translator.
 *
 * Used inside ::wifi_mgr_driver_init before its definition, and intentionally kept
 * file-private (not in wifi_mgr.h).
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

/**
 * @name Boot-epoch NVS keys
 * @{
 */
#define NVS_NS_BOOT                 "boot"    /**< NVS namespace for the boot counter. */
#define NVS_KEY_EPOCH               "epoch"   /**< NVS key for the boot counter. */
/** @} */

/**
 * @brief Load and increment the persistent boot epoch.
 *
 * Reads the boot counter from NVS, increments it (1 on first boot), writes it back,
 * and caches it in @c ws._boot_epoch. The epoch tags outgoing orientation commands
 * so the PC can detect stale/replayed commands across reboots.
 *
 * @return @c ESP_OK on success, or the failing @c esp_err_t from an NVS call.
 */
esp_err_t _get_boot_epoch(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_get_boot_epoch: entry");
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_BOOT, NVS_READWRITE, &h);
    if (err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_get_boot_epoch: nvs_open failed, retval %d", err);
        return err;
    }
    uint32_t epoch = 0;
    err = nvs_get_u32(h, NVS_KEY_EPOCH, &epoch);
    if(err == ESP_ERR_NVS_NOT_FOUND){
        epoch = 1;
    }else
    if(err != ESP_OK){
        nvs_close(h);
        PRINT_MSG(LOG_LVL_ERROR, "_get_boot_epoch: nvs_get_u32 failed, retval %d", err);
        return err;
    }else{
        epoch++;
    }
    err = nvs_set_u32(h, NVS_KEY_EPOCH, epoch);
    if(err == ESP_OK){
        err = nvs_commit(h);
        nvs_close(h);
    }
    if(err != ESP_OK){
        nvs_close(h);
        PRINT_MSG(LOG_LVL_ERROR, "_get_boot_epoch: nvs write/commit failed, retval %d", err);
        return err;
    }
    ws._boot_epoch = epoch;
    PRINT_MSG(LOG_LVL_INFO, "_get_boot_epoch: boot epoch %u", (unsigned)epoch);
    PRINT_MSG(LOG_LVL_DEBUG, "_get_boot_epoch: exit");
    return ESP_OK;
}

/**
 * @name STA credential NVS keys
 * @{
 */
#define SSID_KEY        "ssid"           /**< NVS key: STA SSID. */
#define PSD_KEY         "psd"            /**< NVS key: STA password. */
#define BSSID_BIT_KEY   "bssid_set_bit"  /**< NVS key: whether a BSSID is pinned. */
#define BSSID_KEY       "bssid_key"      /**< NVS key: the pinned BSSID bytes. */
/** @} */

/**
 * @brief Load STA credentials (SSID/password/optional BSSID) from NVS into @c ws.
 *
 * Distinguishes three outcomes per field: not-found (namespace empty), malformed
 * (present but zero/over-length), and a hard NVS error. The SSID/password length
 * checks reject an empty string; an over-length string is impossible because
 * @c nvs_get_str is bounded by the destination buffer and cannot overflow it. The
 * BSSID is only read when @c bssid_set is true.
 *
 * @return @c WIFI_MGR_OK if a full valid set was read; @c WIFI_MGR_CREDS_NVS_EMPTY
 *         if any field is missing; @c WIFI_MGR_CREDS_MALFORMED if a field is
 *         present but invalid; @c WIFI_MGR_FATAL on a hard NVS error.
 */
static wifi_mgr_err_t _retrieve_sta_creds_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_retrieve_sta_creds_: entry");
    memset(&ws.sta_wcfg, 0, sizeof(ws.sta_wcfg));

    size_t len = sizeof(ws.sta_wcfg.sta.ssid);
    esp_err_t err = nvs_get_str(ws.nvs, SSID_KEY, (char*)ws.sta_wcfg.sta.ssid, &len);
    if(err == ESP_ERR_NVS_NOT_FOUND){
        PRINT_MSG(LOG_LVL_WARN, "_retrieve_sta_creds_: SSID not found in NVS");
        return WIFI_MGR_CREDS_NVS_EMPTY;
    }
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: nvs_get_str(ssid) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }
    if(len <= 1){
        PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: SSID malformed (len %u)", (unsigned)len);
        return WIFI_MGR_CREDS_MALFORMED;
    }

    len = sizeof(ws.sta_wcfg.sta.password);
    err = nvs_get_str(ws.nvs, PSD_KEY, (char*)ws.sta_wcfg.sta.password, &len);
    if(err == ESP_ERR_NVS_NOT_FOUND){
        PRINT_MSG(LOG_LVL_WARN, "_retrieve_sta_creds_: password not found in NVS");
        return WIFI_MGR_CREDS_NVS_EMPTY;
    }
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: nvs_get_str(psd) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }
    if(len <= 1){
        PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: password malformed (len %u)", (unsigned)len);
        return WIFI_MGR_CREDS_MALFORMED;
    }

    err = nvs_get_u8(ws.nvs, BSSID_BIT_KEY, (uint8_t*)&ws.sta_wcfg.sta.bssid_set);
    if(err == ESP_ERR_NVS_NOT_FOUND){
        PRINT_MSG(LOG_LVL_WARN, "_retrieve_sta_creds_: bssid_set bit not found in NVS");
        return WIFI_MGR_CREDS_NVS_EMPTY;
    }
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: nvs_get_u8(bssid_bit) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }

    if(ws.sta_wcfg.sta.bssid_set){
        len = sizeof(ws.sta_wcfg.sta.bssid);
        err = nvs_get_blob(ws.nvs, BSSID_KEY, ws.sta_wcfg.sta.bssid, &len);
        if(err == ESP_ERR_NVS_NOT_FOUND){
            PRINT_MSG(LOG_LVL_WARN, "_retrieve_sta_creds_: BSSID not found in NVS");
            return WIFI_MGR_CREDS_NVS_EMPTY;
        }
        if(err != ESP_OK){
            PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: nvs_get_blob(bssid) failed, retval %d", err);
            return WIFI_MGR_FATAL;
        }
        if(len != sizeof(ws.sta_wcfg.sta.bssid)){
            PRINT_MSG(LOG_LVL_ERROR, "_retrieve_sta_creds_: BSSID malformed (len %u)", (unsigned)len);
            return WIFI_MGR_CREDS_MALFORMED;
        }
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_retrieve_sta_creds_: exit (creds loaded)");
    return WIFI_MGR_OK;
}

/**
 * @brief Start station mode: set mode, load creds, apply config, start, connect.
 *
 * If no config is already staged (@c ws.cfg_ptr NULL) it loads credentials from
 * NVS first, propagating an empty/malformed result to the caller so the dispatcher
 * can fall back to AP provisioning. @c ws.is_sta_up is deliberately NOT set here:
 * @c esp_wifi_connect() is asynchronous, so "up" is set later on GOT_IP.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_CREDS_NVS_EMPTY /
 *         @c WIFI_MGR_CREDS_MALFORMED when creds are unusable; @c WIFI_MGR_FATAL on
 *         any esp_wifi_* failure.
 */
static wifi_mgr_err_t wifi_mgr_sta_start(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_sta_start: entry");
    wifi_mgr_err_t retval = WIFI_MGR_OK;
    retval = esp_wifi_set_mode(WIFI_MODE_STA);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_start: esp_wifi_set_mode failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    if(!ws.cfg_ptr){
        retval = _retrieve_sta_creds_();
        if(retval != WIFI_MGR_OK){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_start: _retrieve_sta_creds_ failed, retval %d", retval);
            if(retval == WIFI_MGR_CREDS_NVS_EMPTY || retval == WIFI_MGR_CREDS_MALFORMED){
                PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_sta_start: need fresh credentials");
            }
            return retval;
        }
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    retval = esp_wifi_set_config(WIFI_IF_STA, &ws.sta_wcfg);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_start: esp_wifi_set_config failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    retval = esp_wifi_start();
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_start: esp_wifi_start failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }
    ws.curr_mode = WIFI_MGR_MODE_STA;

    retval = esp_wifi_connect();
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_start: esp_wifi_connect failed, retval %d", retval);
        esp_wifi_stop();
        return WIFI_MGR_FATAL;
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_sta_start: STA started, connect attempt initiated");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_sta_start: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Socket-cleanup helper for a lost link (shared by STA and AP stop).
 *
 * Clears @c is_sta_up and closes whichever of the UDP, TCP-listener, and PC sockets
 * are open, resetting their fds to -1 and clearing @c pc_addr. Called when the
 * router disappears mid-operation, so any of the three may be live: discovery
 * (UDP), listening (TCP), or an active PC connection.
 */
static void _wifi_mgr_router_gone_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_wifi_mgr_router_gone_: entry");
    PRINT_MSG(LOG_LVL_WARN, "_wifi_mgr_router_gone_: link down, cleaning up sockets");
    ws.is_sta_up = false;
    if(ws.sock_fd_udp >= 0){
        close(ws.sock_fd_udp);
        ws.sock_fd_udp = -1;
    }

    if(ws.sock_fd_tcp >= 0){
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
    }

     if(ws.pc_sock_fd >= 0){
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        memset(&(ws.pc_addr), 0, sizeof(struct sockaddr_in));
    }
    PRINT_MSG(LOG_LVL_DEBUG, "_wifi_mgr_router_gone_: exit");
    return;
}

/**
 * @brief Stop station mode: clean up sockets, disconnect, stop the driver.
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL if disconnect/stop fails.
 */
static wifi_mgr_err_t wifi_mgr_sta_stop(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_sta_stop: entry");

    _wifi_mgr_router_gone_();

    esp_err_t retval;
    retval = esp_wifi_disconnect();
    if((retval != ESP_OK) && (retval != ESP_ERR_WIFI_NOT_CONNECT)){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_stop: esp_wifi_disconnect failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    retval = esp_wifi_stop();
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_sta_stop: esp_wifi_stop failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    ws.curr_mode = WIFI_MGR_MODE_NONE;
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_sta_stop: STA stopped");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_sta_stop: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Generate a random human-readable AP pairing code into @p pc.
 *
 * Fills @c PAIRING_CODE_LEN characters from a Crockford-style alphabet (no
 * ambiguous 0/1/I/O) using @c esp_random, then NUL-terminates.
 *
 * @param pc Output buffer of @c PAIRING_CODE_LEN+1 bytes.
 */
static void __generate_pairing_code__(uint8_t pc[PAIRING_CODE_LEN + 1]){
    PRINT_MSG(LOG_LVL_DEBUG, "__generate_pairing_code__: entry");
    static const char alphabet[] = "23456789ABCDEFGHJKLMNOPQRSTUVWXYZ";
    for(int i = 0; i < PAIRING_CODE_LEN; i++){
        pc[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }

    pc[PAIRING_CODE_LEN] = '\0';
    PRINT_MSG(LOG_LVL_DEBUG, "__generate_pairing_code__: exit");
    return;
}

/**
 * @brief Derive the AES session key from the pairing code via PBKDF2-HMAC-SHA256.
 *
 * Runs @c PBKDF2_ITERS iterations over the pairing code salted with
 * @c MASTER_SECRET_COMM to produce an @c AES_KEY_SIZE key used for AES-GCM
 * provisioning decryption.
 *
 * @param pairing_code NUL-terminated pairing code.
 * @param aes_key      Output buffer of @c AES_KEY_SIZE bytes.
 * @return 0 on success; -1 on NULL argument; -2 on md-setup failure; -3 on PBKDF2
 *         failure.
 */
static int __derive_session_key__(const uint8_t* pairing_code, uint8_t aes_key[AES_KEY_SIZE]){
    PRINT_MSG(LOG_LVL_DEBUG, "__derive_session_key__: entry");
    int ret;
    mbedtls_md_context_t md_ctx;
    if(!pairing_code || !aes_key){
        PRINT_MSG(LOG_LVL_ERROR, "__derive_session_key__: NULL argument, returning -1");
        return -1;
    }
    mbedtls_md_init(&md_ctx);
    ret = mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

    if(ret != 0){
        mbedtls_md_free(&md_ctx);
        PRINT_MSG(LOG_LVL_ERROR, "__derive_session_key__: mbedtls_md_setup failed, ret %d, returning -2", ret);
        return -2;
    }

    ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)pairing_code, strlen((const char*)pairing_code),
        (const unsigned char *)MASTER_SECRET_COMM, sizeof(MASTER_SECRET_COMM) - 1,
        PBKDF2_ITERS, AES_KEY_SIZE, aes_key);

    mbedtls_md_free(&md_ctx);
    if(ret != 0){
        PRINT_MSG(LOG_LVL_ERROR, "__derive_session_key__: pbkdf2 failed, ret %d, returning -3", ret);
        return -3;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "__derive_session_key__: exit");
    return 0;
}

/**
 * @brief Start access-point mode for provisioning.
 *
 * Sets AP mode and config (one client max, WPA2-PSK), starts the driver, generates
 * a fresh pairing code, shows it on the LCD (pinning row 0 so it stays visible),
 * and derives the AES session key from it. On key-derivation failure the driver is
 * stopped.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL on any esp_wifi_* or
 *         key-derivation failure.
 */
static wifi_mgr_err_t wifi_mgr_ap_start(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_ap_start: entry");
    esp_err_t retval = ESP_OK;
    retval = esp_wifi_set_mode(WIFI_MODE_AP);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_ap_start: esp_wifi_set_mode failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, ESP_AP_WIFI_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ESP_AP_WIFI_SSID);
    strncpy((char*)ap_cfg.ap.password, ESP_AP_WIFI_PSD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.max_connection = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    retval = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_ap_start: esp_wifi_set_config failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    retval = esp_wifi_start();
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_ap_start: esp_wifi_start failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    __generate_pairing_code__(ws.pairing_code);
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_ap_start: pairing code %s", (char*)ws.pairing_code);
    lcd_clear();
    lcd_printf("PCode : %s", ws.pairing_code);
    lcd_row_pin(0);

    if(__derive_session_key__(ws.pairing_code, ws.aes_key)){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_ap_start: __derive_session_key__ failed");
        esp_wifi_stop();
        return WIFI_MGR_FATAL;
    }

    ws.curr_mode = WIFI_MGR_MODE_AP;
    ws.is_ap_up = true;
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_ap_start: AP started");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_ap_start: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Stop access-point mode and wipe the pairing code / session key.
 *
 * Closes any AP-mode sockets, stops the driver, clears @c is_ap_up and mode, and
 * zeroes the pairing code and AES key. @c is_ap_up is cleared here explicitly
 * because no router-gone event occurs in AP mode (the ESP is itself the AP).
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL if the driver stop fails.
 */
static wifi_mgr_err_t wifi_mgr_ap_stop(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_ap_stop: entry");
    _wifi_mgr_router_gone_();
    esp_err_t retval;
    retval = esp_wifi_stop();
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_ap_stop: esp_wifi_stop failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    ws.is_ap_up = false;
    ws.curr_mode = WIFI_MGR_MODE_NONE;
    memset(ws.pairing_code, 0, PAIRING_CODE_LEN);
    memset(ws.aes_key, 0, AES_KEY_SIZE);
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_ap_stop: AP stopped");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_ap_stop: exit");
    return WIFI_MGR_OK;
}

/** @brief NVS namespace holding STA credentials. */
#define STA_CREDS_NAMESPACE         "nvs_sta_creds"

/**
 * @brief Initialise the Wi-Fi driver, event handlers, netifs, and NVS namespace.
 *
 * Clears the staged config/pairing/key fields, initialises the Wi-Fi driver with
 * default config, assigns a random locally-administered STA MAC, registers the
 * event handler for both WIFI_EVENT and IP_EVENT, creates the default STA and AP
 * netifs, and opens the STA-credentials NVS namespace. On failure at each stage the
 * resources created so far are unwound.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL on any failure.
 */
static wifi_mgr_err_t wifi_mgr_driver_init(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_driver_init: entry");
    memset(&ws.sta_wcfg, 0, sizeof(ws.sta_wcfg));
    memset(ws.pairing_code, 0, sizeof(ws.pairing_code));
    memset(ws.aes_key, 0, sizeof(ws.aes_key));
    wifi_mgr_err_t retval = WIFI_MGR_OK;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    retval = esp_wifi_init(&init_cfg);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: esp_wifi_init failed, retval %d", retval);
        return WIFI_MGR_FATAL;
    }

    /* Assign a fresh locally-administered MAC on each run (set the LAA bit, clear multicast). */
    uint8_t mac[6];
    esp_fill_random(mac, 6);
    mac[0] = (mac[0] & 0xFE) | 0x02;
    retval = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_driver_init: esp_wifi_set_mac failed, retval %d (continuing)", retval);
    }

    retval = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: register WIFI_EVENT handler failed, retval %d", retval);
        esp_wifi_deinit();
        return WIFI_MGR_FATAL;
    }

    retval = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if(retval != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: register IP_EVENT handler failed, retval %d", retval);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        esp_wifi_deinit();
        return WIFI_MGR_FATAL;
    }

    ws.interface_sta = esp_netif_create_default_wifi_sta();
    if(!(ws.interface_sta)){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: create default STA netif failed");
        return WIFI_MGR_FATAL;
    }

    ws.interface_ap = esp_netif_create_default_wifi_ap();
    if(!(ws.interface_sta)){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: create default AP netif failed");
        return WIFI_MGR_FATAL;
    }

    esp_err_t err = nvs_open(STA_CREDS_NAMESPACE, NVS_READWRITE, &ws.nvs);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_init: nvs_open failed, retval %d", err);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        return WIFI_MGR_FATAL;
    }

    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_driver_init: driver ready");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_driver_init: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Deinitialise the driver: unregister handlers, deinit, reset state, destroy netifs.
 *
 * Unregisters both event handlers and deinitialises the Wi-Fi driver; if any of
 * those fail it jumps to the error path and returns FATAL without further teardown.
 * On success it closes NVS, resets @c ws to its static defaults (re-arming the -1
 * socket sentinels), and destroys both netifs.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL if any unregister/deinit fails.
 */
static wifi_mgr_err_t wifi_mgr_driver_deinit(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_driver_deinit: entry");
    esp_err_t retval;
    retval = esp_event_handler_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID, wifi_event_handler);
    if(retval != ESP_OK){
        goto uncleaned_resources;
    }

    retval = esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    if(retval != ESP_OK){
        goto uncleaned_resources;
    }

    retval = esp_wifi_deinit();
    if(retval != ESP_OK){
        goto uncleaned_resources;
    }
    nvs_close(ws.nvs);
    memset(&ws, 0, sizeof(ws));
    ws.sock_fd_tcp = -1;
    ws.sock_fd_udp = -1;
    ws.pc_sock_fd = -1;

    if(ws.interface_sta){
        esp_netif_destroy(ws.interface_sta);
        ws.interface_sta = NULL;
    }

    if(ws.interface_ap){
        esp_netif_destroy(ws.interface_ap);
        ws.interface_ap = NULL;
    }

    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_driver_deinit: driver deinitialised");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_driver_deinit: exit");
    return WIFI_MGR_OK;

uncleaned_resources:
    PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_driver_deinit: unregister/deinit failed, retval %d, returning FATAL", retval);
    return WIFI_MGR_FATAL;
}

/** @brief Scratch buffer size for NUL-terminating fixed-width credential fields. */
#define DUMMY_ARR_SIZE  80

/**
 * @brief Persist the current STA credentials to NVS.
 *
 * Copies SSID/password through a larger NUL-terminated scratch buffer because
 * @c nvs_set_str needs a C string but the config fields are fixed-width byte arrays
 * that may not be NUL-terminated when exactly full. Stores the bssid_set bit, then
 * either the BSSID blob or erases the stale key, and commits.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL on any NVS failure.
 */
static wifi_mgr_err_t _write_creds_to_nvs_(void){
    PRINT_MSG(LOG_LVL_DEBUG, "_write_creds_to_nvs_: entry");
    char dummy[DUMMY_ARR_SIZE] = {0};

    strncpy(dummy, (char*)ws.sta_wcfg.sta.ssid, sizeof(ws.sta_wcfg.sta.ssid));
    esp_err_t err = nvs_set_str(ws.nvs, SSID_KEY, dummy);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_set_str(ssid) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }

    memset(dummy, 0, sizeof(dummy));
    strncpy(dummy, (char*)ws.sta_wcfg.sta.password, sizeof(ws.sta_wcfg.sta.password));
    err = nvs_set_str(ws.nvs, PSD_KEY, dummy);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_set_str(psd) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }

    err = nvs_set_u8(ws.nvs, BSSID_BIT_KEY, ws.sta_wcfg.sta.bssid_set);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_set_u8(bssid_bit) failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }

    if(ws.sta_wcfg.sta.bssid_set){
        err = nvs_set_blob(ws.nvs, BSSID_KEY, ws.sta_wcfg.sta.bssid, sizeof(ws.sta_wcfg.sta.bssid));
        if(err != ESP_OK){
            PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_set_blob(bssid) failed, retval %d", err);
            return WIFI_MGR_FATAL;
        }
    }else{
        err = nvs_erase_key(ws.nvs, BSSID_KEY);
        if(err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND){
            PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_erase_key(bssid) failed, retval %d", err);
            return WIFI_MGR_FATAL;
        }
    }

    err = nvs_commit(ws.nvs);
    if(err != ESP_OK){
        PRINT_MSG(LOG_LVL_ERROR, "_write_creds_to_nvs_: nvs_commit failed, retval %d", err);
        return WIFI_MGR_FATAL;
    }
    PRINT_MSG(LOG_LVL_INFO, "_write_creds_to_nvs_: credentials persisted");
    PRINT_MSG(LOG_LVL_DEBUG, "_write_creds_to_nvs_: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Create and bind the UDP discovery socket, marking STA up.
 *
 * Reaching this point means STA has an IP, so any pending credential write is
 * flushed to NVS first, then @c is_sta_up is set. Any stale UDP/TCP/PC sockets from
 * a previous session are torn down (the STA IP may have just changed) before a
 * fresh UDP socket is created, set REUSEADDR, bound to the well-known discovery
 * port on all interfaces, and given an 800 ms receive timeout so discovery can be
 * retried cooperatively via the queue.
 *
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_FATAL if the pending NVS write
 *         fails; @c WIFI_MGR_UDP_DEAD on any socket/bind/setsockopt failure.
 */
static wifi_mgr_err_t wifi_mgr_create_udp_socket(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_create_udp_socket: entry");
    if(ws.is_creds_write_pending){
        PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_create_udp_socket: flushing pending credential write");
        wifi_mgr_err_t err = _write_creds_to_nvs_();
        if(err == WIFI_MGR_FATAL){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_create_udp_socket: _write_creds_to_nvs_ failed");
            return WIFI_MGR_FATAL;
        }
        ws.is_creds_write_pending = false;
    }
    ws.is_sta_up = true;

    if(ws.sock_fd_udp >= 0){
        close(ws.sock_fd_udp);
        ws.sock_fd_udp = -1;
    }

    /* STA IP may have just changed; tear down stale TCP/PC sockets before restarting discovery. */
    if(ws.pc_sock_fd >= 0){
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        memset(&ws.pc_addr, 0, sizeof(ws.pc_addr));
    }
    if(ws.sock_fd_tcp >= 0){
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
    }

    struct sockaddr_in bind_addr;
    ws.sock_fd_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(ws.sock_fd_udp < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_create_udp_socket: socket() failed, errno %d", errno);
        return WIFI_MGR_UDP_DEAD;
    }

    /* REUSEADDR must be set before bind. */
    int one = 1;
    if(setsockopt(ws.sock_fd_udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_create_udp_socket: setsockopt(REUSEADDR) failed, errno %d", errno);
        close(ws.sock_fd_udp);
        ws.sock_fd_udp = -1;
        return WIFI_MGR_UDP_DEAD;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(ESP_COMM_PORT_UDP);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(ws.sock_fd_udp, (struct sockaddr*)&bind_addr, sizeof(struct sockaddr_in)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_create_udp_socket: bind() failed, errno %d", errno);
        close(ws.sock_fd_udp);
        ws.sock_fd_udp = -1;
        return WIFI_MGR_UDP_DEAD;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 800000};   /* 800 ms recv timeout. */
    if(setsockopt(ws.sock_fd_udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_create_udp_socket: setsockopt(RCVTIMEO) failed, errno %d", errno);
        close(ws.sock_fd_udp);
        ws.sock_fd_udp = -1;
        return WIFI_MGR_UDP_DEAD;
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_create_udp_socket: UDP discovery socket ready");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_create_udp_socket: exit");
    return WIFI_MGR_OK;
}

/**
 * @name Discovery / TCP-server tuning
 * @{
 */
#define ESP_TCP_SERVER_PORT_STA     6000   /**< TCP server port in STA mode. */
#define DISCOV_RECV_COUNTER_MAX     50     /**< Max consecutive recv timeouts before RETRY. */
#define STALE_PKT_COUNTER_MAX       50     /**< Max non-discovery packets tolerated before RETRY. */
#define DISCOV_ACK_COUNT            6      /**< Number of discovery ACKs sent back to the PC. */
/** @} */

/**
 * @brief Listen for a PC discovery broadcast and answer with the TCP server port.
 *
 * Loops on the bound UDP socket validating each datagram (checksum, magic, msg id,
 * name). A run of non-discovery packets or receive timeouts returns
 * @c WIFI_MGR_RETRY without closing the socket (RETRY invariant — the socket is
 * healthy, only no valid request arrived). On a valid discovery request it replies
 * @c DISCOV_ACK_COUNT times with a DISCOVERY_ACK carrying the STA TCP port, then
 * closes the UDP socket and reports the PC found.
 *
 * @return @c WIFI_MGR_FOUND_PC on success; @c WIFI_MGR_RETRY on timeout/stale-packet
 *         exhaustion; @c WIFI_MGR_UDP_DEAD on a hard socket error;
 *         @c WIFI_MGR_BAD_STATE if the link is already down.
 */
static wifi_mgr_err_t wifi_mgr_discover_pc(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_discover_pc: entry");
    if(!ws.is_sta_up){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_discover_pc: link down (is_sta_up false), returning BAD_STATE");
        return WIFI_MGR_BAD_STATE;
    }

    struct sockaddr_in pc_addr;
    socklen_t addr_len = sizeof(pc_addr);
    uint8_t stale_pkt_counter = STALE_PKT_COUNTER_MAX;
    proto_packet_t rx_buf;
    uint8_t discov_recv_counter = 0;
    while(1){
        if(stale_pkt_counter == 0){
            /* Socket is healthy (received many packets, none a valid discovery req).
               RETRY invariant: leave the socket open, dispatcher re-posts DISCOVER_PC. */
            PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_discover_pc: too many stale packets, returning RETRY");
            return WIFI_MGR_RETRY;
        }
        ssize_t n = recvfrom(ws.sock_fd_udp, &rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&pc_addr, &addr_len);
        if(n < 0){
            int err = errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                if(discov_recv_counter != DISCOV_RECV_COUNTER_MAX){
                    discov_recv_counter++;
                    continue;
                }
                PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_discover_pc: recv timeout budget exhausted, returning RETRY");
                return WIFI_MGR_RETRY;
            }
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_discover_pc: recvfrom failed, errno %d, returning UDP_DEAD", err);
            close(ws.sock_fd_udp);
            ws.sock_fd_udp = -1;
            return WIFI_MGR_UDP_DEAD;
        }
        discov_recv_counter = 0;
        if(!proto_verify_chksum(&rx_buf)){
            stale_pkt_counter--;
            continue;
        }
        if(rx_buf.header.msg_id != MSG_DISCOVERY_REQ || strncmp(rx_buf.payload.discovery_req.name, NAME, NAME_SIZE)){
            stale_pkt_counter--;
            continue;
        }

        memset(&rx_buf, 0, sizeof(rx_buf));
        rx_buf.header.magic  = MAGIC_BYTES;
        rx_buf.header.msg_id = MSG_DISCOVERY_ACK;
        rx_buf.header.length = sizeof(discovery_ack_t);

        rx_buf.payload.discovery_ack.tcp_port = ESP_TCP_SERVER_PORT_STA;
        rx_buf.header.chksum = proto_generate_chksum(&rx_buf);

        for(uint8_t i = 0; i < DISCOV_ACK_COUNT; i++){
            if(sendto(ws.sock_fd_udp, &rx_buf, sizeof(proto_hdr_t) + sizeof(discovery_ack_t), 0, (struct sockaddr*)&pc_addr, addr_len) < 0){
                PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_discover_pc: sendto(ACK) failed, errno %d, returning UDP_DEAD", errno);
                close(ws.sock_fd_udp);
                ws.sock_fd_udp = -1;
                return WIFI_MGR_UDP_DEAD;
            }
        }
        close(ws.sock_fd_udp);
        break;
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_discover_pc: PC discovered, ACK sent");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_discover_pc: exit");
    return WIFI_MGR_FOUND_PC;
}

/** @brief TCP listen backlog (one provisioning/command client at a time). */
#define TCP_LISTEN_QUEUE_LEN        1

/**
 * @brief Bring up the TCP listener for the given mode.
 *
 * Closes any stale listener, validates the mode is actually up
 * (@c is_sta_up / @c is_ap_up), then creates a socket, sets REUSEADDR before bind,
 * binds to the mode-specific port (STA vs AP), listens, and sets a 200 ms accept
 * timeout so the accept loop can be driven cooperatively.
 *
 * @param mode The mode whose "up" flag gates the bind and selects the port.
 * @return @c WIFI_MGR_OK on success; @c WIFI_MGR_BAD_STATE if the mode isn't up;
 *         @c WIFI_MGR_LISTENER_DEAD on any socket/bind/listen/setsockopt failure.
 */
static wifi_mgr_err_t wifi_mgr_start_tcp_server(wifi_mgr_mode_t mode){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_start_tcp_server: entry (mode=%d)", (int)mode);

    if(ws.sock_fd_tcp >= 0){
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
    }

    if(mode == WIFI_MGR_MODE_STA){
        if(!ws.is_sta_up){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: STA not up, returning BAD_STATE");
            return WIFI_MGR_BAD_STATE;
        }
    }

    if(mode == WIFI_MGR_MODE_AP){
        if(!ws.is_ap_up){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: AP not up, returning BAD_STATE");
            return WIFI_MGR_BAD_STATE;
        }
    }

    struct sockaddr_in bind_addr;

    ws.sock_fd_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(ws.sock_fd_tcp < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: socket() failed, errno %d", errno);
        return WIFI_MGR_LISTENER_DEAD;
    }

    /* REUSEADDR before bind. */
    int one = 1;
    if(setsockopt(ws.sock_fd_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: setsockopt(REUSEADDR) failed, errno %d", errno);
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
        return WIFI_MGR_LISTENER_DEAD;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));

    bind_addr.sin_family = AF_INET;
    if(ws.is_sta_up){
        bind_addr.sin_port = htons(ESP_TCP_SERVER_PORT_STA);
    }else{
        bind_addr.sin_port = htons(ESP_TCP_SERVER_PORT_AP);
    }
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(ws.sock_fd_tcp, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: bind() failed, errno %d", errno);
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
        return WIFI_MGR_LISTENER_DEAD;
    }

    if(listen(ws.sock_fd_tcp, TCP_LISTEN_QUEUE_LEN) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: listen() failed, errno %d", errno);
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
        return WIFI_MGR_LISTENER_DEAD;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };   /* 200 ms accept timeout. */
    if(setsockopt(ws.sock_fd_tcp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_start_tcp_server: setsockopt(RCVTIMEO) failed, errno %d", errno);
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
        return WIFI_MGR_LISTENER_DEAD;
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_start_tcp_server: listening");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_start_tcp_server: exit");
    return WIFI_MGR_OK;
}

/**
 * @brief Enable aggressive TCP keepalive on @p fd (~4 s dead-peer detection).
 *
 * Probes after 1 s idle, every 1 s, giving up after 3 unanswered probes
 * (~1 + 1*3 = ~4 s). This is how a silently-dropped PC connection is detected
 * quickly instead of hanging for the default minutes.
 *
 * @param fd Connected socket to configure.
 */
static void __set_keepalive__(int fd){
    PRINT_MSG(LOG_LVL_DEBUG, "__set_keepalive__: entry (fd=%d)", fd);
    int yes   = 1;
    int idle  = 1;    /* start probing after 1 s idle. */
    int intvl  = 1;   /* probe every 1 s. */
    int cnt   = 3;    /* 3 unanswered probes -> dead (~4 s). */

    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes,   sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
    PRINT_MSG(LOG_LVL_DEBUG, "__set_keepalive__: exit");
    return;
}

/** @brief Max consecutive accept timeouts before returning RETRY. */
#define ACCEPT_LOOP_COUNTER_MAX    10

/**
 * @brief Accept a PC connection on the TCP listener.
 *
 * Validates the mode is up and a listener exists, then loops on @c accept. Repeated
 * accept timeouts return @c WIFI_MGR_RETRY (listener stays open). On success it
 * enables keepalive and applies a receive timeout that differs by mode (1 s in STA
 * for command/ACK exchange, 200 ms in AP for provisioning).
 *
 * @param mode The mode whose "up" flag gates the accept.
 * @return @c WIFI_MGR_GOT_CONN on success; @c WIFI_MGR_RETRY on accept-timeout
 *         exhaustion; @c WIFI_MGR_BAD_STATE if the mode isn't up;
 *         @c WIFI_MGR_LISTENER_DEAD if the listener is missing/failed;
 *         @c WIFI_MGR_PEER_GONE if setting the receive timeout fails.
 */
static wifi_mgr_err_t wifi_mgr_accept_pc_conn(wifi_mgr_mode_t mode){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_accept_pc_conn: entry (mode=%d)", (int)mode);
    if(mode == WIFI_MGR_MODE_STA){
        if(!ws.is_sta_up){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: STA not up, returning BAD_STATE");
            return WIFI_MGR_BAD_STATE;
        }
    }

    if(mode == WIFI_MGR_MODE_AP){
        if(!ws.is_ap_up){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: AP not up, returning BAD_STATE");
            return WIFI_MGR_BAD_STATE;
        }
    }

    if(ws.sock_fd_tcp < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: no listener, returning LISTENER_DEAD");
        return WIFI_MGR_LISTENER_DEAD;
    }
    socklen_t addr_len = sizeof(struct sockaddr_in);
    uint8_t accept_counter = 0;

    while(1){
        ws.pc_sock_fd = accept(ws.sock_fd_tcp, (struct sockaddr*)&(ws.pc_addr), &addr_len);
        if(ws.pc_sock_fd < 0){
            int err = errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                if(accept_counter != ACCEPT_LOOP_COUNTER_MAX){
                    accept_counter++;
                    continue;
                }
                PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_accept_pc_conn: accept timeout budget exhausted, returning RETRY");
                return WIFI_MGR_RETRY;
            }
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: accept failed, errno %d, returning LISTENER_DEAD", err);
            close(ws.sock_fd_tcp);
            ws.sock_fd_tcp = -1;
            return WIFI_MGR_LISTENER_DEAD;
        }
        break;
    }
    __set_keepalive__(ws.pc_sock_fd);

    if(ws.is_sta_up){
        /* STA: 1 s recv timeout for the command/ACK exchange. */
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        if(setsockopt(ws.pc_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: setsockopt(RCVTIMEO,STA) failed, errno %d, returning PEER_GONE", errno);
            close(ws.pc_sock_fd);
            ws.pc_sock_fd = -1;
            return WIFI_MGR_PEER_GONE;
        }
    }else{
        /* AP: 200 ms recv timeout for provisioning. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        if(setsockopt(ws.pc_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
            PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_accept_pc_conn: setsockopt(RCVTIMEO,AP) failed, errno %d, returning PEER_GONE", errno);
            close(ws.pc_sock_fd);
            ws.pc_sock_fd = -1;
            return WIFI_MGR_PEER_GONE;
        }
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_accept_pc_conn: PC connected");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_accept_pc_conn: exit");
    return WIFI_MGR_GOT_CONN;
}

/** @brief Max consecutive recv timeouts in __recv_exact__ before giving up. */
#define CRED_RECV_COUNTER_MAX       50

/**
 * @brief Receive exactly @p len bytes into @p buf, tolerating partial reads/timeouts.
 *
 * Loops until the full length is read. A peer close (recv==0) or a bounded run of
 * receive timeouts is a failure; EINTR and single timeouts just retry.
 *
 * @param fd  Connected socket.
 * @param buf Destination buffer.
 * @param len Exact number of bytes required.
 * @return @c ESP_OK once @p len bytes are read; @c ESP_FAIL on peer close, real
 *         error, or timeout-budget exhaustion.
 */
static esp_err_t __recv_exact__(int fd, void* buf, size_t len){
    PRINT_MSG(LOG_LVL_DEBUG, "__recv_exact__: entry (len=%u)", (unsigned)len);
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
            PRINT_MSG(LOG_LVL_WARN, "__recv_exact__: peer closed, returning ESP_FAIL");
            return ESP_FAIL;
        }
        if(errno == EINTR){
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            if(++cred_recv_counter >= CRED_RECV_COUNTER_MAX){
                PRINT_MSG(LOG_LVL_WARN, "__recv_exact__: recv timeout budget exhausted, returning ESP_FAIL");
                return ESP_FAIL;
            }
            continue;
        }

        PRINT_MSG(LOG_LVL_ERROR, "__recv_exact__: recv failed, errno %d, returning ESP_FAIL", errno);
        return ESP_FAIL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "__recv_exact__: exit (got %u bytes)", (unsigned)got);
    return ESP_OK;
}

/**
 * @brief AES-256-GCM authenticated-decrypt of a provisioning packet into @p creds.
 *
 * Sets the session key and runs @c mbedtls_gcm_auth_decrypt over the packet's
 * ciphertext with its nonce and tag. A non-zero result means a wrong pairing code,
 * corrupted ciphertext, or tag mismatch — all indistinguishable and all reported as
 * the same crypto failure.
 *
 * @param pkt     Provisioning packet (nonce/tag/ciphertext).
 * @param aes_key The derived session key.
 * @param creds   Output plaintext credentials.
 * @return 0 on success; -1 on NULL argument; -3 on set-key failure; -4 on
 *         authentication/decryption failure.
 */
static int __decrypt_prov_packet__(const proto_packet_t *pkt, const uint8_t aes_key[AES_KEY_SIZE], _sta_credentials_t *creds){
    PRINT_MSG(LOG_LVL_DEBUG, "__decrypt_prov_packet__: entry");
    int ret;

    mbedtls_gcm_context gcm;

    if(!pkt || !aes_key || !creds){
        PRINT_MSG(LOG_LVL_ERROR, "__decrypt_prov_packet__: NULL argument, returning -1");
        return -1;
    }

    mbedtls_gcm_init(&gcm);

    ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, aes_key, 256);

    if(ret != 0){
        mbedtls_gcm_free(&gcm);
        PRINT_MSG(LOG_LVL_ERROR, "__decrypt_prov_packet__: mbedtls_gcm_setkey failed, ret %d, returning -3", ret);
        return -3;
    }

    ret = mbedtls_gcm_auth_decrypt(&gcm, sizeof(pkt->payload.prov_creds.ciphertext), pkt->payload.prov_creds.nonce, NONCE_SIZE, NULL, 0,
                                   pkt->payload.prov_creds.tag, TAG_SIZE, pkt->payload.prov_creds.ciphertext, (uint8_t *)creds);

    mbedtls_gcm_free(&gcm);

    if(ret != 0){
        /* Wrong pairing code, corrupted ciphertext, or tag mismatch — indistinguishable. */
        PRINT_MSG(LOG_LVL_ERROR, "__decrypt_prov_packet__: auth-decrypt failed, ret %d, returning -4", ret);
        return -4;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "__decrypt_prov_packet__: exit");
    return 0;
}

/** @brief Auth mode applied to provisioned STA credentials. */
#define DEFAULT_STA_AUTHMODE        WIFI_AUTH_WPA2_PSK

/**
 * @brief Receive, validate, decrypt, and stage provisioning credentials; ACK the PC.
 *
 * Requires AP mode with an active PC connection. Receives the fixed-size
 * provisioning packet, verifies checksum/magic/msg-id, AES-GCM decrypts it, and
 * range-checks the SSID/password lengths. On success it stages the new STA config
 * (@c cfg_ptr) and marks a pending NVS write, then sends a status ACK to the PC and,
 * for the accepted case, gracefully half-closes (SHUT_WR) and drains until the PC
 * closes so the ACK is confirmed delivered before the sockets are torn down.
 *
 * Failure classification mirrors the taxonomy: crypto/format errors are treated as
 * @c WIFI_MGR_PEER_GONE (restart provisioning with the right code/format), invalid
 * or corrupt packets are @c WIFI_MGR_CREDS_REJECTED (stay AP, re-accept), and a send
 * failure while ACKing is @c WIFI_MGR_PEER_GONE with any staged creds wiped.
 *
 * @return @c WIFI_MGR_CREDS_ACCEPTED on success; @c WIFI_MGR_PEER_GONE on a dead PC
 *         socket or crypto/format rejection; @c WIFI_MGR_CREDS_REJECTED on an
 *         invalid/corrupt packet; @c WIFI_MGR_BAD_STATE if not AP / no connection.
 */
static wifi_mgr_err_t wifi_mgr_get_router_creds(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_get_router_creds: entry");
    memset(&ws.sta_wcfg, 0, sizeof(wifi_config_t));
    if(!ws.is_ap_up){
        if(ws.sock_fd_tcp >= 0){
            close(ws.sock_fd_tcp);
            ws.sock_fd_tcp = -1;
        }
        if(ws.pc_sock_fd >= 0){
            close(ws.pc_sock_fd);
            ws.pc_sock_fd = -1;
        }
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_get_router_creds: AP not up, returning BAD_STATE");
        return WIFI_MGR_BAD_STATE;
    }

    if(ws.pc_sock_fd < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_get_router_creds: no PC connection, returning BAD_STATE");
        return WIFI_MGR_BAD_STATE;
    }

    proto_packet_t rx_buf;
    if(__recv_exact__(ws.pc_sock_fd, &rx_buf, sizeof(proto_hdr_t)+sizeof(prov_creds_t)) != ESP_OK){
        /* Size is known up front (fixed provisioning packet); no header-first sizing needed. */
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_get_router_creds: __recv_exact__ failed, returning PEER_GONE");
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        return WIFI_MGR_PEER_GONE;
    }

    bool quit_processing = false;

    int status = 0;

    if(!proto_verify_chksum(&rx_buf)){
        quit_processing = true;
        status = PROV_STATUS_CORRUPT_PKT;
    }

    if(!quit_processing && (rx_buf.header.magic != MAGIC_BYTES || rx_buf.header.msg_id != MSG_PROV_CREDS)){
        quit_processing = true;
        status = PROV_STATUS_INVALID_PKT;
    }

    _sta_credentials_t sta = {0};
    if(!quit_processing && __decrypt_prov_packet__(&rx_buf, ws.aes_key, &sta)){
        quit_processing = true;
        status = PROV_STATUS_BAD_CRYPTO;
    }

    /* Credentials valid: range-check lengths (reject empty or exactly-full = unterminated), then stage. */
    if(!quit_processing){
        size_t ssid_len = strnlen(sta.ssid, sizeof(sta.ssid));
        size_t psd_len = strnlen(sta.password, sizeof(sta.password));

        if(ssid_len == 0 || ssid_len == sizeof(sta.ssid) || psd_len == 0 || psd_len == sizeof(sta.password)){
            quit_processing = true;
            status = PROV_STATUS_BAD_FORMAT;
        }else{
            memcpy(ws.sta_wcfg.sta.ssid, sta.ssid, ssid_len);
            memcpy(ws.sta_wcfg.sta.password, sta.password, psd_len);
            if(sta.bssid_set){
                ws.sta_wcfg.sta.bssid_set = true;
                memcpy(ws.sta_wcfg.sta.bssid, sta.bssid, sizeof(ws.sta_wcfg.sta.bssid));
            }
            ws.sta_wcfg.sta.threshold.authmode = DEFAULT_STA_AUTHMODE;
            ws.cfg_ptr = &(ws.sta_wcfg);
            ws.is_creds_write_pending = true;
            status = PROV_STATUS_ACCEPTED;
        }
    }

    /* Creds captured; ACK the PC, then tear down AP and bring up STA with the new config. */
    proto_packet_t ack = {0};
    ack.header.magic = MAGIC_BYTES;
    ack.header.msg_id = MSG_PROV_ACK;
    ack.header.length = sizeof(prov_ack_t);
    ack.payload.prov_ack.status = status;
    ack.header.chksum = proto_generate_chksum(&ack);

    ssize_t s = send(ws.pc_sock_fd, &ack, sizeof(proto_hdr_t) + sizeof(prov_ack_t), MSG_NOSIGNAL);
    if(s < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_get_router_creds: send(ACK) failed, errno %d, returning PEER_GONE", errno);
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        /* ACK never left: wipe any creds we just staged. */
        if(!quit_processing){
            memset(ws.sta_wcfg.sta.ssid, 0, sizeof(ws.sta_wcfg.sta.ssid));
            memset(ws.sta_wcfg.sta.password, 0, sizeof(ws.sta_wcfg.sta.password));
            if(ws.sta_wcfg.sta.bssid_set){
                ws.sta_wcfg.sta.bssid_set = false;
                memset(ws.sta_wcfg.sta.bssid, 0, sizeof(ws.sta_wcfg.sta.bssid));
            }
            ws.sta_wcfg.sta.threshold.authmode = 0;
            ws.cfg_ptr = NULL;
        }
        return WIFI_MGR_PEER_GONE;
    }

    if(quit_processing){
        if(((status & PROV_STATUS_BAD_CRYPTO) == PROV_STATUS_BAD_CRYPTO) || ((status & PROV_STATUS_BAD_FORMAT) == PROV_STATUS_BAD_FORMAT)){
            /* Bad crypto = wrong pairing code, bad format = malformed creds: caller re-provisions. */
            PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_get_router_creds: crypto/format rejected (status %d), returning PEER_GONE", status);
            close(ws.pc_sock_fd);
            ws.pc_sock_fd = -1;
            return WIFI_MGR_PEER_GONE;
        }
        if(((status & PROV_STATUS_INVALID_PKT) == PROV_STATUS_INVALID_PKT)  || ((status & PROV_STATUS_CORRUPT_PKT) == PROV_STATUS_CORRUPT_PKT)){
            PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_get_router_creds: invalid/corrupt packet (status %d), returning CREDS_REJECTED", status);
            return WIFI_MGR_CREDS_REJECTED;
        }
    }

    /* Half-close and drain so the ACK is confirmed delivered before closing. */
    shutdown(ws.pc_sock_fd, SHUT_WR);

    char tmp[8];
    int drain_tries = 50;
    while(drain_tries-- > 0){
        ssize_t r = recv(ws.pc_sock_fd, tmp, sizeof(tmp), 0);
        if(r == 0){
            break;                         /* PC closed -> ACK confirmed delivered. */
        }
        if(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
            continue;                      /* timeout tick, keep waiting. */
        }
        if(r < 0){
            break;                         /* real error: stop waiting, proceed to close. */
        }
        /* r > 0: PC sent unexpected bytes; discard and keep draining. */
    }

    close(ws.pc_sock_fd);
    ws.pc_sock_fd = -1;
    if(ws.sock_fd_tcp >= 0){
        close(ws.sock_fd_tcp);
        ws.sock_fd_tcp = -1;
    }
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_get_router_creds: credentials accepted");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_get_router_creds: exit");
    return WIFI_MGR_CREDS_ACCEPTED;
}

/** @brief Max resends of an orientation command while waiting for its ACK. */
#define SEND_COUNTER_MAX            4

/**
 * @brief Send an orientation command and wait for its matching ACK.
 *
 * Requires STA up with an active PC connection (otherwise @c WIFI_MGR_NOT_READY so
 * the caller can cache the orientation). Builds an orientation-command packet
 * stamped with the boot epoch and a microsecond sequence number, sends it once, then
 * waits for an ACK, validating checksum, msg-id, and that both seq_us and boot_epoch
 * match this command. Genuine receive timeouts resend the SAME packet up to
 * @c SEND_COUNTER_MAX times; a closed/errored socket is @c WIFI_MGR_PEER_GONE.
 *
 * @param orient The orientation to send.
 * @return @c WIFI_MGR_OK on a validated ACK; @c WIFI_MGR_NOT_READY if STA/connection
 *         isn't ready; @c WIFI_MGR_PEER_GONE if the socket dies or retries are
 *         exhausted.
 */
static wifi_mgr_err_t wifi_mgr_send_command(display_orient_t orient){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_send_command: entry (orient=%d)", (int)orient);

    if(!ws.is_sta_up){
        /* STA not up yet: possible when the command lands before the station comes up. */
        PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_send_command: STA not up, returning NOT_READY");
        return WIFI_MGR_NOT_READY;
    }

    if(ws.pc_sock_fd < 0){
        /* No active connection right now (never created, or router refreshed the fd away). */
        PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_send_command: no PC connection, returning NOT_READY");
        return WIFI_MGR_NOT_READY;
    }
    proto_packet_t pkt = {0};
    pkt.header.magic = MAGIC_BYTES;
    pkt.header.msg_id = MSG_ORIENT_CMD;
    pkt.header.length = sizeof(orient_cmd_t);

    pkt.payload.orient_cmd.seq.boot_epoch = ws._boot_epoch;
    pkt.payload.orient_cmd.orientation = orient;

    pkt.payload.orient_cmd.seq.seq_us = (uint64_t)esp_timer_get_time();   /* us since boot. */
    pkt.header.chksum = proto_generate_chksum(&pkt);

    size_t pkt_len = sizeof(proto_hdr_t) + pkt.header.length;
    uint8_t send_counter = 0;

    /* Send once up front. */
    if(send(ws.pc_sock_fd, &pkt, pkt_len, MSG_NOSIGNAL) < 0){
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_send_command: initial send failed, errno %d, returning PEER_GONE", errno);
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        return WIFI_MGR_PEER_GONE;
    }

    while(1){
        /* Wait for the ACK (pc_sock_fd has the 1 s SO_RCVTIMEO set at accept). */
        proto_packet_t ack;
        ssize_t n = recv(ws.pc_sock_fd, &ack, sizeof(ack), 0);

        if(n > 0){
            /* Validate this is the ACK for THIS command's seq. */
            if(proto_verify_chksum(&ack) &&
               (ack.header.msg_id == MSG_ORIENT_ACK)  &&
               (ack.payload.orient_ack.seq.seq_us == pkt.payload.orient_cmd.seq.seq_us) &&
               (ack.payload.orient_ack.seq.boot_epoch == pkt.payload.orient_cmd.seq.boot_epoch)){
               PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_send_command: ACK validated");
               PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_send_command: exit (OK)");
               return WIFI_MGR_OK;
            }
            /* Some other/stale packet; keep waiting. */
            continue;
        }

        if(n == 0){
            PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_send_command: peer closed, returning PEER_GONE");
            close(ws.pc_sock_fd);
            ws.pc_sock_fd = -1;
            return WIFI_MGR_PEER_GONE;
        }

        /* n < 0 */
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            /* Genuine timeout: resend the SAME packet (same seq_us), bounded retries. */
            if(send_counter >= SEND_COUNTER_MAX){
                PRINT_MSG(LOG_LVL_WARN, "wifi_mgr_send_command: resend budget exhausted, returning PEER_GONE");
                close(ws.pc_sock_fd);
                ws.pc_sock_fd = -1;
                return WIFI_MGR_PEER_GONE;
            }
            send_counter++;
            if(send(ws.pc_sock_fd, &pkt, pkt_len, MSG_NOSIGNAL) < 0){
                PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_send_command: resend failed, errno %d, returning PEER_GONE", errno);
                close(ws.pc_sock_fd);
                ws.pc_sock_fd = -1;
                return WIFI_MGR_PEER_GONE;
            }
            continue;
        }
        if(errno == EINTR){
            continue;
        }
        PRINT_MSG(LOG_LVL_ERROR, "wifi_mgr_send_command: recv failed, errno %d, returning PEER_GONE", errno);
        close(ws.pc_sock_fd);
        ws.pc_sock_fd = -1;
        return WIFI_MGR_PEER_GONE;
    }
}

/**
 * @brief Global state used by the public API and the dispatcher.
 */
struct global_state {
    QueueHandle_t wifi_queue;                    /**< The command/event queue. */
    TaskHandle_t wifi_task_handle;               /**< The dispatcher task handle. */
    TaskHandle_t caller_handle;                  /**< Task notified (die_bit) on exit. */
    display_orient_t (*get_imm_orient)(void);    /**< Callback to read immediate orientation. */
    uint8_t die_bit;                             /**< Notification bit set on caller at exit. */
};

struct global_state gs = {
    .wifi_queue = NULL,
    .wifi_task_handle = NULL,
    .caller_handle = NULL,
    .get_imm_orient = NULL,
    .die_bit = 0
};

/**
 * @brief Orientation cache that survives a manager init->deinit->init cycle.
 */
struct global_orientation_cache {
    bool is_last_orient;             /**< True if a cached orientation is pending. */
    display_orient_t last_orient;    /**< The cached orientation. */
};

struct global_orientation_cache goc = {.is_last_orient = false, .last_orient = ORIENT_INVALID};

/**
 * @name Dispatcher retry caps
 * @{
 */
#define UDP_CREATE_MAX      5   /**< Max UDP-socket rebuild attempts. */
#define TCP_CREATE_MAX      5   /**< Max TCP-server rebuild attempts. */
#define ACCEPT_COUNTER_MAX  2   /**< Max STA re-accept attempts before rediscovery. */
/** @} */

/**
 * @brief Dispatcher-only bookkeeping (retry counters and exit flag).
 */
struct dispatcher_state {
    bool exit_task;                  /**< Set to break the dispatch loop and exit the task. */
    uint8_t udp_create_counter;      /**< UDP rebuild attempts so far. */
    uint8_t tcp_create_counter;      /**< TCP rebuild attempts so far. */
    uint8_t accept_counter;          /**< STA re-accept attempts so far. */
    bool is_last_orient;             /**< True if a cached orientation should be sent on reconnect. */
    display_orient_t last_orient;    /**< The cached orientation. */
};

struct dispatcher_state ds = {
    .exit_task = false,
    .udp_create_counter = 0,
    .tcp_create_counter = 0,
    .accept_counter = 0,
    .is_last_orient = false,
    .last_orient = 0,
};

/**
 * @brief Shared flag: distinguishes an intentional disconnect from a real one.
 *
 * Set by the dispatcher before a self-initiated STA stop so ::wifi_event_handler
 * can ignore the resulting disconnect event instead of treating it as a fault.
 */
struct wifi_event_handler_shared_state {
    atomic_bool self_disconnect;
};

static struct wifi_event_handler_shared_state wehss = {.self_disconnect = false};

/**
 * @brief The dispatcher task: drain the queue and drive the state machine.
 *
 * Posts DRIVER_INIT to itself, then loops: receive a ::wifi_msg_t, run the matching
 * worker, and enqueue the next message(s) based on the ::wifi_mgr_err_t result per
 * the taxonomy in the file header. The @c ds retry counters bound rebuild attempts
 * before escalating (e.g. UDP/TCP rebuild caps, STA re-accept cap). On a FATAL or a
 * completed DEINIT the loop breaks, the caller is notified via the die-bit, and the
 * task self-deletes.
 *
 * @param caller_handle Task to notify (die-bit) when the manager task exits.
 */
void wifi_task(void* caller_handle){
    PRINT_MSG(LOG_LVL_INFO, "wifi_task: started");
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: entry");
    esp_netif_ip_info_t ip_info;
    wifi_msg_t msg;
    msg.id = WIFI_MGR_DRIVER_INIT;
    xQueueSend(gs.wifi_queue, &msg, 0);
    wifi_mgr_err_t retval = WIFI_MGR_OK;
    while(1){
        if(xQueueReceive(gs.wifi_queue, &msg, portMAX_DELAY)){
            switch(msg.id){
                case WIFI_MGR_DRIVER_INIT:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch DRIVER_INIT");
                    retval = wifi_mgr_driver_init();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: DRIVER_INIT retval %d", retval);
                    if(retval == WIFI_MGR_OK){
                        msg.id = WIFI_MGR_STA_START;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else{
                        ds.exit_task = true;
                    }
                    break;

                case WIFI_MGR_DRIVER_DEINIT:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch DRIVER_DEINIT");
                    retval = wifi_mgr_driver_deinit();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: DRIVER_DEINIT retval %d", retval);
                    /* Return value can't change anything: the task must exit now. */
                    ds.exit_task = true;
                    break;

                case WIFI_MGR_STA_START:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch STA_START");
                    retval = wifi_mgr_sta_start();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: STA_START retval %d", retval);
                    if(retval == WIFI_MGR_FATAL){
                        msg.id = WIFI_MGR_DRIVER_DEINIT;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_CREDS_NVS_EMPTY || retval == WIFI_MGR_CREDS_MALFORMED){
                        msg.id = WIFI_MGR_AP_START;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_STA_STOP:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch STA_STOP");
                    bool sd = atomic_load(&wehss.self_disconnect);
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: self_disconnect was %d, setting true", sd);
                    atomic_store(&wehss.self_disconnect, true);
                    retval = wifi_mgr_sta_stop();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: STA_STOP retval %d", retval);
                    if(retval == WIFI_MGR_FATAL){
                        msg.id = WIFI_MGR_DRIVER_DEINIT;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_AP_START:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch AP_START");
                    retval = wifi_mgr_ap_start();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: AP_START retval %d", retval);
                    if(retval == WIFI_MGR_OK){
                        msg.u.wifi_mode = WIFI_MGR_MODE_AP;
                        msg.id = WIFI_MGR_START_TCP_SERVER;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_FATAL){
                        msg.id = WIFI_MGR_DRIVER_DEINIT;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }

                    esp_netif_get_ip_info(ws.interface_ap, &ip_info);
                    lcd_printf("AP: "IPSTR, IP2STR(&ip_info.ip));

                    break;

                case WIFI_MGR_AP_STOP:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch AP_STOP");
                    retval = wifi_mgr_ap_stop();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: AP_STOP retval %d", retval);
                    if(retval == WIFI_MGR_FATAL){
                        msg.id = WIFI_MGR_DRIVER_DEINIT;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_CREATE_UDP_SOCKET:
                    /* Now in STA mode with an IP: show it on the LCD, then create the UDP socket. */
                    esp_netif_get_ip_info(ws.interface_sta, &ip_info);
                    lcd_clear();
                    lcd_printf("STATION IP:");
                    lcd_row_pin(0);
                    lcd_printf(IPSTR, IP2STR(&ip_info.ip));
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch CREATE_UDP_SOCKET");
                    retval = wifi_mgr_create_udp_socket();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: CREATE_UDP_SOCKET retval %d", retval);
                    if(retval == WIFI_MGR_OK){
                        ds.udp_create_counter = 0;
                        msg.id = WIFI_MGR_DISCOVER_PC;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_UDP_DEAD){
                        if(ds.udp_create_counter == UDP_CREATE_MAX){
                            msg.id = WIFI_MGR_STA_STOP;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            msg.id = WIFI_MGR_DRIVER_DEINIT;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            ds.udp_create_counter = 0;
                        }else{
                            msg.id = WIFI_MGR_CREATE_UDP_SOCKET;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            ds.udp_create_counter++;
                        }
                    }else if(retval == WIFI_MGR_FATAL){
                        msg.id = WIFI_MGR_STA_STOP;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                        msg.id = WIFI_MGR_DRIVER_DEINIT;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_DISCOVER_PC:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch DISCOVER_PC");
                    retval = wifi_mgr_discover_pc();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: DISCOVER_PC retval %d", retval);
                    if(retval == WIFI_MGR_BAD_STATE){
                        msg.id = WIFI_MGR_STA_STOP;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                        msg.id = WIFI_MGR_STA_START;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_RETRY){
                        msg.id = WIFI_MGR_DISCOVER_PC;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_UDP_DEAD){
                        msg.id = WIFI_MGR_CREATE_UDP_SOCKET;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_FOUND_PC){
                        msg.u.wifi_mode = WIFI_MGR_MODE_STA;
                        msg.id = WIFI_MGR_START_TCP_SERVER;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_START_TCP_SERVER:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch START_TCP_SERVER");
                    retval = wifi_mgr_start_tcp_server(msg.u.wifi_mode);
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: START_TCP_SERVER retval %d", retval);
                    if(retval == WIFI_MGR_BAD_STATE ){
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_STA){
                            msg.id = WIFI_MGR_STA_STOP;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            msg.id = WIFI_MGR_STA_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }else{
                            msg.id = WIFI_MGR_AP_STOP;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            msg.id = WIFI_MGR_AP_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }
                    }else
                    if(retval == WIFI_MGR_LISTENER_DEAD){
                        if(ds.tcp_create_counter == TCP_CREATE_MAX){
                            if(msg.u.wifi_mode == WIFI_MGR_MODE_STA){
                                msg.id = WIFI_MGR_STA_STOP;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                                msg.id = WIFI_MGR_STA_START;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                            }else{
                                msg.id = WIFI_MGR_AP_STOP;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                                msg.id = WIFI_MGR_AP_START;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                            }
                        }else{
                            msg.id = WIFI_MGR_START_TCP_SERVER;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            ds.tcp_create_counter++;
                        }
                   }else
                   if(retval == WIFI_MGR_OK){
                       ds.tcp_create_counter = 0;
                       msg.id = WIFI_MGR_ACCEPT_PC_CONN;
                       xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                break;

                case WIFI_MGR_ACCEPT_PC_CONN:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch ACCEPT_PC_CONN");
                    retval = wifi_mgr_accept_pc_conn(msg.u.wifi_mode);
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: ACCEPT_PC_CONN retval %d", retval);
                    if(retval == WIFI_MGR_BAD_STATE){
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_STA){
                            msg.id = WIFI_MGR_STA_STOP;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            msg.id = WIFI_MGR_STA_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }else
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_AP){
                            msg.id = WIFI_MGR_AP_STOP;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            msg.id = WIFI_MGR_AP_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }
                    }else
                    if(retval == WIFI_MGR_LISTENER_DEAD){
                        msg.id = WIFI_MGR_START_TCP_SERVER;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_RETRY || retval == WIFI_MGR_PEER_GONE){
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_AP){
                            /* AP: stay up and keep accepting. Rebuilding would drop the client
                               mid-DHCP and rotate the pairing code. */
                            msg.id = WIFI_MGR_ACCEPT_PC_CONN;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }else{
                            /* STA: cap re-accepts, then fall back to rediscovery. */
                            if(ds.accept_counter == ACCEPT_COUNTER_MAX){
                                ds.accept_counter = 0;
                                msg.id = WIFI_MGR_CREATE_UDP_SOCKET;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                            }else{
                                ds.accept_counter++;
                                msg.id = WIFI_MGR_ACCEPT_PC_CONN;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                            }
                        }
                    }else
                    if(retval == WIFI_MGR_GOT_CONN){
                        ds.accept_counter = 0;
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_AP){
                            msg.id = WIFI_MGR_GET_ROUTER_CREDS;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }else
                        if(msg.u.wifi_mode == WIFI_MGR_MODE_STA){
                            if(ds.is_last_orient){
                                ds.is_last_orient = false;
                                msg.u.orient = ds.last_orient;
                                ds.last_orient = 0;
                                msg.id = WIFI_MGR_SEND_COMMAND;
                                xQueueSend(gs.wifi_queue, &msg, 0);
                            }else{
                                /* Freshly (re)connected: pull immediate orientation (also covers
                                   boot-time reconnect where no last orientation exists). */
                                msg.u.orient = gs.get_imm_orient();
                                if(msg.u.orient == ORIENT_INVALID){
                                    /* Sensor faulty/uncommunicative: fatal. */
                                    msg.id = WIFI_MGR_STA_STOP;
                                    xQueueSend(gs.wifi_queue, &msg, 0);
                                    msg.id = WIFI_MGR_DRIVER_DEINIT;
                                    xQueueSend(gs.wifi_queue, &msg, 0);
                                }else{
                                    msg.id = WIFI_MGR_SEND_COMMAND;
                                    xQueueSend(gs.wifi_queue, &msg, 0);
                                }
                            }
                        }
                    }
                    break;

                case WIFI_MGR_GET_ROUTER_CREDS:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch GET_ROUTER_CREDS");
                    retval = wifi_mgr_get_router_creds();
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: GET_ROUTER_CREDS retval %d", retval);
                    if(retval == WIFI_MGR_BAD_STATE){
                        msg.id = WIFI_MGR_AP_STOP;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                        msg.id = WIFI_MGR_AP_START;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_PEER_GONE){
                        /* get_router_creds is part of AP state: re-accept for AP. */
                        msg.u.wifi_mode = WIFI_MGR_MODE_AP;
                        msg.id = WIFI_MGR_ACCEPT_PC_CONN;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_CREDS_REJECTED){
                        /* Indefinite retries: wait for a clean provisioning packet. */
                        msg.id = WIFI_MGR_GET_ROUTER_CREDS;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }else
                    if(retval == WIFI_MGR_CREDS_ACCEPTED){
                        msg.id = WIFI_MGR_AP_STOP;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                        msg.id = WIFI_MGR_STA_START;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                case WIFI_MGR_SEND_COMMAND:
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: dispatch SEND_COMMAND");
                    /*
                     * No "is the driver up yet" guard here, deliberately.
                     *
                     * There was one. It warned and then sent anyway -- no break, no early return --
                     * so it was a log line dressed as a check, and the flag behind it was written in
                     * two places and read nowhere else. Both are gone.
                     *
                     * Nothing is lost, because the not-yet-connected case is already handled one
                     * line below: wifi_mgr_send_command returns WIFI_MGR_NOT_READY, the orientation
                     * is cached in goc, and it is replayed the moment the PC connects. A guard here
                     * would only cache it in a different place for the same result.
                     */
                    retval = wifi_mgr_send_command(msg.u.orient);
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: SEND_COMMAND retval %d", retval);
                    if(retval != WIFI_MGR_OK){
                        goc.is_last_orient = true;
                        goc.last_orient = msg.u.orient;
                    }

                    if(retval == WIFI_MGR_PEER_GONE){
                        msg.u.wifi_mode = WIFI_MGR_MODE_STA;
                        msg.id = WIFI_MGR_ACCEPT_PC_CONN;
                        xQueueSend(gs.wifi_queue, &msg, 0);
                    }
                    break;

                default:
                    PRINT_MSG(LOG_LVL_WARN, "wifi_task: unknown event id %d", (int)msg.id);
                    break;
            }
        }
        if(ds.exit_task){
            ds.exit_task = false;
            break;
        }
    }

    if(caller_handle){
        PRINT_MSG(LOG_LVL_INFO, "wifi_task: notifying caller of task exit");
        xTaskNotify((TaskHandle_t)caller_handle, 1 << gs.die_bit, eSetBits);
    }
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_task: exit (deleted)");
    vTaskDelete(NULL);
}

/**
 * @name STA reconnect retry caps
 * @{
 */
#define NO_AP_CONN_RETRIES_MAX              10   /**< Retries when the AP was never seen. */
#define ROUTER_GONE_CONN_RETRIES_MAX        10   /**< Retries when a known router vanished. */
/** @} */

/**
 * @brief Event-handler state selecting which retry budget a NO_AP_FOUND uses.
 *
 * @c router_gone classifies the two situations that both surface as NO_AP_FOUND: a
 * router that was connected then vanished (use @c router_gone_conn_retries) versus
 * an AP that was never seen after boot (use @c no_ap_conn_retries). It only selects
 * a retry budget; it does not drive control flow beyond that.
 */
struct wifi_event_handler_state {
    uint16_t no_ap_conn_retries;         /**< Retries used when the AP was never seen. */
    uint16_t router_gone_conn_retries;   /**< Retries used when a known router vanished. */
    bool router_gone;                    /**< True if a connected router dropped mid-operation. */
};

struct wifi_event_handler_state wehs = {.no_ap_conn_retries = 0, .router_gone_conn_retries = 0, .router_gone = false};

/**
 * @brief Translate raw ESP-IDF Wi-Fi/IP events into internal queue messages.
 *
 * The only events acted on are STA connected/disconnected (WIFI_EVENT) and GOT_IP
 * (IP_EVENT). On disconnect it classifies the reason: NO_AP_FOUND retries the
 * connection using the router-gone or no-AP budget (falling back to AP provisioning
 * when exhausted); a flagged self-disconnect is ignored once and cleared; auth
 * failure switches to AP provisioning; anything else is treated as a router-gone
 * drop that stops and restarts STA. GOT_IP clears the retry counters and kicks off
 * UDP-socket creation. This handler only enqueues messages; it never touches
 * sockets directly.
 *
 * @param arg Unused user argument.
 * @param event_base WIFI_EVENT or IP_EVENT.
 * @param event_id   The specific event within the base.
 * @param event_data Event payload (e.g. disconnect reason).
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
    wifi_msg_t msg = {0};
    if(event_base == WIFI_EVENT){
        PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: WIFI_EVENT id %d", (int)event_id);
        switch(event_id){
            case WIFI_EVENT_STA_CONNECTED:
                /* Connected to the AP; DHCP still pending. */
                PRINT_MSG(LOG_LVL_INFO, "wifi_event_handler: STA connected");
                wehs.router_gone = false;
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                PRINT_MSG(LOG_LVL_INFO, "wifi_event_handler: STA disconnected");
                wifi_event_sta_disconnected_t* disconn = event_data;
                PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: disconnect reason %d", disconn->reason);

                bool sd = atomic_load(&wehss.self_disconnect);
                PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: self_disconnect %d", sd);
                if(disconn->reason == WIFI_REASON_NO_AP_FOUND){
                    PRINT_MSG(LOG_LVL_WARN, "wifi_event_handler: no AP found, retrying");
                    msg.id = WIFI_MGR_STA_STOP;
                    xQueueSend(gs.wifi_queue, &msg, 0);

                    if(wehs.router_gone){
                        if(wehs.router_gone_conn_retries < ROUTER_GONE_CONN_RETRIES_MAX){
                            PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: router-gone retry %d", wehs.router_gone_conn_retries);
                            msg.id = WIFI_MGR_STA_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            (wehs.router_gone_conn_retries)++;
                        }else{
                            PRINT_MSG(LOG_LVL_WARN, "wifi_event_handler: router-gone retries exhausted, switching to AP");
                            wehs.router_gone_conn_retries = 0;
                            wehs.router_gone = false;
                            msg.id = WIFI_MGR_AP_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }
                    }else{
                        if(wehs.no_ap_conn_retries < NO_AP_CONN_RETRIES_MAX){
                            PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: no-AP retry %d", wehs.no_ap_conn_retries);
                            msg.id = WIFI_MGR_STA_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                            (wehs.no_ap_conn_retries)++;
                        }else{
                            PRINT_MSG(LOG_LVL_WARN, "wifi_event_handler: no-AP retries exhausted, switching to AP");
                            wehs.no_ap_conn_retries = 0;
                            msg.id = WIFI_MGR_AP_START;
                            xQueueSend(gs.wifi_queue, &msg, 0);
                        }
                    }

                }else
                if(sd){
                    atomic_store(&wehss.self_disconnect, false);
                    PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: self-disconnect, ignoring and clearing flag");
                    break;
                }else
                if((disconn->reason == WIFI_REASON_AUTH_FAIL) || (disconn->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)){
                    PRINT_MSG(LOG_LVL_WARN, "wifi_event_handler: bad credentials, switching to AP provisioning");
                    msg.id = WIFI_MGR_STA_STOP;
                    xQueueSend(gs.wifi_queue, &msg, 0);
                    msg.id = WIFI_MGR_AP_START;
                    xQueueSend(gs.wifi_queue, &msg, 0);
                }else{
                    PRINT_MSG(LOG_LVL_WARN, "wifi_event_handler: router gone, restarting STA");
                    wehs.router_gone = true;
                    msg.id = WIFI_MGR_STA_STOP;
                    xQueueSend(gs.wifi_queue, &msg, 0);
                    msg.id = WIFI_MGR_STA_START;
                    xQueueSend(gs.wifi_queue, &msg, 0);
                }
                break;

            default:
                break;
        }
    }else
    if(event_base == IP_EVENT){
        PRINT_MSG(LOG_LVL_DEBUG, "wifi_event_handler: IP_EVENT id %d", (int)event_id);
        switch(event_id){
            case IP_EVENT_STA_GOT_IP:
                PRINT_MSG(LOG_LVL_INFO, "wifi_event_handler: got IP, starting discovery");
                wehs.no_ap_conn_retries = 0;
                wehs.router_gone_conn_retries = 0;
                msg.id = WIFI_MGR_CREATE_UDP_SOCKET;
                xQueueSend(gs.wifi_queue, &msg, 0);
                break;

            default:
                break;
        }
    }
    return;
}

/**
 * @name Public API task parameters
 * @{
 */
#define TASK_NAME                   "wifi_task"   /**< FreeRTOS name of the manager task. */
#define WIFI_QUEUE_LEN              10            /**< Depth of the command/event queue. */
#define WIFI_TASK_STACK_SIZE        4096          /**< Manager task stack size (bytes). */
#define WIFI_TASK_PRIOR             5             /**< Manager task priority. */
/** @} */

/**
 * @brief Initialise the Wi-Fi manager: load boot epoch, create the queue and task.
 *
 * Records the caller handle, orientation callback, and die-bit, bumps the persistent
 * boot epoch, creates the message queue, and spawns ::wifi_task (which drives its own
 * driver init). Does not block on connection.
 *
 * @note The parameter contract lives with the DECLARATION, in wifi_mgr.h -- not here. That is where
 *       a caller looks, and duplicating it would mean one contract with two copies free to drift
 *       apart.
 */
void wifi_mgr_init(TaskHandle_t caller_handle, display_orient_t (*get_immediate_orient)(void), uint8_t die_bit){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_init: entry");
    gs.die_bit = die_bit;
    gs.caller_handle = caller_handle;
    gs.get_imm_orient = get_immediate_orient;
    ESP_ERROR_CHECK(_get_boot_epoch());   /* once per boot */
    gs.wifi_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(wifi_msg_t));
    BaseType_t ret = xTaskCreate(wifi_task, TASK_NAME, WIFI_TASK_STACK_SIZE, (void*)&gs.caller_handle, WIFI_TASK_PRIOR, &gs.wifi_task_handle);
    PRINT_MSG(LOG_LVL_INFO, "wifi_mgr_init: xTaskCreate returned %d", ret);
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_init: exit");
    return;
}

/**
 * @brief Deinitialise the manager bookkeeping and delete the queue.
 *
 * Clears the caller/callback/die-bit and task handle and deletes the message queue.
 *
 * @note This does not signal a running ::wifi_task to stop; the task exits only via
 *       its own DRIVER_DEINIT path. Call this only after the task has already exited,
 *       or the task may run on against a deleted queue.
 */
void wifi_mgr_deinit(void){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_deinit: entry");
    gs.die_bit = 0;
    gs.caller_handle = NULL;
    gs.get_imm_orient = NULL;
    gs.wifi_task_handle = NULL;
    if(gs.wifi_queue){
        vQueueDelete(gs.wifi_queue);
        gs.wifi_queue = NULL;
    }
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_deinit: exit");
    return;
}

/**
 * @brief Queue an orientation command for the manager task to send.
 *
 * @note Parameter contract is on the declaration, in wifi_mgr.h.
 */
void wifi_mgr_send_cmd(display_orient_t orient){
    PRINT_MSG(LOG_LVL_DEBUG, "wifi_mgr_send_cmd: queueing orientation %d", (int)orient);
    wifi_msg_t msg = {.id = WIFI_MGR_SEND_COMMAND, .u.orient = orient};
    xQueueSend(gs.wifi_queue, &msg, 0);
    return;
}

