#!/usr/bin/env python3
"""Exercise the real full-RP store and NVS adapter with persistence failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FidoStoragePlatformTest(unittest.TestCase):
    def test_real_adapter_migration_deletion_listing_and_failures(self):
        adapter = (ROOT / 'main/auth_fido_store_platform.c').read_text()
        for header in ['esp_random.h', 'mbedtls/sha256.h', 'nvs.h']:
            adapter = adapter.replace(f'#include "{header}"', '')
        harness = r'''
#include "auth_fido_store.h"
#include "totp_sha2.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define NVS_TYPE_BLOB 7
typedef struct { char key[16]; } nvs_entry_info_t;
typedef struct { unsigned index; } iter_t;
typedef iter_t *nvs_iterator_t;
typedef struct { char key[16]; uint8_t data[AUTH_FIDO_RECORD_SIZE]; size_t len; } blob_t;
static blob_t blobs[64];
static unsigned used, opens, closes, randoms, writes, commits;
static int open_error, read_error, write_error, commit_error, hash_error;
static bool collision;
static const char *read_fail_key;
static int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    assert(!strcmp(ns,"fido_sec") && (mode==0 || mode==1));++opens;*h=1;return open_error;
}
static void nvs_close(nvs_handle_t h) {assert(h==1);++closes;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *len) {
    assert(h==1);if(read_error || (read_fail_key && !strcmp(key,read_fail_key)))return ESP_FAIL;
    for(unsigned i=0;i<used;++i)if(!strcmp(key,blobs[i].key)) {
        if(*len<blobs[i].len)return ESP_FAIL;
        *len=blobs[i].len;memcpy(out,blobs[i].data,*len);return 0;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t len) {
    assert(h==1);++writes;if(write_error)return ESP_FAIL;
    unsigned i=0;while(i<used && strcmp(key,blobs[i].key))++i;
    assert(i<64 && len<=sizeof(blobs[i].data));if(i==used)++used;
    snprintf(blobs[i].key,sizeof(blobs[i].key),"%s",key);
    memcpy(blobs[i].data,data,len);blobs[i].len=len;return 0;
}
static int nvs_commit(nvs_handle_t h) {assert(h==1);++commits;return commit_error;}
static void esp_fill_random(void *out,size_t len) {memset(out,(int)++randoms,len);}
static int mbedtls_sha256(const void *data,size_t len,uint8_t out[32],int sha224) {
    assert(!sha224);if(hash_error)return ESP_FAIL;
    totp_hmac_sha2((const uint8_t *)"test",4,data,len,false,out);
    if(collision)memset(out,0,32);return 0;
}
static int nvs_entry_find(const char *part,const char *ns,int type,nvs_iterator_t *it) {
    static iter_t iterator;assert(!strcmp(part,"nvs") && !strcmp(ns,"fido_sec") && type==NVS_TYPE_BLOB);
    iterator.index=0;*it=used?&iterator:NULL;return used?0:ESP_ERR_NVS_NOT_FOUND;
}
static void nvs_entry_info(nvs_iterator_t it,nvs_entry_info_t *info) {
    memcpy(info->key,blobs[it->index].key,16);
}
static int nvs_entry_next(nvs_iterator_t *it) {
    if(++(*it)->index>=used){*it=NULL;return ESP_ERR_NVS_NOT_FOUND;}return 0;
}
static void nvs_release_iterator(nvs_iterator_t it) {(void)it;}
ADAPTER
static void reset(void) {
    memset(blobs,0,sizeof(blobs));used=opens=closes=randoms=writes=commits=0;
    open_error=read_error=write_error=commit_error=hash_error=0;collision=false;read_fail_key=NULL;
}
static void legacy(const char *prefix) {
    char key[16];uint8_t salt[16];memset(salt,0x39,16);
    snprintf(key,sizeof(key),"s_%.12s",prefix);assert(!nvs_set_blob(1,key,salt,16));
    snprintf(key,sizeof(key),"u_%.12s",prefix);assert(!nvs_set_blob(1,key,"old",3));
}
int main(void) {
    const char *a="same-prefix-site-a.example", *b="same-prefix-site-b.example";
    auth_fido_record_t ra,rb,out;
    auth_fido_site_t sites[8];
    reset();for(unsigned i=0;i<8;++i){char prefix[16];snprintf(prefix,sizeof(prefix),"legacy%06u",i);legacy(prefix);}
    legacy(a);
    assert(auth_fido_load(a,false,&ra)==AUTH_FIDO_OK);
    assert(auth_fido_save_user(a,(const uint8_t *)"Alice",5));
    assert(auth_fido_list(sites,8)==8 && !sites[0].legacy && !strcmp(sites[0].rp_id,a));
    for(unsigned i=1;i<8;++i)assert(sites[i].legacy);
    assert(auth_fido_delete(sites[0].rp_id));
    /* Every wrapper reopens NVS: the tombstone survives and wins over legacy. */
    assert(auth_fido_load(a,false,&out)==AUTH_FIDO_MISSING);
    assert(auth_fido_load(b,false,&rb)==AUTH_FIDO_OK && !memcmp(ra.salt,rb.salt,16));
    assert(rb.user_len==3 && !memcmp(rb.user_id,"old",3));
    assert(auth_fido_load(a,true,&out)==AUTH_FIDO_OK && memcmp(out.salt,ra.salt,16));
    assert(auth_fido_load(b,false,&out)==AUTH_FIDO_OK && !memcmp(out.salt,rb.salt,16));
    assert(opens==closes);
    for(int mode=0;mode<5;++mode) {
        reset();legacy(a);unsigned initial_writes=writes;
        if(mode==0)open_error=ESP_FAIL;
        if(mode==1)read_error=ESP_FAIL;
        if(mode==2)write_error=ESP_FAIL;
        if(mode==3)commit_error=ESP_FAIL;
        if(mode==4)hash_error=ESP_FAIL;
        memset(&out,0xff,sizeof(out));assert(auth_fido_load(a,true,&out)==AUTH_FIDO_ERROR);
        assert(!out.rp_id[0] && !out.user_len && !randoms);
        if(mode!=2)assert(writes==initial_writes);
    }
    reset();legacy(a);read_fail_key="u_same-prefix-";
    assert(auth_fido_load(a,true,&out)==AUTH_FIDO_ERROR && !randoms);
    reset();commit_error=ESP_FAIL;
    assert(auth_fido_load(a,true,&out)==AUTH_FIDO_ERROR && randoms==1);
    /* A readable uncommitted blob remains unusable while commit fails. */
    assert(auth_fido_load(a,false,&out)==AUTH_FIDO_ERROR && randoms==1);
    commit_error=0;assert(auth_fido_load(a,false,&out)==AUTH_FIDO_OK && randoms==1);
    for(int mode=0;mode<3;++mode) {
        reset();assert(auth_fido_load(a,true,&ra)==AUTH_FIDO_OK);
        assert(auth_fido_save_user(a,(const uint8_t *)"Alice",5));
        if(mode==0)open_error=ESP_FAIL;
        if(mode==1)write_error=ESP_FAIL;
        if(mode==2)commit_error=ESP_FAIL;
        assert(!auth_fido_save_user(a,(const uint8_t *)"Bob",3));
        assert(!auth_fido_delete(a));
    }
    puts("FIDO NVS adapter, mixed-list priority and commit failure tests: PASS");
}
'''.replace('ADAPTER', adapter)
        with tempfile.TemporaryDirectory(dir=os.environ.get('COMMANDCODE_SCRATCHPAD')) as folder:
            c = Path(folder) / 'test.c'
            exe = Path(folder) / 'test'
            c.write_text(harness)
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-DESP_PLATFORM', '-Imain', str(c), 'main/auth_fido_store.c',
                                     'main/totp_sha2.c', 'main/totp_crypto.c', '-o', str(exe)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


    def test_selected_full_name_scroll_survives_periodic_refresh(self):
        source = (ROOT / 'main/auth_ui.c').read_text()
        start = source.index('void auth_ui_update_fido_accounts(')
        function = source[start:source.index('\n}', start) + 2]
        harness = r'''
#include "auth_fido_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define AUTH_MAX_FIDO_RPS 8
#define AUTH_VIEW_FIDO_ACCOUNTS 1
#define LV_LABEL_LONG_SCROLL_CIRCULAR 1
#define LV_LABEL_LONG_DOT 0
#define LV_ANIM_OFF 0
typedef int lv_label_long_mode_t;
typedef struct { int mode; char text[96]; } lv_obj_t;
static lv_obj_t rows[8],msg;
static lv_obj_t *s_lbl_fido_acc_items[8];
static lv_obj_t *s_lbl_fido_acc_msg=&msg;
static int s_current_view=1, color_text_mut, color_red, color_cyan, color_text_pri;
static unsigned mode_changes;
void lv_label_set_long_mode(lv_obj_t *obj,int mode) {obj->mode=mode;++mode_changes;}
int lv_label_get_long_mode(lv_obj_t *obj) {return obj->mode;}
static void set_label_text_if_changed(lv_obj_t *obj,const char *text) {
    snprintf(obj->text,sizeof(obj->text),"%s",text);
}
static void lv_obj_set_style_text_color(lv_obj_t *obj,int color,int part) {(void)obj;(void)color;(void)part;}
static void lv_obj_scroll_to_view(lv_obj_t *obj,int anim) {(void)obj;(void)anim;}
FUNCTION
int main(void) {
    for(unsigned i=0;i<8;++i)s_lbl_fido_acc_items[i]=&rows[i];
    auth_fido_site_t sites[2]={0};
    snprintf(sites[0].rp_id,sizeof(sites[0].rp_id),"same-prefix-site-a.example");
    snprintf(sites[1].rp_id,sizeof(sites[1].rp_id),"same-prefix-site-b.example");
    for(unsigned i=0;i<200;++i)auth_ui_update_fido_accounts(sites,2,0,false);
    assert(mode_changes==1 && rows[0].mode==1 && rows[1].mode==0);
    assert(strstr(rows[0].text,sites[0].rp_id) && strstr(rows[1].text,sites[1].rp_id));
    auth_ui_update_fido_accounts(sites,2,1,true);
    assert(mode_changes==3 && strstr(msg.text,"again"));
    sites[1].legacy=true;auth_ui_update_fido_accounts(sites,2,1,true);
    assert(strstr(rows[1].text,"Legacy:") && strstr(msg.text,"migrate"));
    puts("FIDO full-name periodic scrolling and legacy UI: PASS");
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(dir=os.environ.get('COMMANDCODE_SCRATCHPAD')) as folder:
            c = Path(folder) / 'test.c'
            exe = Path(folder) / 'test'
            c.write_text(harness)
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-Imain', str(c), '-o', str(exe)], cwd=ROOT,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
