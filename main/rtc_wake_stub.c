/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * 校准寄存器协议参考固定 IDF 5.5.6 的 rtc_time.c。
 * 此端口仅用于 C3：用有界、可取消轮询替代普通 API 的等待，不修改看门狗。
 */
#include "rtc_clock_internal.h"
#include "rtc_wake_policy.h"
#include "sdkconfig.h"

#if CONFIG_AUTH_RTC_WAKE_STUB
#include "bsp_pins.h"
#include "esp_attr.h"
#include "esp32c3/rom/rtc.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "hal/clk_tree_ll.h"
#include "hal/rtc_cntl_ll.h"
#include "soc/gpio_reg.h"
#include "soc/rtc.h"
#include "soc/system_struct.h"
#include "soc/timer_group_reg.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 6)
#error "Re-audit the RTC port before enabling it on another IDF version"
#endif

static bool RTC_IRAM_ATTR stub_key_seen(void)
{
    return !(REG_READ(GPIO_IN_REG) & (UINT32_C(1) << BSP_BTN_GPIO)) ||
           rtc_cntl_ll_gpio_get_wakeup_status() != 0;
}

static bool RTC_IRAM_ATTR stub_poll(rtc_wake_watch_t *watch)
{
    return rtc_wake_watch_poll(watch, rtc_clock_platform_ticks(), stub_key_seen());
}

static void RTC_IRAM_ATTR stub_return_boot(rtc_wake_watch_t *watch, rtc_clock_fault_t fault)
{
    rtc_clock_session_t *session = &rtc_clock_retained.session;
    if (!rtc_drift_ledger_advance(&session->ledger, rtc_clock_platform_ticks())) {
        session->utc_trusted = 0;
        fault = RTC_CLOCK_FAULT_LEDGER;
    }
    if (watch->key_seen) session->user_wake = 1;
    if (watch->max_gap_upper_us > session->pre_tail_gap_upper_us) session->pre_tail_gap_upper_us = watch->max_gap_upper_us;
    if (fault != RTC_CLOCK_FAULT_NONE) {
        session->fault = fault;
        rtc_clock_retained.maintenance_blocked = 1;
    }
    session->maintenance_enabled = 0;
    rtc_clock_session_seal(session);
}

static bool RTC_IRAM_ATTR stub_calibrate(rtc_wake_watch_t *watch, uint32_t *period)
{
    /* ROM/SDK 已离开普通任务环境；只开 TG0 总线，不复位其看门狗寄存器。 */
    SYSTEM.perip_clk_en0.reg_timergroup_clk_en = 1;
    if (GET_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_START_CYCLING)) {
        REG_SET_FIELD(TIMG_RTCCALICFG2_REG(0), TIMG_RTC_CALI_TIMEOUT_THRES, 1);
        bool stopped = false;
        for (unsigned poll = 0; poll < 64; ++poll) {
            if (!stub_poll(watch)) return false;
            if (GET_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_RDY) ||
                GET_PERI_REG_MASK(TIMG_RTCCALICFG2_REG(0), TIMG_RTC_CALI_TIMEOUT)) {
                stopped = true;
                break;
            }
            esp_rom_delay_us(10);
        }
        if (!stopped) return false;
    }
    REG_SET_FIELD(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_CLK_SEL, RTC_CAL_RTC_MUX);
    CLEAR_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_START_CYCLING);
    REG_SET_FIELD(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_MAX, 1024);
    REG_SET_FIELD(TIMG_RTCCALICFG2_REG(0), TIMG_RTC_CALI_TIMEOUT_THRES,
                  RTC_SLOW_CLK_150K_CAL_TIMEOUT_THRES(1024));
    CLEAR_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_START);
    SET_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_START);
    uint32_t counted = 0;
    for (unsigned poll = 0; poll < 4096; ++poll) {
        if (!stub_poll(watch)) break;
        if (GET_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_RDY)) {
            counted = REG_GET_FIELD(TIMG_RTCCALICFG1_REG(0), TIMG_RTC_CALI_VALUE);
            break;
        }
        if (GET_PERI_REG_MASK(TIMG_RTCCALICFG2_REG(0), TIMG_RTC_CALI_TIMEOUT)) break;
        esp_rom_delay_us(10);
    }
    CLEAR_PERI_REG_MASK(TIMG_RTCCALICFG_REG(0), TIMG_RTC_CALI_START);
    if (!counted || !stub_poll(watch)) return false;
    const uint64_t divider = UINT64_C(40) * 1024;
    const uint64_t calibrated = (((uint64_t)counted << 19) + divider / 2 - 1) / divider;
    if (calibrated > UINT32_MAX || !rtc_drift_period_valid((uint32_t)calibrated)) return false;
    *period = (uint32_t)calibrated;
    return true;
}

