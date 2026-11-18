/* BLE FIDO 实验传输：NimBLE 回调解析报文后通知应用，不在回调里直接操作界面。
 * 停止时先禁止新发送、等待 tick 和 host 退出，再反初始化；失败时资源仍归本模块。
 * 响应分片和 mbuf 所有权遵循 NimBLE 接口约定，不能在提交后重复释放。
 */
#include "auth_ble.h"
#include "fido_protocol.h"
#include "fido_pin.h"
#include "auth_fido_store.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "store/config/ble_store_config.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"

static int auth_mbedtls_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static const char *TAG = "auth_ble";

static const char *BLE_DEV_NAME = "FT_FIDO";

/* FIDO2 authenticator identity. With fmt:"none" the AAGUID is informational;
 * an all-zero AAGUID avoids implying a certified vendor implementation whose
 * metadata (UV/PIN capabilities) we do not match. */
static const uint8_t s_aaguid[16] = { 0 };

/* Keep the MakeCredential response below the ~150-byte / 8-fragment limit used
 * by some mobile CTAP2-over-BLE clients. authData is 145 bytes + credIdLen, so
 * a 4-byte credential ID yields a 149-byte response (8 notifications at MTU 23). */
#define FIDO_CRED_ID_LEN 4
#define CTAP_MAX_MSG_LEN 1024

/* Official FIDO GATT Service UUID: 0xFFFD */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFFFD);

/* Official FIDO Alliance 128-bit Characteristic UUIDs (Little-Endian) */
/* fidoControlPoint: F1D0FFF1-DEAA-ECEE-B42F-C9BA7ED623BB */
static const ble_uuid128_t s_char_fido_cp_uuid = BLE_UUID128_INIT(
    0xBB, 0x23, 0xD6, 0x7E, 0xBA, 0xC9, 0x2F, 0xB4,
    0xEE, 0xEC, 0xAA, 0xDE, 0xF1, 0xFF, 0xD0, 0xF1
);

/* fidoStatus: F1D0FFF2-DEAA-ECEE-B42F-C9BA7ED623BB */
static const ble_uuid128_t s_char_fido_status_uuid = BLE_UUID128_INIT(
    0xBB, 0x23, 0xD6, 0x7E, 0xBA, 0xC9, 0x2F, 0xB4,
    0xEE, 0xEC, 0xAA, 0xDE, 0xF2, 0xFF, 0xD0, 0xF1
);

/* fidoControlPointLength: F1D0FFF3-DEAA-ECEE-B42F-C9BA7ED623BB */
static const ble_uuid128_t s_char_fido_cplen_uuid = BLE_UUID128_INIT(
    0xBB, 0x23, 0xD6, 0x7E, 0xBA, 0xC9, 0x2F, 0xB4,
    0xEE, 0xEC, 0xAA, 0xDE, 0xF3, 0xFF, 0xD0, 0xF1
);

/* fidoServiceRevisionBitfield: F1D0FFF4-DEAA-ECEE-B42F-C9BA7ED623BB */
static const ble_uuid128_t s_char_fido_rev_uuid = BLE_UUID128_INIT(
    0xBB, 0x23, 0xD6, 0x7E, 0xBA, 0xC9, 0x2F, 0xB4,
    0xEE, 0xEC, 0xAA, 0xDE, 0xF4, 0xFF, 0xD0, 0xF1
);

/* Web Companion GATT Service (0xFFE0 / 0xFFE1 / 0xFFE2) - bypasses Web Bluetooth FIDO blocklist */
static const ble_uuid16_t s_companion_svc_uuid = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t s_companion_cp_uuid = BLE_UUID16_INIT(0xFFE1);
static const ble_uuid16_t s_companion_status_uuid = BLE_UUID16_INIT(0xFFE2);
static const ble_uuid16_t s_fido_service_revision_uuid = BLE_UUID16_INIT(0x2A28);
static uint16_t s_companion_status_val_handle = 0;

static uint16_t s_fido_status_val_handle = 0;
static uint16_t s_status_val_handle = 0;
static uint16_t s_subscribed_status_handle = 0;
static uint8_t  s_fido_selected_revision = 0x20;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_addr_type;
static volatile bool s_is_connected = false;
static volatile bool s_ble_initialized = false;
static volatile bool s_host_stopped = false;
static volatile bool s_stopping = false;
static volatile bool s_tick_active = false;
static portMUX_TYPE s_tick_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_tx_mutex_buf;
static SemaphoreHandle_t s_tx_mutex = NULL;
static auth_ble_req_cb_t s_req_cb = NULL;
static TaskHandle_t s_ble_host_task = NULL;

/* Master Key Derivation */
static uint8_t s_fido_master_seed[32];
static bool s_master_seed_loaded = false;

static void clear_key_bytes(void *data, size_t len)
{
    volatile uint8_t *p = data;
    while (len--) *p++ = 0;
}

static bool load_fido_material(const char *key, uint8_t *data, size_t size, bool create)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("fido_sec", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        clear_key_bytes(data, size);
        return false;
    }
    size_t len = size;
    err = nvs_get_blob(h, key, data, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND && create) {
        esp_fill_random(data, size);
        err = nvs_set_blob(h, key, data, size);
    } else if (err == ESP_OK && len != size) {
        err = ESP_FAIL;
    }
    // Also confirm readable material: a previous failed commit may have left
    // pending data in the backend. A successful read alone is not durability.
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) clear_key_bytes(data, size);
    return err == ESP_OK;
}

static bool next_fido_counter(uint32_t *counter)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("fido_sec", NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    uint32_t value = 0;
    err = nvs_get_u32(h, "sign_ctr", &value);
    if (err == ESP_ERR_NVS_NOT_FOUND) { value = 0; err = ESP_OK; }
    if (err == ESP_OK && value == UINT32_MAX) err = ESP_FAIL;
    if (err == ESP_OK) err = nvs_set_u32(h, "sign_ctr", ++value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) *counter = value;
    return err == ESP_OK;
}

static bool ensure_master_seed(void)
{
    if (s_master_seed_loaded) return true;
    s_master_seed_loaded = load_fido_material("seed", s_fido_master_seed,
                                             sizeof(s_fido_master_seed), true);
    return s_master_seed_loaded;
}

void auth_ble_reload_seed(void)
{
    clear_key_bytes(s_fido_master_seed, sizeof(s_fido_master_seed));
    s_master_seed_loaded = false;
}

