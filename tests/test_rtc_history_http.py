#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_key_wake_contract import function_body, read

ROOT = Path(__file__).resolve().parents[1]


class HistoryHttpTests(unittest.TestCase):
    def test_owner_mailbox_and_real_handler(self):
        source = read('main/auth_wifi.c')
        self.assertIn('void auth_wifi_history_service(bool unlocked)', source)
        support = source[source.index('typedef struct {\n    uint64_t generation;\n    size_t cursor;'):source.index('static esp_err_t index_handler')]
        context = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtc_clock.h"
typedef int esp_err_t;
typedef struct { const char *query; } httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_RESP_USE_STRLEN -1
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p), ++locks)
#define portEXIT_CRITICAL(p) ((void)(p), --locks)
#define pdMS_TO_TICKS(ms) (ms)
static int locks, s_snapshot_lock, code, calls, mode;
static bool in_owner, real_pages, metadata_wide;
static rtc_history_t history;
static void lifecycle_during_owner(void);
static struct { bool unlocked; struct { uint32_t learning_reason; } clock; } s_snapshot = {true,{13}};
static char s_session[33] = "0123456789ABCDEF0123456789ABCDEF";
static const char *token = s_session;
static char output[12000];
static size_t used;
static int httpd_resp_set_status(httpd_req_t *r,const char *s) { (void)r; code=atoi(s); return 0; }
static int httpd_resp_set_type(httpd_req_t *r,const char *s) { (void)r;(void)s;return 0; }
static int httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) { (void)r;(void)k;(void)v;return 0; }
static int httpd_resp_send(httpd_req_t *r,const char *s,int n) { (void)r;assert(!locks); if(n<0)n=(int)strlen(s);assert(used+n<sizeof output);memcpy(output+used,s,n);used+=n;output[used]=0;return 0; }
static int httpd_resp_send_chunk(httpd_req_t *r,const char *s,int n) { assert(n>=0 && n<768);return n ? httpd_resp_send(r,s,n) : 0; }
static size_t httpd_req_get_url_query_len(httpd_req_t *r) { return strlen(r->query); }
static int httpd_req_get_url_query_str(httpd_req_t *r,char *s,size_t n) { if(strlen(r->query)>=n)return -1;strcpy(s,r->query);return 0; }
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *k,char *s,size_t n) { (void)r;(void)k;if(!token || strlen(token)>=n)return -1;strcpy(s,token);return 0; }
void auth_wifi_history_service(bool unlocked);
static void history_mailbox_reset(bool enabled);
static void vTaskDelay(int ms) { (void)ms;assert(!locks);if(mode==1)return;if(mode==2){history_mailbox_reset(false);return;}in_owner=true;auth_wifi_history_service(mode!=3);in_owner=false; }
rtc_clock_history_summary_t rtc_clock_history_owner_summary(void) {
 assert(in_owner && !locks);calls++;
 if(mode==5)lifecycle_during_owner();
 if(real_pages)return (rtc_clock_history_summary_t){.ready=true,.generation=history.generation,.durable_count=history.count};
 rtc_clock_history_summary_t s={0};s.ready=mode!=4 && mode!=6;s.generation=UINT64_MAX;s.accepted_sequence=UINT64_MAX;s.durable_sequence=UINT64_MAX-1;s.dropped=UINT64_MAX;s.save_budget_us=UINT64_MAX;s.pending_count=UINT32_MAX;s.durable_count=UINT32_MAX;s.flags=UINT32_MAX;s.store_status=mode==4?RTC_HISTORY_STORE_IO_ERROR:mode==6?RTC_HISTORY_STORE_BLOCKED:INT32_MAX;s.save_result=INT32_MAX;s.legacy_candidate_ppm=INT32_MIN;s.active_ppm=-870;s.active_qualified=true;s.shadow.ppm=INFINITY;s.shadow.result=RTC_HISTORY_ESTIMATE_READY;s.shadow.version=UINT32_MAX;s.shadow.source=UINT32_MAX;s.shadow.samples=UINT32_MAX;s.shadow.inliers=UINT32_MAX;s.shadow.excluded=UINT32_MAX;s.shadow.uncertainty_ppm=1.7976931348623157e308;s.shadow.holdout_baseline_error_us=-1.7976931348623157e308;s.shadow.holdout_candidate_error_us=1.7976931348623157e308;if(metadata_wide){s.calibration=(rtc_clock_calibration_t){UINT32_MAX,UINT32_MAX,UINT64_MAX,UINT64_MAX,UINT32_MAX,1};s.saved_ppm_valid=true;s.saved_ppm=INT32_MIN;s.coefficient_save_blocked=true;s.coefficient_save_budget_us=UINT64_MAX;}return s;
}
bool rtc_clock_history_owner_page(uint64_t generation,size_t cursor,rtc_history_page_t *p) {
 assert(in_owner && !locks);if(real_pages)return rtc_history_page(&history,generation,cursor,p);assert(generation==UINT64_MAX);memset(p,0,sizeof *p);p->generation=generation;p->total=100;p->count=cursor<100?5:0;p->cursor=cursor+p->count;
 for(size_t i=0;i<p->count;i++){p->rows[i]=(rtc_history_record_t){.sequence=UINT64_MAX,.epoch=123456789,.source=2,.utc_end_us=UINT64_MAX,.raw={UINT64_MAX,UINT32_MAX},.awake_delta_us=UINT64_MAX,.utc_delta_us=UINT64_MAX,.combined_uncertainty_us=UINT64_MAX,.flags=UINT16_MAX,.temperature_centi_c=INT16_MIN,.battery_mv=UINT16_MAX,.temp_age_s=UINT16_MAX,.battery_age_s=UINT16_MAX};}return true;
}
'''
        main = r'''
