/* 传输无关的命令与 CBOR 处理：长度、容器数量和嵌套深度均受输入缓冲约束。
 * 解析失败必须向上传递，不允许截断 clientDataHash 后仍把报文视为有效。
 */
#include "fido_protocol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

void fido_session_init(fido_session_t *session)
{
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->state = FIDO_STATE_IDLE;
}

bool fido_session_start_req(fido_session_t *session,
                            fido_request_type_t type,
                            const char *rp_id,
                            const char *user_name,
                            const char *challenge,
                            uint64_t current_ms,
                            uint32_t timeout_ms)
{
    if (!session || !rp_id) return false;

    session->current_req.type = type;
    snprintf(session->current_req.rp_id, sizeof(session->current_req.rp_id), "%s", rp_id);
    snprintf(session->current_req.user_name, sizeof(session->current_req.user_name), "%s", user_name ? user_name : "");
    snprintf(session->current_req.challenge, sizeof(session->current_req.challenge), "%s", challenge ? challenge : "");
    session->current_req.start_ms = current_ms;
    session->current_req.timeout_ms = timeout_ms > 0 ? timeout_ms : FIDO_DEFAULT_TIMEOUT_MS;

    session->state = FIDO_STATE_UP_PENDING;
    return true;
}

bool fido_session_approve(fido_session_t *session)
{
    if (!session || session->state != FIDO_STATE_UP_PENDING) {
        return false;
    }
    session->state = FIDO_STATE_APPROVED;
    return true;
}

bool fido_session_reject(fido_session_t *session)
{
    if (!session || session->state != FIDO_STATE_UP_PENDING) {
        return false;
    }
    session->state = FIDO_STATE_REJECTED;
    return true;
}

bool fido_session_tick(fido_session_t *session, uint64_t current_ms, uint32_t *remaining_sec)
{
    if (!session) return false;

    if (session->state == FIDO_STATE_UP_PENDING) {
        uint64_t elapsed = (current_ms >= session->current_req.start_ms) ?
                           (current_ms - session->current_req.start_ms) : 0;

        if (elapsed >= session->current_req.timeout_ms) {
            session->state = FIDO_STATE_TIMEOUT;
            if (remaining_sec) *remaining_sec = 0;
            return true; /* Transitioned to timeout */
        }

        if (remaining_sec) {
            *remaining_sec = (uint32_t)((session->current_req.timeout_ms - elapsed + 999) / 1000);
        }
        return false;
    }

    if (remaining_sec) *remaining_sec = 0;
    return false;
}

void fido_session_reset(fido_session_t *session)
{
    if (!session) return;
    session->state = FIDO_STATE_IDLE;
    memset(&session->current_req, 0, sizeof(session->current_req));
}

