// 运行真实 BSP 回调及共享采样器，仅以公开接口替代 SDK 和 ADC。
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../main/key_guard.h"
#include "../components/bsp/src/bsp_button.c"

_Static_assert(BSP_BTN_PRESS == 0 && BSP_BTN_CLICK == 1 && BSP_BTN_DOUBLE == 2 &&
               BSP_BTN_LONG == 3 && BSP_BTN_RELEASE == 4 && BSP_BTN_IDLE == 5,
               "existing BSP event values must not change");

struct button_dev_t {
    button_driver_t *driver;
    bool live;
    button_cb_t callbacks[BUTTON_EVENT_MAX];
    void *users[BUTTON_EVENT_MAX];
};
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, fail_create, fail_callback;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events, updated_events;
static int64_t clock_us;
static bool button_timer_running, core_callback_inflight;
static int fail_timer_stop, fail_barrier_create, fail_barrier_stop, hold_barrier;
struct test_timer { esp_timer_create_args_t args; bool active; };
static struct test_timer barrier;
esp_err_t iot_button_stop(void) {
    if (fail_timer_stop) return ESP_FAIL;
    if (!button_timer_running) return ESP_ERR_INVALID_STATE;
    button_timer_running = false;
    return ESP_OK;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *h) {
    assert(args->dispatch_method == ESP_TIMER_TASK);
    if (fail_barrier_create) return ESP_ERR_NO_MEM;
    barrier = (struct test_timer){.args = *args}; *h = &barrier; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t timeout) {
    assert(h == &barrier && !button_timer_running && timeout > 0);
    h->active = true; return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t h, uint32_t timeout) {
    (void)timeout; assert(h == &barrier);
    if (fail_barrier_stop) return ESP_ERR_TIMEOUT;
    h->active = false; return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t h) {
    assert(h == &barrier && !h->active); return ESP_OK;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc) return ESP_ERR_NO_MEM;
    adc_live = 1; *h = &adc_token; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    assert(h == &adc_token && adc_live && !live_buttons);
    adc_live = 0; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw) {
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read) return ESP_FAIL;
    *raw = raw_mv; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *h) {
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal) return ESP_ERR_NO_MEM;
    cal_live = 1; *h = &cal_token; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) {
    assert(h == &cal_token && cal_live && !live_buttons);
    cal_live = 0; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    assert(h == &cal_token && cal_live);
    if (fail_convert) { *mv = 0; return ESP_FAIL; }
    *mv = raw; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }

// 深睡交接使用的 GPIO 与延迟替身。
static int gpio_direction_calls, gpio_pull_calls, gpio_level_calls, delay_calls, fail_gpio;
static gpio_num_t gpio_last_pin;
static gpio_mode_t gpio_last_mode;
static gpio_pull_mode_t gpio_last_pull;
static int gpio_level = 1;

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) {
    ++gpio_direction_calls; gpio_last_pin = pin; gpio_last_mode = mode;
    return fail_gpio ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull) {
    ++gpio_pull_calls; gpio_last_pin = pin; gpio_last_pull = pull;
    return fail_gpio ? ESP_FAIL : ESP_OK;
}
int gpio_get_level(gpio_num_t pin) {
    (void)pin; ++gpio_level_calls; return gpio_level;
}
void vTaskDelay(TickType_t ticks) {
    ++delay_calls;
    clock_us += (int64_t)ticks * 1000;
    if (barrier.active && !hold_barrier) {
        /* Serial timer task finishes the old button core callback first. */
        core_callback_inflight = false;
        barrier.active = false;
        barrier.args.callback(barrier.args.arg);
    }
}
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver, button_handle_t *h) {
    // 判定门限必须由 BSP 显式下发；ADC 没有唤醒中断，不能停止轮询。
    assert(cfg->short_press_time == BSP_BTN_SHORT_PRESS_MS);
    assert(cfg->long_press_time == BSP_BTN_LONG_PRESS_MS);
    assert(!driver->enable_power_save);
    if (++create_calls == fail_create) return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live) continue;
        buttons[i] = (struct button_dev_t){ .driver = (button_driver_t *)driver, .live = true };
        *h = &buttons[i]; ++live_buttons; button_timer_running = true;
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false); return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h) {
    assert(h && h->live && adc_live && cal_live);
    assert(!button_timer_running && !core_callback_inflight);
    if (fail_delete) return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false; --live_buttons; return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *u) {
    (void)args;
    assert(h->live && ev < BUTTON_EVENT_MAX && !h->callbacks[ev]);
    const int before = reads;
    cb(h, u); // 模拟初始化中途的定时器抢占，不能产生用户事件或残留手势状态。
    assert(!s_ready && !s_active_gestures && !s_release_qualifying && !s_idle_notified);
    assert(!s_sample_valid && s_release_since == 0 && reads == before);
    if (++callback_calls == fail_callback) return ESP_ERR_NO_MEM;
    h->callbacks[ev] = cb;
    h->users[ev] = u;
    return ESP_OK;
}
static void fire(bsp_btn_t btn, button_event_t ev) {
    button_handle_t h = &buttons[btn];
    assert(h->live && h->callbacks[ev]);
    h->callbacks[ev](h, h->users[ev]);
}
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    if (ev == BSP_BTN_IDLE) { assert(u == &events); return; }
    assert(btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && u == &events);
    ++events;
}
static void updated_event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    if (ev == BSP_BTN_IDLE) { assert(u == &updated_events); return; }
    assert(btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && u == &updated_events);
    ++updated_events;
}
static void reset_faults(void) {
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = create_calls = callback_calls = 0;
}
static void assert_clean(void) {
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    assert(!s_active_gestures && !s_release_qualifying && !s_idle_notified);
    assert(!s_sample_valid && s_sample_mv == -1 && s_sample_time == 0 && s_release_since == 0);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_btn[i]);
}

