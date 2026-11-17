#!/usr/bin/env python3
"""编译真实 NVS 边界函数；只模拟 SDK 存取，不把它算作掉电实测。"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from test_key_wake_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


class RtcStorageBoundary(unittest.TestCase):
    def test_selected_source_measurement(self):
        platform = (ROOT / "main/rtc_clock_platform.c").read_text(encoding="utf-8")
        self.assertIn("uint32_t rtc_clock_platform_source(void)", platform)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
enum { SOC_RTC_SLOW_CLK_SRC_RC_SLOW = 0, SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 = 2,
       RTC_CAL_RTC_MUX = 0, RTC_CAL_INTERNAL_OSC = 1 };
static uint32_t source, after_source, xtal = 40, measured_selector, measured_cycles, calls;
uint32_t rtc_clk_slow_src_get(void) { return source; }
uint32_t rtc_clk_xtal_freq_get(void) { return xtal; }
uint32_t rtc_clk_cal(uint32_t selector, uint32_t cycles) {
    ++calls; measured_selector = selector; measured_cycles = cycles;
    source = after_source; return 7600000;
}
'''
        for name, signature in [
            ("rtc_clock_platform_source", "uint32_t rtc_clock_platform_source(void)"),
            ("rtc_clock_platform_supported", "bool rtc_clock_platform_supported(void)"),
            ("rtc_clock_platform_measure", "uint32_t rtc_clock_platform_measure(void)"),
        ]:
            code += signature + " {\n" + function_body(platform, name) + "\n}\n"
        code += r'''
int main(void) {
    for (unsigned i = 0; i < 2; ++i) {
        source = after_source = i ? SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 : SOC_RTC_SLOW_CLK_SRC_RC_SLOW;
        assert(rtc_clock_platform_source() == source);
        assert(rtc_clock_platform_supported());
        assert(rtc_clock_platform_measure() == 7600000);
        assert(measured_selector == RTC_CAL_RTC_MUX && measured_cycles == 3000);
    }
    unsigned before = calls; source = after_source = 1;
    assert(!rtc_clock_platform_supported() && rtc_clock_platform_measure() == 0 && calls == before);
    source = after_source = SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256; xtal = 26;
    assert(!rtc_clock_platform_supported() && rtc_clock_platform_measure() == 0 && calls == before);
    xtal = 40; source = SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256; after_source = SOC_RTC_SLOW_CLK_SRC_RC_SLOW;
    assert(rtc_clock_platform_measure() == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            path = Path(directory)
            (path / "source.c").write_text(code, encoding="utf-8")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "source.c"), "-o", str(path / "source")], check=True)
            subprocess.run([str(path / "source")], check=True)

    def test_actual_coefficient_storage_functions(self):
        platform = (ROOT / "main/rtc_clock_platform.c").read_text(encoding="utf-8")
        core = (ROOT / "main/rtc_clock.c").read_text(encoding="utf-8")
        bodies = ""
        for name, signature in [
            ("rtc_clock_platform_source", "uint32_t rtc_clock_platform_source(void)"),
            ("rtc_clock_platform_supported", "bool rtc_clock_platform_supported(void)"),
            ("rtc_clock_checksum", "uint32_t rtc_clock_checksum(const void *data, size_t size)"),
            ("rtc_clock_platform_load_coefficient", "rtc_clock_store_result_t rtc_clock_platform_load_coefficient(int32_t *ppm)"),
            ("rtc_clock_platform_save_coefficient", "bool rtc_clock_platform_save_coefficient(int32_t ppm)"),
        ]:
            bodies += signature + " {\n" + function_body(core if name == "rtc_clock_checksum" else platform, name) + "\n}\n"
        record_match = re.search(r"typedef struct\s*\{[^}]+\}\s*clock_coefficient_record_t;", platform, re.S)
        self.assertIsNotNone(record_match)
        version_match = re.search(r"^#define CLOCK_COEFFICIENT_RECORD_VERSION\s+[^\n]+", platform, re.M)
        self.assertIsNotNone(version_match)
        declarations = r'''
#include "rtc_clock_internal.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#undef RTC_CLOCK_VERSION
#define RTC_CLOCK_VERSION 99u
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 0x1102,
       ESP_ERR_NVS_INVALID_LENGTH = 0x110c, NVS_READONLY = 0, NVS_READWRITE = 1,
       SOC_RTC_SLOW_CLK_SRC_RC_SLOW = 0, SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 = 2 };
static uint32_t current_source, xtal = 40;
static bool change_source, change_xtal;
uint32_t rtc_clk_slow_src_get(void) { return current_source; }
uint32_t rtc_clk_xtal_freq_get(void) { return xtal; }
struct entry { const char *key; unsigned char blob[64]; size_t size; bool present; };
static struct entry entries[] = {
    { .key = "ppm_slow_v1" }, { .key = "ppm_fast_v1" },
    { .key = "model_v1", .blob = {17}, .size = 1, .present = true },
    { .key = "model_fast_v1", .blob = {23}, .size = 1, .present = true }
};
static unsigned opens, closes, sets, commits, live;
static int open_error, get_error, set_error, commit_error;
static struct entry *lookup(const char *key) {
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
        if (!strcmp(entries[i].key, key)) return &entries[i];
    assert(false); return NULL;
}
int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(!strcmp(name, "rtc_cal")); assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    assert(live == 0); ++opens;
    if (open_error) return open_error;
    ++live; *handle = mode + 1; return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && live == 1);
    struct entry *e = lookup(key);
    if (change_source) current_source = current_source == 0 ? 2 : 0;
    if (change_xtal) xtal = 26;
    if (get_error) return get_error;
    if (!e->present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < e->size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->blob, e->size); *size = e->size; return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    assert(handle == 2 && live == 1); ++sets;
    struct entry *e = lookup(key);
    if (set_error) return set_error;
    assert(size <= sizeof(e->blob)); memcpy(e->blob, data, size);
    e->size = size; e->present = true; return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) { assert(handle == 2 && live == 1); ++commits; return commit_error; }
void nvs_close(nvs_handle_t handle) { assert((handle == 1 || handle == 2) && live == 1); --live; ++closes; }
'''
        cases = r'''
_Static_assert(sizeof(((clock_coefficient_record_t *)0)->qualified) == sizeof(uint32_t), "qualified DTO");
_Static_assert(sizeof(((clock_coefficient_record_t *)0)->ppm) == sizeof(int32_t), "ppm DTO");
static void expect_load(rtc_clock_store_result_t expected, int32_t wanted) {
    int32_t output = 123456;
    struct entry before[4]; memcpy(before, entries, sizeof(entries));
    unsigned old_sets = sets, old_commits = commits, old_closes = closes, old_opens = opens;
    assert(rtc_clock_platform_load_coefficient(&output) == expected);
    assert(output == (expected == RTC_CLOCK_STORE_VALID ? wanted : 123456));
    assert(sets == old_sets && commits == old_commits && !memcmp(before, entries, sizeof(entries)));
    assert(live == 0 && closes - old_closes == (open_error ? 0 : opens - old_opens));
}
static void put_record(clock_coefficient_record_t record) {
    record.checksum = rtc_clock_checksum(&record, offsetof(clock_coefficient_record_t, checksum));
    struct entry *e = &entries[current_source == 2 ? 1 : 0];
    memcpy(e->blob, &record, sizeof(record)); e->size = sizeof(record); e->present = true;
}
int main(void) {
    expect_load(RTC_CLOCK_STORE_MISSING, 0);
    open_error = ESP_ERR_NVS_NOT_FOUND; expect_load(RTC_CLOCK_STORE_MISSING, 0);
    open_error = ESP_FAIL; expect_load(RTC_CLOCK_STORE_BLOCKED, 0); open_error = 0;
    get_error = ESP_FAIL; expect_load(RTC_CLOCK_STORE_BLOCKED, 0); get_error = 0;
    unsigned before = opens;
    assert(rtc_clock_platform_load_coefficient(NULL) == RTC_CLOCK_STORE_BLOCKED && opens == before);
    for (unsigned source = 0; source <= 2; source += 2) {
        current_source = source;
        int32_t ppm = source == 0 ? -20000 : 20000;
        assert(rtc_clock_platform_save_coefficient(ppm) && live == 0);
        expect_load(RTC_CLOCK_STORE_VALID, ppm);
        clock_coefficient_record_t wire;
        memcpy(&wire, entries[source == 2 ? 1 : 0].blob, sizeof(wire));
        assert(wire.version == 1 && wire.source == source && wire.xtal_mhz == 40 && wire.qualified == 1 && wire.ppm == ppm);
    }
    current_source = 0; expect_load(RTC_CLOCK_STORE_VALID, -20000);
    current_source = 2; expect_load(RTC_CLOCK_STORE_VALID, 20000);
    clock_coefficient_record_t good; memcpy(&good, entries[1].blob, sizeof(good));
    entries[1].blob[offsetof(clock_coefficient_record_t, checksum)] ^= 1;
    expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    get_error = ESP_ERR_NVS_NOT_FOUND; expect_load(RTC_CLOCK_STORE_MISSING, 0); get_error = 0;
    for (unsigned fault = 0; fault < 10; ++fault) {
        clock_coefficient_record_t wire = good;
        switch (fault) {
            case 0: wire.version = 2; break;
            case 1: wire.source = 0; break;
            case 2: wire.source = 1; break;
            case 3: wire.xtal_mhz = 26; break;
            case 4: wire.qualified = 0; break;
            case 5: wire.qualified = 2; break;
            case 6: wire.ppm = -20001; break;
            case 7: wire.ppm = 20001; break;
            case 8: wire.ppm = INT32_MIN; break;
            case 9: wire.ppm = INT32_MAX; break;
        }
        put_record(wire); expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    }
    put_record(good); --entries[1].size; expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    put_record(good); ++entries[1].size; expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    put_record(good); change_source = true; expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    change_source = false; current_source = 2;
    change_xtal = true; expect_load(RTC_CLOCK_STORE_BLOCKED, 0); change_xtal = false; xtal = 40;
    before = opens; current_source = 1;
    expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    assert(!rtc_clock_platform_save_coefficient(0) && opens == before);
    current_source = 2; xtal = 26; expect_load(RTC_CLOCK_STORE_BLOCKED, 0);
    assert(!rtc_clock_platform_save_coefficient(0) && opens == before); xtal = 40;
    const int32_t invalid[] = { -20001, 20001, INT32_MIN, INT32_MAX };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!rtc_clock_platform_save_coefficient(invalid[i]) && opens == before);
    open_error = ESP_FAIL;
    assert(!rtc_clock_platform_save_coefficient(0) && live == 0); open_error = 0;
    unsigned old_closes = closes, old_commits = commits;
    set_error = ESP_FAIL; assert(!rtc_clock_platform_save_coefficient(0)); set_error = 0;
    assert(live == 0 && closes == old_closes + 1 && commits == old_commits);
    old_closes = closes; commit_error = ESP_FAIL;
    assert(!rtc_clock_platform_save_coefficient(0)); commit_error = 0;
    assert(live == 0 && closes == old_closes + 1 && commits == old_commits + 1);
    before = commits;
    assert(rtc_clock_platform_save_coefficient(0) && rtc_clock_platform_save_coefficient(1));
    assert(commits == before + 2 && live == 0);
    expect_load(RTC_CLOCK_STORE_VALID, 1);
    assert(entries[2].size == 1 && entries[2].blob[0] == 17);
    assert(entries[3].size == 1 && entries[3].blob[0] == 23);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            source = Path(directory) / "coefficient.c"
            binary = Path(directory) / "coefficient"
            source.write_text(declarations + version_match.group(0) + "\n" + record_match.group(0) + "\n" + bodies + cases, encoding="utf-8")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(source), "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_actual_timer_arm_configurations(self):
        platform = (ROOT / "main/rtc_clock_platform.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_SLEEP_WAKEUP_TIMER = 1, GPIO_WAKE = 2 };
static unsigned wake_sources, enables, disables, restores, installs;
static int enable_error;
static bool stub_ok;
static uint64_t interval;
void esp_wake_deep_sleep(void) {}
void old_stub(void) {}
static void (*entry)(void) = old_stub;
void esp_set_deep_sleep_wake_stub(void (*stub)(void)) { entry = stub; ++restores; }
int esp_sleep_enable_timer_wakeup(uint64_t us) {
    assert(entry == esp_wake_deep_sleep); interval = us; ++enables;
    wake_sources |= ESP_SLEEP_WAKEUP_TIMER; return enable_error;
}
int esp_sleep_disable_wakeup_source(int source) {
    assert(source == ESP_SLEEP_WAKEUP_TIMER); ++disables; wake_sources &= ~source; return ESP_OK;
}
bool rtc_clock_stub_install(void) { ++installs; if (stub_ok) entry = old_stub; return stub_ok; }
'''
        code += "bool rtc_clock_platform_arm(bool request_maintenance) {\n" + function_body(platform, "rtc_clock_platform_arm") + "\n}\n"
        code += r'''
static void reset(void) {
    wake_sources = GPIO_WAKE | ESP_SLEEP_WAKEUP_TIMER;
    enables = disables = restores = installs = 0;
    enable_error = ESP_OK; stub_ok = true; interval = 0; entry = old_stub;
}
int main(void) {
    reset(); assert(!rtc_clock_platform_arm(false));
    assert(entry == esp_wake_deep_sleep && restores > 0 && enables == 0 && installs == 0);
    assert(wake_sources == GPIO_WAKE && disables == 1);
    reset();
#if CONFIG_AUTH_RTC_MAINTENANCE
    assert(rtc_clock_platform_arm(true));
    assert(entry == esp_wake_deep_sleep && restores > 0 && installs == 0);
    assert(enables == 1 && interval == UINT64_C(900000000) && disables == 0);
    assert(wake_sources == (GPIO_WAKE | ESP_SLEEP_WAKEUP_TIMER));
    reset(); enable_error = ESP_FAIL;
    assert(!rtc_clock_platform_arm(true));
    assert(entry == esp_wake_deep_sleep && restores > 0 && installs == 0);
    assert(enables == 1 && interval == UINT64_C(900000000));
    assert(wake_sources == GPIO_WAKE && disables == 1);
#elif CONFIG_AUTH_RTC_WAKE_STUB
    assert(rtc_clock_platform_arm(true) && installs == 1 && enables == 0);
    reset(); stub_ok = false; assert(!rtc_clock_platform_arm(true));
    assert(entry == esp_wake_deep_sleep && restores > 0 && installs == 1 && enables == 0);
    assert(wake_sources == GPIO_WAKE && disables == 1);
#else
    assert(!rtc_clock_platform_arm(true));
    assert(entry == esp_wake_deep_sleep && restores > 0 && installs == 0 && enables == 0);
    assert(wake_sources == GPIO_WAKE && disables == 1);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            source = Path(directory) / "arm.c"
            binary = Path(directory) / "arm"
            source.write_text(code, encoding="utf-8")
            for maintenance, experimental in [(1, 0), (0, 0), (0, 1)]:
                with self.subTest(maintenance=maintenance, experimental=experimental):
                    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    f"-DCONFIG_AUTH_RTC_MAINTENANCE={maintenance}", f"-DCONFIG_AUTH_RTC_WAKE_STUB={experimental}",
                                    str(source), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)

    def test_actual_model_storage_functions(self):
        platform = (ROOT / "main/rtc_clock_platform.c").read_text(encoding="utf-8")
        core = (ROOT / "main/rtc_clock.c").read_text(encoding="utf-8")
        record = re.search(r"typedef struct\s*\{[^}]+\}\s*clock_model_record_t;", platform, re.S).group(0)
        declarations = r'''
#include "rtc_clock_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 0x1102,
       ESP_ERR_NVS_INVALID_LENGTH = 0x110c, NVS_READONLY = 0, NVS_READWRITE = 1,
       SOC_RTC_SLOW_CLK_SRC_RC_SLOW = 0, SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 = 2 };
#undef RTC_CLOCK_VERSION
#define RTC_CLOCK_VERSION 99u
static uint32_t current_source;
uint32_t rtc_clock_platform_source(void) { return current_source; }
static unsigned char blob[256];
static size_t blob_size;
static int open_error, get_error, set_error, commit_error;
static unsigned opens, closes, sets, commits;
static bool supported = true;
bool rtc_clock_platform_supported(void) { return supported; }
int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(!strcmp(name, "rtc_cal")); assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    ++opens; if (open_error) return open_error; *handle = 1; return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && !strcmp(key, current_source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "model_fast_v1" : "model_v1"));
    if (get_error) return get_error;
    if (*size < blob_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, blob, blob_size); *size = blob_size; return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    assert(handle == 1 && !strcmp(key, current_source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "model_fast_v1" : "model_v1")); ++sets;
    if (set_error) return set_error;
    assert(size <= sizeof(blob)); memcpy(blob, data, size); blob_size = size; return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) { assert(handle == 1); ++commits; return commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); ++closes; }
'''
        version = re.search(r"^#define CLOCK_MODEL_RECORD_VERSION\s+[^\n]+", platform, re.M).group(0)
        functions = version + "\n" + record + "\nuint32_t rtc_clock_checksum(const void *data, size_t size) {\n" + function_body(core, "rtc_clock_checksum") + "\n}\n"
        functions += "rtc_clock_store_result_t rtc_clock_platform_load(rtc_drift_model_t *model) {\n" + function_body(platform, "rtc_clock_platform_load") + "\n}\n"
        functions += "bool rtc_clock_platform_save(const rtc_drift_model_t *model) {\n" + function_body(platform, "rtc_clock_platform_save") + "\n}\n"
        cases = r'''
static rtc_drift_model_t model(void) {
    rtc_drift_model_t m = {0}; m.coeff_q19[0] = 3.0 * RTC_DRIFT_Q19;
    m.min_temp_mc = 10000; m.max_temp_mc = 21000;
    m.observations = 12; m.occupied_bins = 12; m.qualified = true; return m;
}
static void put_record(clock_model_record_t *wire) {
    wire->checksum = rtc_clock_checksum(wire, offsetof(clock_model_record_t, checksum));
    memcpy(blob, wire, sizeof(*wire)); blob_size = sizeof(*wire);
}
int main(void) {
    rtc_drift_model_t m = model(), out = {0}, before;
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_MISSING && closes == 0);
    open_error = ESP_FAIL;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED && closes == 0);
    open_error = 0; get_error = ESP_ERR_NVS_NOT_FOUND;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_MISSING && closes == 1);
    get_error = 0;
    assert(rtc_clock_platform_save(&m) && commits == 1 && closes == 2);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_VALID && closes == 3);
    assert(out.qualified && out.coeff_q19[0] == m.coeff_q19[0]);
    memcpy(&before, &out, sizeof(out));
    unsigned char good[256]; memcpy(good, blob, sizeof(blob)); size_t good_size = blob_size;
    blob[0] ^= 1;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    assert(!memcmp(&before, &out, sizeof(out)));
    memcpy(blob, good, sizeof(blob)); blob_size = good_size - 1;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    clock_model_record_t wire; memcpy(&wire, good, sizeof(wire));
    wire.qualified = 2; put_record(&wire);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    memcpy(&wire, good, sizeof(wire)); wire.source = 1; put_record(&wire);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    memcpy(&wire, good, sizeof(wire)); wire.xtal_mhz = 26; put_record(&wire);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    memcpy(&wire, good, sizeof(wire)); wire.coefficients[0] = NAN; put_record(&wire);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    assert(!memcmp(&before, &out, sizeof(out)));
    unsigned old_opens = opens; m.rms_ppm = 101;
    assert(!rtc_clock_platform_save(&m) && opens == old_opens);
    m = model(); supported = false;
    assert(!rtc_clock_platform_save(&m) && opens == old_opens);
    supported = true; set_error = ESP_FAIL;
    unsigned old_closes = closes, old_commits = commits;
    assert(!rtc_clock_platform_save(&m));
    assert(closes == old_closes + 1 && commits == old_commits);
    set_error = 0; commit_error = ESP_FAIL; old_closes = closes;
    assert(!rtc_clock_platform_save(&m));
    assert(closes == old_closes + 1 && commits == old_commits + 1);
    commit_error = 0; current_source = SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256;
    assert(rtc_clock_platform_save(&m));
    memcpy(&wire, blob, sizeof(wire));
    assert(wire.source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 && wire.version == 1 && wire.version == CLOCK_MODEL_RECORD_VERSION);
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_VALID);
    wire.source = SOC_RTC_SLOW_CLK_SRC_RC_SLOW; put_record(&wire);
    before = out;
    assert(rtc_clock_platform_load(&out) == RTC_CLOCK_STORE_BLOCKED);
    assert(!memcmp(&before, &out, sizeof(out)));
    puts("RTC actual NVS boundary: PASS"); return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rtc-nvs-test-", dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            source.write_text(declarations + functions + cases, encoding="utf-8")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(source), "main/rtc_drift.c", "-lm", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
