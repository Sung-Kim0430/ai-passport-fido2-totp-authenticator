#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fido_protocol.h"

static void test_fido_state_flow(void)
{
    fido_session_t session;
    fido_session_init(&session);
    assert(session.state == FIDO_STATE_IDLE);

    /* Start an auth request */
    uint64_t t0 = 10000;
    assert(fido_session_start_req(&session, FIDO_REQ_AUTHENTICATION,
                                  "github.com", "sung-kim", "abc123challenge",
                                  t0, 15000));
    assert(session.state == FIDO_STATE_UP_PENDING);
    assert(strcmp(session.current_req.rp_id, "github.com") == 0);
    assert(strcmp(session.current_req.user_name, "sung-kim") == 0);

    /* Tick at t0 + 5000ms: still pending, 10s remaining */
    uint32_t rem = 0;
    assert(!fido_session_tick(&session, t0 + 5000, &rem));
    assert(session.state == FIDO_STATE_UP_PENDING);
    assert(rem == 10);

    /* Approve */
    assert(fido_session_approve(&session));
    assert(session.state == FIDO_STATE_APPROVED);

    /* Reset */
    fido_session_reset(&session);
    assert(session.state == FIDO_STATE_IDLE);

    /* Reject flow */
    assert(fido_session_start_req(&session, FIDO_REQ_REGISTRATION, "google.com", "user@gmail.com", "", t0, 10000));
    assert(fido_session_reject(&session));
    assert(session.state == FIDO_STATE_REJECTED);

    /* Timeout flow */
    fido_session_reset(&session);
    assert(fido_session_start_req(&session, FIDO_REQ_AUTHENTICATION, "aws.amazon.com", "admin", "", t0, 5000));
    assert(fido_session_tick(&session, t0 + 5001, &rem));
    assert(session.state == FIDO_STATE_TIMEOUT);
    assert(rem == 0);

    printf("FIDO session state transitions: PASS\n");
}

static void test_command_parser(void)
{
    /* AUTH_REQ */
    fido_parsed_cmd_t cmd = fido_parse_command("AUTH_REQ github.com developer challenge_hex_123");
    assert(cmd.type == FIDO_CMD_AUTH_REQ);
    assert(strcmp(cmd.param1, "github.com") == 0);
    assert(strcmp(cmd.param2, "developer") == 0);
    assert(strcmp(cmd.param3, "challenge_hex_123") == 0);

    /* SYNC_TIME */
    cmd = fido_parse_command("SYNC_TIME 1772983192");
    assert(cmd.type == FIDO_CMD_SYNC_TIME);
    assert(cmd.num_param == 1772983192ULL);

    /* ADD_TOTP */
    cmd = fido_parse_command("ADD_TOTP GitHub admin JBSWY3DPEHPK3PXP");
    assert(cmd.type == FIDO_CMD_ADD_TOTP);
    assert(strcmp(cmd.param1, "GitHub") == 0);
    assert(strcmp(cmd.param2, "admin") == 0);
    assert(strcmp(cmd.param3, "JBSWY3DPEHPK3PXP") == 0);

    /* SET_BRIGHTNESS */
    cmd = fido_parse_command("SET_BRIGHTNESS 85\r\n");
    assert(cmd.type == FIDO_CMD_SET_BRIGHTNESS);
    assert(cmd.num_param == 85);

    /* SET_SOUND */
    cmd = fido_parse_command("SET_SOUND 0");
    assert(cmd.type == FIDO_CMD_SET_SOUND);
    assert(cmd.num_param == 0);

    /* CANCEL */
    cmd = fido_parse_command("CANCEL");
    assert(cmd.type == FIDO_CMD_CANCEL);

    printf("FIDO command parser: PASS\n");
}