static bool parse_decimal(const char *text, uint64_t *value)
{
    if (!text || !*text) return false;
    uint64_t parsed = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        const unsigned digit = (unsigned)(*text - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

fido_parsed_cmd_t fido_parse_command(const char *cmd_line)
{
    fido_parsed_cmd_t res = {0};
    res.otp_digits = 6;
    res.otp_period = 30;
    if (!cmd_line || !*cmd_line) return res;

    char buf[256];
    res.type = FIDO_CMD_INVALID;
    if (strlen(cmd_line) >= sizeof(buf)) return res;
    memcpy(buf, cmd_line, strlen(cmd_line) + 1);
    char *tokens[4];
    size_t count = 0;
    char *cursor = buf;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        if (count == sizeof(tokens) / sizeof(tokens[0])) return res;
        tokens[count++] = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor) *cursor++ = 0;
    }
    if (!count) { res.type = FIDO_CMD_NONE; return res; }

    const char *cmd = tokens[0];
    fido_cmd_type_t type = FIDO_CMD_INVALID;
    if (!strcmp(cmd, "AUTH_REQ")) type = FIDO_CMD_AUTH_REQ;
    else if (!strcmp(cmd, "REG_REQ")) type = FIDO_CMD_REG_REQ;
    else if (!strcmp(cmd, "ADD_TOTP")) type = FIDO_CMD_ADD_TOTP;
    if (type != FIDO_CMD_INVALID) {
        if (count != 4 || strlen(tokens[1]) >= sizeof(res.param1) ||
            strlen(tokens[2]) >= sizeof(res.param2) || strlen(tokens[3]) >= sizeof(res.param3)) return res;
        strcpy(res.param1, tokens[1]);
        strcpy(res.param2, tokens[2]);
        strcpy(res.param3, tokens[3]);
        res.type = type;
        return res;
    }

    if (!strcmp(cmd, "SYNC_TIME")) type = FIDO_CMD_SYNC_TIME;
    else if (!strcmp(cmd, "DEL_TOTP")) type = FIDO_CMD_DEL_TOTP;
    else if (!strcmp(cmd, "SET_BRIGHTNESS")) type = FIDO_CMD_SET_BRIGHTNESS;
    else if (!strcmp(cmd, "SET_SOUND")) type = FIDO_CMD_SET_SOUND;
    if (type != FIDO_CMD_INVALID) {
        if (count != 2 || !parse_decimal(tokens[1], &res.num_param)) return res;
        if (type == FIDO_CMD_SET_SOUND && res.num_param > 1) return res;
        if (type == FIDO_CMD_SET_BRIGHTNESS && (res.num_param < 10 || res.num_param > 100)) return res;
        res.type = type;
        return res;
    }
    if (count != 1) return res;
    if (!strcmp(cmd, "CANCEL")) res.type = FIDO_CMD_CANCEL;
    else if (!strcmp(cmd, "LIST_TOTP")) res.type = FIDO_CMD_LIST_TOTP;
    else if (!strcmp(cmd, "GET_STATUS")) res.type = FIDO_CMD_GET_STATUS;
    else if (!strcmp(cmd, "WIPE_ALL")) res.type = FIDO_CMD_RESET;
    return res;
}

/* Helper to read CBOR type & length */
static bool cbor_get_type_len(const uint8_t *buf, size_t max_len, size_t *offset,
                              uint8_t *out_major, size_t *out_len)
{
    if (*offset >= max_len) return false;
    uint8_t b = buf[*offset];
    *out_major = b >> 5;
    uint8_t add = b & 0x1F;
    (*offset)++;
    if (add < 24) {
        *out_len = add;
        return true;
    } else if (add == 24) {
        if (*offset + 1 > max_len) return false;
        *out_len = buf[*offset];
        (*offset)++;
        return true;
    } else if (add == 25) {
        if (*offset + 2 > max_len) return false;
        *out_len = ((size_t)buf[*offset] << 8) | buf[*offset + 1];
        *offset += 2;
        return true;
    } else if (add == 26) {
        if (*offset + 4 > max_len) return false;
        *out_len = ((size_t)buf[*offset] << 24) | ((size_t)buf[*offset + 1] << 16) |
                   ((size_t)buf[*offset + 2] << 8) | buf[*offset + 3];
        *offset += 4;
        return true;
    }
    return false;
}

static bool cbor_skip_item_depth(const uint8_t *buf, size_t max_len, size_t *offset, unsigned depth)
{
    if (depth >= 16 || *offset >= max_len) return false;
    uint8_t major = 0;
    size_t val = 0;
    if (!cbor_get_type_len(buf, max_len, offset, &major, &val)) return false;
    switch (major) {
    case 0:
    case 1:
    case 7:
        return true;
    case 2:
    case 3:
        if (val > max_len - *offset) return false;
        *offset += val;
        return true;
    case 4:
        if (val > max_len - *offset) return false;
        for (size_t i = 0; i < val; i++) {
            if (!cbor_skip_item_depth(buf, max_len, offset, depth + 1)) return false;
        }
        return true;
    case 5:
        if (val > (max_len - *offset) / 2) return false;
        for (size_t i = 0; i < val; i++) {
            if (!cbor_skip_item_depth(buf, max_len, offset, depth + 1) ||
                !cbor_skip_item_depth(buf, max_len, offset, depth + 1)) return false;
        }
        return true;
    default:
        return false;
    }
}

static bool cbor_skip_item(const uint8_t *buf, size_t max_len, size_t *offset)
{
    return cbor_skip_item_depth(buf, max_len, offset, 0);
}