static bool derive_fido_key(const char *rpid, uint8_t out_priv[32],
                            uint8_t *out_cred_id, size_t cred_id_len, bool create)
{
    memset(out_priv, 0, 32);
    memset(out_cred_id, 0, cred_id_len);
    if (!ensure_master_seed()) return false;

    /* The salt is migrated unchanged; the derivation remains compatible. */
    auth_fido_record_t record;
    if (auth_fido_load(rpid, create, &record) != AUTH_FIDO_OK) return false;
    uint8_t salt[16];
    memcpy(salt, record.salt, sizeof(salt));
    clear_key_bytes(&record, sizeof(record));

    uint8_t msg[128];
    size_t rlen = strlen(rpid);
    if (rlen > sizeof(msg) - 4 - sizeof(salt)) {
        rlen = sizeof(msg) - 4 - sizeof(salt);
    }

    memcpy(msg, "priv", 4);
    memcpy(msg + 4, rpid, rlen);
    memcpy(msg + 4 + rlen, salt, sizeof(salt));
    int err = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                               s_fido_master_seed, 32,
                               msg, 4 + rlen + sizeof(salt), out_priv);

    out_priv[0] &= 0x7F;
    if (out_priv[0] == 0) out_priv[0] = 1;

    uint8_t full_cred_id[32];
    memcpy(msg, "cred", 4);
    if (!err) err = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                   s_fido_master_seed, 32,
                                   msg, 4 + rlen + sizeof(salt), full_cred_id);
    if (cred_id_len > sizeof(full_cred_id)) err = -1;
    if (!err) memcpy(out_cred_id, full_cred_id, cred_id_len);
    else clear_key_bytes(out_priv, 32);
    clear_key_bytes(full_cred_id, sizeof(full_cred_id));
    clear_key_bytes(salt, sizeof(salt));
    clear_key_bytes(msg, sizeof(msg));
    return err == 0;
}

/* Fragmented CTAP BLE notification sender */
static bool ble_send_ctap_frame(uint8_t cmd, const uint8_t *payload, size_t payload_len)
{
    if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "CTAP TX mutex timeout");
        return false;
    }

    bool ok = false;
    if (!s_is_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        goto out;
    }

    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < 23) mtu = 23;
    size_t max_chunk = mtu - 3;
    if (max_chunk < 20) max_chunk = 20;

    uint8_t buf[520];
    if (max_chunk > sizeof(buf)) max_chunk = sizeof(buf);
    buf[0] = cmd;
    buf[1] = (uint8_t)((payload_len >> 8) & 0xFF);
    buf[2] = (uint8_t)(payload_len & 0xFF);

    size_t first_len = (payload_len < (max_chunk - 3)) ? payload_len : (max_chunk - 3);
    if (first_len > 0 && payload) {
        memcpy(buf + 3, payload, first_len);
    }
    size_t first_packet_len = 3 + first_len;

    /* CTAP responses must ALWAYS be notified on official FIDO Status characteristic */
    uint16_t handle = s_fido_status_val_handle ? s_fido_status_val_handle : s_status_val_handle;
    if (handle == 0) {
        ESP_LOGE(TAG, "FIDO status handle is not registered!");
        goto out;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, first_packet_len);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, handle, om);
    }

    size_t sent = first_len;
    uint8_t seq = 0;
    while (sent < payload_len) {
        if (!s_is_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_stopping) {
            goto out;
        }
        size_t chunk_data_len = payload_len - sent;
        if (chunk_data_len > (max_chunk - 1)) {
            chunk_data_len = max_chunk - 1;
        }
        buf[0] = seq & 0x7F;
        memcpy(buf + 1, payload + sent, chunk_data_len);
        sent += chunk_data_len;
        seq = (seq + 1) & 0x7F;

        struct os_mbuf *om_cont = ble_hs_mbuf_from_flat(buf, 1 + chunk_data_len);
        if (om_cont) {
            ble_gatts_notify_custom(s_conn_handle, handle, om_cont);
        }
        /* Pace fragments above the peer's connection interval. Android commonly
         * negotiates 30-50 ms; sending faster fills the controller TX queue and
         * makes the client wait forever for a fragment that was never sent. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ok = true;
out:
    if (s_tx_mutex) xSemaphoreGive(s_tx_mutex);
    return ok;
}

/* User Presence & Keep-alive State */
typedef enum {
    CTAP_OP_NONE = 0,
    CTAP_OP_MAKE_CREDENTIAL,
    CTAP_OP_GET_ASSERTION
} pending_ctap_op_t;

typedef struct {
    pending_ctap_op_t op;
    char rpid[64];
    uint8_t user_id[64];
    size_t user_id_len;
    uint8_t allow_list[CTAP_MAX_MSG_LEN];
    size_t allow_list_len;
    uint8_t rpid_hash[32];
    uint8_t cdh[32];
    bool uv;
} pending_ctap_snapshot_t;

static volatile pending_ctap_op_t s_pending_op = CTAP_OP_NONE;
static char s_pending_rpid[64] = "webauthn.io";
static char s_pending_username[64] = "Passkey User";
static uint8_t s_pending_user_id[64] = {0};
static size_t s_pending_user_id_len = 0;
static uint8_t s_pending_allow_list[CTAP_MAX_MSG_LEN];
static size_t s_pending_allow_list_len;
static uint8_t s_pending_rpid_hash[32];
static uint8_t s_pending_cdh[32];
static volatile bool s_pending_uv = false;
static volatile uint32_t s_op_start_tick = 0;
static volatile uint32_t s_last_keepalive_tick = 0;
static pending_ctap_snapshot_t s_app_confirm_snap;

void auth_ble_tick(void)
{
    portENTER_CRITICAL(&s_tick_lock);
    if (!s_ble_initialized || s_stopping || s_tick_active) {
        portEXIT_CRITICAL(&s_tick_lock);
        return;
    }
    s_tick_active = true;
    portEXIT_CRITICAL(&s_tick_lock);

    bool send_keepalive = false;
    bool timed_out = false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_pending_lock);
    if (s_pending_op != CTAP_OP_NONE && s_is_connected) {
        if (now - s_op_start_tick >= FIDO_DEFAULT_TIMEOUT_MS + 2000U) {
            s_pending_op = CTAP_OP_NONE;
            s_pending_uv = false;
            s_pending_allow_list_len = 0;
            timed_out = true;
        } else if (now - s_last_keepalive_tick >= 500U) {
            s_last_keepalive_tick = now;
            send_keepalive = true;
        }
    }
    portEXIT_CRITICAL(&s_pending_lock);

    if (timed_out) {
        ESP_LOGW(TAG, "CTAP operation timed out on device!");
        uint8_t err_resp[1] = { 0x32 }; /* CTAP2_ERR_USER_ACTION_TIMEOUT */
        ble_send_ctap_frame(0x83, err_resp, 1);
        if (s_req_cb) {
            fido_parsed_cmd_t cmd_evt = { 0 };
            cmd_evt.type = FIDO_CMD_CANCEL;
            s_req_cb(&cmd_evt);
        }
    } else if (send_keepalive) {
        uint8_t tup[1] = { 0x02 }; /* STATUS_UP_NEEDED (500ms CTAP standard) */
        ble_send_ctap_frame(0x82, tup, 1);
    }

    portENTER_CRITICAL(&s_tick_lock);
    s_tick_active = false;
    portEXIT_CRITICAL(&s_tick_lock);
}

