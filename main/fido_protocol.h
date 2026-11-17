/* 协议与应用命令定义：应用管理枚举不是 CTAP 操作码，不能混用两种编号。
 * 解析必须检查报文边界、哈希长度及嵌套深度；构建器的长度和签名字节必须一致。
 */
#ifndef FIDO_PROTOCOL_H
#define FIDO_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIDO_RP_ID_MAX_LEN      64
#define FIDO_USER_MAX_LEN       64
#define FIDO_CHALLENGE_MAX_LEN  64
#define FIDO_DEFAULT_TIMEOUT_MS 15000
#define FIDO_PIN_UV_AUTH_MAX_LEN 32

typedef struct {
    uint8_t pin_uv_auth_param[FIDO_PIN_UV_AUTH_MAX_LEN];
    size_t  pin_uv_auth_param_len;
    uint8_t pin_uv_auth_protocol;
    bool    uv_requested;
    bool    rk_requested;
} fido_auth_params_t;

typedef enum {
    FIDO_REQ_AUTHENTICATION = 0,
    FIDO_REQ_REGISTRATION
} fido_request_type_t;

typedef enum {
    FIDO_STATE_IDLE = 0,
    FIDO_STATE_UP_PENDING,
    FIDO_STATE_APPROVED,
    FIDO_STATE_REJECTED,
    FIDO_STATE_TIMEOUT
} fido_state_t;

typedef struct {
    fido_request_type_t type;
    char     rp_id[FIDO_RP_ID_MAX_LEN];
    char     user_name[FIDO_USER_MAX_LEN];
    char     challenge[FIDO_CHALLENGE_MAX_LEN];
    uint64_t start_ms;
    uint32_t timeout_ms;
} fido_request_t;

typedef struct {
    fido_state_t   state;
    fido_request_t current_req;
} fido_session_t;

/* Serial / BLE command types */
typedef enum {
    FIDO_CMD_NONE = 0,
    FIDO_CMD_AUTH_REQ,
    FIDO_CMD_REG_REQ,
    FIDO_CMD_CANCEL,
    FIDO_CMD_SYNC_TIME,
    FIDO_CMD_ADD_TOTP,
    FIDO_CMD_DEL_TOTP,
    FIDO_CMD_LIST_TOTP,
    FIDO_CMD_GET_STATUS,
    FIDO_CMD_SET_BRIGHTNESS,
    FIDO_CMD_SET_SOUND,
    FIDO_CMD_RESET,
    FIDO_CMD_EDIT_TOTP,
    FIDO_CMD_SLEEP,
    FIDO_CMD_INVALID
} fido_cmd_type_t;

typedef struct {
    fido_cmd_type_t type;
    char     param1[64];
    char     param2[64];
    char     param3[128];
    uint64_t num_param;
    uint32_t request_id;
    uint32_t account_revision;
    uint8_t otp_algorithm;
    uint8_t otp_digits;
    uint8_t otp_period;
    bool time_reference_present;
    bool time_reference_quality;
    uint64_t time_reference_id;
    uint64_t time_reference_utc_us;
    int64_t time_reference_hrt_us;
    uint64_t time_reference_uncertainty_us;
} fido_parsed_cmd_t;

void fido_session_init(fido_session_t *session);

/**
 * @brief Start a FIDO2 User Presence (UP) request.
 */
bool fido_session_start_req(fido_session_t *session,
                            fido_request_type_t type,
                            const char *rp_id,
                            const char *user_name,
                            const char *challenge,
                            uint64_t current_ms,
                            uint32_t timeout_ms);

/**
 * @brief User presses OK button to approve.
 */
bool fido_session_approve(fido_session_t *session);

/**
 * @brief User or system rejects/cancels request.
 */
bool fido_session_reject(fido_session_t *session);

/**
 * @brief Periodic tick to handle timeout.
 * @return true if state transitioned to TIMEOUT during this tick.
 */
bool fido_session_tick(fido_session_t *session, uint64_t current_ms, uint32_t *remaining_sec);

/**
 * @brief Reset session back to IDLE.
 */
void fido_session_reset(fido_session_t *session);

/**
 * @brief Parse CTAP2 authenticatorMakeCredential (0x01) CBOR request.
 */
bool ctap2_parse_make_credential(const uint8_t *payload, size_t len,
                                 char *out_rpid, size_t rpid_max,
                                 uint8_t out_cdh[32],
                                 char *out_username, size_t user_max,
                                 uint8_t *out_userid, size_t *out_userid_len,
                                 fido_auth_params_t *out_auth);

/**
 * @brief Parse CTAP2 authenticatorGetAssertion (0x02) CBOR request.
 */
bool ctap2_parse_get_assertion(const uint8_t *payload, size_t len,
                               char *out_rpid, size_t rpid_max,
                               uint8_t out_cdh[32],
                               uint8_t *out_allow_id, size_t *out_allow_id_len,
                               fido_auth_params_t *out_auth);

/**
 * @brief Build canonical CBOR response for authenticatorMakeCredential.
 * @return length of response written to out_buf, or 0 on error.
 */
size_t ctap2_build_make_credential_resp(const uint8_t rpid_hash[32],
                                        const uint8_t aaguid[16],
                                        const uint8_t *cred_id,
                                        size_t cred_id_len,
                                        const uint8_t qx[32],
                                        const uint8_t qy[32],
                                        uint32_t sign_count,
                                        uint8_t flags,
                                        uint8_t *out_buf,
                                        size_t max_buf);

/**
 * @brief Build canonical CBOR response for authenticatorGetAssertion.
 * @return length of response written to out_buf, or 0 on error.
 */
size_t ctap2_build_get_assertion_resp(const uint8_t rpid_hash[32],
                                      const uint8_t *cred_id,
                                      size_t cred_id_len,
                                      uint8_t flags,
                                      uint32_t sign_count,
                                      const uint8_t *sig_der,
                                      size_t sig_len,
                                      const uint8_t *user_id,
                                      size_t user_id_len,
                                      uint8_t *out_buf,
                                      size_t max_buf);


/**
 * @brief Parse a text command line received over Serial or BLE.
 */
fido_parsed_cmd_t fido_parse_command(const char *cmd_line);

#ifdef __cplusplus
}
#endif

#endif /* FIDO_PROTOCOL_H */