static bool cbor_read_text_str(const uint8_t *buf, size_t max_len, size_t *offset,
                               char *out_str, size_t max_out)
{
    uint8_t major = 0;
    size_t s_len = 0;
    if (!cbor_get_type_len(buf, max_len, offset, &major, &s_len) || major != 3) {
        return false;
    }
    if (s_len > max_len - *offset ||
        (out_str && (max_out == 0 || s_len >= max_out ||
                     memchr(buf + *offset, 0, s_len)))) return false;
    if (out_str && max_out > 0) {
        size_t cp = s_len;
        memcpy(out_str, buf + *offset, cp);
        out_str[cp] = '\0';
    }
    *offset += s_len;
    return true;
}

// Display labels may be shortened; RP identities must use the strict reader.
static bool cbor_read_label(const uint8_t *buf, size_t len, size_t *off,
                            char *out, size_t capacity)
{
    uint8_t major;
    size_t size;
    if (!cbor_get_type_len(buf, len, off, &major, &size) || major != 3 ||
        size > len - *off || memchr(buf + *off, 0, size)) return false;
    if (out && capacity) {
        size_t copy = size < capacity - 1 ? size : capacity - 1;
        if (copy < size) {
            while (copy && (buf[*off + copy] & 0xc0) == 0x80) --copy;
        }
        memcpy(out, buf + *off, copy);
        out[copy] = 0;
    }
    *off += size;
    return true;
}

static bool cbor_read_byte_str(const uint8_t *buf, size_t max_len, size_t *offset,
                               uint8_t *out_buf, size_t max_out, size_t *out_len)
{
    uint8_t major = 0;
    size_t b_len = 0;
    if (!cbor_get_type_len(buf, max_len, offset, &major, &b_len) || major != 2) {
        return false;
    }
    if (b_len > max_len - *offset || b_len > max_out) return false;
    if (out_buf && b_len > 0) memcpy(out_buf, buf + *offset, b_len);
    if (out_len) *out_len = out_buf ? b_len : 0;
    *offset += b_len;
    return true;
}

static bool cbor_read_uint(const uint8_t *buf, size_t max_len, size_t *offset, uint64_t *out)
{
    uint8_t major = 0;
    size_t val = 0;
    if (!cbor_get_type_len(buf, max_len, offset, &major, &val) || major != 0) {
        return false;
    }
    *out = val;
    return true;
}

static bool cbor_read_bool(const uint8_t *buf, size_t max_len, size_t *offset, bool *out)
{
    uint8_t major = 0;
    size_t val = 0;
    if (!cbor_get_type_len(buf, max_len, offset, &major, &val) || major != 7) {
        return false;
    }
    if (val == 20) {
        *out = false;
        return true;
    }
    if (val == 21) {
        *out = true;
        return true;
    }
    return false;
}

static bool cbor_parse_options(const uint8_t *payload, size_t len, size_t *off,
                               fido_auth_params_t *out_auth)
{
    uint8_t major = 0;
    size_t pairs = 0;
    if (!cbor_get_type_len(payload, len, off, &major, &pairs) || major != 5) return false;
    for (size_t i = 0; i < pairs; i++) {
        char name[16] = {0};
        if (!cbor_read_text_str(payload, len, off, name, sizeof(name))) return false;
        if (!strcmp(name, "uv")) {
            if (!cbor_read_bool(payload, len, off, &out_auth->uv_requested)) return false;
        } else if (!strcmp(name, "rk")) {
            if (!cbor_read_bool(payload, len, off, &out_auth->rk_requested)) return false;
        } else if (!cbor_skip_item(payload, len, off)) return false;
    }
    return true;
}

