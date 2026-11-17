/* CTAP clientPIN 协议处理：密钥协商、加解密、重试持久化和临时 token 由互斥量保护。
 * 协议 1/2 的封装与认证长度不同；PIN 验证不是设备的生物识别能力，不可伪造 UV。
 */
#include "fido_pin.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mbedtls/aes.h"
#include "mbedtls/ecp.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

static const char *TAG = "fido_pin";

#define PIN_NVS_NAMESPACE "fido_pin"
#define PIN_DEFAULT_RETRIES 8
#define PIN_MAX_RETRIES 8

#define CTAP2_ERR_INVALID_CBOR       0x12
#define CTAP2_ERR_UNSUPPORTED_OPTION 0x2B
#define CTAP2_ERR_PIN_INVALID        0x31
#define CTAP2_ERR_PIN_BLOCKED        0x32
#define CTAP2_ERR_PIN_AUTH_INVALID   0x33
#define CTAP2_ERR_PIN_NOT_SET        0x35

typedef struct {
    bool initialized;
    bool pin_set;
    uint8_t pin_hash[16];
    uint8_t retries;

    uint8_t protocol;
    bool key_agreement_valid;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;

    uint8_t shared_secret[32];
    uint8_t aes_key[32];
    uint8_t hmac_key[32];

    uint8_t pin_token[32];
    bool token_valid;
    uint8_t token_protocol;
} fido_pin_state_t;

static fido_pin_state_t s_pin;
static SemaphoreHandle_t s_mutex = NULL;

static void pin_lock(void)
{
    if (s_mutex) (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void pin_unlock(void)
{
    if (s_mutex) (void)xSemaphoreGive(s_mutex);
}

static int pin_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CBOR helpers                                                              */
/* ------------------------------------------------------------------------- */

static bool cbor_head(const uint8_t *buf, size_t len, size_t *off,
                      uint8_t *major, uint64_t *val)
{
    if (*off >= len) return false;
    uint8_t b = buf[(*off)++];
    *major = b >> 5;
    uint8_t add = b & 0x1F;
    if (add < 24) {
        *val = add;
        return true;
    }
    size_t n;
    if (add == 24) n = 1;
    else if (add == 25) n = 2;
    else if (add == 26) n = 4;
    else return false;
    if (*off + n > len) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | buf[*off + i];
    *off += n;
    *val = v;
    return true;
}

static bool cbor_skip(const uint8_t *buf, size_t len, size_t *off)
{
    uint8_t major = 0;
    uint64_t val = 0;
    if (!cbor_head(buf, len, off, &major, &val)) return false;
    switch (major) {
    case 0:
    case 1:
    case 7:
        return true;
    case 2:
    case 3:
        if (*off + val > len) return false;
        *off += (size_t)val;
        return true;
    case 4:
        for (uint64_t i = 0; i < val; i++) {
            if (!cbor_skip(buf, len, off)) return false;
        }
        return true;
    case 5:
        for (uint64_t i = 0; i < val; i++) {
            if (!cbor_skip(buf, len, off)) return false;
            if (!cbor_skip(buf, len, off)) return false;
        }
        return true;
    default:
        return false;
    }
}

static bool cbor_uint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out)
{
    uint8_t major = 0;
    uint64_t val = 0;
    if (!cbor_head(buf, len, off, &major, &val) || major != 0) return false;
    *out = val;
    return true;
}

static bool cbor_bstr(const uint8_t *buf, size_t len, size_t *off,
                      uint8_t *out, size_t max_out, size_t *out_len)
{
    uint8_t major = 0;
    uint64_t n = 0;
    if (!cbor_head(buf, len, off, &major, &n) || major != 2) return false;
    if (*off + n > len) return false;
    if (out_len) *out_len = (size_t)n;
    if (out && max_out > 0) {
        size_t cp = (n < max_out) ? (size_t)n : max_out;
        memcpy(out, buf + *off, cp);
    }
    *off += (size_t)n;
    return true;
}

static bool cbor_cose_xy(const uint8_t *buf, size_t len, size_t *off,
                         uint8_t x[32], uint8_t y[32])
{
    uint8_t major = 0;
    uint64_t pairs = 0;
    bool have_x = false, have_y = false;

    if (!cbor_head(buf, len, off, &major, &pairs) || major != 5) return false;
    for (uint64_t i = 0; i < pairs; i++) {
        uint8_t km = 0;
        uint64_t kv = 0;
        if (!cbor_head(buf, len, off, &km, &kv)) return false;
        if (km == 1) {
            /* Key -2 encodes as major 1 with value 1. */
            if (kv == 1) {
                size_t n = 0;
                if (!cbor_bstr(buf, len, off, x, 32, &n) || n != 32) return false;
                have_x = true;
            } else if (kv == 2) {
                size_t n = 0;
                if (!cbor_bstr(buf, len, off, y, 32, &n) || n != 32) return false;
                have_y = true;
            } else {
                if (!cbor_skip(buf, len, off)) return false;
            }
        } else {
            if (!cbor_skip(buf, len, off)) return false;
        }
    }
    return have_x && have_y;
}

/* ------------------------------------------------------------------------- */
/* Crypto helpers                                                            */
/* ------------------------------------------------------------------------- */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len, uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md) (void)mbedtls_md_hmac(md, key, key_len, data, data_len, out);
}