static void send_fido2_make_credential_response(const pending_ctap_snapshot_t *snap)
{
    const char *rpid = snap->rpid;
    const uint8_t *rpid_hash = snap->rpid_hash;
    uint8_t priv[32], cred_id[FIDO_CRED_ID_LEN];
    if (!derive_fido_key(rpid, priv, cred_id, sizeof(cred_id), true)) {
        const uint8_t error = 0x7f; /* CTAP2_ERR_OTHER: stable key material unavailable. */
        ble_send_ctap_frame(0x83, &error, 1);
        return;
    }

    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_mpi_read_binary(&ctx.d, priv, 32);
    clear_key_bytes(priv, sizeof(priv));
    mbedtls_ecp_mul(&ctx.grp, &ctx.Q, &ctx.d, &ctx.grp.G, auth_mbedtls_rng, NULL);

    uint8_t qx[32], qy[32];
    mbedtls_mpi_write_binary(&ctx.Q.X, qx, 32);
    mbedtls_mpi_write_binary(&ctx.Q.Y, qy, 32);
    mbedtls_ecdsa_free(&ctx);

    /* Success requires the user handle to be durable as well as the key. */
    if (snap->user_id_len > 0) {
        if (!auth_fido_save_user(rpid, snap->user_id, snap->user_id_len)) {
            const uint8_t error = 0x7f;
            ble_send_ctap_frame(0x83, &error, 1);
            return;
        }
    }

    uint8_t cbor_resp[384];
    size_t cr = ctap2_build_make_credential_resp(rpid_hash, s_aaguid, cred_id,
                                                 sizeof(cred_id), qx, qy, 0,
                                                 snap->uv ? 0x45 : 0x41,
                                                 cbor_resp, sizeof(cbor_resp));

    ESP_LOGI(TAG, "Sending FIDO2 MakeCredential response (%u bytes, rp=%s)", (unsigned int)cr, rpid);
    ESP_LOG_BUFFER_HEX(TAG, cbor_resp, cr);
    ble_send_ctap_frame(0x83, cbor_resp, cr);
}

static void send_fido2_get_assertion_response(const pending_ctap_snapshot_t *snap)
{
    const char *rpid = snap->rpid;
    const uint8_t *rpid_hash = snap->rpid_hash;
    const uint8_t *cdh = snap->cdh;
    uint32_t sign_count = 0;
    const uint8_t auth_flags = snap->uv ? 0x05 : 0x01; /* UP | UV when verified */

    uint8_t priv[32], cred_id[FIDO_CRED_ID_LEN];
    if (!derive_fido_key(rpid, priv, cred_id, sizeof(cred_id), false)) {
        const uint8_t error = 0x7f;
        ble_send_ctap_frame(0x83, &error, 1);
        return;
    }

    if (snap->allow_list_len &&
        !ctap2_allow_list_contains(snap->allow_list, snap->allow_list_len,
                                   cred_id, sizeof(cred_id))) {
        clear_key_bytes(priv, sizeof(priv));
        const uint8_t err = 0x2E; /* CTAP2_ERR_NO_CREDENTIALS */
        ble_send_ctap_frame(0x83, &err, 1);
        return;
    }

    if (!next_fido_counter(&sign_count)) {
        clear_key_bytes(priv, sizeof(priv));
        const uint8_t error = 0x7f;
        ble_send_ctap_frame(0x83, &error, 1);
        return;
    }

    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_mpi_read_binary(&ctx.d, priv, 32);
    clear_key_bytes(priv, sizeof(priv));
    mbedtls_ecp_mul(&ctx.grp, &ctx.Q, &ctx.d, &ctx.grp.G, auth_mbedtls_rng, NULL);

    /* authData (37 bytes): rpIdHash (32) + flags (1) + signCount (4) */
    uint8_t auth_data[37];
    memcpy(auth_data, rpid_hash, 32);
    auth_data[32] = auth_flags;
    auth_data[33] = (uint8_t)((sign_count >> 24) & 0xFF);
    auth_data[34] = (uint8_t)((sign_count >> 16) & 0xFF);
    auth_data[35] = (uint8_t)((sign_count >> 8) & 0xFF);
    auth_data[36] = (uint8_t)(sign_count & 0xFF);

    /* Signature over authData || clientDataHash */
    uint8_t to_sign[69];
    memcpy(to_sign, auth_data, 37);
    memcpy(to_sign + 37, cdh, 32);
    uint8_t hash[32];
    mbedtls_sha256(to_sign, sizeof(to_sign), hash, 0);

    uint8_t sig_der[72];
    size_t sig_len = 0;
    mbedtls_ecdsa_write_signature(&ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig_der, sizeof(sig_der), &sig_len, auth_mbedtls_rng, NULL);
    mbedtls_ecdsa_free(&ctx);

    auth_fido_record_t record;
    if (auth_fido_load(rpid, false, &record) != AUTH_FIDO_OK) {
        const uint8_t error = 0x7f;
        ble_send_ctap_frame(0x83, &error, 1);
        return;
    }

    uint8_t cbor_resp[384];
    size_t cr = ctap2_build_get_assertion_resp(rpid_hash, cred_id, sizeof(cred_id),
                                                auth_flags, sign_count,
                                                sig_der, sig_len,
                                                record.user_len ? record.user_id : NULL, record.user_len,
                                                cbor_resp, sizeof(cbor_resp));

    clear_key_bytes(&record, sizeof(record));
    ESP_LOGI(TAG, "Sending FIDO2 GetAssertion response (%u bytes, rp=%s)", (unsigned int)cr, rpid);
    ble_send_ctap_frame(0x83, cbor_resp, cr);
}