void RTC_IRAM_ATTR rtc_clock_wake_stub(void)
{
    const uint32_t cause = rtc_cntl_ll_get_wakeup_cause();
    const bool key = (cause & RTC_GPIO_TRIG_EN) != 0 || stub_key_seen();
    rtc_wake_watch_t watch;
    rtc_wake_watch_begin(&watch, rtc_clock_platform_ticks(), key);
    esp_default_wake_deep_sleep();
    rtc_clock_session_t *session = &rtc_clock_retained.session;
    const bool valid = rtc_clock_session_valid(session);
    if (!rtc_wake_should_maintain(cause == RTC_TIMER_TRIG_EN, key, valid,
                                  valid && session->maintenance_enabled && !rtc_clock_retained.maintenance_blocked)) {
        if (valid) stub_return_boot(&watch, RTC_CLOCK_FAULT_NONE);
        return;
    }
    if (!stub_poll(&watch)) {
        stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE : RTC_CLOCK_FAULT_GAP);
        return;
    }
    const soc_cpu_clk_src_t cpu_source = clk_ll_cpu_get_src();
    if ((cpu_source != SOC_CPU_CLK_SRC_XTAL && cpu_source != SOC_CPU_CLK_SRC_PLL) ||
        clk_ll_rtc_slow_get_src() != SOC_RTC_SLOW_CLK_SRC_RC_SLOW || clk_ll_xtal_load_freq_mhz() != 40) {
        stub_return_boot(&watch, RTC_CLOCK_FAULT_SOURCE);
        return;
    }
    uint32_t period;
    if (!stub_calibrate(&watch, &period)) {
        stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE :
                         (watch.gap_exceeded ? RTC_CLOCK_FAULT_GAP : RTC_CLOCK_FAULT_CALIBRATION));
        return;
    }
    const uint64_t commit_tick = rtc_clock_platform_ticks();
    if (!stub_poll(&watch)) {
        stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE : RTC_CLOCK_FAULT_GAP);
        return;
    }
    if (!rtc_drift_ledger_commit(&session->ledger, commit_tick, period)) {
        stub_return_boot(&watch, RTC_CLOCK_FAULT_LEDGER);
        return;
    }
    ++session->samples;
    if (watch.max_gap_upper_us > session->pre_tail_gap_upper_us) session->pre_tail_gap_upper_us = watch.max_gap_upper_us;
    rtc_clock_session_seal(session);
    const uint64_t interval_ticks = (RTC_CLOCK_MAINTENANCE_US << 19) / period;
    rtc_cntl_ll_set_wakeup_timer((rtc_clock_platform_ticks() + interval_ticks) & RTC_DRIFT_TICK_MASK);
    esp_set_deep_sleep_wake_stub(rtc_clock_wake_stub);
    if (!stub_poll(&watch)) {
        stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE : RTC_CLOCK_FAULT_GAP);
        return;
    }
    /* CRC 后只改栈和 MMIO；若更新保留数据，会破坏下次 ROM 的 CRC 验证。 */
    set_rtc_memory_crc();
    if (!stub_poll(&watch)) {
        stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE : RTC_CLOCK_FAULT_GAP);
        return;
    }
    rtc_cntl_ll_sleep_enable();
    __asm__ volatile("wfi");
    /* 实验回退仍待硬件证明：清 SLEEP_EN 不等于已证明同步取消了在途睡眠。 */
    CLEAR_PERI_REG_MASK(RTC_CNTL_STATE0_REG, RTC_CNTL_SLEEP_EN);
    stub_poll(&watch);
    stub_return_boot(&watch, watch.key_seen ? RTC_CLOCK_FAULT_NONE : RTC_CLOCK_FAULT_SLEEP_RETURNED);
}

bool rtc_clock_stub_install(void)
{
    if (esp_sleep_enable_timer_wakeup(RTC_CLOCK_MAINTENANCE_US) != ESP_OK) return false;
    esp_set_deep_sleep_wake_stub(rtc_clock_wake_stub);
    return true;
}
#endif
