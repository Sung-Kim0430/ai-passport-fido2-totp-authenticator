#!/usr/bin/env python3
"""Run the real BLE key-material functions with failing NVS operations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_key_wake_contract import function_body, read


class BleMaterialTest(unittest.TestCase):
    def test_storage_errors_never_replace_or_accept_volatile_identity(self):
        source = read('main/auth_ble.c')
        # Include helpers alongside the actual seed cache and derivation code.
        material = source[source.index('/* Master Key Derivation */'):source.index('/* Fragmented CTAP BLE notification sender */')]
        harness = r'''
#include "auth_fido_store.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define MBEDTLS_MD_SHA256 1
static int open_error, read_error, write_error, commit_error, crypto_error;
static int writes, commits, randoms, closes;
static uint32_t counter;
static size_t stored_len;
static char last_site_rp[AUTH_FIDO_RP_MAX];
static int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    assert(!strcmp(ns,"fido_sec") && mode == NVS_READWRITE); *h=1; return open_error;
}
static int nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    assert(h==1 && key); if(read_error)return read_error;
    memset(out,0x31,*len); if(stored_len)*len=stored_len; return 0;
}
static int nvs_set_blob(nvs_handle_t h, const char *key, const void *p, size_t n) {
    assert(h==1 && key && p && n); ++writes; return write_error;
}
static int nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    assert(h==1 && !strcmp(key,"sign_ctr")); *out=counter; return read_error;
}
static int nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value) {
    assert(h==1 && !strcmp(key,"sign_ctr")); ++writes; counter=value; return write_error;
}
static int nvs_commit(nvs_handle_t h) { assert(h==1); ++commits; return commit_error; }
static void nvs_close(nvs_handle_t h) { assert(h==1); ++closes; }
static void esp_fill_random(void *out, size_t n) { ++randoms; memset(out,0x71,n); }
static const void *mbedtls_md_info_from_type(int type) { (void)type; return (const void*)1; }
static int mbedtls_md_hmac(const void *md, const void *key, size_t kl,
                           const void *msg, size_t ml, void *out) {
    assert(md && key && kl==32 && msg && ml); memset(out,0x51,32); return crypto_error;
}
static auth_fido_result_t auth_fido_load_stub(const char *rp, bool create, auth_fido_record_t *out) {
    snprintf(last_site_rp,sizeof(last_site_rp),"%s",rp);
    memset(out,0,sizeof(*out));
    if(open_error || read_error==ESP_FAIL)return AUTH_FIDO_ERROR;
    if(read_error==ESP_ERR_NVS_NOT_FOUND) {
        if(!create)return AUTH_FIDO_MISSING;
        ++randoms;++writes;if(write_error || commit_error)return AUTH_FIDO_ERROR;
    }
    if(commit_error)return AUTH_FIDO_ERROR;
    memset(out->salt,0x31,sizeof(out->salt));return AUTH_FIDO_OK;
}
#define auth_fido_load auth_fido_load_stub
MATERIAL
static void reset(void) {
    open_error=read_error=write_error=commit_error=crypto_error=0;
    writes=commits=randoms=closes=0; stored_len=0;counter=0; auth_ble_reload_seed();
}
static void zeroed(const uint8_t *p, size_t n) { for(size_t i=0;i<n;++i)assert(!p[i]); }
int main(void) {
    reset(); open_error=ESP_FAIL;
    assert(!ensure_master_seed()); assert(!s_master_seed_loaded && !writes && !randoms);
    zeroed(s_fido_master_seed,32);
    reset(); read_error=ESP_FAIL;
    assert(!ensure_master_seed()); assert(!writes && !randoms && closes==1);
    reset(); stored_len=31;
    assert(!ensure_master_seed()); assert(!writes && !randoms);
    for(int mode=0;mode<2;++mode) {
        reset(); read_error=ESP_ERR_NVS_NOT_FOUND;
        if(mode)commit_error=ESP_FAIL;else write_error=ESP_FAIL;
        assert(!ensure_master_seed()); assert(!s_master_seed_loaded && writes==1);
        zeroed(s_fido_master_seed,32);
    }
    reset(); read_error=ESP_ERR_NVS_NOT_FOUND;commit_error=ESP_FAIL;
    assert(!ensure_master_seed());
    /* A failed commit may leave a readable pending blob in an NVS backend. */
    read_error=0;assert(!ensure_master_seed() && !s_master_seed_loaded);
    reset(); read_error=ESP_ERR_NVS_NOT_FOUND;
    assert(ensure_master_seed() && s_master_seed_loaded && writes==1 && commits==1);
    reset(); assert(ensure_master_seed()); assert(!writes && !randoms);
    uint8_t priv[32],id[4];
    read_error=ESP_FAIL; memset(priv,0xff,32);memset(id,0xff,4);
    assert(!derive_fido_key("example.com",priv,id,sizeof(id),true));
    assert(!writes && !randoms);zeroed(priv,32);zeroed(id,4);
    read_error=ESP_ERR_NVS_NOT_FOUND;
    assert(!derive_fido_key("example.com",priv,id,sizeof(id),false));
    assert(!writes && !randoms);zeroed(priv,32);zeroed(id,4);
    write_error=ESP_FAIL;
    assert(!derive_fido_key("example.com",priv,id,sizeof(id),true));
    assert(writes==1);zeroed(priv,32);zeroed(id,4);
    read_error=write_error=0;crypto_error=ESP_FAIL;
    assert(!derive_fido_key("example.com",priv,id,sizeof(id),false));
    zeroed(priv,32);zeroed(id,4);
    crypto_error=0;
    assert(derive_fido_key("example.com",priv,id,sizeof(id),false));
    char first_rp[AUTH_FIDO_RP_MAX];
    assert(derive_fido_key("same-prefix-site-a.example",priv,id,sizeof(id),false));
    memcpy(first_rp,last_site_rp,sizeof(first_rp));
    assert(derive_fido_key("same-prefix-site-b.example",priv,id,sizeof(id),false));
    assert(strcmp(first_rp,last_site_rp) != 0);
    uint32_t next=0;
    for(int mode=0;mode<4;++mode) {
        reset(); if(mode==0)open_error=ESP_FAIL;else if(mode==1)read_error=ESP_FAIL;
        else if(mode==2)write_error=ESP_FAIL;else commit_error=ESP_FAIL;
        assert(!next_fido_counter(&next));
    }
    reset();counter=UINT32_MAX;assert(!next_fido_counter(&next) && !writes);
    reset();read_error=ESP_ERR_NVS_NOT_FOUND;
    assert(next_fido_counter(&next) && next==1);
    reset();counter=41;assert(next_fido_counter(&next) && next==42);
    puts("BLE durable key material fault injection: PASS");
}
'''.replace('MATERIAL', material)
        with tempfile.TemporaryDirectory(dir=os.environ.get('COMMANDCODE_SCRATCHPAD')) as folder:
            c = Path(folder) / 'test.c'
            exe = Path(folder) / 'test'
            c.write_text(harness)
            result = subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Imain',str(c),'-o',str(exe)], capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__ == '__main__':
    unittest.main()