static bool take_pending_snapshot(pending_ctap_snapshot_t *out)
{
    portENTER_CRITICAL(&s_pending_lock);
    if (!s_ble_initialized || s_pending_op == CTAP_OP_NONE) {
        portEXIT_CRITICAL(&s_pending_lock);
        return false;
    }
    out->op = s_pending_op;
    memcpy(out->rpid, s_pending_rpid, sizeof(out->rpid));
    memcpy(out->user_id, s_pending_user_id, sizeof(out->user_id));
    out->user_id_len = s_pending_user_id_len;
    out->allow_list_len = s_pending_allow_list_len;
    if (s_pending_allow_list_len) {
        memcpy(out->allow_list, s_pending_allow_list, s_pending_allow_list_len);
    }
    memcpy(out->rpid_hash, s_pending_rpid_hash, sizeof(out->rpid_hash));
    memcpy(out->cdh, s_pending_cdh, sizeof(out->cdh));
    out->uv = s_pending_uv;

    s_pending_op = CTAP_OP_NONE;
    s_pending_uv = false;
    s_pending_allow_list_len = 0;
    s_pending_user_id_len = 0;
    portEXIT_CRITICAL(&s_pending_lock);
    return true;
}

void auth_ble_fido_user_confirm(bool approved)
{
    if (!take_pending_snapshot(&s_app_confirm_snap)) return;

    if (approved) {
        if (s_app_confirm_snap.op == CTAP_OP_MAKE_CREDENTIAL) {
            send_fido2_make_credential_response(&s_app_confirm_snap);
        } else if (s_app_confirm_snap.op == CTAP_OP_GET_ASSERTION) {
            send_fido2_get_assertion_response(&s_app_confirm_snap);
        }
    } else {
        uint8_t err_resp[1] = { 0x27 }; /* CTAP2_ERR_OPERATION_DENIED */
        ble_send_ctap_frame(0x83, err_resp, 1);
    }
    clear_key_bytes(&s_app_confirm_snap, sizeof(s_app_confirm_snap));
}

void auth_ble_fido_user_timeout(void)
{
    if (!take_pending_snapshot(&s_app_confirm_snap)) return;
    uint8_t err_resp[1] = { 0x32 }; /* CTAP2_ERR_USER_ACTION_TIMEOUT */
    ble_send_ctap_frame(0x83, err_resp, 1);
    clear_key_bytes(&s_app_confirm_snap, sizeof(s_app_confirm_snap));
}

/* CTAP RX Buffer for Multi-packet Reassembly */

typedef struct {
    uint8_t  cmd;
    uint16_t total_len;
    uint16_t received_len;
    uint8_t  expected_seq;
    bool     in_progress;
    uint8_t  buf[CTAP_MAX_MSG_LEN];
} ctap_rx_ctx_t;

static ctap_rx_ctx_t s_rx_ctx = { 0 };

static size_t build_getinfo_response(uint8_t *out, size_t max_out)
{
    if (max_out < 160) return 0;

    const bool pin_set = fido_pin_is_set();
    size_t n = 0;

    out[n++] = 0x00; /* CTAP1_ERR_SUCCESS */
    out[n++] = 0xA7; /* Map(7) */
    out[n++] = 0x01; out[n++] = 0x82;
    out[n++] = 0x68; memcpy(out + n, "FIDO_2_1", 8); n += 8;
    out[n++] = 0x68; memcpy(out + n, "FIDO_2_0", 8); n += 8;
    out[n++] = 0x03; out[n++] = 0x50; memset(out + n, 0, 16); n += 16;
    out[n++] = 0x04; out[n++] = 0xA7; /* options Map(7) */
    out[n++] = 0x62; memcpy(out + n, "rk", 2); n += 2; out[n++] = 0xF5;
    out[n++] = 0x62; memcpy(out + n, "up", 2); n += 2; out[n++] = 0xF5;
    out[n++] = 0x62; memcpy(out + n, "uv", 2); n += 2; out[n++] = 0xF4;
    out[n++] = 0x64; memcpy(out + n, "plat", 4); n += 4; out[n++] = 0xF4;
    out[n++] = 0x68; memcpy(out + n, "alwaysUv", 8); n += 8; out[n++] = 0xF4;
    out[n++] = 0x69; memcpy(out + n, "clientPin", 9); n += 9; out[n++] = pin_set ? 0xF5 : 0xF4;
    out[n++] = 0x70; memcpy(out + n, "makeCredUvNotRqd", 16); n += 16; out[n++] = 0xF5;
    out[n++] = 0x05; out[n++] = 0x19; out[n++] = 0x04; out[n++] = 0x00;
    out[n++] = 0x06; out[n++] = 0x82; out[n++] = 0x02; out[n++] = 0x01; /* pinUvAuthProtocols: [2, 1] */
    out[n++] = 0x09; out[n++] = 0x81; out[n++] = 0x63;
    memcpy(out + n, "ble", 3); n += 3;
    out[n++] = 0x0A; out[n++] = 0x81; out[n++] = 0xA2;
    out[n++] = 0x63; memcpy(out + n, "alg", 3); n += 3; out[n++] = 0x26;
    out[n++] = 0x64; memcpy(out + n, "type", 4); n += 4;
    out[n++] = 0x6A; memcpy(out + n, "public-key", 10); n += 10;
    return n;
}

static void clear_pending_op(void)
{
    portENTER_CRITICAL(&s_pending_lock);
    s_pending_op = CTAP_OP_NONE;
    s_pending_uv = false;
    s_pending_allow_list_len = 0;
    s_pending_user_id_len = 0;
    portEXIT_CRITICAL(&s_pending_lock);
}

