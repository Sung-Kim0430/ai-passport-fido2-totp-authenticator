from pathlib import Path
import csv
import os
import subprocess
import tempfile
import unittest
from test_key_wake_contract import function_body

ROOT = Path(__file__).resolve().parents[1]

class HistoryPlatform(unittest.TestCase):
    def test_layout(self):
        rows = list(csv.reader(line for line in (ROOT / 'partitions.csv').read_text().splitlines() if not line.startswith('#')))
        table = {r[0].strip(): tuple(x.strip() for x in r[1:5]) for r in rows if r}
        self.assertEqual(table, {
            'nvs': ('data', 'nvs', '0x9000', '0x6000'),
            'phy_init': ('data', 'phy', '0xf000', '0x1000'),
            'factory': ('app', 'factory', '0x10000', '0x7e0000'),
            'rtc_hist': ('data', 'nvs', '0x7f0000', '0x10000')})

    def test_backend(self):
        source = (ROOT / 'main/rtc_clock_platform.c').read_text()
        signatures = [
            ('history_nvs_read', 'static rtc_history_store_result_t history_nvs_read(void *u, const char *key, void *data, size_t *size)'),
            ('history_nvs_write', 'static bool history_nvs_write(void *u, const char *key, const void *data, size_t size)'),
            ('history_nvs_commit', 'static bool history_nvs_commit(void *u)'),
            ('rtc_history_store_platform_io', 'rtc_history_store_result_t rtc_history_store_platform_io(rtc_history_store_io_t *io)')]
        code = r'''
#include "rtc_clock.h"
rtc_mode_t rtc_clock_get_mode(void) { return RTC_MODE_ECO; }
#include <assert.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=1, ESP_ERR_NVS_INVALID_LENGTH=2,
ESP_ERR_NVS_NO_FREE_PAGES=3, ESP_ERR_NVS_NEW_VERSION_FOUND=4, NVS_READONLY=0, NVS_READWRITE=1 };
static int initerr, openerr, geterr, seterr, commiterr, live, opens, closes, commits;
int nvs_flash_init_partition(const char *p) { assert(!strcmp(p,"rtc_hist")); return initerr; }
int nvs_open_from_partition(const char *p,const char *n,int mode,int *h) {
assert(!strcmp(p,"rtc_hist") && !strcmp(n,"hist")); assert(mode==0 || mode==1);
if(openerr) return openerr; assert(!live); live=1; ++opens; *h=1; return 0; }
void nvs_close(int h) { assert(h==1 && live); live=0; ++closes; }
int nvs_get_blob(int h,const char *k,void *p,size_t *n) {
assert(h==1 && live && !strcmp(k,"c0")); (void)p; (void)n; return geterr; }
int nvs_set_blob(int h,const char *k,const void *p,size_t n) {
assert(h==1 && live && !strcmp(k,"c0")); (void)p; (void)n; return seterr; }
int nvs_commit(int h) { assert(h==1 && live); ++commits; return commiterr; }
'''
        for name, signature in signatures:
            code += signature + '{' + function_body(source, name) + '}\n'
        code += r'''
int main(void) {
rtc_history_store_io_t io; char p[832]; size_t n=sizeof p;
assert(rtc_history_store_platform_io(NULL)==RTC_HISTORY_STORE_BLOCKED);
for(int e=1;e<=4;++e) { initerr=e; assert(rtc_history_store_platform_io(&io)!=RTC_HISTORY_STORE_VALID); assert(!live); }
initerr=0; openerr=-1; assert(rtc_history_store_platform_io(&io)==RTC_HISTORY_STORE_IO_ERROR);
openerr=0; assert(rtc_history_store_platform_io(&io)==RTC_HISTORY_STORE_VALID);
assert(io.read(io.user,"c0",p,&n)==RTC_HISTORY_STORE_VALID);
geterr=ESP_ERR_NVS_NOT_FOUND; assert(io.read(io.user,"c0",p,&n)==RTC_HISTORY_STORE_MISSING);
geterr=ESP_ERR_NVS_INVALID_LENGTH; assert(io.read(io.user,"c0",p,&n)==RTC_HISTORY_STORE_BLOCKED);
geterr=-1; assert(io.read(io.user,"c0",p,&n)==RTC_HISTORY_STORE_IO_ERROR);
assert(io.write(io.user,"c0",p,n)); assert(commits==1); assert(io.commit(io.user));
seterr=-1; assert(!io.write(io.user,"c0",p,n)); assert(commits==1);
seterr=0; commiterr=-1; assert(!io.write(io.user,"c0",p,n)); assert(commits==2);
openerr=-1; assert(!io.write(io.user,"c0",p,n));
assert(io.read(io.user,"c0",p,&n)==RTC_HISTORY_STORE_IO_ERROR);
assert(!live && opens==closes); return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get('COMMANDCODE_SCRATCHPAD')) as d:
            p = Path(d)
            (p / 'test.c').write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Imain', str(p / 'test.c'), '-o', str(p / 'test')], cwd=ROOT, check=True)
            subprocess.run([str(p / 'test')], check=True)

if __name__ == '__main__':
    unittest.main()
