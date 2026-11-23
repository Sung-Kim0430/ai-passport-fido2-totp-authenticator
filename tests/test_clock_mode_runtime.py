#!/usr/bin/env python3
"""Exercise real application mode switching and power configuration with SDK stubs."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from test_deep_sleep_contract import function_body

ROOT = Path(__file__).resolve().parents[1]

class ClockModeRuntimeTest(unittest.TestCase):
    def test_actual_power_and_persistence_boundaries(self):
        source = (ROOT / "main/main.c").read_text()
        bodies = {name: function_body(source, name) for name in
                  ("init_power_management", "clock_mode_apply")}
        harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "rtc_mode.h"
#define CONFIG_PM_ENABLE 1
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct {int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable;} esp_pm_config_t;
static rtc_mode_t current_mode;
static bool s_clock_mode_save_failed, save_ok, light_ready;
static unsigned light_calls, inhibited, saves, restarts, revoke;
static int configure_failure;
static esp_pm_config_t last_config;
static rtc_mode_t saved_mode;
static rtc_mode_t rtc_clock_get_mode(void) { return current_mode; }
static bool rtc_clock_start_light_sleep(void) { ++light_calls; return light_ready; }
static void rtc_light_sleep_platform_inhibit(void) { ++inhibited; }
static int esp_pm_configure(const esp_pm_config_t *p) { last_config=*p; if(configure_failure){--configure_failure; return 1;} return 0; }
static bool auth_storage_clock_mode_save(rtc_mode_t mode) { ++saves; if(!save_ok)return false; saved_mode=mode;return true; }
static void auth_time_init(bool deep) { assert(!deep); ++revoke; }
static void esp_restart(void) { ++restarts; }
static void init_power_management(void) { POWER_BODY }
static bool clock_mode_apply(rtc_mode_t mode) { APPLY_BODY }
int main(void) {
    light_ready=true;
    for(unsigned mode=0;mode<RTC_MODE_COUNT;++mode){
        current_mode=(rtc_mode_t)mode;light_calls=0;init_power_management();
        assert(last_config.min_freq_mhz==40 && last_config.max_freq_mhz==160);
        assert(last_config.light_sleep_enable==(mode!=RTC_MODE_XTAL));
        assert(light_calls==(mode!=RTC_MODE_XTAL));
    }
    current_mode=RTC_MODE_ECO;configure_failure=1;init_power_management();
    assert(inhibited==1 && !last_config.light_sleep_enable);
    assert(!clock_mode_apply(RTC_MODE_ECO));
    assert(!clock_mode_apply((rtc_mode_t)RTC_MODE_COUNT));
    assert(saves==0 && restarts==0 && revoke==0);
    save_ok=false;assert(!clock_mode_apply(RTC_MODE_XTAL));
    assert(s_clock_mode_save_failed && saves==1 && restarts==0 && revoke==0);
    assert(current_mode==RTC_MODE_ECO);
    save_ok=true;assert(clock_mode_apply(RTC_MODE_FREQUENT));
    assert(!s_clock_mode_save_failed && saved_mode==RTC_MODE_FREQUENT);
    assert(saves==2 && restarts==1 && revoke==1);
    return 0;
}
'''
        harness = harness.replace("POWER_BODY", bodies["init_power_management"]).replace("APPLY_BODY", bodies["clock_mode_apply"])
        with tempfile.TemporaryDirectory(prefix="clock-mode-runtime-") as directory:
            c = Path(directory) / "runtime.c"
            binary = Path(directory) / "runtime"
            c.write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(c), "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_actual_menu_confirmation_and_cancel(self):
        source = (ROOT / "main/main.c").read_text()
        start = source.index("else if (view == AUTH_VIEW_CLOCK_MODES)", source.index("static void auth_app_task"))
        end = source.index("else if (view == AUTH_VIEW_WIFI)", start)
        branch = source[start:end].removeprefix("else ")
        harness = r'''#include <assert.h>
#include "auth_ui.h"
enum {BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK};
enum {BSP_BTN_CLICK, BSP_BTN_LONG};
static uint8_t s_clock_mode_idx,s_clock_mode_page;
static bool s_clock_mode_confirm,s_clock_mode_save_failed;
static unsigned saves,refreshes;
static auth_view_t current_view=AUTH_VIEW_CLOCK_MODES;
static rtc_mode_t current_mode=RTC_MODE_ECO,saved_mode;
rtc_mode_t rtc_clock_get_mode(void){return current_mode;}
static void auth_enter_view(auth_view_t view){current_view=view;}
static void refresh_display(void){++refreshes;}
static bool clock_mode_apply(rtc_mode_t mode){++saves;saved_mode=mode;s_clock_mode_save_failed=true;return false;}
static void key(int btn,int kev){auth_view_t view=current_view; BRANCH }
int main(void){
 key(BSP_BTN_DOWN,BSP_BTN_CLICK);assert(s_clock_mode_idx==RTC_MODE_FREQUENT);
 for(unsigned p=1;p<=4;++p){key(BSP_BTN_OK,BSP_BTN_CLICK);assert(s_clock_mode_page==p && !saves);}
 key(BSP_BTN_OK,BSP_BTN_CLICK);assert(s_clock_mode_confirm && !saves);
 key(BSP_BTN_UP,BSP_BTN_CLICK);assert(!s_clock_mode_confirm && s_clock_mode_page==3);
 key(BSP_BTN_OK,BSP_BTN_CLICK);assert(s_clock_mode_page==4 && !saves);
 key(BSP_BTN_OK,BSP_BTN_CLICK);assert(s_clock_mode_confirm);
 key(BSP_BTN_OK,BSP_BTN_CLICK);assert(saves==1 && saved_mode==RTC_MODE_FREQUENT && s_clock_mode_save_failed && !s_clock_mode_confirm);
 key(BSP_BTN_OK,BSP_BTN_LONG);assert(!s_clock_mode_page && !s_clock_mode_save_failed && current_view==AUTH_VIEW_CLOCK_MODES);
 key(BSP_BTN_UP,BSP_BTN_CLICK);assert(s_clock_mode_idx==RTC_MODE_ECO);
 for(unsigned i=0;i<5;++i)key(BSP_BTN_OK,BSP_BTN_CLICK);
 assert(!s_clock_mode_page && !s_clock_mode_confirm && saves==1);
 key(BSP_BTN_UP,BSP_BTN_CLICK);assert(s_clock_mode_idx==AUTH_CLOCK_MODES_BACK_INDEX);
 key(BSP_BTN_OK,BSP_BTN_CLICK);assert(current_view==AUTH_VIEW_SETTINGS);
 current_view=AUTH_VIEW_CLOCK_MODES;key(BSP_BTN_OK,BSP_BTN_LONG);assert(current_view==AUTH_VIEW_SETTINGS);
 assert(refreshes>0);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="clock-menu-runtime-") as directory:
            c=Path(directory)/"test.c";binary=Path(directory)/"test"
            c.write_text(harness.replace("BRANCH",branch))
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(c), "main/auth_ui.c", "-o", str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True)

    def test_real_clock_preference_nvs_errors(self):
        source=(ROOT / "main/auth_storage.c").read_text()
        bodies=""
        for name,signature in [("load_byte","static bool load_byte(const char *key,uint8_t *value)"),
                               ("save_byte","static bool save_byte(const char *key,uint8_t value)"),
                               ("auth_storage_clock_mode_load","bool auth_storage_clock_mode_load(rtc_mode_t *mode)"),
                               ("auth_storage_clock_mode_save","bool auth_storage_clock_mode_save(rtc_mode_t mode)")]:
            bodies += signature+" {"+function_body(source,name)+"}\n"
        harness=r'''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rtc_mode.h"
#define ESP_PLATFORM 1
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 2
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define STORAGE_NVS_NAMESPACE "fido_auth"
typedef int esp_err_t;
typedef int nvs_handle_t;
static int open_err,get_err,set_err,commit_err;
static uint8_t stored;
static unsigned opens,closes,sets,commits;
static int nvs_open(const char* ns,int mode,int* h){assert(!strcmp(ns,"fido_auth") && (mode==0 || mode==1));++opens;*h=1;return open_err;}
static int nvs_get_u8(int h,const char* key,uint8_t* v){assert(h==1 && !strcmp(key,"clock_mode"));if(!get_err)*v=stored;return get_err;}
static int nvs_set_u8(int h,const char* key,uint8_t v){assert(h==1 && !strcmp(key,"clock_mode"));++sets;if(!set_err)stored=v;return set_err;}
static int nvs_commit(int h){assert(h==1);++commits;return commit_err;}
static void nvs_close(int h){assert(h==1);++closes;}
BODIES
int main(void){
 rtc_mode_t mode=RTC_MODE_XTAL;
 open_err=ESP_ERR_NVS_NOT_FOUND;assert(auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO && !sets);
 open_err=3;assert(!auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO && !sets);
 open_err=0;get_err=ESP_ERR_NVS_NOT_FOUND;assert(auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO);
 get_err=3;assert(!auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO);
 get_err=0;stored=255;assert(!auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO && !sets);
 stored=RTC_MODE_FREQUENT;assert(auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_FREQUENT);
 unsigned before=opens;assert(!auth_storage_clock_mode_save((rtc_mode_t)255) && opens==before);
 set_err=3;assert(!auth_storage_clock_mode_save(RTC_MODE_XTAL) && !commits && stored==RTC_MODE_FREQUENT);
 set_err=0;commit_err=3;assert(!auth_storage_clock_mode_save(RTC_MODE_XTAL) && commits==1);
 commit_err=0;assert(auth_storage_clock_mode_save(RTC_MODE_ECO) && commits==2);
 assert(auth_storage_clock_mode_load(&mode) && mode==RTC_MODE_ECO);
 assert(opens==closes+2);
 return 0;
}
'''.replace("BODIES",bodies)
        with tempfile.TemporaryDirectory(prefix="clock-preference-") as directory:
            c=Path(directory)/"test.c";binary=Path(directory)/"test";c.write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(c), "-o", str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True)

    def test_quartz_standby_locks_and_recovers_without_sleep(self):
        source = (ROOT / "main/main.c").read_text()
        enter = function_body(source, "quartz_standby_enter")
        wake = function_body(source, "quartz_standby_wake")
        harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "rtc_mode.h"
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define BSP_BTN_MV_RELEASED_MIN 3000
#define AUTH_VIEW_LOCK 0
typedef int esp_err_t;
static bool s_quartz_blank, s_quartz_wake_guard, s_session_unlocked=true;
static struct {unsigned brightness;} s_cfg={80};
static struct {bool ble_fido_enabled;} s_lab_cfg={true};
static unsigned s_guard, s_last_activity_ms;
static bool lock_ok=true, radio_ok=true;
static int panel_error, button_mv=3300;
static unsigned panel_calls, backlight, lock_depth, audio_off, ble_started;
static rtc_mode_t mode=RTC_MODE_XTAL;
static rtc_mode_t rtc_clock_get_mode(void){return mode;}
static int bsp_button_read_mv(void){return button_mv;}
static bool auth_wifi_ap_stop(void){return radio_ok;}
static bool auth_ble_stop(void){return radio_ok;}
static void audio_off_now(void){++audio_off;}
static bool bsp_lvgl_lock(int timeout){assert(timeout==1000);if(lock_ok)++lock_depth;return lock_ok;}
static void bsp_lvgl_unlock(void){assert(lock_depth==1);--lock_depth;}
static void bsp_display_backlight(unsigned brightness){assert(lock_depth==1);backlight=brightness;}
static void* bsp_display_panel(void){return &panel_calls;}
static int esp_lcd_panel_disp_on_off(void *panel,bool on){(void)on;assert(panel==&panel_calls && lock_depth==1);++panel_calls;return panel_error;}
static void passcode_guard_lock(unsigned *guard){++*guard;}
static void auth_enter_view(int view){assert(view==AUTH_VIEW_LOCK);}
static int64_t esp_timer_get_time(void){return 25000000;}
static void on_ble_cmd(void){}
static bool auth_ble_init(void(*callback)(void)){assert(callback==on_ble_cmd);return true;}
static void auth_ble_start_advertising(void){++ble_started;}
static bool quartz_standby_enter(void){ ENTER }
static void quartz_standby_wake(void){ WAKE }
int main(void){
 mode=RTC_MODE_ECO;assert(!quartz_standby_enter() && !s_quartz_blank);
 mode=RTC_MODE_XTAL;button_mv=1000;assert(!quartz_standby_enter());
 button_mv=3300;radio_ok=false;assert(!quartz_standby_enter());assert(panel_calls==0);
 radio_ok=true;lock_ok=false;assert(!quartz_standby_enter());assert(panel_calls==0);
 lock_ok=true;panel_error=1;assert(!quartz_standby_enter());assert(backlight==80 && !s_quartz_blank);
 panel_error=0;assert(quartz_standby_enter());assert(s_quartz_blank && !s_session_unlocked && s_guard==1 && backlight==0);
 unsigned calls=panel_calls;assert(quartz_standby_enter() && panel_calls==calls);
 lock_ok=false;quartz_standby_wake();assert(s_quartz_blank);
 lock_ok=true;panel_error=1;quartz_standby_wake();assert(s_quartz_blank);
 panel_error=0;quartz_standby_wake();assert(!s_quartz_blank && s_quartz_wake_guard);
 assert(!s_session_unlocked && backlight==80 && s_last_activity_ms==25000 && ble_started==1);
 assert(lock_depth==0 && audio_off>0);
 return 0;
}
'''
        harness = harness.replace("ENTER", enter).replace("WAKE", wake)
        with tempfile.TemporaryDirectory(prefix="quartz-standby-") as directory:
            c=Path(directory)/"test.c"; binary=Path(directory)/"test"
            c.write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain", str(c), "-o", str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],check=True)

if __name__ == "__main__":
    unittest.main()
