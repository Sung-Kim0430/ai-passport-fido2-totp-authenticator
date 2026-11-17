#!/usr/bin/env python3
"""验证真实状态 handler 的只读时钟证据，不用串口或设备复位采样。"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_key_wake_contract import function_body, read

ROOT = Path(__file__).resolve().parents[1]


class RtcHttpStatusTests(unittest.TestCase):
    def test_reference_parser_and_dispatch(self):
        source = read("main/auth_wifi.c")
        self.assertIn("time_reference_present", read("main/fido_protocol.h"))
        support = source[source.index("typedef struct {\n    uint32_t id;\n    int64_t hrt_us;"):source.index("bool auth_wifi_init_identity(void)")]
        context = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "auth_wifi.h"
#include "totp_crypto.h"
typedef int esp_err_t;
typedef struct { size_t content_len; const char *body; } httpd_req_t;
#define ESP_OK 0
#define HTTPD_RESP_USE_STRLEN -1
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static void history_mailbox_reset(bool enabled) { (void)enabled; }
static int s_snapshot_lock, status_code, calls;
static struct { bool unlocked; } s_snapshot = {true};
static char s_session[33] = "00000000000000000000000000000000";
static uint32_t s_request_id, random_number = 0x12345678;
static bool queue_ok = true;
static int64_t now = 10000;
static fido_parsed_cmd_t queued;
static bool callback(const fido_parsed_cmd_t *cmd) { queued = *cmd; calls++; return queue_ok; }
static auth_wifi_cmd_cb_t s_cmd_cb = callback;
static bool s_dns_run, s_wifi_started, s_wifi_initialized;
static void *s_dns_task, *s_server, *s_ap_netif;
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ms) { (void)ms; }
static int httpd_stop(void *server) { (void)server; return 0; }
static int esp_wifi_stop(void) { return 0; }
static int esp_wifi_deinit(void) { return 0; }
static void esp_netif_destroy_default_wifi(void *netif) { (void)netif; }
static uint32_t esp_random(void) { return ++random_number; }
static int64_t esp_timer_get_time(void) { return now; }
static const char *content_type = "application/json", *origin = "http://192.168.4.1", *token = s_session;
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t cap) {
    (void)r; const char *s = !strcmp(name,"Content-Type") ? content_type : !strcmp(name,"Origin") ? origin : token;
    if (!s || strlen(s) >= cap) return -1;
    strcpy(out,s); return 0;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) { (void)r; (void)name; return origin ? strlen(origin) : 0; }
static int httpd_req_recv(httpd_req_t *r, char *out, size_t n) { memcpy(out,r->body,n); return (int)n; }
static int httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; status_code=atoi(s); return 0; }
static int httpd_resp_set_type(httpd_req_t *r, const char *s) { (void)r; (void)s; return 0; }
static int httpd_resp_send(httpd_req_t *r, const char *s, int n) { (void)r; (void)s; (void)n; return 0; }
typedef struct cJSON { const char *name; char *valuestring; double valuedouble; int type; } cJSON;
static cJSON fields[8];
static cJSON root = {.type=1};
static const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *j, const char *key) {
    (void)j; for (unsigned i=0;i<8;i++) if (fields[i].name && !strcmp(fields[i].name,key)) return &fields[i]; return NULL;
}
static bool cJSON_IsObject(const cJSON *j) { return j && j->type==1; }
static bool cJSON_IsNumber(const cJSON *j) { return j && j->type==2; }
static bool cJSON_IsString(const cJSON *j) { return j && j->type==3; }
static bool cJSON_IsTrue(const cJSON *j) { return j && j->type==4; }
static cJSON *cJSON_ParseWithLengthOpts(const char *s, size_t n, const char **end, bool required) {
    (void)n; (void)end; (void)required; return s[0]=='{' ? &root : NULL;
}
static void cJSON_Delete(cJSON *j) { (void)j; }
int totp_base32_decode(const char *s, uint8_t *out, size_t n) { (void)s; (void)out; (void)n; return 1; }
static void fixture(uint32_t id) {
    memset(fields,0,sizeof fields);
    fields[0]=(cJSON){"command","SYNC_TIME",0,3};
    fields[1]=(cJSON){"value",NULL,1700000000,2};
    fields[2]=(cJSON){"reference_probe_id",NULL,id,2};
    fields[3]=(cJSON){"reference_utc_us",NULL,1700000000123456.0,2};
    fields[4]=(cJSON){"reference_rtt_us",NULL,40001,2};
    fields[5]=(cJSON){"reference_clock_delta_us",NULL,40000,2};
}
'''
        context = context.replace('#include <stdio.h>', '#include <stdio.h>\n#include <stdlib.h>')
        functions = "\n".join("static " + signature + " {\n" + function_body(source, name) + "\n}" for name, signature in [
            ("error_response", "esp_err_t error_response(httpd_req_t *req, const char *status, const char *message)"),
            ("string_field", "bool string_field(const cJSON *json, const char *name, char *out, size_t capacity)"),
            ("number_field", "bool number_field(const cJSON *json, const char *name, uint64_t max, uint64_t *out)"),
            ("parse_time_reference", "void parse_time_reference(const cJSON *json, fido_parsed_cmd_t *cmd)"),
            ("parse_command_json", "bool parse_command_json(const cJSON *json, fido_parsed_cmd_t *cmd)"),
            ("command_handler", "esp_err_t command_handler(httpd_req_t *req)")])
        functions += "\nbool auth_wifi_ap_stop(void) {\n" + function_body(source, "auth_wifi_ap_stop") + "\n}\n"
        main = r'''
static void dispatch(void) { httpd_req_t req={2,"{}"}; status_code=0; assert(command_handler(&req)==0); }
static void legacy(void) {
    fido_parsed_cmd_t c; assert(parse_command_json(&root,&c));
    assert(c.type==FIDO_CMD_SYNC_TIME && c.num_param==1700000000);
    assert(!c.time_reference_present && !c.time_reference_quality && c.time_reference_hrt_us==0);
}
int main(void) {
    time_probe_new_session();
    uint32_t nonce = s_time_probe_nonce;
    time_probe_t p=time_probe_record(9000,21,true);
    fixture(p.id);
    fido_parsed_cmd_t c; assert(parse_command_json(&root,&c));
    assert(c.time_reference_present && c.time_reference_quality);
    assert(c.time_reference_id==((uint64_t)nonce<<32 | p.id));
    assert(c.time_reference_utc_us==1700000000123456ULL && c.time_reference_hrt_us==9000);
    assert(c.time_reference_uncertainty_us==1020012 && c.num_param==1700000000);
    now=5009000; assert(parse_command_json(&root,&c) && c.time_reference_quality);
    now++; assert(parse_command_json(&root,&c) && c.time_reference_present && !c.time_reference_quality);
    now=8999; legacy(); now=10000;
    const double bad[][2]={{2,0},{2,4294967296.0},{2,1.5},{3,1577836799999999.0},{3,4102444800000001.0},{3,9007199254740992.0},{3,NAN},{3,INFINITY},{3,1700000000000000.5},{4,0},{4,-1},{4,2000001},{4,0.5},{5,140002},{5,-60000},{5,NAN},{5,40000.5}};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) { fixture(p.id); fields[(int)bad[i][0]].valuedouble=bad[i][1]; legacy(); }
    for(unsigned i=2;i<6;i++) { fixture(p.id); fields[i].type=3; legacy(); fields[i].name=NULL; legacy(); }
    fixture(p.id); fields[5].valuedouble=-59999; assert(parse_command_json(&root,&c) && c.time_reference_quality);
    fixture(p.id); fields[3].valuedouble=4102444800000000.0; fields[4].valuedouble=2000000; fields[5].valuedouble=2000000;
    assert(parse_command_json(&root,&c) && c.time_reference_uncertainty_us==2000011);
    fixture(p.id); fields[1].valuedouble=-1; assert(!parse_command_json(&root,&c));
    fixture(p.id); fields[0].valuestring="SET_SOUND"; fields[1].valuedouble=1; dispatch(); assert(!queued.time_reference_present);
    fixture(p.id); s_snapshot.unlocked=false; int oldcalls=calls; dispatch(); assert(status_code==423 && calls==oldcalls);
    s_snapshot.unlocked=true; token="11111111111111111111111111111111"; dispatch(); assert(status_code==403 && calls==oldcalls); token=s_session;
    origin="https://evil.invalid"; dispatch(); assert(status_code==403 && calls==oldcalls); origin="http://192.168.4.1";
    content_type="text/plain"; dispatch(); assert(status_code==415 && calls==oldcalls); content_type="application/json";
    httpd_req_t huge={768,"{}"}; assert(command_handler(&huge)==0 && status_code==413);
    httpd_req_t nested={4,"[{}]"}; assert(command_handler(&nested)==0 && status_code==400);
    fixture(p.id); queue_ok=false; dispatch(); assert(status_code==503 && queued.time_reference_present);
    assert(queued.request_id!=0); uint64_t saved_id=queued.time_reference_id;
    now=20000000; for(unsigned i=0;i<4;i++) time_probe_record(19000000+i,20,true);
    assert(queued.time_reference_hrt_us==9000 && queued.time_reference_id==saved_id && queued.time_reference_quality);
    queue_ok=true; fixture(p.id); dispatch(); assert(status_code==202 && !queued.time_reference_present);
    p=time_probe_record(now,1000,true); fixture(p.id); dispatch(); assert(queued.time_reference_present);
    dispatch(); assert(!queued.time_reference_present);
    p=time_probe_record(now,20,true); fixture(p.id); fields[4].valuedouble=2000001; dispatch(); assert(!queued.time_reference_present);
    fixture(p.id); fields[5].valuedouble=140002; dispatch(); assert(!queued.time_reference_present);
    fixture(p.id); char max_body[768]; memset(max_body,' ',767); max_body[0]='{'; max_body[1]='}'; max_body[767]=0;
    httpd_req_t maximum={767,max_body}; assert(command_handler(&maximum)==0 && status_code==202 && queued.time_reference_present);
    p=time_probe_record(now,20,true); fixture(p.id); now+=5000001; dispatch(); assert(queued.time_reference_present && !queued.time_reference_quality);
    p=time_probe_record(now,20,true); fixture(p.id); s_cmd_cb=NULL; dispatch(); assert(status_code==503); s_cmd_cb=callback; dispatch(); assert(!queued.time_reference_present);
    p=time_probe_record(now,1001,true); fixture(p.id); legacy();
    p=time_probe_record(now,20,false); fixture(p.id); legacy();
    p=time_probe_record(now,20,true); fixture(p.id);
    for(unsigned i=0;i<3;i++) time_probe_record(now,20,true);
    assert(parse_command_json(&root,&c) && c.time_reference_present);
    time_probe_record(now,20,true); legacy();
    p=time_probe_record(now,20,true); fixture(p.id);
    s_server=(void *)1; s_wifi_started=true; s_wifi_initialized=true; s_ap_netif=(void *)1;
    assert(auth_wifi_ap_stop()); legacy(); assert(s_cmd_cb==NULL && !s_server && !s_wifi_started && !s_ap_netif);
    time_probe_new_session(); legacy();
    time_probe_t next=time_probe_record(now,20,true); assert(next.id>p.id && s_time_probe_nonce!=nonce);
    s_time_probe_counter=UINT32_MAX-1;
    p=time_probe_record(INT64_MAX-1000,1000,true); assert(p.id==UINT32_MAX);
    next=time_probe_record(now,20,true); assert(next.id==1); fixture(p.id); legacy();
    puts("Actual reference parser, four-slot cache and guarded dispatch: PASS");
    return 0;
}
'''
        self.compile_run(context + support + functions + main)
        start = function_body(source, "auth_wifi_ap_start")
        stop = function_body(source, "auth_wifi_ap_stop")
        self.assertLess(start.index("time_probe_new_session()"), start.index("httpd_start("))
        self.assertIn("memset(s_time_probes, 0, sizeof(s_time_probes))", stop)

    def compile_run(self, source):
        with tempfile.TemporaryDirectory(dir=os.environ["COMMANDCODE_SCRATCHPAD"]) as directory:
            path = Path(directory)
            c, binary = path / "reference.c", path / "reference"
            c.write_text(source, encoding="utf-8")
            result = subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"), str(c), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_clock_evidence_and_invalid_samples(self):
        source_text = read("main/auth_wifi.c")
        body = function_body(source_text, "status_handler")
        support = source_text[source_text.index("typedef struct {\n    uint32_t id;\n    int64_t hrt_us;"):source_text.index("bool auth_wifi_init_identity(void)")]
        self.assertNotIn("rtc_clock_", body.replace("rtc_clock_diagnostics_t", ""))
        context = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "rtc_clock.h"
#define gettimeofday status_gettimeofday
typedef int esp_err_t;
typedef int httpd_req_t;
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_snapshot_lock, scenario, timers, direct_reads;
typedef struct { uint32_t id; bool ok; } command_result_t;
static struct { char device_id[13], ssid[32]; } s_identity = {
    "0123456789AB", "Passport-6789AB"
};
static char s_session[33] = "00000000000000000000000000000000";
static struct {
    uint64_t time;
    uint8_t count, brightness;
    bool sound, unlocked, time_set;
    command_result_t results[4];
    rtc_clock_diagnostics_t clock;
} s_snapshot;
static char response[2048];
static size_t response_len;
rtc_clock_diagnostics_t rtc_clock_diagnostics(void) {
    ++direct_reads; return (rtc_clock_diagnostics_t){0};
}
int gettimeofday(struct timeval *t, void *z) {
    (void)z;
    if (scenario == 1) return -1;
    t->tv_sec = 1700000000; t->tv_usec = 123456; return 0;
}
static uint32_t esp_random(void) { return 0xFEDCBA98; }
int64_t esp_timer_get_time(void) {
    if (scenario == 5) return timers++ ? INT64_MAX : INT64_MAX - 20;
    if (scenario == 6) return timers++ ? 20 : -1;
    return timers++ ? (scenario == 2 ? 12000 : scenario == 4 ? 9000 : 10020) : 10000;
}
static size_t heap_caps_get_free_size(int c) { (void)c; return 100000; }
static size_t heap_caps_get_largest_free_block(int c) { (void)c; return 50000; }
static int httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) {
    (void)r; (void)k; (void)v; return 0;
}
static int httpd_resp_set_type(httpd_req_t *r, const char *v) { (void)r; (void)v; return 0; }
static int httpd_resp_send_500(httpd_req_t *r) { (void)r; return -1; }
static int httpd_resp_send(httpd_req_t *r, const char *s, int n) {
    (void)r; assert(n > 0 && (size_t)n < sizeof response);
    memcpy(response, s, (size_t)n); response[n] = 0; response_len = (size_t)n; return 0;
}
'''
        main = r'''
int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]);
    s_snapshot.time = 1700000000; s_snapshot.count = 16;
    s_snapshot.brightness = 100; s_snapshot.sound = true;
    s_snapshot.unlocked = false; s_snapshot.time_set = scenario != 3;
    for (unsigned i = 0; i < 4; ++i) s_snapshot.results[i].id = UINT32_MAX;
    s_snapshot.clock.reconciled_elapsed_us = UINT64_MAX;
    s_snapshot.clock.sdk_correction_us = INT64_MIN;
    s_snapshot.clock.sdk_sample_valid = true;
    s_snapshot.clock.period_q19 = UINT32_MAX;
    s_snapshot.clock.samples = UINT32_MAX;
    s_snapshot.clock.pre_tail_gap_upper_us = UINT32_MAX;
    s_snapshot.clock.fault = UINT32_MAX;
    s_snapshot.clock.model_observations = UINT32_MAX;
    s_snapshot.clock.model_rms_millippm = UINT32_MAX;
    s_snapshot.clock.source = UINT32_MAX;
    s_snapshot.clock.ppm = INT32_MIN;
    s_snapshot.clock.corrected_elapsed_us = UINT64_MAX;
    s_snapshot.clock.raw_chain_us = UINT64_MAX;
    s_snapshot.clock.corrected_chain_us = UINT64_MAX;
    s_snapshot.clock.awake_total_us = UINT64_MAX;
    s_snapshot.clock.maintenance_count = UINT32_MAX;
    s_snapshot.clock.learning_reason = UINT32_MAX;
    s_snapshot.clock.reference_status = RTC_CLOCK_REFERENCE_UNCERTAIN;
    time_probe_new_session();
    s_time_probe_counter = UINT32_MAX - 1;
    if (scenario == 5) s_snapshot.time = UINT64_MAX;
    assert(status_handler(NULL) == 0);
    assert(direct_reads == 0 && timers == 2 && response_len <= 1791);
    assert(s_time_probes[(UINT32_MAX - 1) % 4].id == UINT32_MAX);
    puts(response); return 0;
}
'''
        source = context + support + "\nstatic esp_err_t status_handler(httpd_req_t *req) {\n" + body + "\n}\n" + main
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            path = Path(directory)
            c = path / "status.c"
            binary = path / "status"
            c.write_text(source, encoding="utf-8")
            compiled = subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                       "-I", str(ROOT / "main"), str(c), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for scenario in range(7):
                output = subprocess.check_output([str(binary), str(scenario)], text=True)
                status = json.loads(output)
                self.assertIn("time_probe", status)
                probe = status["time_probe"]
                self.assertEqual(probe["id"], 2**32 - 1)
                self.assertEqual(probe["valid"], scenario in (0, 1, 3, 5))
                if probe["valid"]:
                    self.assertEqual(probe["hrt_us"], 2**63 - 11 if scenario == 5 else 10010)
                    self.assertEqual(probe["window_us"], 20)
                else:
                    self.assertEqual(probe["window_us"], 0)
                self.assertIn("time_sample_valid", status)
                self.assertEqual(status["time_sample_valid"], scenario in (0, 5))
                self.assertEqual(status["time_set"], scenario != 3)
                self.assertEqual(status["rtc"]["policy"], "selected_source_ratio")
                self.assertEqual(status["rtc"]["source"], 2**32 - 1)
                self.assertEqual(status["rtc"]["ppm"], -(2**31))
                self.assertFalse(status["rtc"]["ppm_qualified"])
                for key in ("corrected_elapsed_us", "raw_chain_us", "corrected_chain_us", "awake_total_us"):
                    self.assertEqual(status["rtc"][key], 2**64 - 1)
                self.assertEqual(status["rtc"]["maintenance_count"], 2**32 - 1)
                self.assertEqual(status["rtc"]["learning_reason"], 2**32 - 1)
                self.assertEqual(status["rtc"]["reference_status"], 2)
                self.assertEqual(status["rtc"]["reconciled_elapsed_us"], 2**64 - 1)
                self.assertEqual(status["rtc"]["sdk_correction_us"], -(2**63))
                self.assertTrue(status["rtc"]["sdk_sample_valid"])
                self.assertNotIn("secret", output.lower())
                self.assertNotIn("issuer", status)
                if scenario in (0, 5):
                    self.assertEqual(status["time_us"], 1700000000123456)
                    self.assertEqual(status["time_sample_hrt_us"], probe["hrt_us"])
                    self.assertEqual(status["time_sample_window_us"], 20)
                else:
                    self.assertEqual(status["time_us"], 0)


if __name__ == "__main__":
    unittest.main()