bool ctap2_parse_make_credential(const uint8_t *payload, size_t len,
                                 char *out_rpid, size_t rpid_max,
                                 uint8_t out_cdh[32],
                                 char *out_username, size_t user_max,
                                 uint8_t *out_userid, size_t *out_userid_len,
                                 fido_auth_params_t *out_auth)
{
    if (!payload || len == 0) return false;
    if (out_rpid && rpid_max > 0) out_rpid[0] = '\0';
    if (out_username && user_max > 0) out_username[0] = '\0';
    if (out_cdh) memset(out_cdh, 0, 32);
    if (out_userid_len) *out_userid_len = 0;
    if (out_auth) memset(out_auth, 0, sizeof(*out_auth));
    bool cdh_seen = false;

    size_t off = 0;
    if (payload[0] == 0x01) off = 1; /* Skip command code */
    size_t end = off;
    if (!cbor_skip_item(payload, len, &end) || end != len) return false;

    uint8_t top_major = 0;
    size_t num_pairs = 0;
    if (!cbor_get_type_len(payload, len, &off, &top_major, &num_pairs) || top_major != 5) {
        return false;
    }

    for (size_t p = 0; p < num_pairs && off < len; p++) {
        uint8_t k_major = 0;
        size_t key_num = 0;
        if (!cbor_get_type_len(payload, len, &off, &k_major, &key_num) || k_major != 0) {
            return false;
        }

        if (key_num == 1) {
            /* Key 1: clientDataHash */
            size_t cdh_len = 0;
            if (!cbor_read_byte_str(payload, len, &off, out_cdh, 32, &cdh_len) ||
                cdh_len != 32) {
                return false;
            }
            cdh_seen = true;
        } else if (key_num == 2) {
            /* Key 2: rp Map */
            uint8_t rp_major = 0;
            size_t rp_pairs = 0;
            if (cbor_get_type_len(payload, len, &off, &rp_major, &rp_pairs) && rp_major == 5) {
                for (size_t r = 0; r < rp_pairs && off < len; r++) {
                    char field_name[32] = {0};
                    if (cbor_read_text_str(payload, len, &off, field_name, sizeof(field_name))) {
                        if (strcmp(field_name, "id") == 0) {
                            if (!cbor_read_text_str(payload, len, &off, out_rpid, rpid_max)) return false;
                        } else {
                            if (!cbor_skip_item(payload, len, &off)) return false;
                        }
                    } else {
                        return false;
                    }
                }
            } else {
                return false;
            }
        } else if (key_num == 3) {
            /* Key 3: user Map */
            uint8_t u_major = 0;
            size_t u_pairs = 0;
            if (cbor_get_type_len(payload, len, &off, &u_major, &u_pairs) && u_major == 5) {
                for (size_t u = 0; u < u_pairs && off < len; u++) {
                    char field_name[32] = {0};
                    if (cbor_read_text_str(payload, len, &off, field_name, sizeof(field_name))) {
                        if (strcmp(field_name, "name") == 0 ||
                            (strcmp(field_name, "displayName") == 0 && out_username && out_username[0] == '\0')) {
                            if (!cbor_read_label(payload, len, &off, out_username, user_max)) return false;
                        } else if (strcmp(field_name, "id") == 0) {
                            if (!cbor_read_byte_str(payload, len, &off, out_userid, 64, out_userid_len)) return false;
                        } else {
                            if (!cbor_skip_item(payload, len, &off)) return false;
                        }
                    } else {
                        return false;
                    }
                }
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else if (key_num == 7) {
            /* Key 7: options map { "uv": bool, "rk": bool } */
            if (out_auth) {
                if (!cbor_parse_options(payload, len, &off, out_auth)) return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else if (key_num == 8) {
            /* Key 8: pinUvAuthParam */
            if (out_auth) {
                if (!cbor_read_byte_str(payload, len, &off,
                                   out_auth->pin_uv_auth_param,
                                   sizeof(out_auth->pin_uv_auth_param),
                                   &out_auth->pin_uv_auth_param_len)) return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else if (key_num == 9) {
            /* Key 9: pinUvAuthProtocol */
            if (out_auth) {
                uint64_t proto = 0;
                if (cbor_read_uint(payload, len, &off, &proto) && proto <= UINT8_MAX) {
                    out_auth->pin_uv_auth_protocol = (uint8_t)proto;
                } else return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else {
            /* Other keys: skip value */
            if (!cbor_skip_item(payload, len, &off)) return false;
        }
    }

    if (!cdh_seen) return false;
    if (!out_rpid || rpid_max == 0 || out_rpid[0] == '\0') return false;
    return true;
}

bool ctap2_parse_get_assertion(const uint8_t *payload, size_t len,
                               char *out_rpid, size_t rpid_max,
                               uint8_t out_cdh[32],
                               uint8_t *out_allow_id, size_t *out_allow_id_len,
                               fido_auth_params_t *out_auth)
{
    if (!payload || len == 0) return false;
    if (out_rpid && rpid_max > 0) out_rpid[0] = '\0';
    if (out_cdh) memset(out_cdh, 0, 32);
    if (out_allow_id_len) *out_allow_id_len = 0;
    if (out_auth) memset(out_auth, 0, sizeof(*out_auth));
    bool cdh_seen = false;

    size_t off = 0;
    if (payload[0] == 0x02) off = 1; /* Skip command code */
    size_t end = off;
    if (!cbor_skip_item(payload, len, &end) || end != len) return false;

    uint8_t top_major = 0;
    size_t num_pairs = 0;
    if (!cbor_get_type_len(payload, len, &off, &top_major, &num_pairs) || top_major != 5) {
        return false;
    }

    for (size_t p = 0; p < num_pairs && off < len; p++) {
        uint8_t k_major = 0;
        size_t key_num = 0;
        if (!cbor_get_type_len(payload, len, &off, &k_major, &key_num) || k_major != 0) {
            return false;
        }

        if (key_num == 1) {
            /* Key 1: rpId directly as text string */
            if (!cbor_read_text_str(payload, len, &off, out_rpid, rpid_max)) return false;
        } else if (key_num == 2) {
            /* Key 2: clientDataHash as byte string */
            size_t cdh_len = 0;
            if (!cbor_read_byte_str(payload, len, &off, out_cdh, 32, &cdh_len) ||
                cdh_len != 32) {
                return false;
            }
            cdh_seen = true;
        } else if (key_num == 3) {
            /* Key 3: allowList Array */
            const size_t list_start = off;
            uint8_t arr_major = 0;
            size_t arr_len = 0;
            if (cbor_get_type_len(payload, len, &off, &arr_major, &arr_len) && arr_major == 4) {
                for (size_t a = 0; a < arr_len && off < len; a++) {
                    uint8_t desc_major = 0;
                    size_t desc_pairs = 0;
                    if (cbor_get_type_len(payload, len, &off, &desc_major, &desc_pairs) && desc_major == 5) {
                        for (size_t d = 0; d < desc_pairs && off < len; d++) {
                            char field[32] = {0};
                            if (cbor_read_text_str(payload, len, &off, field, sizeof(field))) {
                                if (strcmp(field, "id") == 0 && out_allow_id && out_allow_id_len) {
                                    const size_t id_start = off;
                                    size_t ignored = 0;
                                    if (!cbor_read_byte_str(payload, len, &off, NULL, SIZE_MAX, &ignored)) return false;
                                    size_t header = id_start, id_len = 0;
                                    uint8_t id_major = 0;
                                    if (!cbor_get_type_len(payload, len, &header, &id_major, &id_len)) return false;
                                    *out_allow_id_len = id_len <= 64 ? id_len : 0;
                                    if (*out_allow_id_len) memcpy(out_allow_id, payload + header, id_len);
                                } else {
                                    if (!cbor_skip_item(payload, len, &off)) return false;
                                }
                            } else {
                                return false;
                            }
                        }
                    } else {
                        return false;
                    }
                }
            } else {
                return false;
            }
            if (out_auth) {
                out_auth->allow_list = payload + list_start;
                out_auth->allow_list_len = off - list_start;
            }
        } else if (key_num == 5) {
            /* Key 5: options map { "uv": bool } */
            if (out_auth) {
                if (!cbor_parse_options(payload, len, &off, out_auth)) return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else if (key_num == 6) {
            /* Key 6: pinUvAuthParam */
            if (out_auth) {
                if (!cbor_read_byte_str(payload, len, &off,
                                   out_auth->pin_uv_auth_param,
                                   sizeof(out_auth->pin_uv_auth_param),
                                   &out_auth->pin_uv_auth_param_len)) return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else if (key_num == 7) {
            /* Key 7: pinUvAuthProtocol */
            if (out_auth) {
                uint64_t proto = 0;
                if (cbor_read_uint(payload, len, &off, &proto) && proto <= UINT8_MAX) {
                    out_auth->pin_uv_auth_protocol = (uint8_t)proto;
                } else return false;
            } else {
                if (!cbor_skip_item(payload, len, &off)) return false;
            }
        } else {
            if (!cbor_skip_item(payload, len, &off)) return false;
        }
    }

    if (!cdh_seen) return false;
    if (!out_rpid || rpid_max == 0 || out_rpid[0] == '\0') return false;
    return true;
}

bool ctap2_allow_list_contains(const uint8_t *list, size_t len,
                               const uint8_t *credential, size_t credential_len)
{
    if (!list || !credential || !credential_len) return false;
    size_t end = 0;
    if (!cbor_skip_item(list, len, &end) || end != len) return false;
    size_t off = 0, count = 0;
    uint8_t major;
    if (!cbor_get_type_len(list, len, &off, &major, &count) || major != 4) return false;
    bool matched = false;
    for (size_t i = 0; i < count; ++i) {
        size_t pairs = 0;
        if (!cbor_get_type_len(list, len, &off, &major, &pairs) || major != 5) return false;
        bool id_seen = false;
        for (size_t j = 0; j < pairs; ++j) {
            char field[32];
            if (!cbor_read_text_str(list, len, &off, field, sizeof(field))) return false;
            if (!strcmp(field, "id")) {
                if (id_seen) return false;
                id_seen = true;
                size_t id_len = 0;
                if (!cbor_get_type_len(list, len, &off, &major, &id_len) ||
                    major != 2 || id_len > len - off) return false;
                if (id_len == credential_len && !memcmp(list + off, credential, id_len)) matched = true;
                off += id_len;
            } else if (!cbor_skip_item(list, len, &off)) return false;
        }
        if (!id_seen) return false;
    }
    return matched;
}

size_t ctap2_build_make_credential_resp(const uint8_t rpid_hash[32],
                                        const uint8_t aaguid[16],
                                        const uint8_t *cred_id,
                                        size_t cred_id_len,
                                        const uint8_t qx[32],
                                        const uint8_t qy[32],
                                        uint32_t sign_count,
                                        uint8_t flags,
                                        uint8_t *out_buf,
                                        size_t max_buf)
{
    if (!rpid_hash || !aaguid || !cred_id || cred_id_len == 0 || cred_id_len > 123 ||
        !qx || !qy || !out_buf || max_buf < 300) {
        return 0;
    }

    /* 1. Assemble authData */
    uint8_t auth_data[256];
    size_t ad = 0;

    /* rpIdHash (32 bytes) */
    memcpy(auth_data + ad, rpid_hash, 32); ad += 32;

    /* flags: UP/UV/AT as requested by the verified pinUvAuthParam. */
    auth_data[ad++] = flags;

    /* signCount (4 bytes big-endian) */
    auth_data[ad++] = (uint8_t)((sign_count >> 24) & 0xFF);
    auth_data[ad++] = (uint8_t)((sign_count >> 16) & 0xFF);
    auth_data[ad++] = (uint8_t)((sign_count >> 8) & 0xFF);
    auth_data[ad++] = (uint8_t)(sign_count & 0xFF);

    /* aaguid (16 bytes) */
    memcpy(auth_data + ad, aaguid, 16); ad += 16;

    /* credIdLen + credId */
    auth_data[ad++] = (uint8_t)((cred_id_len >> 8) & 0xFF);
    auth_data[ad++] = (uint8_t)(cred_id_len & 0xFF);
    memcpy(auth_data + ad, cred_id, cred_id_len); ad += cred_id_len;

    /* COSE key (Canonical order: 1:2, 3:-7, -1:1, -2:qx, -3:qy) */
    auth_data[ad++] = 0xA5; /* Map(5) */
    auth_data[ad++] = 0x01; auth_data[ad++] = 0x02; /* 1: 2 (EC2) */
    auth_data[ad++] = 0x03; auth_data[ad++] = 0x26; /* 3: -7 (ES256) */
    auth_data[ad++] = 0x20; auth_data[ad++] = 0x01; /* -1: 1 (P-256) */
    auth_data[ad++] = 0x21; auth_data[ad++] = 0x58; auth_data[ad++] = 0x20;
    memcpy(auth_data + ad, qx, 32); ad += 32;
    auth_data[ad++] = 0x22; auth_data[ad++] = 0x58; auth_data[ad++] = 0x20;
    memcpy(auth_data + ad, qy, 32); ad += 32;
    size_t auth_data_len = ad;

    /* 2. Assemble CBOR response:
       0x00 status + Map(3) { 1: authData, 2: "none", 3: {} } */
    size_t cr = 0;
    out_buf[cr++] = 0x00; /* CTAP1_ERR_SUCCESS */
    out_buf[cr++] = 0xA3; /* Map(3) */

    /* Key 1: authData -> byte string */
    out_buf[cr++] = 0x01;
    out_buf[cr++] = 0x58;
    out_buf[cr++] = (uint8_t)auth_data_len;
    memcpy(out_buf + cr, auth_data, auth_data_len);
    cr += auth_data_len;

    /* Key 2: "fmt" -> "none" */
    out_buf[cr++] = 0x02;
    out_buf[cr++] = 0x64;
    out_buf[cr++] = 'n'; out_buf[cr++] = 'o'; out_buf[cr++] = 'n'; out_buf[cr++] = 'e';

    /* Key 3: "attStmt" -> empty map */
    out_buf[cr++] = 0x03;
    out_buf[cr++] = 0xA0;

    return cr;
}

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
                                      size_t max_buf)
{
    if (!rpid_hash || !cred_id || cred_id_len == 0 || cred_id_len > 255 ||
        !sig_der || sig_len == 0 || sig_len > 255 || !out_buf ||
        user_id_len > 64 || (user_id_len && !user_id)) {
        return 0;
    }

    const size_t required = 66 + (cred_id_len < 24 ? 1 : 2) + cred_id_len + sig_len +
                            (user_id_len ? 7 + user_id_len : 0);
    if (required > max_buf) return 0;

    /* 1. Assemble authData (37 bytes): rpIdHash (32) + flags (1) + signCount (4).
     * The caller must sign exactly this buffer; the flags and counter here must
     * therefore match the authData used for the ECDSA signature. */
    uint8_t auth_data[37];
    memcpy(auth_data, rpid_hash, 32);
    auth_data[32] = flags;
    auth_data[33] = (uint8_t)((sign_count >> 24) & 0xFF);
    auth_data[34] = (uint8_t)((sign_count >> 16) & 0xFF);
    auth_data[35] = (uint8_t)((sign_count >> 8) & 0xFF);
    auth_data[36] = (uint8_t)(sign_count & 0xFF);

    /* 2. Assemble CBOR response:
       0x00 status + Map(3 or 4) {
         01 (credential): { "id": cred_id, "type": "public-key" },
         02 (authData): auth_data,
         03 (signature): sig_der,
         [optional 04 (user): { "id": user_id }]
       } */
    size_t cr = 0;
    out_buf[cr++] = 0x00; /* CTAP1_ERR_SUCCESS */

    bool has_user = (user_id != NULL && user_id_len > 0 && user_id_len <= 64);
    out_buf[cr++] = has_user ? 0xA4 : 0xA3; /* Map(4) or Map(3) */

    /* Key 1: credential descriptor Map(2) */
    out_buf[cr++] = 0x01;
    out_buf[cr++] = 0xA2;
    /* "id": cred_id */
    out_buf[cr++] = 0x62; out_buf[cr++] = 'i'; out_buf[cr++] = 'd';
    if (cred_id_len < 24) {
        out_buf[cr++] = (uint8_t)(0x40 | cred_id_len);
    } else {
        out_buf[cr++] = 0x58; out_buf[cr++] = (uint8_t)cred_id_len;
    }
    memcpy(out_buf + cr, cred_id, cred_id_len); cr += cred_id_len;
    /* "type": "public-key" */
    out_buf[cr++] = 0x64; out_buf[cr++] = 't'; out_buf[cr++] = 'y'; out_buf[cr++] = 'p'; out_buf[cr++] = 'e';
    out_buf[cr++] = 0x6A;
    memcpy(out_buf + cr, "public-key", 10); cr += 10;

    /* Key 2: authData (37 bytes) */
    out_buf[cr++] = 0x02;
    out_buf[cr++] = 0x58;
    out_buf[cr++] = 37;
    memcpy(out_buf + cr, auth_data, 37); cr += 37;

    /* Key 3: signature (sig_der) */
    out_buf[cr++] = 0x03;
    out_buf[cr++] = 0x58;
    out_buf[cr++] = (uint8_t)sig_len;
    memcpy(out_buf + cr, sig_der, sig_len); cr += sig_len;

    /* Key 4 (optional): user Map(1) (CTAP2 Section 6.2 Key 4) */
    if (has_user) {
        out_buf[cr++] = 0x04;
        out_buf[cr++] = 0xA1;
        out_buf[cr++] = 0x62; out_buf[cr++] = 'i'; out_buf[cr++] = 'd';
        out_buf[cr++] = 0x58; out_buf[cr++] = (uint8_t)user_id_len;
        memcpy(out_buf + cr, user_id, user_id_len); cr += user_id_len;
    }

    return cr;
}