static void lifecycle_during_owner(void) {
 history_mailbox_reset(false);history_mailbox_reset(true);
 s_history_mailbox.pending=true;s_history_mailbox.cursor=95;s_history_mailbox.has_generation=false;
}
static void dispatch(const char *q,int expected) { httpd_req_t r={q};used=0;code=200;assert(history_handler(&r)==0);assert(code==expected);assert(!locks); }
int main(void) {
 history_mailbox_reset(true);
 token=NULL;dispatch("",403);assert(!calls);token="bad";dispatch("",403);token="1123456789ABCDEF0123456789ABCDEF";dispatch("",403);token=s_session;
 s_snapshot.unlocked=false;dispatch("",423);assert(!calls);s_snapshot.unlocked=true;
 const char *bad[]={"cursor=1","cursor=-1","cursor=101","cursor=1.0","cursor=18446744073709551616","generation=18446744073709551616","generation=","cursor=","x=1","cursor=0&cursor=0","generation=1&generation=1","cursor=0&","cursor=%30","cursor=0&&generation=1"};
 for(size_t i=0;i<sizeof bad/sizeof bad[0];i++)dispatch(bad[i],400);
 char huge[100];memset(huge,'1',sizeof huge-1);huge[sizeof huge-1]=0;dispatch(huge,400);
 dispatch("generation=1",409);mode=4;dispatch("",503);assert(strstr(output,"RTC_HISTORY_IO_ERROR"));mode=6;dispatch("",503);assert(strstr(output,"RTC_HISTORY_BLOCKED"));mode=3;dispatch("",423);
 mode=1;dispatch("",504);mode=0;dispatch("",200);puts(output);
 metadata_wide=true;dispatch("",200);puts(output);metadata_wide=false;
 dispatch("cursor=95&generation=18446744073709551615",200);assert(strstr(output,"\"cursor\":95"));
 dispatch("cursor=100&generation=18446744073709551615",200);assert(strstr(output,"\"rows\":[]"));
 mode=2;dispatch("",503);mode=0;dispatch("",503);history_mailbox_reset(true);dispatch("",200);
 s_history_mailbox.pending=true;dispatch("",503);history_mailbox_reset(true);
 mode=5;dispatch("",503);assert(s_history_mailbox.pending && !s_history_mailbox.complete);
 mode=0;in_owner=true;auth_wifi_history_service(true);in_owner=false;assert(s_history_mailbox.complete && s_history_mailbox.page.cursor==100);
 history_mailbox_reset(true);int before=calls;auth_wifi_history_service(false);assert(calls==before);
 real_pages=true;
 const unsigned totals[]={0,1,3,5,6,100};
 for(unsigned t=0;t<sizeof totals/sizeof totals[0];++t){
  rtc_history_init(&history);
  for(unsigned i=0;i<totals[t];++i){
   rtc_history_record_t row={.source=2,.sequence=i+1,.epoch=1,.utc_end_us=RTC_HISTORY_UTC_MIN_US+(i+1)*UINT64_C(43200000000),.raw={UINT64_C(43200000000),0},.utc_delta_us=UINT64_C(43200000000)};
   assert(rtc_history_append(&history,&row));
  }
  size_t cursor=0;
  do {
   char query[80],expected[80];
   snprintf(query,sizeof query,"cursor=%zu&generation=%llu",cursor,(unsigned long long)history.generation);
   dispatch(query,200);
   size_t next=cursor+(history.count-cursor<5?history.count-cursor:5);
   snprintf(expected,sizeof expected,"\"cursor\":%zu,\"total\":%zu,\"next_cursor\":%zu",cursor,history.count,next);
   assert(strstr(output,expected));
   cursor=next;
  }while(cursor<history.count);
 }
 assert(sizeof s_history_mailbox<=1024);fprintf(stderr,"mailbox=%zu summary=%zu page=%zu\n",sizeof s_history_mailbox,sizeof(rtc_clock_history_summary_t),sizeof(rtc_history_page_t));return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get('COMMANDCODE_SCRATCHPAD')) as directory:
            c = Path(directory) / 'history.c'
            binary = Path(directory) / 'history'
            c.write_text(context + support + main)
            result = subprocess.run([shutil.which('cc') or 'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'main'), str(c), str(ROOT / 'main/rtc_history.c'), str(ROOT / 'main/rtc_drift.c'), '-lm', '-o', str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stderr.strip())
            import json
            data, wide = map(json.loads, result.stdout.splitlines())
            self.assertIsNone(data['summary']['saved_ppm'])
            self.assertEqual(wide['summary']['calibration']['evidence_sequence'], str(2**64-1))
            self.assertEqual(wide['summary']['calibration']['uncertainty_millippm'], 2**32-1)
            self.assertEqual(wide['summary']['saved_ppm'], -2**31)
            self.assertEqual(wide['summary']['coefficient_save_budget_us'], str(2**64-1))
            self.assertEqual(data['generation'], str(2**64-1))
            self.assertEqual(len(data['rows']), 5)
            self.assertEqual(data['rows'][0]['raw_us'], str(2**64-1))
            self.assertEqual(data['summary']['active_ppm'], -870)
            self.assertEqual(data['summary']['shadow']['ppm'], 0)
            self.assertIn('calibration', data['summary'])
            self.assertEqual(data['summary']['calibration']['quality'], 0)
            self.assertIs(data['summary']['calibration']['uncertainty_valid'], False)
            self.assertEqual(data['summary']['calibration']['evidence_sequence'], '0')
            self.assertIs(data['summary']['coefficient_saved'], False)
            self.assertIs(data['summary']['coefficient_save_blocked'], False)
            self.assertEqual(data['summary']['coefficient_save_budget_us'], '0')
            for forbidden in ('epoch', 'reference_id', 'session', 'account', 'device_id', 'secret', 'probe', 'wire'):
                self.assertNotIn(forbidden, result.stdout)
        handler = function_body(source, 'history_handler')
        self.assertNotIn('rtc_clock_history_owner_', handler)
        self.assertNotIn('rtc_clock_history_owner_', function_body(source, 'status_handler'))
        self.assertIn('history_mailbox_reset(false)', function_body(source, 'auth_wifi_ap_stop'))
        self.assertIn('history_mailbox_reset(true)', function_body(source, 'auth_wifi_ap_start'))
        self.assertIn('"/api/rtc-history"', source)


if __name__ == '__main__':
    unittest.main()