static bool hkdf_sha256(const uint8_t ikm[32], const char *info, uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t salt[32] = {0};
    if (!md) return false;
    return mbedtls_hkdf(md, salt, sizeof(salt), ikm, 32,
                        (const uint8_t *)info, strlen(info), out, 32) == 0;
}

static bool aes_cbc_crypt_iv(const uint8_t key[32], int mode, const uint8_t iv_in[16],
                             const uint8_t *in, size_t len, uint8_t *out)
{
    if (len == 0 || (len % 16) != 0) return false;
    mbedtls_aes_context aes;
    uint8_t iv[16];
    memcpy(iv, iv_in, sizeof(iv));
    mbedtls_aes_init(&aes);
    int rc = (mode == MBEDTLS_AES_DECRYPT)
                 ? mbedtls_aes_setkey_dec(&aes, key, 256)
                 : mbedtls_aes_setkey_enc(&aes, key, 256);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&aes, mode, len, iv, in, out);
    mbedtls_aes_free(&aes);
    return rc == 0;
}

static bool aes_cbc_crypt(const uint8_t key[32], int mode,
                          const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t zero_iv[16] = {0};
    return aes_cbc_crypt_iv(key, mode, zero_iv, in, len, out);
}

static bool aes_cbc_decrypt_prefixed_iv(const uint8_t key[32], const uint8_t *in, size_t in_len,
                                        uint8_t *out, size_t *out_len)
{
    if (in_len < 32 || ((in_len - 16) % 16) != 0) return false;
    if (!aes_cbc_crypt_iv(key, MBEDTLS_AES_DECRYPT, in, in + 16, in_len - 16, out)) return false;
    if (out_len) *out_len = in_len - 16;
    return true;
}

static bool aes_cbc_encrypt_prefixed_iv(const uint8_t key[32], const uint8_t *in, size_t in_len,
                                        uint8_t *out, size_t *out_len)
{
    if ((in_len % 16) != 0) return false;
    esp_fill_random(out, 16);
    if (!aes_cbc_crypt_iv(key, MBEDTLS_AES_ENCRYPT, out, in, in_len, out + 16)) return false;
    if (out_len) *out_len = in_len + 16;
    return true;
}

static bool compute_shared_secret(uint8_t protocol, const uint8_t x[32], const uint8_t y[32])
{
    mbedtls_ecp_point client;
    mbedtls_ecp_point shared;
    uint8_t secret[32];
    bool ok = false;

    mbedtls_ecp_point_init(&client);
    mbedtls_ecp_point_init(&shared);

    uint8_t client_buf[65];
    client_buf[0] = 0x04;
    memcpy(client_buf + 1, x, 32);
    memcpy(client_buf + 33, y, 32);
    if (mbedtls_ecp_point_read_binary(&s_pin.grp, &client, client_buf, sizeof(client_buf)) != 0) goto out;
    if (mbedtls_ecp_check_pubkey(&s_pin.grp, &client) != 0) goto out;
    if (mbedtls_ecp_mul(&s_pin.grp, &shared, &s_pin.d, &client, pin_rng, NULL) != 0) goto out;

    uint8_t shared_buf[65];
    size_t shared_len = 0;
    if (mbedtls_ecp_point_write_binary(&s_pin.grp, &shared, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &shared_len, shared_buf, sizeof(shared_buf)) != 0 ||
        shared_len != sizeof(shared_buf)) {
        goto out;
    }
    memcpy(secret, shared_buf + 1, sizeof(secret));

    if (protocol == 1) {
        mbedtls_sha256(secret, sizeof(secret), s_pin.shared_secret, 0);
        ok = true;
    } else if (protocol == 2) {
        memcpy(s_pin.shared_secret, secret, sizeof(secret));
        ok = hkdf_sha256(s_pin.shared_secret, "CTAP2 AES key", s_pin.aes_key) &&
             hkdf_sha256(s_pin.shared_secret, "CTAP2 HMAC key", s_pin.hmac_key);
    }

out:
    mbedtls_ecp_point_free(&client);
    mbedtls_ecp_point_free(&shared);
    return ok;
}