// 先回收驱动与共享 ADC，再恢复数字上拉；失败时不得假报交接成功。
static void check_deep_sleep_prepare(void) {
    reset_faults();
    gpio_direction_calls = gpio_pull_calls = gpio_level_calls = delay_calls = 0;
    fail_gpio = 0; gpio_level = 1;
    assert(bsp_button_init(event_cb, &events) == ESP_OK);

    int level = -1;
    assert(bsp_button_prepare_deep_sleep(&level) == ESP_OK);
    assert(level == 1);
    assert(gpio_direction_calls == 1 && gpio_pull_calls == 1 && gpio_level_calls == 1);
    assert(delay_calls >= 1);
    assert(gpio_last_pin == BSP_BTN_GPIO);
    assert(gpio_last_mode == GPIO_MODE_INPUT);
    assert(gpio_last_pull == GPIO_PULLUP_ONLY);
    assert_clean();

    // 仍按住时返回低电平，供调用者拒绝入睡。
    gpio_level = 0; level = -1;
    assert(bsp_button_prepare_deep_sleep(&level) == ESP_OK);
    assert(level == 0);

    fail_gpio = 1; level = -1;
    assert(bsp_button_prepare_deep_sleep(&level) == ESP_FAIL);
    assert(level == 0);
    fail_gpio = 0;
    assert(bsp_button_prepare_deep_sleep(NULL) == ESP_OK);

    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    gpio_direction_calls = gpio_pull_calls = gpio_level_calls = 0;
    fail_delete = 1; level = -1;
    assert(bsp_button_prepare_deep_sleep(&level) != ESP_OK);
    assert(level == 0);
    assert(gpio_direction_calls == 0 && gpio_pull_calls == 0 && gpio_level_calls == 0);
    fail_delete = 0;
    assert(button_cleanup() == ESP_OK);
    assert_clean();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
}
static void retry_success(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(callback_calls == BSP_BTN_COUNT * 6);
    const int before = updated_events;
    assert(bsp_button_init(updated_event_cb, &updated_events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && callback_calls == BSP_BTN_COUNT * 6);
    fire(BSP_BTN_OK, BUTTON_SINGLE_CLICK);
    assert(updated_events == before + 1);
    assert(button_cleanup() == ESP_OK); assert_clean();
}
// One button-component polling cycle (5 ms): every driver asks for its level.
static int poll(void) {
    clock_us += 5000;
    int active = -1;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_drivers[i].base.get_key_level(&s_drivers[i].base) == BUTTON_ACTIVE) {
            assert(active == -1);   // never two keys at once
            active = i;
        }
    }
    return active;
}
// Hold a voltage for ms and return the decoded key afterwards.
static int hold(int mv, int ms) {
    raw_mv = mv;
    int active = -1;
    for (int t = 0; t < ms; t += 5) active = poll();
    return active;
}
static void check_voltage(int mv, int expected) {
    assert(hold(mv, 200) == expected);   // longer than confirm and hold windows
    const int before = reads;
    assert(poll() == expected);
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);   // one shared sample
}