static void process_complete_ctap_message(uint8_t cmd, const uint8_t *payload, size_t len)
{
    ESP_LOGI(TAG, "Process CTAP Message: cmd=0x%02X len=%u", cmd, (unsigned int)len);

    if (cmd == 0x81) {
        /* CMD_PING: Echo back */
        ble_send_ctap_frame(0x81, payload, len);
        return;
    }

    if (cmd == 0x83) {
        if (len == 0) return;
        uint8_t ctap_cmd = payload[0];
        ESP_LOGI(TAG, "CTAP2 operation: 0x%02X", ctap_cmd);

        if (ctap_cmd == 0x04) {
            /* authenticatorGetInfo: dynamic because clientPin is stateful. */
            uint8_t getinfo[192];
            size_t glen = build_getinfo_response(getinfo, sizeof(getinfo));
            if (glen > 0) {
                ble_send_ctap_frame(0x83, getinfo, glen);
            } else {
                uint8_t err_resp[1] = { 0x01 };
                ble_send_ctap_frame(0x83, err_resp, 1);
            }
            return;
        }

        if (ctap_cmd == 0x01) {
            /* authenticatorMakeCredential */
            ESP_LOGI(TAG, "Passkey Registration (MakeCredential) requested!");
            portENTER_CRITICAL(&s_pending_lock);
            bool busy = s_pending_op != CTAP_OP_NONE;
            portEXIT_CRITICAL(&s_pending_lock);
            if (busy) {
                uint8_t busy_resp[1] = { 0x2A }; /* CTAP2_ERR_OPERATION_PENDING */
                ble_send_ctap_frame(0x83, busy_resp, 1);
                return;
            }
            static char rx_rpid[64];
            static char rx_username[64];
            static uint8_t rx_cdh[32];
            static uint8_t rx_user_id[64];
            size_t rx_user_id_len = 0;
            fido_auth_params_t auth;
            if (!ctap2_parse_make_credential(payload, len,
                                             rx_rpid, sizeof(rx_rpid),
                                             rx_cdh,
                                             rx_username, sizeof(rx_username),
                                             rx_user_id, &rx_user_id_len,
                                             &auth)) {
                ESP_LOGE(TAG, "MakeCredential CBOR parse failed");
                uint8_t err_resp[1] = { 0x12 }; /* CTAP2_ERR_INVALID_CBOR */
                ble_send_ctap_frame(0x83, err_resp, 1);
                return;
            }
            ESP_LOGI(TAG, "MakeCredential rp='%s' user='%s' cdh0=%02X uvReq=%d pinAuth=%u",
                     rx_rpid, rx_username, rx_cdh[0],
                     (int)auth.uv_requested, (unsigned)auth.pin_uv_auth_param_len);

            bool uv = false;
            if (auth.pin_uv_auth_param_len > 0) {
                if (!fido_pin_verify_pin_auth(rx_cdh, auth.pin_uv_auth_param,
                                              auth.pin_uv_auth_param_len)) {
                    ESP_LOGW(TAG, "MakeCredential pinUvAuthParam invalid");
                    uint8_t err_resp[1] = { 0x33 }; /* PIN_AUTH_INVALID */
                    ble_send_ctap_frame(0x83, err_resp, 1);
                    return;
                }
                uv = true;
            }
            if (auth.uv_requested && !uv) {
                /* No PIN token was provided. Continue with UP-only: clients that
                   requested "preferred" accept UV=0, while a strict "required"
                   client will reject the response after inspecting the flags. */
                uv = false;
            }

            uint8_t rx_rpid_hash[32];
            mbedtls_sha256((const unsigned char *)rx_rpid, strlen(rx_rpid), rx_rpid_hash, 0);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            portENTER_CRITICAL(&s_pending_lock);
            if (s_pending_op != CTAP_OP_NONE) {
                portEXIT_CRITICAL(&s_pending_lock);
                uint8_t busy_resp[1] = { 0x2A };
                ble_send_ctap_frame(0x83, busy_resp, 1);
                return;
            }
            memcpy(s_pending_rpid, rx_rpid, sizeof(s_pending_rpid));
            memcpy(s_pending_username, rx_username, sizeof(s_pending_username));
            memcpy(s_pending_user_id, rx_user_id, sizeof(s_pending_user_id));
            s_pending_user_id_len = rx_user_id_len;
            s_pending_allow_list_len = 0;
            memcpy(s_pending_cdh, rx_cdh, sizeof(s_pending_cdh));
            memcpy(s_pending_rpid_hash, rx_rpid_hash, sizeof(s_pending_rpid_hash));
            s_pending_uv = uv;
            s_pending_op = CTAP_OP_MAKE_CREDENTIAL;
            s_op_start_tick = now;
            s_last_keepalive_tick = now;
            portEXIT_CRITICAL(&s_pending_lock);

            /* Immediate keepalive to client */
            uint8_t tup[1] = { 0x02 }; /* STATUS_UP_NEEDED */
            ble_send_ctap_frame(0x82, tup, 1);

            if (s_req_cb) {
                fido_parsed_cmd_t cmd_evt = { 0 };
                cmd_evt.type = FIDO_CMD_REG_REQ;
                strncpy(cmd_evt.param1, rx_rpid, sizeof(cmd_evt.param1) - 1);
                strncpy(cmd_evt.param2, rx_username[0] ? rx_username : "Passkey User", sizeof(cmd_evt.param2) - 1);
                s_req_cb(&cmd_evt);
            }
            return;
        }

        if (ctap_cmd == 0x02) {
            /* authenticatorGetAssertion */
            ESP_LOGI(TAG, "Passkey Authentication (GetAssertion) requested!");
            portENTER_CRITICAL(&s_pending_lock);
            bool busy = s_pending_op != CTAP_OP_NONE;
            portEXIT_CRITICAL(&s_pending_lock);
            if (busy) {
                uint8_t busy_resp[1] = { 0x2A }; /* CTAP2_ERR_OPERATION_PENDING */
                ble_send_ctap_frame(0x83, busy_resp, 1);
                return;
            }
            static char rx_rpid[64];
            static uint8_t rx_cdh[32];
            uint8_t allow_id[64] = {0};
            size_t allow_id_len = 0;
            fido_auth_params_t auth;
            if (!ctap2_parse_get_assertion(payload, len,
                                           rx_rpid, sizeof(rx_rpid),
                                           rx_cdh,
                                           allow_id, &allow_id_len,
                                           &auth)) {
                ESP_LOGE(TAG, "GetAssertion CBOR parse failed");
                uint8_t err_resp[1] = { 0x12 }; /* CTAP2_ERR_INVALID_CBOR */
                ble_send_ctap_frame(0x83, err_resp, 1);
                return;
            }
            ESP_LOGI(TAG, "GetAssertion rp='%s' allowIdLen=%u cdh0=%02X uvReq=%d pinAuth=%u",
                     rx_rpid, (unsigned)allow_id_len, rx_cdh[0],
                     (int)auth.uv_requested, (unsigned)auth.pin_uv_auth_param_len);
            if (auth.allow_list_len > sizeof(s_pending_allow_list)) {
                const uint8_t error = 0x15; /* CTAP2_ERR_LIMIT_EXCEEDED */
                ble_send_ctap_frame(0x83, &error, 1);
                return;
            }

            bool uv = false;
            if (auth.pin_uv_auth_param_len > 0) {
                if (!fido_pin_verify_pin_auth(rx_cdh, auth.pin_uv_auth_param,
                                              auth.pin_uv_auth_param_len)) {
                    ESP_LOGW(TAG, "GetAssertion pinUvAuthParam invalid");
                    uint8_t err_resp[1] = { 0x33 }; /* PIN_AUTH_INVALID */
                    ble_send_ctap_frame(0x83, err_resp, 1);
                    return;
                }
                uv = true;
            }
            if (auth.uv_requested && !uv) {
                /* No PIN token was provided. Continue with UP-only: clients that
                   requested "preferred" accept UV=0, while a strict "required"
                   client will reject the response after inspecting the flags. */
                uv = false;
            }

            uint8_t rx_rpid_hash[32];
            mbedtls_sha256((const unsigned char *)rx_rpid, strlen(rx_rpid), rx_rpid_hash, 0);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            portENTER_CRITICAL(&s_pending_lock);
            if (s_pending_op != CTAP_OP_NONE) {
                portEXIT_CRITICAL(&s_pending_lock);
                uint8_t busy_resp[1] = { 0x2A };
                ble_send_ctap_frame(0x83, busy_resp, 1);
                return;
            }
            memcpy(s_pending_rpid, rx_rpid, sizeof(s_pending_rpid));
            memcpy(s_pending_cdh, rx_cdh, sizeof(s_pending_cdh));
            memcpy(s_pending_rpid_hash, rx_rpid_hash, sizeof(s_pending_rpid_hash));
            if (auth.allow_list_len) memcpy(s_pending_allow_list, auth.allow_list, auth.allow_list_len);
            s_pending_allow_list_len = auth.allow_list_len;
            s_pending_user_id_len = 0;
            s_pending_uv = uv;
            s_pending_op = CTAP_OP_GET_ASSERTION;
            s_op_start_tick = now;
            s_last_keepalive_tick = now;
            portEXIT_CRITICAL(&s_pending_lock);

            /* Immediate keepalive to client */
            uint8_t tup[1] = { 0x02 }; /* STATUS_UP_NEEDED */
            ble_send_ctap_frame(0x82, tup, 1);

            if (s_req_cb) {
                fido_parsed_cmd_t cmd_evt = { 0 };
                cmd_evt.type = FIDO_CMD_AUTH_REQ;
                strncpy(cmd_evt.param1, rx_rpid, sizeof(cmd_evt.param1) - 1);
                strncpy(cmd_evt.param2, "Passkey User", sizeof(cmd_evt.param2) - 1);
                s_req_cb(&cmd_evt);
            }
            return;
        }

        if (ctap_cmd == 0x06) {
            uint8_t resp[512];
            size_t rlen = fido_pin_handle_client_pin(payload, len, resp, sizeof(resp));
            if (rlen > 0) {
                ble_send_ctap_frame(0x83, resp, rlen);
            } else {
                uint8_t err_resp[1] = { 0x01 };
                ble_send_ctap_frame(0x83, err_resp, 1);
            }
            return;
        }

        if (ctap_cmd == 0x07) {
            uint8_t denied[1] = { 0x30 };
            ble_send_ctap_frame(0x83, denied, sizeof(denied));
            return;
        }

        /* CTAP1 U2F APDU Fallback */
        if (payload[0] == 0x00 && len >= 2) {
            if (payload[1] == 0x03) {
                /* U2F_VERSION */
                static const uint8_t u2f_ver[] = { 0x00, 'U', '2', 'F', '_', 'V', '2', 0x90, 0x00 };
                ble_send_ctap_frame(0x83, u2f_ver, sizeof(u2f_ver));
                return;
            }
        }

        /* Unhandled / unsupported CTAP2 command: return CTAP1_ERR_INVALID_COMMAND (0x01) */
        ESP_LOGW(TAG, "Unsupported CTAP2 command: 0x%02X", ctap_cmd);
        uint8_t err_cmd[1] = { 0x01 };
        ble_send_ctap_frame(0x83, err_cmd, 1);
        return;
    }
}


