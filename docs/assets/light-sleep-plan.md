<p align="right"><a href="light-sleep-plan.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Automatic Light Sleep Implementation Plan

> Execute in the current application checkout using `superpowers:executing-plans`.
> Do not commit, push or flash without the corresponding user authorization.

**Goal:** Implement the approved [design](light-sleep-design.md) with independently
testable sleep accounting and normal application responsiveness.

**Architecture:** Collect successful SDK RTC substitution intervals in a bounded
platform accumulator. Reconcile them in the existing application task. Keep
backlight and LVGL timing changes in the BSP.

**Tech stack:** ESP32-C3, ESP-IDF 5.5.6, C11 host tests, LVGL 9.

- [x] Add failing arithmetic tests in `tests/test_rtc_light_sleep.c` for fractions,
  rejected requests, wrap, invalid periods and overflow. Implement the pure
  accumulator in `main/rtc_light_sleep.h`. Compile with
  `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_light_sleep.c -o /tmp/test_rtc_light_sleep`
  and run `/tmp/test_rtc_light_sleep`; require PASS.
- [x] Extend `tests/test_rtc_clock.c` with fake cumulative snapshots and verify
  failure before adding owner reconciliation in `main/rtc_clock.c`. Cover
  exactly-once UTC, awake coordinates, delayed references, errors and deep sleep.
  Run `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_clock.c main/rtc_history.c main/rtc_history_store.c main/rtc_drift.c -lm -o /tmp/test_rtc_clock`
  and `/tmp/test_rtc_clock`; require all existing and new scenarios to pass.
- [x] Isolate SDK callbacks/context access in `main/rtc_light_sleep_platform.c`;
  add platform-stub coverage and register both source and tests in
  `main/CMakeLists.txt` and `tools/validate.sh`. Verify registration failure,
  rejection, success and inhibition using the real adapter with SDK stubs.
- [x] Enable required settings in `sdkconfig.defaults`; connect initialization
  and event draining in `main/main.c`. Add counters to existing numeric startup
  diagnostics. Update backlight clock/retention in
  `components/bsp/src/bsp_display.c` and LVGL period in
  `components/bsp/src/bsp_display_lvgl.c`. Exercise existing BSP host tests.
- [x] Update the application README and paired design documents with final
  behavior and limits. Run `./tools/validate.sh` in ESP-IDF 5.5.6, review the diff
  and verified firmware archive, and offer separately authorized device testing.

All tests run against actual arithmetic/state-machine/adapter code. SDK stubs
replace hardware boundaries only. Configuration checks supplement runtime tests.