// Issue #97: a contact break inside a held key sweeps the voltage up through the
// windows above it. The lock must neither report the neighbour nor end the hold.
static void check_key_lock(void) {
    const int released = 3300, up = 50, down = 300, ok = 600;
    assert(hold(released, 200) == -1);

    // Press-down: a key must be stable for the confirm time, and the windows swept
    // through while the voltage falls are never reported.
    assert(hold(ok, 20) == -1 && hold(down, 20) == -1);
    assert(hold(up, BSP_BTN_CONFIRM_MS - 10) == -1);
    assert(hold(up, 20) == BSP_BTN_UP);

    // Held UP: a contact break dwells in DOWN, OK, then released for tens of ms
    // each, far longer than the component's ~10 ms debounce, then recovers.
    const uint32_t glitches = bsp_button_glitch_count();
    for (int i = 0; i < 5; ++i) {
        assert(hold(down, 30) == BSP_BTN_UP);
        assert(hold(ok, 40) == BSP_BTN_UP);
        assert(hold(released, 40) == BSP_BTN_UP);
        assert(hold(up, 30) == BSP_BTN_UP);
    }
    assert(bsp_button_glitch_count() == glitches + 5);

    // A real release is reported once it lasts the hold time, including the sweep
    // through the upper windows on the way out.
    assert(hold(down, 10) == BSP_BTN_UP && hold(ok, 10) == BSP_BTN_UP);
    assert(hold(released, BSP_BTN_HOLD_MS - 10) == BSP_BTN_UP);
    assert(hold(released, 20) == -1);

    // A failed read counts as "no key": it cannot press, and a held key survives it.
    fail_read = 1; assert(hold(0, 200) == -1); fail_read = 0;
    assert(hold(ok, 100) == BSP_BTN_OK);
    fail_read = 1; assert(hold(0, 60) == BSP_BTN_OK); fail_read = 0;
    assert(hold(ok, 20) == BSP_BTN_OK);
    assert(hold(released, 200) == -1);

    // Re-initialization after a teardown starts from "no key" again.
    assert(hold(down, 100) == BSP_BTN_DOWN);
    assert(button_cleanup() == ESP_OK);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    raw_mv = released;
    assert(poll() == -1);
    assert(hold(released, 200) == -1);
}

