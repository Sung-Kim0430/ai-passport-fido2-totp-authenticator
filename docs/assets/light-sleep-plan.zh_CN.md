<p align="right"><strong>简体中文</strong> · <a href="light-sleep-plan.md">English</a></p>

# 自动浅睡眠实施计划

> 在当前应用检出目录中使用 `superpowers:executing-plans` 执行。
> 未获相应用户授权，不提交、不推送、不刷入。

**目标：** 实现已批准的[设计](light-sleep-design.zh_CN.md)，支持独立测试睡眠记账并维持应用响应。

**架构：** 平台累计器有界收集 SDK 成功替换的 RTC 区间，由现有应用任务结算。
背光和 LVGL 定时修改保留在 BSP。

**技术栈：** ESP32-C3、ESP-IDF 5.5.6、C11 主机测试、LVGL 9。

- [x] 在 `tests/test_rtc_light_sleep.c` 添加先失败的算术测试，覆盖小数、拒绝请求、回绕、
  非法周期和溢出，再在 `main/rtc_light_sleep.h` 实现纯累计器。
  运行 `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_light_sleep.c -o /tmp/test_rtc_light_sleep`
  及 `/tmp/test_rtc_light_sleep`，要求 PASS。
- [x] 在 `tests/test_rtc_clock.c` 添加模拟累计快照，验证失败后，才在 `main/rtc_clock.c`
  添加所有者结算。覆盖 UTC 只修正一次、清醒坐标、延迟参考、错误和深睡眠。
  运行 `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_clock.c main/rtc_history.c main/rtc_history_store.c main/rtc_drift.c -lm -o /tmp/test_rtc_clock`
  及 `/tmp/test_rtc_clock`，要求已有和新增场景全部通过。
- [x] 将 SDK 回调与上下文访问隔离在 `main/rtc_light_sleep_platform.c`，
  添加平台桩测试，并在 `main/CMakeLists.txt` 和 `tools/validate.sh` 注册源码及测试。
  用实际适配器和 SDK 桩验证注册失败、拒绝、成功及阻止睡眠。
- [x] 在 `sdkconfig.defaults` 启用所需配置，在 `main/main.c` 接入初始化和事件结算，
  在现有数值启动诊断中加入计数。在 `components/bsp/src/bsp_display.c` 修改背光时钟和保持方式，
  在 `components/bsp/src/bsp_display_lvgl.c` 修改 LVGL 周期，并运行已有 BSP 主机测试。
- [x] 更新应用 README 和双语设计文档，说明最终行为与限制。
  在 ESP-IDF 5.5.6 中运行 `./tools/validate.sh`，审阅差异及已验证固件归档，
  并提议另行授权的设备测试。

所有测试运行实际算术、状态机或适配器；SDK 桩仅替换硬件边界。
配置检查只作为运行测试的补充。