static bool verify_pin_uv_auth(uint8_t protocol, const uint8_t *message, size_t message_len,
                               const uint8_t *param, size_t param_len)
{
    const uint8_t *key = (protocol == 2) ? s_pin.hmac_key : s_pin.shared_secret;
    uint8_t mac[32];
    hmac_sha256(key, 32, message, message_len, mac);
    if (protocol == 2) {
        return param_len >= 32 && memcmp(mac, param, 32) == 0;
    }
    return param_len >= 16 && memcmp(mac, param, 16) == 0;
}

static bool decrypt_new_pin(const uint8_t *enc, size_t enc_len, uint8_t pin_hash[16])
{
    uint8_t plain[288];
    size_t plain_len = 0;
    if (s_pin.protocol == 1) {
        if (enc_len < 16 || (enc_len % 16) != 0 || enc_len > sizeof(plain)) return false;
        if (!aes_cbc_crypt(s_pin.shared_secret, MBEDTLS_AES_DECRYPT, enc, enc_len, plain)) return false;
        plain_len = enc_len;
    } else {
        if (enc_len > sizeof(plain) + 16) return false;
        if (!aes_cbc_decrypt_prefixed_iv(s_pin.aes_key, enc, enc_len, plain, &plain_len)) return false;
    }

    /* newPinEnc contains the UTF-8 PIN padded with zeroes, not the PIN hash. */
    if (plain_len < 4) return false;
    size_t pin_len = 0;
    while (pin_len < plain_len && plain[pin_len] != 0) pin_len++;
    if (pin_len < 4 || pin_len > 63) return false;

    uint8_t digest[32];
    mbedtls_sha256(plain, pin_len, digest, 0);
    memcpy(pin_hash, digest, 16);
    return true;
}

static bool decrypt_pin_hash(const uint8_t *enc, size_t enc_len, uint8_t pin_hash[16])
{
    uint8_t plain[32];
    size_t plain_len = 0;
    if (s_pin.protocol == 1) {
        if (enc_len != 16) return false;
        if (!aes_cbc_crypt(s_pin.shared_secret, MBEDTLS_AES_DECRYPT, enc, enc_len, plain)) return false;
        plain_len = enc_len;
    } else {
        if (enc_len != 32) return false;
        if (!aes_cbc_decrypt_prefixed_iv(s_pin.aes_key, enc, enc_len, plain, &plain_len)) return false;
    }
    if (plain_len < 16) return false;
    memcpy(pin_hash, plain, 16);
    return true;
}

static bool encrypt_pin_token(const uint8_t pin_token[32], uint8_t *out, size_t *out_len)
{
    if (s_pin.protocol == 1) {
        if (!aes_cbc_crypt(s_pin.shared_secret, MBEDTLS_AES_ENCRYPT, pin_token, 32, out)) return false;
        *out_len = 32;
        return true;
    }
    return aes_cbc_encrypt_prefixed_iv(s_pin.aes_key, pin_token, 32, out, out_len);
}

/* ------------------------------------------------------------------------- */
/* Storage                                                                   */
/* ------------------------------------------------------------------------- */

