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

static void test_invalid_commands_do_not_mutate_accounts(void)
{
    const char *invalid[] = {
        "DEL_TOTP", "DEL_TOTP junk", "DEL_TOTP -1", "DEL_TOTP +0",
        "DEL_TOTP 0junk", "DEL_TOTP 0 1", "DEL_TOTP 18446744073709551616",
        "SYNC_TIME", "SYNC_TIME 1.5", "SET_BRIGHTNESS -10", "SET_SOUND garbage",
        "SET_SOUND 2", "WIPE_ALL extra", "CANCEL extra", "ADD_TOTP a b",
        "AUTH_REQ", "REG_REQ a b c extra"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(fido_parse_command(invalid[i]).type == FIDO_CMD_INVALID);
    assert(fido_parse_command("DEL_TOTP 0 \t\r\n").type == FIDO_CMD_DEL_TOTP);
    assert(fido_parse_command("DEL_TOTP 18446744073709551615").num_param == UINT64_MAX);
    char oversized[300];
    memset(oversized, ' ', sizeof(oversized));
    memcpy(oversized, "WIPE_ALL", 8);
    oversized[sizeof(oversized) - 1] = 0;
    assert(fido_parse_command(oversized).type == FIDO_CMD_INVALID);
    puts("Invalid text commands cannot become account mutations: PASS");
}

static void test_cbor_container_and_identity_bounds(void)
{
    uint8_t request[160] = {0x02, 0xa2, 0x01, 0x61, 'a', 0x02, 0x58, 0x20};
    char rp[64];
    uint8_t hash[32], allow[64];
    size_t allow_len;
    fido_auth_params_t auth;
    assert(ctap2_parse_get_assertion(request, 40, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    request[1] = 0xa3; /* Missing declared pair. */
    assert(!ctap2_parse_get_assertion(request, 40, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    request[1] = 0xa2; request[40] = 0;
    assert(!ctap2_parse_get_assertion(request, 41, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    /* An RP ID must retain its full identity, including embedded zero bytes. */
    request[3] = 0x78; request[4] = 68;
    memset(request + 5, 'a', 68);
    request[73] = 0x02; request[74] = 0x58; request[75] = 0x20;
    memset(request + 76, 0, 32);
    assert(!ctap2_parse_get_assertion(request, 108, rp, sizeof(rp), hash, allow, &allow_len, &auth));
    uint8_t nul_rp[41] = {0x02, 0xa2, 0x01, 0x62, 'a', 0, 0x02, 0x58, 0x20};
    assert(!ctap2_parse_get_assertion(nul_rp, sizeof(nul_rp), rp, sizeof(rp), hash, allow, &allow_len, &auth));
    puts("CBOR containers and RP identity bounds: PASS");
}

static void test_display_labels_may_be_shortened(void)
{
    uint8_t request[160] = {0x01,0xa3,1,0x58,0x20};
    size_t n=37;
    const uint8_t rp[] = {2,0xa1,0x62,'i','d',0x61,'a'};
    const uint8_t user[] = {3,0xa2,0x62,'i','d',0x41,1,0x64,'n','a','m','e',0x78,64};
    memcpy(request+n,rp,sizeof(rp));n+=sizeof(rp);
    memcpy(request+n,user,sizeof(user));n+=sizeof(user);
    memset(request+n,'u',64);n+=64;
    char rpid[64], name[64]; uint8_t hash[32],id[64];size_t id_len;fido_auth_params_t auth;
    assert(ctap2_parse_make_credential(request,n,rpid,sizeof(rpid),hash,name,sizeof(name),id,&id_len,&auth));
    assert(strlen(name)==63 && id_len==1 && id[0]==1);
    /* Three-byte UTF-8 labels cannot end in a partial character. */
    for(size_t i=0;i<63;i+=3) memcpy(request+n-64+i,"\xe6\x97\xb6",3);
    request[n-1]='u';
    char short_name[63];
    assert(ctap2_parse_make_credential(request,n,rpid,sizeof(rpid),hash,short_name,sizeof(short_name),id,&id_len,&auth));
    assert(strlen(short_name)==60);
    puts("Display-only labels preserve bounded UTF-8 truncation: PASS");
}

static void test_allow_list_matches_every_position(void)
{
    uint8_t list[] = {0x83,
        0xa1,0x62,'i','d',0x44,1,2,3,4,
        0xa1,0x62,'i','d',0x44,5,6,7,8,
        0xa1,0x62,'i','d',0x44,9,10,11,12};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t id[] = {1+i*4,2+i*4,3+i*4,4+i*4};
        assert(ctap2_allow_list_contains(list,sizeof(list),id,sizeof(id)));
    }
    uint8_t other[] = {13,14,15,16};
    assert(!ctap2_allow_list_contains(list,sizeof(list),other,sizeof(other)));
    assert(!ctap2_allow_list_contains(list,sizeof(list)-1,list+6,4));
    const uint8_t empty[] = {0x80};
    assert(!ctap2_allow_list_contains(empty,sizeof(empty),other,sizeof(other)));
    puts("Allow-list matches first, middle and last credentials: PASS");
}

static void test_response_buffer_bounds(void)
{
    uint8_t bytes[256] = {0}, out[1024];
    memset(out, 0xa5, sizeof(out));
    assert(!ctap2_build_make_credential_resp(bytes, bytes, bytes, 255,
                                             bytes, bytes, 0, 0x41, out, sizeof(out)));
    assert(!ctap2_build_get_assertion_resp(bytes, bytes, 255, 1, 0,
                                           bytes, 255, bytes, 64, out, 256));
    assert(out[256] == 0xa5);
    assert(!ctap2_build_get_assertion_resp(bytes, bytes, 256, 1, 0,
                                           bytes, 70, NULL, 0, out, sizeof(out)));
    puts("Response builders reject overflowing buffers: PASS");
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

    req_ga[1] = 0xa3;
    req_ga[idx++] = 3;
    const uint8_t list[] = {0x81,0xa1,0x62,'i','d',0x44,1,2,3,4};
    memcpy(req_ga + idx,list,sizeof(list)); idx += sizeof(list);
    assert(ctap2_parse_get_assertion(req_ga,idx,rpid,sizeof(rpid),cdh_out,allow_id,&allow_id_len,&auth));
    assert(auth.allow_list_len == sizeof(list) && auth.allow_list >= req_ga &&
           auth.allow_list + auth.allow_list_len == req_ga + idx);
    uint8_t pending[sizeof(list)];
    memcpy(pending,auth.allow_list,auth.allow_list_len);
    req_ga[idx-1] = 99;
    const uint8_t expected_id[] = {1,2,3,4};
    assert(ctap2_allow_list_contains(pending,sizeof(pending),expected_id,sizeof(expected_id)));

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
    test_invalid_commands_do_not_mutate_accounts();
    test_cbor_container_and_identity_bounds();
    test_display_labels_may_be_shortened();
    test_allow_list_matches_every_position();
    test_response_buffer_bounds();
    test_rejects_oversized_hash_and_deep_cbor();
    test_fido_state_flow();
    test_command_parser();
    test_ctap2_cbor_parsers_and_builders();
    printf("All FIDO protocol host tests: PASS\n");
    return 0;
}