static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* 1. Official FIDO Control Point (Write) */
                .uuid = &s_char_fido_cp_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* 2. Official FIDO Status (Notify only per CTAP2 spec) */
                .uuid = &s_char_fido_status_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_fido_status_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /* 3. Official FIDO Control Point Length (Read: Dynamic MTU - 3) */
                .uuid = &s_char_fido_cplen_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                /* 4. Official FIDO Service Revision Bitfield (Read/Write: 0x20) */
                .uuid = &s_char_fido_rev_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                /* 5. FIDO Service Revision (0x2A28, read: "2.0") */
                .uuid = &s_fido_service_revision_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 } /* End of characteristics */
        },
    },
    {
        /* Web Companion Custom Service (0xFFE0) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_companion_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_companion_cp_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_companion_status_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_companion_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 } /* End of services */
};

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    /* 1. Control Point Length (0xFFF3) */
    if (ble_uuid_cmp(ctxt->chr->uuid, &s_char_fido_cplen_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint16_t mtu = ble_att_mtu(conn_handle);
            uint16_t cpl = (mtu > 3) ? (mtu - 3) : 20;
            if (cpl > 512) cpl = 512;
            if (cpl < 20) cpl = 20;
            uint8_t cplen[2] = { (uint8_t)(cpl >> 8), (uint8_t)(cpl & 0xFF) };
            ESP_LOGI(TAG, "Control Point Length read: mtu=%u value=%u", mtu, cpl);
            int rc = os_mbuf_append(ctxt->om, cplen, sizeof(cplen));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    /* 2. Service Revision Bitfield (0xFFF4) */
    else if (ble_uuid_cmp(ctxt->chr->uuid, &s_char_fido_rev_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t rev = s_fido_selected_revision;
            int rc = os_mbuf_append(ctxt->om, &rev, 1);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint8_t rev_val = 0;
            ble_hs_mbuf_to_flat(ctxt->om, &rev_val, 1, NULL);
            ESP_LOGI(TAG, "Client selected FIDO revision: 0x%02X", rev_val);
            if (rev_val & 0x20) {
                s_fido_selected_revision = rev_val;
            }
            return 0;
        }
    }
    /* 3. FIDO Service Revision (0x2A28) */
    else if (ble_uuid_cmp(ctxt->chr->uuid, &s_fido_service_revision_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            static const char fido_rev[] = "2.0";
            int rc = os_mbuf_append(ctxt->om, fido_rev, sizeof(fido_rev) - 1);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    /* 4. Companion Status Read (0xFFE2) */
    else if (ble_uuid_cmp(ctxt->chr->uuid, &s_companion_status_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *status_msg = "READY";
            int rc = os_mbuf_append(ctxt->om, status_msg, strlen(status_msg));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    /* 4. Control Point Write (0xFFF1, 0xFFE1) */
    else if (ble_uuid_cmp(ctxt->chr->uuid, &s_char_fido_cp_uuid.u) == 0 ||
             ble_uuid_cmp(ctxt->chr->uuid, &s_companion_cp_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            static uint8_t rx_buf[CTAP_MAX_MSG_LEN];
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len > sizeof(rx_buf)) len = sizeof(rx_buf);
            int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, len, NULL);
            if (rc != 0 || len == 0) return BLE_ATT_ERR_UNLIKELY;

            /* Handle CMD_CANCEL (0xBE) */
            if (rx_buf[0] == 0xBE) {
                ESP_LOGI(TAG, "Client sent CMD_CANCEL (0xBE)");
                clear_pending_op();
                s_rx_ctx.in_progress = false;
                static const uint8_t s_cancel_resp[] = { 0x2D }; /* CTAP2_ERR_KEEPALIVE_CANCEL */
                ble_send_ctap_frame(0x83, s_cancel_resp, sizeof(s_cancel_resp));
                if (s_req_cb) {
                    fido_parsed_cmd_t cmd_evt = { 0 };
                    cmd_evt.type = FIDO_CMD_CANCEL;
                    s_req_cb(&cmd_evt);
                }
                return 0;
            }

            /* Check if binary CTAP frame (Initialization packet has high bit set) */
            if (rx_buf[0] & 0x80) {
                uint8_t cmd = rx_buf[0];
                uint16_t total_len = 0;
                if (len >= 3) {
                    total_len = ((uint16_t)rx_buf[1] << 8) | (uint16_t)rx_buf[2];
                }
                ESP_LOGI(TAG, "CTAP Init Frame: cmd=0x%02X total_len=%u pkt_len=%u", cmd, total_len, (unsigned int)len);

                if (total_len == 0 || total_len > CTAP_MAX_MSG_LEN) {
                    ESP_LOGW(TAG, "Rejecting invalid CTAP message length: %u", (unsigned int)total_len);
                    s_rx_ctx.in_progress = false;
                    uint8_t err = 0x03; /* CTAP1_ERR_INVALID_LENGTH */
                    ble_send_ctap_frame(0x83, &err, 1);
                    return 0;
                }

                s_rx_ctx.cmd = cmd;
                s_rx_ctx.total_len = total_len;
                s_rx_ctx.received_len = 0;
                s_rx_ctx.expected_seq = 0;
                s_rx_ctx.in_progress = true;

                size_t chunk = len > 3 ? (len - 3) : 0;
                if (chunk > total_len) chunk = total_len;
                if (chunk > CTAP_MAX_MSG_LEN) chunk = CTAP_MAX_MSG_LEN;
                memcpy(s_rx_ctx.buf, rx_buf + 3, chunk);
                s_rx_ctx.received_len = chunk;

                if (s_rx_ctx.received_len >= s_rx_ctx.total_len) {
                    s_rx_ctx.in_progress = false;
                    process_complete_ctap_message(s_rx_ctx.cmd, s_rx_ctx.buf, s_rx_ctx.total_len);
                }
                return 0;
            } else if (s_rx_ctx.in_progress) {
                /* Continuation packet */
                uint8_t seq = rx_buf[0] & 0x7F;
                ESP_LOGD(TAG, "CTAP Cont Frame: seq=%u expected=%u pkt_len=%u", seq, s_rx_ctx.expected_seq, (unsigned int)len);
                if (seq == s_rx_ctx.expected_seq) {
                    s_rx_ctx.expected_seq++;
                    size_t chunk = len > 1 ? (len - 1) : 0;
                    if (s_rx_ctx.received_len + chunk > s_rx_ctx.total_len) {
                        chunk = s_rx_ctx.total_len - s_rx_ctx.received_len;
                    }
                    if (s_rx_ctx.received_len + chunk <= CTAP_MAX_MSG_LEN) {
                        memcpy(s_rx_ctx.buf + s_rx_ctx.received_len, rx_buf + 1, chunk);
                        s_rx_ctx.received_len += chunk;
                    }
                    if (s_rx_ctx.received_len >= s_rx_ctx.total_len) {
                        s_rx_ctx.in_progress = false;
                        process_complete_ctap_message(s_rx_ctx.cmd, s_rx_ctx.buf, s_rx_ctx.total_len);
                    }
                } else {
                    ESP_LOGW(TAG, "CTAP sequence mismatch! Resetting RX");
                    s_rx_ctx.in_progress = false;
                }
                return 0;
            } else {
                /* ASCII command from Companion */
                if (len >= sizeof(rx_buf)) {
                    len = sizeof(rx_buf) - 1;
                }
                rx_buf[len] = '\0';
                ESP_LOGD(TAG, "BLE companion command received");
                if (s_req_cb) {
                    fido_parsed_cmd_t cmd_parsed = fido_parse_command((char *)rx_buf);
                    s_req_cb(&cmd_parsed);
                }
                return 0;
            }
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_is_connected = true;
            s_rx_ctx.in_progress = false;
            clear_pending_op();
            fido_pin_reset_token();
            ESP_LOGI(TAG, "BLE connected handle=%d", s_conn_handle);
        } else {
            s_is_connected = false;
            auth_ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_is_connected = false;
        s_rx_ctx.in_progress = false;
        clear_pending_op();
        fido_pin_reset_token();
        auth_ble_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        auth_ble_start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe event: attr_handle=%d cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.cur_notify) {
            s_subscribed_status_handle = event->subscribe.attr_handle;
        } else if (s_subscribed_status_handle == event->subscribe.attr_handle) {
            s_subscribed_status_handle = 0;
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* Auto-accept Just Works pairing */
        struct ble_sm_io pkey = { 0 };
        pkey.action = event->passkey.params.action;
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP ||
            event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.numcmp_accept = 1;
        }
        ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change: status=%d", event->enc_change.status);
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "Encryption failed (%d), deleting peer bond & dropping link to allow re-pairing",
                     event->enc_change.status);
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        break;

    default:
        break;
    }
    return 0;
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_addr_type);
    }
    if (rc == 0) {
        auth_ble_start_advertising();
    } else {
        ESP_LOGE(TAG, "BLE on_sync failed: rc=%d", rc);
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    ESP_LOGI(TAG, "NimBLE host task exiting");
    s_host_stopped = true;
    s_ble_host_task = NULL;
    vTaskDelete(NULL);
}

void auth_ble_start_advertising(void)
{
    if (!s_ble_initialized || s_stopping) {
        ESP_LOGW(TAG, "Cannot advertise: NimBLE is not ready");
        return;
    }

    /* 1. Main advertisement fields */
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BLE_DEV_NAME;
    fields.name_len = (uint8_t)strlen(BLE_DEV_NAME);
    fields.name_is_complete = 1;

    /* Add FIDO 16-bit Service UUID */
    static const ble_uuid16_t adv_uuids[] = { BLE_UUID16_INIT(0xFFFD) };
    fields.uuids16 = adv_uuids;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    /* FIDO Service Data for 0xFFFD: 0x20 (FIDO2 / CTAP2) */
    static const uint8_t s_fido_svc_data[3] = { 0xFD, 0xFF, 0x20 };
    fields.svc_data_uuid16 = s_fido_svc_data;
    fields.svc_data_uuid16_len = sizeof(s_fido_svc_data);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set adv fields: rc=%d", rc);
    }

    /* 2. Scan response fields: Name + Companion UUID (0xFFE0) */
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)BLE_DEV_NAME;
    rsp.name_len = (uint8_t)strlen(BLE_DEV_NAME);
    rsp.name_is_complete = 1;
    static const ble_uuid16_t rsp_uuids[] = { BLE_UUID16_INIT(0xFFE0) };
    rsp.uuids16 = rsp_uuids;
    rsp.num_uuids16 = 1;
    rsp.uuids16_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    /* 3. Fast Advertising params: 20ms - 40ms interval, connectable undirected */
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 32; /* 20 ms */
    params.itvl_max = 64; /* 40 ms */

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, ble_gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Failed to start adv: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started as '%s'", BLE_DEV_NAME);
    }
}

