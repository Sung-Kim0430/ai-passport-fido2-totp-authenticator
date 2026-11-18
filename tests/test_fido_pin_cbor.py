#!/usr/bin/env python3
"""Host C tests for fido_pin CBOR depth/bounds guards and changePIN auth ordering."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class FidoPinCborTest(unittest.TestCase):
    def test_cbor_depth_bounds_and_change_pin_auth_order(self):
        src = (ROOT / 'main' / 'fido_pin.c').read_text(encoding='utf-8')
        cbor_block = src[src.index('/* CBOR helpers'):src.index('/* Crypto helpers')]
        parse_block = src[src.index('typedef struct {\n    uint8_t protocol;'):src.index('static size_t handle_get_retries')]
        change_pin_block = src[src.index('static size_t handle_change_pin'):src.index('static size_t handle_get_pin_token')]

        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PIN_DEFAULT_RETRIES        8
#define CTAP2_ERR_PIN_INVALID      0x31
#define CTAP2_ERR_PIN_BLOCKED      0x32
#define CTAP2_ERR_PIN_AUTH_INVALID 0x33
#define CTAP2_ERR_PIN_NOT_SET      0x35
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), (void)(fmt))
static const char *const TAG = "fido_pin";

typedef struct {
    bool pin_set;
    uint8_t pin_hash[16];
    uint8_t retries;
    bool key_agreement_valid;
    uint8_t pin_token[32];
    bool token_valid;
} stub_pin_state_t;

static stub_pin_state_t s_pin;
static int g_save_calls;
static int g_decrypt_hash_calls;
static bool g_stub_auth_ok;
static bool g_stub_hash_match;

static void clear_secret_bytes(void *data, size_t len)
{
    volatile uint8_t *p = data;
    while (len--) *p++ = 0;
}

static bool compute_shared_secret(uint8_t protocol, const uint8_t x[32], const uint8_t y[32])
{
    (void)protocol; (void)x; (void)y;
    return true;
}

static bool verify_pin_uv_auth(uint8_t protocol, const uint8_t *msg, size_t msg_len,
                               const uint8_t *param, size_t param_len)
{
    (void)protocol; (void)msg; (void)msg_len; (void)param; (void)param_len;
    return g_stub_auth_ok;
}

static bool decrypt_pin_hash(const uint8_t *enc, size_t enc_len, uint8_t out_hash[16])
{
    (void)enc; (void)enc_len;
    ++g_decrypt_hash_calls;
    memset(out_hash, g_stub_hash_match ? 0xAA : 0x55, 16);
    return true;
}

static bool decrypt_new_pin(const uint8_t *enc, size_t enc_len, uint8_t out_hash[16])
{
    (void)enc; (void)enc_len;
    memset(out_hash, 0xBB, 16);
    return true;
}

static void pin_save(void)
{
    ++g_save_calls;
}
''' + cbor_block + '\n' + parse_block + '\n' + change_pin_block + r'''

int main(void)
{
    /* 1. CBOR recursion limit: 20 nested 1-elem arrays must fail cleanly */
    uint8_t deep[24];
    for (size_t i = 0; i < 20; i++) deep[i] = 0x81;
    deep[20] = 0x00;
    size_t off = 0;
    assert(!cbor_skip(deep, 21, &off));

    /* Shallow 3-level nested array succeeds */
    uint8_t shallow[4] = {0x81, 0x81, 0x81, 0x01};
    off = 0;
    assert(cbor_skip(shallow, sizeof(shallow), &off));
    assert(off == sizeof(shallow));

    /* Crafted oversized map pair count cannot trigger integer/loop blowup */
    uint8_t bad_map[6] = {0xBA, 0x7F, 0xFF, 0xFF, 0xFF, 0x01};
    off = 0;
    assert(!cbor_skip(bad_map, sizeof(bad_map), &off));

    /* 2. parse_client_pin strictness */
    client_pin_req_t req;
    /* Declared map(3), but only 1 pair present -> must fail */
    uint8_t trunc_pin[] = {0x06, 0xA3, 0x01, 0x01};
    assert(!parse_client_pin(trunc_pin, sizeof(trunc_pin), &req));

    /* Trailing byte after valid map(2) -> must fail */
    uint8_t trail_pin[] = {0x06, 0xA2, 0x01, 0x02, 0x02, 0x01, 0xFF};
    assert(!parse_client_pin(trail_pin, sizeof(trail_pin), &req));

    /* Valid map(2) for protocol=2, subCommand=1 -> passes */
    uint8_t ok_pin[] = {0x06, 0xA2, 0x01, 0x02, 0x02, 0x01};
    assert(parse_client_pin(ok_pin, sizeof(ok_pin), &req));
    assert(req.protocol == 2 && req.sub_cmd == 1);

    /* 3. changePIN verifies pinUvAuthParam BEFORE decrementing retries */
    memset(&s_pin, 0, sizeof(s_pin));
    s_pin.key_agreement_valid = true;
    s_pin.pin_set = true;
    s_pin.retries = 5;
    memset(s_pin.pin_hash, 0xAA, sizeof(s_pin.pin_hash));

    client_pin_req_t cp_req;
    memset(&cp_req, 0, sizeof(cp_req));
    cp_req.protocol = 2;
    cp_req.sub_cmd = 0x04;
    cp_req.has_key_agreement = true;
    cp_req.pin_uv_auth_len = 16;
    cp_req.new_pin_enc_len = 80;
    cp_req.pin_hash_enc_len = 32;

    uint8_t out[16];
    g_save_calls = 0;
    g_decrypt_hash_calls = 0;
    g_stub_auth_ok = false;
    g_stub_hash_match = false;
    size_t n = handle_change_pin(&cp_req, out, sizeof(out));
    assert(n == 1 && out[0] == CTAP2_ERR_PIN_AUTH_INVALID);
    assert(s_pin.retries == 5);
    assert(g_save_calls == 0);
    assert(g_decrypt_hash_calls == 0);

    /* Bad PIN with valid MAC decrements retries */
    g_stub_auth_ok = true;
    g_stub_hash_match = false;
    n = handle_change_pin(&cp_req, out, sizeof(out));
    assert(n == 1 && out[0] == CTAP2_ERR_PIN_INVALID);
    assert(s_pin.retries == 4);
    assert(g_save_calls == 1);
    assert(g_decrypt_hash_calls == 1);

    /* Valid MAC + matching PIN succeeds and resets retries */
    g_stub_hash_match = true;
    n = handle_change_pin(&cp_req, out, sizeof(out));
    assert(n == 2 && out[0] == 0x00 && out[1] == 0xA0);
    assert(s_pin.retries == PIN_DEFAULT_RETRIES);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='fido-pin-cbor-test-') as tmp:
            tmp_path = Path(tmp)
            c_path = tmp_path / 'test_fido_pin_cbor.c'
            exe_path = tmp_path / 'test_fido_pin_cbor'
            c_path.write_text(harness, encoding='utf-8')
            cc = os.environ.get('CC', 'cc')
            subprocess.run(
                [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c_path), '-o', str(exe_path)],
                check=True,
            )
            subprocess.run([str(exe_path)], check=True)


if __name__ == '__main__':
    unittest.main()