static void pin_save(void)
{
    nvs_handle_t h;
    if (nvs_open(PIN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_pin.pin_set) {
        (void)nvs_set_blob(h, "hash", s_pin.pin_hash, sizeof(s_pin.pin_hash));
    } else {
        (void)nvs_erase_key(h, "hash");
    }
    (void)nvs_set_u8(h, "retries", s_pin.retries);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void pin_load(void)
{
    nvs_handle_t h;
    s_pin.pin_set = false;
    s_pin.retries = PIN_DEFAULT_RETRIES;
    if (nvs_open(PIN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_pin.pin_hash);
    if (nvs_get_blob(h, "hash", s_pin.pin_hash, &len) == ESP_OK && len == sizeof(s_pin.pin_hash)) {
        s_pin.pin_set = true;
    }
    uint8_t retries = PIN_DEFAULT_RETRIES;
    if (nvs_get_u8(h, "retries", &retries) == ESP_OK && retries <= PIN_MAX_RETRIES) {
        s_pin.retries = retries;
    }
    nvs_close(h);
}

void fido_pin_init(void)
{
    if (s_pin.initialized) return;

    memset(&s_pin, 0, sizeof(s_pin));
    s_mutex = xSemaphoreCreateMutex();
    mbedtls_ecp_group_init(&s_pin.grp);
    mbedtls_mpi_init(&s_pin.d);
    mbedtls_ecp_point_init(&s_pin.Q);
    if (mbedtls_ecp_group_load(&s_pin.grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        ESP_LOGE(TAG, "Failed to load P-256 group");
    }
    pin_load();

    s_pin.initialized = true;
    ESP_LOGI(TAG, "PIN state loaded: set=%d retries=%u", (int)s_pin.pin_set, s_pin.retries);
}

bool fido_pin_is_set(void)
{
    fido_pin_init();
    pin_lock();
    bool set = s_pin.pin_set;
    pin_unlock();
    return set;
}

uint8_t fido_pin_retries_left(void)
{
    fido_pin_init();
    pin_lock();
    uint8_t r = s_pin.retries;
    pin_unlock();
    return r;
}

void fido_pin_reset_token(void)
{
    if (!s_pin.initialized) return;
    pin_lock();
    memset(s_pin.pin_token, 0, sizeof(s_pin.pin_token));
    s_pin.token_valid = false;
    s_pin.key_agreement_valid = false;
    pin_unlock();
}

void fido_pin_factory_reset(void)
{
    fido_pin_init();
    pin_lock();
    memset(s_pin.pin_hash, 0, sizeof(s_pin.pin_hash));
    s_pin.pin_set = false;
    s_pin.retries = PIN_DEFAULT_RETRIES;
    memset(s_pin.pin_token, 0, sizeof(s_pin.pin_token));
    s_pin.token_valid = false;
    s_pin.key_agreement_valid = false;

    nvs_handle_t h;
    if (nvs_open(PIN_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, "hash");
        (void)nvs_set_u8(h, "retries", s_pin.retries);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    pin_unlock();
    ESP_LOGI(TAG, "PIN factory reset");
}

/* ------------------------------------------------------------------------- */
/* clientPIN handling                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t protocol;
    uint8_t sub_cmd;
    bool has_key_agreement;
    uint8_t client_x[32];
    uint8_t client_y[32];
    uint8_t pin_uv_auth_param[32];
    size_t pin_uv_auth_len;
    uint8_t new_pin_enc[288];
    size_t new_pin_enc_len;
    uint8_t pin_hash_enc[48];
    size_t pin_hash_enc_len;
} client_pin_req_t;

static bool parse_client_pin(const uint8_t *payload, size_t len, client_pin_req_t *req)
{
    memset(req, 0, sizeof(*req));
    if (!payload || len < 2 || payload[0] != 0x06) return false;

    size_t off = 1;
    uint8_t major = 0;
    uint64_t pairs = 0;
    if (!cbor_head(payload, len, &off, &major, &pairs) || major != 5) return false;

    for (uint64_t i = 0; i < pairs && off < len; i++) {
        uint8_t km = 0;
        uint64_t key = 0;
        if (!cbor_head(payload, len, &off, &km, &key)) return false;
        if (km != 0) {
            if (!cbor_skip(payload, len, &off)) return false;
            continue;
        }
        switch (key) {
        case 1: {
            uint64_t v = 0;
            if (!cbor_uint(payload, len, &off, &v)) return false;
            req->protocol = (uint8_t)v;
            break;
        }
        case 2: {
            uint64_t v = 0;
            if (!cbor_uint(payload, len, &off, &v)) return false;
            req->sub_cmd = (uint8_t)v;
            break;
        }
        case 3:
            if (!cbor_cose_xy(payload, len, &off, req->client_x, req->client_y)) return false;
            req->has_key_agreement = true;
            break;
        case 4:
            if (!cbor_bstr(payload, len, &off, req->pin_uv_auth_param,
                           sizeof(req->pin_uv_auth_param), &req->pin_uv_auth_len)) return false;
            break;
        case 5:
            if (!cbor_bstr(payload, len, &off, req->new_pin_enc,
                           sizeof(req->new_pin_enc), &req->new_pin_enc_len)) return false;
            break;
        case 6:
            if (!cbor_bstr(payload, len, &off, req->pin_hash_enc,
                           sizeof(req->pin_hash_enc), &req->pin_hash_enc_len)) return false;
            break;
        default:
            if (!cbor_skip(payload, len, &off)) return false;
            break;
        }
    }
    return req->protocol == 1 || req->protocol == 2;
}

static size_t pin_error(uint8_t *out, uint8_t err)
{
    out[0] = err;
    return 1;
}

static size_t handle_get_retries(uint8_t *out, size_t max_out)
{
    if (max_out < 4) return 0;
    out[0] = 0x00;
    out[1] = 0xA1;
    out[2] = 0x03;
    out[3] = s_pin.retries;
    return 4;
}

static size_t handle_get_key_agreement(const client_pin_req_t *req,
                                       uint8_t *out, size_t max_out)
{
    if (max_out < 81) return 0;
    s_pin.protocol = req->protocol;
    s_pin.key_agreement_valid = false;
    s_pin.token_valid = false;
    memset(s_pin.pin_token, 0, sizeof(s_pin.pin_token));

    mbedtls_mpi_free(&s_pin.d);
    mbedtls_mpi_init(&s_pin.d);
    mbedtls_ecp_point_free(&s_pin.Q);
    mbedtls_ecp_point_init(&s_pin.Q);

    if (mbedtls_ecp_gen_privkey(&s_pin.grp, &s_pin.d, pin_rng, NULL) != 0 ||
        mbedtls_ecp_mul(&s_pin.grp, &s_pin.Q, &s_pin.d, &s_pin.grp.G, pin_rng, NULL) != 0) {
        ESP_LOGE(TAG, "Key agreement generation failed");
        return pin_error(out, 0x01);
    }

    uint8_t x[32], y[32];
    uint8_t pub[65];
    size_t pub_len = 0;
    if (mbedtls_ecp_point_write_binary(&s_pin.grp, &s_pin.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &pub_len, pub, sizeof(pub)) != 0 ||
        pub_len != sizeof(pub)) {
        return pin_error(out, 0x01);
    }
    memcpy(x, pub + 1, 32);
    memcpy(y, pub + 33, 32);
    s_pin.key_agreement_valid = true;

    size_t n = 0;
    out[n++] = 0x00;
    out[n++] = 0xA1; /* Map(1) */
    out[n++] = 0x01; /* keyAgreement */
    out[n++] = 0xA5; /* COSE_Key Map(5) */
    out[n++] = 0x01; out[n++] = 0x02;             /* 1: kty=EC2 */
    out[n++] = 0x03; out[n++] = 0x38; out[n++] = 0x18; /* 3: alg=-25 */
    out[n++] = 0x20; out[n++] = 0x01;             /* -1: crv=P-256 */
    out[n++] = 0x21; out[n++] = 0x58; out[n++] = 0x20;
    memcpy(out + n, x, 32); n += 32;
    out[n++] = 0x22; out[n++] = 0x58; out[n++] = 0x20;
    memcpy(out + n, y, 32); n += 32;
    return n;
}

static size_t handle_set_pin(const client_pin_req_t *req, uint8_t *out, size_t max_out)
{
    if (max_out < 2) return 0;
    if (!s_pin.key_agreement_valid || !req->has_key_agreement ||
        req->new_pin_enc_len == 0 || req->pin_uv_auth_len < 16) {
        return pin_error(out, 0x02);
    }
    if (!compute_shared_secret(req->protocol, req->client_x, req->client_y)) {
        return pin_error(out, 0x02);
    }
    if (!verify_pin_uv_auth(req->protocol, req->new_pin_enc, req->new_pin_enc_len,
                            req->pin_uv_auth_param, req->pin_uv_auth_len)) {
        return pin_error(out, CTAP2_ERR_PIN_AUTH_INVALID);
    }

    uint8_t new_hash[16];
    if (!decrypt_new_pin(req->new_pin_enc, req->new_pin_enc_len, new_hash)) {
        return pin_error(out, 0x02);
    }

    memcpy(s_pin.pin_hash, new_hash, sizeof(s_pin.pin_hash));
    s_pin.pin_set = true;
    s_pin.retries = PIN_DEFAULT_RETRIES;
    s_pin.token_valid = false;
    memset(s_pin.pin_token, 0, sizeof(s_pin.pin_token));
    pin_save();

    out[0] = 0x00;
    out[1] = 0xA0;
    ESP_LOGI(TAG, "Client PIN set");
    return 2;
}

static size_t handle_change_pin(const client_pin_req_t *req, uint8_t *out, size_t max_out)
{
    if (max_out < 2) return 0;
    if (!s_pin.key_agreement_valid || !req->has_key_agreement ||
        req->new_pin_enc_len == 0 || req->pin_hash_enc_len == 0 ||
        req->pin_uv_auth_len < 16) {
        return pin_error(out, 0x02);
    }
    if (!s_pin.pin_set) return pin_error(out, CTAP2_ERR_PIN_NOT_SET);
    if (s_pin.retries == 0) return pin_error(out, CTAP2_ERR_PIN_BLOCKED);

    if (!compute_shared_secret(req->protocol, req->client_x, req->client_y)) {
        return pin_error(out, 0x02);
    }

    uint8_t current_hash[16];
    if (!decrypt_pin_hash(req->pin_hash_enc, req->pin_hash_enc_len, current_hash) ||
        memcmp(current_hash, s_pin.pin_hash, sizeof(current_hash)) != 0) {
        if (s_pin.retries > 0) s_pin.retries--;
        pin_save();
        return pin_error(out, s_pin.retries == 0 ? CTAP2_ERR_PIN_BLOCKED : CTAP2_ERR_PIN_INVALID);
    }
    uint8_t mac_msg[352];
    if (req->new_pin_enc_len + req->pin_hash_enc_len > sizeof(mac_msg)) {
        return pin_error(out, 0x02);
    }
    memcpy(mac_msg, req->new_pin_enc, req->new_pin_enc_len);
    memcpy(mac_msg + req->new_pin_enc_len, req->pin_hash_enc, req->pin_hash_enc_len);
    if (!verify_pin_uv_auth(req->protocol, mac_msg,
                            req->new_pin_enc_len + req->pin_hash_enc_len,
                            req->pin_uv_auth_param, req->pin_uv_auth_len)) {
        return pin_error(out, CTAP2_ERR_PIN_AUTH_INVALID);
    }

    uint8_t new_hash[16];
    if (!decrypt_new_pin(req->new_pin_enc, req->new_pin_enc_len, new_hash)) {
        return pin_error(out, 0x02);
    }
    memcpy(s_pin.pin_hash, new_hash, sizeof(s_pin.pin_hash));
    s_pin.retries = PIN_DEFAULT_RETRIES;
    s_pin.token_valid = false;
    memset(s_pin.pin_token, 0, sizeof(s_pin.pin_token));
    pin_save();

    out[0] = 0x00;
    out[1] = 0xA0;
    ESP_LOGI(TAG, "Client PIN changed");
    return 2;
}

static size_t handle_get_pin_token(const client_pin_req_t *req, uint8_t *out, size_t max_out)
{
    if (max_out < 8) return 0;
    if (!s_pin.key_agreement_valid || !req->has_key_agreement || req->pin_hash_enc_len == 0) {
        return pin_error(out, 0x02);
    }
    if (!s_pin.pin_set) return pin_error(out, CTAP2_ERR_PIN_NOT_SET);
    if (s_pin.retries == 0) return pin_error(out, CTAP2_ERR_PIN_BLOCKED);

    if (!compute_shared_secret(req->protocol, req->client_x, req->client_y)) {
        return pin_error(out, 0x02);
    }

    uint8_t current_hash[16];
    if (!decrypt_pin_hash(req->pin_hash_enc, req->pin_hash_enc_len, current_hash) ||
        memcmp(current_hash, s_pin.pin_hash, sizeof(current_hash)) != 0) {
        if (s_pin.retries > 0) s_pin.retries--;
        pin_save();
        ESP_LOGW(TAG, "Client PIN invalid, retries=%u", s_pin.retries);
        return pin_error(out, s_pin.retries == 0 ? CTAP2_ERR_PIN_BLOCKED : CTAP2_ERR_PIN_INVALID);
    }

    s_pin.retries = PIN_DEFAULT_RETRIES;
    esp_fill_random(s_pin.pin_token, sizeof(s_pin.pin_token));
    s_pin.token_valid = true;
    s_pin.token_protocol = req->protocol;
    pin_save();

    uint8_t enc[64];
    size_t enc_len = 0;
    if (!encrypt_pin_token(s_pin.pin_token, enc, &enc_len)) {
        return pin_error(out, 0x01);
    }

    size_t n = 0;
    out[n++] = 0x00;
    out[n++] = 0xA1;
    out[n++] = 0x02;
    out[n++] = 0x58;
    out[n++] = (uint8_t)enc_len;
    memcpy(out + n, enc, enc_len);
    n += enc_len;
    ESP_LOGI(TAG, "Client PIN token issued");
    return n;
}

size_t fido_pin_handle_client_pin(const uint8_t *payload, size_t len,
                                  uint8_t *out, size_t max_out)
{
    fido_pin_init();
    if (!out || max_out < 8) return 0;

    client_pin_req_t req;
    if (!parse_client_pin(payload, len, &req)) {
        return pin_error(out, CTAP2_ERR_INVALID_CBOR);
    }

    pin_lock();
    size_t n = 0;
    switch (req.sub_cmd) {
    case 0x01:
        n = handle_get_retries(out, max_out);
        break;
    case 0x02:
        n = handle_get_key_agreement(&req, out, max_out);
        break;
    case 0x03:
        n = handle_set_pin(&req, out, max_out);
        break;
    case 0x04:
        n = handle_change_pin(&req, out, max_out);
        break;
    case 0x05:
    case 0x09: /* getPinUvAuthTokenUsingPinWithPermissions */
        n = handle_get_pin_token(&req, out, max_out);
        break;
    default:
        n = pin_error(out, CTAP2_ERR_UNSUPPORTED_OPTION);
        break;
    }
    pin_unlock();
    return n;
}

bool fido_pin_verify_pin_auth(const uint8_t client_data_hash[32],
                              const uint8_t *pin_uv_auth_param,
                              size_t pin_uv_auth_param_len)
{
    if (!client_data_hash || !pin_uv_auth_param || pin_uv_auth_param_len < 16) return false;
    fido_pin_init();
    pin_lock();
    bool ok = false;
    if (s_pin.pin_set && s_pin.token_valid) {
        uint8_t mac[32];
        hmac_sha256(s_pin.pin_token, sizeof(s_pin.pin_token),
                    client_data_hash, 32, mac);
        if (s_pin.token_protocol == 2) {
            ok = pin_uv_auth_param_len >= 32 && memcmp(mac, pin_uv_auth_param, 32) == 0;
        } else {
            ok = pin_uv_auth_param_len >= 16 && memcmp(mac, pin_uv_auth_param, 16) == 0;
        }
    }
    pin_unlock();
    return ok;
}

#else /* !ESP_PLATFORM */

void fido_pin_init(void) {}
bool fido_pin_is_set(void) { return false; }
uint8_t fido_pin_retries_left(void) { return 8; }

size_t fido_pin_handle_client_pin(const uint8_t *payload, size_t len,
                                  uint8_t *out, size_t max_out)
{
    (void)payload; (void)len;
    if (out && max_out > 0) out[0] = 0x2B;
    return (out && max_out > 0) ? 1 : 0;
}

bool fido_pin_verify_pin_auth(const uint8_t client_data_hash[32],
                              const uint8_t *pin_uv_auth_param,
                              size_t pin_uv_auth_param_len)
{
    (void)client_data_hash; (void)pin_uv_auth_param; (void)pin_uv_auth_param_len;
    return false;
}

void fido_pin_reset_token(void) {}
void fido_pin_factory_reset(void) {}

#endif /* ESP_PLATFORM */