bool auth_ble_init(auth_ble_req_cb_t on_req)
{
    if (s_ble_initialized) {
        if (s_stopping) return false;
        s_req_cb = on_req;
        return true;
    }

    s_req_cb = on_req;
    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_buf);
    s_host_stopped = false;
    s_stopping = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_is_connected = false;
    s_rx_ctx.in_progress = false;
    clear_pending_op();
    fido_pin_init();

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return false;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Initialize NimBLE NVS store callbacks for secure persistent bonding */
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_att_set_preferred_mtu(517);

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        (void)nimble_port_deinit();
        return false;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        (void)nimble_port_deinit();
        return false;
    }

    ble_svc_gap_device_name_set(BLE_DEV_NAME);

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    /* Security Manager: LE Secure Connections Just Works (免配对码确认) */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Mark initialized before the host task can invoke ble_on_sync(). */
    s_ble_initialized = true;
    BaseType_t ret = xTaskCreate(ble_host_task, "ble_host", 8192, NULL, configMAX_PRIORITIES - 4, &s_ble_host_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NimBLE host task");
        s_ble_initialized = false;
        (void)nimble_port_deinit();
        return false;
    }
    return true;
}

bool auth_ble_send_response(const char *response_str)
{
    if (!s_ble_initialized || !s_is_connected ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !response_str) {
        return false;
    }

    uint16_t handle = s_companion_status_val_handle;
    if (handle == 0) {
        ESP_LOGE(TAG, "s_companion_status_val_handle is 0!");
        return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(response_str, strlen(response_str));
    if (!om) return false;

    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    return (rc == 0);
}

bool auth_ble_is_enabled(void)
{
    return s_ble_initialized && !s_stopping;
}

bool auth_ble_is_connected(void)
{
    return auth_ble_is_enabled() && s_is_connected;
}

bool auth_ble_stop(void)
{
    if (!s_ble_initialized) return true;
    portENTER_CRITICAL(&s_tick_lock);
    s_stopping = true;
    portEXIT_CRITICAL(&s_tick_lock);
    for (unsigned i = 0; i < 200 && s_tick_active; i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_tick_active) {
        ESP_LOGE(TAG, "BLE sender still active; retaining stack for retry");
        return false;
    }
    ESP_LOGI(TAG, "Stopping BLE...");
    s_is_connected = false;
    clear_pending_op();
    s_rx_ctx.in_progress = false;
    fido_pin_reset_token();
    if (!s_host_stopped) {
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        (void)ble_gap_adv_stop();
        esp_err_t err = nimble_port_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_stop failed: %d", err);
            return false;
        }
        for (unsigned i = 0; i < 200 && !s_host_stopped; i++) vTaskDelay(pdMS_TO_TICKS(5));
        if (!s_host_stopped) {
            ESP_LOGE(TAG, "NimBLE host still active; retaining stack for retry");
            return false;
        }
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %d", err);
        return false;
    }
    s_ble_initialized = false;
    ESP_LOGI(TAG, "BLE stopped");
    return true;
}

#else

/* Host mock implementations */
bool auth_ble_init(auth_ble_req_cb_t on_req) { (void)on_req; return true; }
void auth_ble_start_advertising(void) {}
bool auth_ble_send_response(const char *response_str) { (void)response_str; return true; }
void auth_ble_fido_user_confirm(bool approved) { (void)approved; }
void auth_ble_fido_user_timeout(void) {}
void auth_ble_tick(void) {}
bool auth_ble_is_enabled(void) { return false; }
bool auth_ble_is_connected(void) { return false; }
bool auth_ble_stop(void) { return true; }

#endif