static const int64_t period_us = CONFIG_BUTTON_PERIOD_TIME_MS * 1000LL;
static const int64_t stable_us = CONFIG_BUTTON_PERIOD_TIME_MS * CONFIG_BUTTON_DEBOUNCE_TICKS * 1000LL;
static key_guard_t wake_guard;
static struct { bsp_btn_t btn; bsp_btn_ev_t ev; } event_log[64];
static int log_count, idle_count, delivered;
static void record_event(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    assert(u == event_log && log_count < (int)(sizeof(event_log) / sizeof(event_log[0])));
    event_log[log_count].btn = btn;
    event_log[log_count++].ev = ev;
    if (ev == BSP_BTN_IDLE) ++idle_count;
    if (!key_guard_consume(&wake_guard, ev == BSP_BTN_IDLE)) ++delivered;
}
static void start_behavior(void) {
    assert(button_cleanup() == ESP_OK);
    assert_clean(); reset_faults();
    log_count = idle_count = delivered = 0;
    raw_mv = 3300;
    clock_us = 0;
    key_guard_arm(&wake_guard);
    const int before = reads;
    assert(bsp_button_init(record_event, event_log) == ESP_OK);
    assert(reads == before && log_count == 0 && wake_guard.armed);
    assert(callback_calls == BSP_BTN_COUNT * 6);
}
static void poll_pad(int mv, int64_t advance_us) {
    raw_mv = mv; clock_us += advance_us;
    const int before = reads;
    for (int i = BSP_BTN_COUNT - 1; i >= 0; --i) {
        const uint8_t level = buttons[i].driver->get_key_level(buttons[i].driver);
        assert(level == (s_decoder.locked == i));
    }
    // 即便 ADC 失败，同一轮三个键也只能共享一次采样尝试。
    assert(reads - before == (fail_read ? 1 : CONFIG_ADC_BUTTON_SAMPLE_TIMES));
}
static void check_stable_idle(void) {
    const int before = idle_count;
    poll_pad(3300, period_us);
    assert(idle_count == before);
    poll_pad(3300, stable_us - 1000);
    assert(idle_count == before && wake_guard.armed);
    poll_pad(3300, 1000);
    assert(idle_count == before + 1 && !wake_guard.armed && delivered == 0);
    assert(event_log[log_count - 1].ev == BSP_BTN_IDLE);
    poll_pad(3300, stable_us * 2);
    assert(idle_count == before + 1);
}
static void check_startup_released(void) {
    start_behavior();
    // 初始化之前已松开，没有 PRESS_UP 也必须产生首次空闲。
    check_stable_idle();
    assert(log_count == 1);
    poll_pad(600, period_us);
    fire(BSP_BTN_OK, BUTTON_PRESS_DOWN);
    poll_pad(3300, period_us);
    fire(BSP_BTN_OK, BUTTON_PRESS_UP);
    fire(BSP_BTN_OK, BUTTON_SINGLE_CLICK);
    fire(BSP_BTN_OK, BUTTON_PRESS_END);
    assert(delivered == 3);
    poll_pad(3300, period_us);
    poll_pad(3300, stable_us);
    assert(idle_count == 2 && delivered == 3);
}
static void check_delayed_tail(bool twice) {
    start_behavior();
    poll_pad(600, period_us);
    fire(BSP_BTN_OK, BUTTON_PRESS_DOWN);
    poll_pad(3300, period_us);
    fire(BSP_BTN_OK, BUTTON_PRESS_UP);
    if (twice) {
        // 同一个未结束手势中的第二次按下必须继续隔离。
        poll_pad(600, period_us);
        fire(BSP_BTN_OK, BUTTON_PRESS_DOWN);
        poll_pad(3300, period_us);
        fire(BSP_BTN_OK, BUTTON_PRESS_UP);
    }
    poll_pad(3300, stable_us * 3);
    assert(wake_guard.armed && idle_count == 0 && delivered == 0);
    fire(BSP_BTN_OK, twice ? BUTTON_DOUBLE_CLICK : BUTTON_SINGLE_CLICK);
    assert(wake_guard.armed && delivered == 0);
    fire(BSP_BTN_OK, BUTTON_PRESS_END);
    assert(wake_guard.armed && idle_count == 0);
    check_stable_idle();
    assert(log_count == (twice ? 6 : 4));
    assert(event_log[0].ev == BSP_BTN_PRESS && event_log[1].ev == BSP_BTN_RELEASE);
    assert(event_log[log_count - 2].ev == (twice ? BSP_BTN_DOUBLE : BSP_BTN_CLICK));
    poll_pad(0, period_us);
    fire(BSP_BTN_UP, BUTTON_PRESS_DOWN);
    poll_pad(3300, period_us);
    fire(BSP_BTN_UP, BUTTON_PRESS_UP);
    fire(BSP_BTN_UP, BUTTON_SINGLE_CLICK);
    fire(BSP_BTN_UP, BUTTON_PRESS_END);
    assert(delivered == 3);
}
static void check_held_beyond_timeout(void) {
    start_behavior();
    poll_pad(0, period_us);
    fire(BSP_BTN_UP, BUTTON_PRESS_DOWN);
    poll_pad(0, 11000000);
    fire(BSP_BTN_UP, BUTTON_LONG_PRESS_START);
    assert(wake_guard.armed && delivered == 0);
    // 长按仍活跃时，即使出现稳定松开采样也不能提前撤防。
    poll_pad(3300, period_us);
    poll_pad(3300, stable_us * 3);
    assert(wake_guard.armed && idle_count == 0);
    fire(BSP_BTN_UP, BUTTON_PRESS_UP);
    assert(wake_guard.armed);
    fire(BSP_BTN_UP, BUTTON_PRESS_END);
    check_stable_idle();
    assert(log_count == 4 && event_log[1].ev == BSP_BTN_LONG && event_log[2].ev == BSP_BTN_RELEASE);
}
static void check_invalid_and_bounce(void) {
    start_behavior();
    poll_pad(3300, period_us);
    poll_pad(3300, stable_us - 1000);
    fail_read = 1;
    poll_pad(3300, 1000);
    assert(idle_count == 0 && wake_guard.armed);
    fail_read = 0;
    poll_pad(3300, period_us);
    poll_pad(3300, stable_us - 1000);
    fail_convert = 1;
    poll_pad(3300, 1000);
    assert(idle_count == 0 && wake_guard.armed);
    fail_convert = 0;
    poll_pad(3300, period_us);
    poll_pad(3300, stable_us - 1000);
    poll_pad(0, 1000); // 未形成完整消抖事件的按下也必须重置松开计时。
    assert(idle_count == 0);
    poll_pad(-1, period_us);
    check_stable_idle();
    // 已空闲后发生无效采样，同样需要重新确认一次空闲转变。
    fail_read = 1;
    poll_pad(3300, period_us);
    fail_read = 0;
    poll_pad(BSP_BTN_MV_RELEASED_MIN, period_us);
    poll_pad(BSP_BTN_MV_RELEASED_MIN, stable_us - 1000);
    assert(idle_count == 1);
    poll_pad(BSP_BTN_MV_RELEASED_MIN, 1000);
    assert(idle_count == 2 && delivered == 0);
}
static void check_ladder_handoff(void) {
    start_behavior();
    const int voltages[] = {0, 300, 600};
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        poll_pad(voltages[i], period_us);
        if (i) fire((bsp_btn_t)(i - 1), BUTTON_PRESS_UP);
        fire((bsp_btn_t)i, BUTTON_PRESS_DOWN);
    }
    poll_pad(3300, period_us);
    fire(BSP_BTN_OK, BUTTON_PRESS_UP);
    // 三个独立按键的延迟尾事件可以重叠；每个 END 只能清除自己的那一位。
    const bsp_btn_t order[] = {BSP_BTN_DOWN, BSP_BTN_UP, BSP_BTN_OK};
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        fire(order[i], BUTTON_SINGLE_CLICK);
        fire(order[i], BUTTON_PRESS_END);
        if (i < BSP_BTN_COUNT - 1) {
            poll_pad(3300, stable_us * 3);
            assert(wake_guard.armed && idle_count == 0 && delivered == 0);
        }
    }
    check_stable_idle();
    assert(log_count == 10);
}
static void check_second_key_before_first_end(void) {
    start_behavior();
    poll_pad(0, period_us);
    fire(BSP_BTN_UP, BUTTON_PRESS_DOWN);
    poll_pad(3300, period_us);
    fire(BSP_BTN_UP, BUTTON_PRESS_UP);
    // 第二颗键的原始按下先于其消抖回调，旧手势 END 也不能把它当作空闲。
    poll_pad(300, period_us);
    fire(BSP_BTN_UP, BUTTON_SINGLE_CLICK);
    fire(BSP_BTN_UP, BUTTON_PRESS_END);
    poll_pad(300, stable_us * 3);
    assert(wake_guard.armed && idle_count == 0);
    fire(BSP_BTN_DOWN, BUTTON_PRESS_DOWN);
    poll_pad(3300, period_us);
    fire(BSP_BTN_DOWN, BUTTON_PRESS_UP);
    poll_pad(3300, stable_us * 3);
    assert(wake_guard.armed && idle_count == 0);
    fire(BSP_BTN_DOWN, BUTTON_SINGLE_CLICK);
    fire(BSP_BTN_DOWN, BUTTON_PRESS_END);
    check_stable_idle();
}
static void check_key_lock_with_wake_guard(void) {
    start_behavior();
    poll_pad(0, period_us);
    poll_pad(0, BSP_BTN_CONFIRM_MS * 1000LL);
    assert(s_decoder.locked == BSP_BTN_UP);
    fire(BSP_BTN_UP, BUTTON_PRESS_DOWN);

    // A contact break must preserve both the decoded key and wake isolation.
    poll_pad(300, period_us);
    poll_pad(600, 30000);
    poll_pad(3300, 40000);
    poll_pad(0, 30000);
    assert(s_decoder.locked == BSP_BTN_UP);
    assert(wake_guard.armed && idle_count == 0 && delivered == 0);
    fire(BSP_BTN_UP, BUTTON_LONG_PRESS_START);

    poll_pad(3300, period_us);
    poll_pad(3300, BSP_BTN_HOLD_MS * 1000LL);
    assert(s_decoder.locked == -1 && wake_guard.armed);
    fire(BSP_BTN_UP, BUTTON_PRESS_UP);
    fire(BSP_BTN_UP, BUTTON_PRESS_END);
    check_stable_idle();
    assert(log_count == 4 && delivered == 0);

    // The first new confirmed gesture after idle reaches the application.
    poll_pad(600, period_us);
    poll_pad(600, BSP_BTN_CONFIRM_MS * 1000LL);
    assert(s_decoder.locked == BSP_BTN_OK);
    fire(BSP_BTN_OK, BUTTON_PRESS_DOWN);
    assert(delivered == 1);
}
static void check_cleanup_quiescence(void) {
    reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    core_callback_inflight = true;
    fail_timer_stop = 1;
    assert(button_cleanup() != ESP_OK);
    assert(live_buttons == BSP_BTN_COUNT && adc_live && cal_live);
    fail_timer_stop = 0; fail_barrier_create = 1;
    assert(button_cleanup() != ESP_OK);
    assert(live_buttons == BSP_BTN_COUNT && adc_live && cal_live);
    fail_barrier_create = 0; hold_barrier = 1;
    assert(button_cleanup() == ESP_ERR_TIMEOUT);
    assert(live_buttons == BSP_BTN_COUNT && adc_live && cal_live && core_callback_inflight);
    fail_barrier_stop = 1;
    assert(button_cleanup() == ESP_ERR_TIMEOUT);
    assert(live_buttons == BSP_BTN_COUNT && adc_live && cal_live && barrier.active);
    fail_barrier_stop = 0;
    /* Retry while the old sentinel is still pending must drain it first. */
    hold_barrier = 0;
    assert(button_cleanup() == ESP_OK);
    assert_clean();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    core_callback_inflight = true;hold_barrier = 1;
    assert(button_cleanup() == ESP_ERR_TIMEOUT);
    /* A late acknowledgement after timeout cannot authorize an earlier deletion. */
    hold_barrier = 0;
    vTaskDelay(1);
    core_callback_inflight = true;
    assert(button_cleanup() == ESP_OK);
    assert_clean();
}