static void test_ctap2_cbor_parsers_and_builders(void)
{
    /* Test 1: authenticatorMakeCredential request */
    /* Map(3) { 1: clientDataHash(32), 2: { id: "google.com", name: "Google" }, 3: { id: h'1234', name: "sungkim" } } */
    uint8_t cdh_in[32];
    for (int i = 0; i < 32; i++) cdh_in[i] = (uint8_t)(i + 1);

    uint8_t req_mc[128];
    size_t idx = 0;
    req_mc[idx++] = 0x01; /* MakeCredential command */
    req_mc[idx++] = 0xA3; /* Map(3) */
    /* Key 1: clientDataHash */
    req_mc[idx++] = 0x01;
    req_mc[idx++] = 0x58; req_mc[idx++] = 0x20;
    memcpy(req_mc + idx, cdh_in, 32); idx += 32;
    /* Key 2: rp Map(2) { "id": "google.com", "name": "Google" } */
    req_mc[idx++] = 0x02;
    req_mc[idx++] = 0xA2;
    req_mc[idx++] = 0x62; req_mc[idx++] = 'i'; req_mc[idx++] = 'd';
    req_mc[idx++] = 0x6A;
    memcpy(req_mc + idx, "google.com", 10); idx += 10;
    req_mc[idx++] = 0x64; req_mc[idx++] = 'n'; req_mc[idx++] = 'a'; req_mc[idx++] = 'm'; req_mc[idx++] = 'e';
    req_mc[idx++] = 0x66;
    memcpy(req_mc + idx, "Google", 6); idx += 6;
    /* Key 3: user Map(2) { "name": "sungkim", "id": h'A1B2' } */
    req_mc[idx++] = 0x03;
    req_mc[idx++] = 0xA2;
    req_mc[idx++] = 0x64; req_mc[idx++] = 'n'; req_mc[idx++] = 'a'; req_mc[idx++] = 'm'; req_mc[idx++] = 'e';
    req_mc[idx++] = 0x67;
    memcpy(req_mc + idx, "sungkim", 7); idx += 7;
    req_mc[idx++] = 0x62; req_mc[idx++] = 'i'; req_mc[idx++] = 'd';
    req_mc[idx++] = 0x42; req_mc[idx++] = 0xA1; req_mc[idx++] = 0xB2;

    char rpid[64] = {0};
    char username[64] = {0};
    uint8_t cdh_out[32] = {0};
    uint8_t user_id[64] = {0};
    size_t user_id_len = 0;
    fido_auth_params_t auth = {0};

    assert(ctap2_parse_make_credential(req_mc, idx, rpid, sizeof(rpid),
                                       cdh_out, username, sizeof(username),
                                       user_id, &user_id_len, &auth));
    assert(strcmp(rpid, "google.com") == 0);
    assert(strcmp(username, "sungkim") == 0);
    assert(memcmp(cdh_out, cdh_in, 32) == 0);
    assert(user_id_len == 2);
    assert(user_id[0] == 0xA1 && user_id[1] == 0xB2);

    /* Test 2: authenticatorGetAssertion request */
    /* Map(2) { 1: "github.com", 2: clientDataHash(32) } */
    uint8_t req_ga[128];
    idx = 0;
    req_ga[idx++] = 0x02; /* GetAssertion command */
    req_ga[idx++] = 0xA2; /* Map(2) */
    /* Key 1: rpId directly as text string */
    req_ga[idx++] = 0x01;
    req_ga[idx++] = 0x6A;
    memcpy(req_ga + idx, "github.com", 10); idx += 10;
    /* Key 2: clientDataHash */
    req_ga[idx++] = 0x02;
    req_ga[idx++] = 0x58; req_ga[idx++] = 0x20;
    memcpy(req_ga + idx, cdh_in, 32); idx += 32;

    memset(rpid, 0, sizeof(rpid));
    memset(cdh_out, 0, sizeof(cdh_out));
    uint8_t allow_id[64] = {0};
    size_t allow_id_len = 0;

    assert(ctap2_parse_get_assertion(req_ga, idx, rpid, sizeof(rpid),
                                     cdh_out, allow_id, &allow_id_len, &auth));
    assert(strcmp(rpid, "github.com") == 0);
    assert(memcmp(cdh_out, cdh_in, 32) == 0);
    assert(auth.pin_uv_auth_param_len == 0);
    assert(!auth.uv_requested);

    /* Test 3: authenticatorMakeCredential response builder */
    uint8_t rpid_hash[32];
    memset(rpid_hash, 0x42, 32);
    uint8_t aaguid[16] = { 0xee, 0x04, 0x1b, 0xce };
    uint8_t cred_id[32];
    memset(cred_id, 0x99, 32);
    uint8_t qx[32], qy[32];
    memset(qx, 0x11, 32);
    memset(qy, 0x22, 32);

    uint8_t resp_buf[512];
    size_t resp_len = ctap2_build_make_credential_resp(rpid_hash, aaguid, cred_id, 32,
                                                      qx, qy, 1, 0x45,
                                                      resp_buf, sizeof(resp_buf));
    assert(resp_len > 0);
    assert(resp_buf[0] == 0x00); /* Status: Success */
    assert(resp_buf[1] == 0xA3); /* Map(3) */
    assert(resp_buf[2] == 0x01); /* Key 1: authData (CTAP2 spec) */
    assert(resp_buf[3] == 0x58); /* Byte string 1-byte length prefix */
    /* Check flags inside authData */
    /* authData starts at offset 5:
       0x00 (status 1B) + 0xA3 (map 1B) + 0x01 (key 1B) + 0x58 (type 1B) + len (1B) = 5B */
    size_t authdata_offset = 5;
    assert(resp_buf[authdata_offset + 32] == 0x45); /* Flags: UP(1) + UV(4) + AT(64) */

    /* Test 4: authenticatorGetAssertion response builder */
    uint8_t sig_der[70];
    memset(sig_der, 0x30, 70);
    resp_len = ctap2_build_get_assertion_resp(rpid_hash, cred_id, 32,
                                            0x05, 1,
                                            sig_der, sizeof(sig_der),
                                            user_id, user_id_len,
                                            resp_buf, sizeof(resp_buf));
    assert(resp_len > 0);
    assert(resp_buf[0] == 0x00); /* Status: Success */
    assert(resp_buf[1] == 0xA4); /* Map(4) because user_id is included */
    assert(resp_buf[2] == 0x01); /* Key 1: credential */
    /* authData starts at 60; flags at 60+32 and signCount at 60+33. They must
     * match the values the caller signs, otherwise servers reject the login. */
    assert(resp_buf[92] == 0x05); /* Flags: UP + UV */
    assert(resp_buf[93] == 0x00 && resp_buf[94] == 0x00 &&
           resp_buf[95] == 0x00 && resp_buf[96] == 0x01); /* signCount = 1 */

    printf("CTAP2 CBOR parsers and response builders: PASS\n");
}

static void test_rejects_oversized_hash_and_deep_cbor(void)
{
    uint8_t request[160] = {0x02, 0xa2, 0x01, 0x61, 'a', 0x02, 0x58, 0x21};
    char rp[64];
    uint8_t hash[32], allow[64];
    size_t allow_len;
    fido_auth_params_t auth;
    assert(!ctap2_parse_get_assertion(request, 41, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    request[1] = 0xa3;
    request[7] = 0x20;
    size_t len = 40;
    request[len++] = 0x18;
    request[len++] = 0x63;
    for (unsigned i = 0; i < 64; i++) request[len++] = 0x81;
    request[len++] = 0;
    assert(!ctap2_parse_get_assertion(request, len, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    puts("CBOR oversized hash and recursion limits: PASS");
}

int main(void)
{
    test_rejects_oversized_hash_and_deep_cbor();
    test_fido_state_flow();
    test_command_parser();
    test_ctap2_cbor_parsers_and_builders();
    printf("All FIDO protocol host tests: PASS\n");
    return 0;
}