int main(void) {
    check_cleanup_quiescence();
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT * 6; ++i) {
        reset_faults(); fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    reset_faults(); fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    assert(events == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    fire(BSP_BTN_OK, BUTTON_SINGLE_CLICK); assert(events == 1);
    check_voltage(0, BSP_BTN_UP); check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN); check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK); check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1); check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    check_key_lock();
    fail_read = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0; fail_convert = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_delete = 1; assert(button_cleanup() != ESP_OK);
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    // 删除失败仍保留 ADC 所有权，但残留驱动的回调不能采样、改状态或通知应用。
    const int before_reads = reads, before_events = events;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        fire((bsp_btn_t)i, BUTTON_PRESS_DOWN);
        fire((bsp_btn_t)i, BUTTON_SINGLE_CLICK);
        fire((bsp_btn_t)i, BUTTON_PRESS_END);
        assert(!buttons[i].driver->get_key_level(buttons[i].driver));
    }
    assert(reads == before_reads && events == before_events && !s_active_gestures);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0; assert(button_cleanup() == ESP_OK); retry_success();
    check_deep_sleep_prepare();
    check_startup_released();
    check_delayed_tail(false);
    check_delayed_tail(true);
    check_held_beyond_timeout();
    check_invalid_and_bounce();
    check_ladder_handoff();
    check_second_key_before_first_end();
    check_key_lock_with_wake_guard();
    assert(button_cleanup() == ESP_OK); assert_clean();
    puts("BSP button fault-injection, key-lock, and wake-gesture tests: PASS");
}
