<p align="right"><strong>简体中文</strong> · <a href="fido-rp-storage-plan.md">English</a></p>

# 完整 RP ID 存储实施计划

**目标：** 按完整的受支持 RP ID 隔离 BLE FIDO 站点材料，迁移时保持已有密钥有效。

**架构：** 可在主机测试的存储模块编码版本化记录，包含完整 RP ID、盐、用户句柄和有效／已删除状态。NVS 适配层使用 SHA-256 派生的 15 字节键，每次访问都检查完整标识，遇到摘要碰撞时拒绝操作。保留旧盐供按需迁移；独立删除标记阻止被删除的凭据从旧记录中恢复；重新注册使用新盐，拒绝旧允许列表凭据。列表先放完整标识，再放旧前缀，保留八行上限。延续现有每个 RP 一个凭据的实验模型及 63 字节 RP ID 上限。

**实施方式：** 在当前已有修改的工作区内继续实施，保留之前的修改。本轮未经新授权不提交、不推送、不刷入。

- [x] 用失败的回归测试复现旧版同前缀冲突。新增 `tests/test_auth_fido_store.c`，覆盖独立盐与句柄、旧材料保留、独立删除、删除后重新注册、损坏记录、摘要碰撞和读取／写入／提交失败。
- [x] 新增 `main/auth_fido_store.h` 和 `main/auth_fido_store.c`，通过读取、写入、随机数、摘要回调实现有界序列化和迁移。盐与句柄保存在同一记录中；存储错误时不重新生成材料，已存在的新记录损坏时不回退旧格式。
- [x] 新增 `main/auth_fido_store_platform.c`，实现 NVS 和 SHA-256 适配，加入 `main/CMakeLists.txt`。新材料和已可读材料均确认提交；独立删除保留共享旧键。`main/auth_ble.c` 保留现有种子和计数器契约。
- [x] 更新 `main/auth_ble.c`、`main/auth_storage.h`、`main/auth_storage.c`、`main/main.c`、`main/auth_ui.h` 和 `main/auth_ui.c`：所有操作使用完整 RP ID；完整名称滚动显示；旧前缀记录只读并提示迁移。删除只影响选中的 RP。
- [x] 在 `tools/validate.sh` 注册主机测试，更新 `tests/test_ble_material.py`，运行 `./tools/validate.sh --static`；按需覆盖真实适配器故障及界面／按键集成。
- [x] 更新 `README.md` 及成对审查记录，将未修复限制改为迁移契约。在 ESP-IDF 5.5.6 中运行 `./tools/validate.sh`，进行独立代码复核、验证归档产物并邀请设备测试。

**验收：** 前 12 字符相同的两个 RP ID 各自保留用户句柄；删除和重新注册其中一个不改变另一个凭据。迁移旧记录后派生凭据必须与原先一致。旧前缀冲突可能已经覆盖用户句柄，丢失的历史值无法恢复。旧前缀不能确定完整 RP，因此不允许独立删除；Reset FIDO 仍是明确的整个命名空间擦除操作。

**待设备验证：** 两个同前缀站点注册／登录、升级前凭据迁移登录、断电持久化、删除和重新注册隔离、完整名称显示及旧记录按键。主机测试和构建不能代替这些结果。

## 验证与交付

2026-11-17，ESP-IDF v5.5.6 完整 `./tools/validate.sh` 门禁通过，明确取消设置 `COMMANDCODE_SCRATCHPAD`。仓库检查、主机测试、固件构建、真实 NVS 容量测试、RTC 唤醒检查和合并布局验证均通过。主机存储用例另通过 ASan／UBSan；回归先复现旧前缀冲突及定期刷新重置滚动动画，再验证修复。两轮独立复核无阻塞问题；归档单独重新校验通过。

Build: PASS。Host tests: PASS。Device tests: NOT RUN。Unverified：上方设备验收清单。本轮未刷入、未提交、未推送；只读检测发现 `/dev/cu.usbmodem101`。升级测试应使用兼容分段镜像保留 NVS 和 RTC 历史；从 0x0 写入合并镜像可能覆盖存储数据。

Full-image SHA-256: `eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a`.

ELF SHA-256: `3b9f26135a4436cb175a0b911126742dd0a87baba5b8caf0e4de6fe68f5333e5`.

`build/firmware/eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a/`: application 1,877,424 bytes; merged image 1,942,960 bytes.

## 单独授权的应用刷入 — 2026-11-17

用户明确要求刷入后，重新验证了未改动的 `eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a` 归档。设备出现在 `/dev/cu.usbmodem101`；esptool 确认 ESP32-C3 和 8 MB Flash。读取 `0x8000` 的 3072 字节分区元数据，与归档分区表逐字节一致。

仅在 `0x10000` 写入已校验的 1,877,424 字节应用。esptool 报告擦写扇区为 `0x10000–0x1DAFFF`，写入数据摘要校验成功，复位前独立 `verify_flash` 也确认摘要一致。引导程序、分区表、安全 NVS、PHY 和 `rtc_hist` 均不在写入目标中。未执行整片擦除、重新构建、提交或推送。

随后进行了有界 15 秒串口观察，捕获到 `USB_UART_CHIP_RESET` 后启动、匹配的 `3b9f26135…` ELF 前缀及 ESP-IDF v5.5.6。应用报告初始化成功，显示／LVGL／按键完成初始化，自动 Light sleep 已启用；观察窗口内未发现 panic、看门狗或重复启动。音频进入低功耗时出现两条 I2S“通道尚未启用”错误日志，随后应用继续完成初始化；这不能算音频功能验收通过。RTC 记账报告 `restored=0`，精度测量前需用手机重新建立时间参考。串口连接已关闭并释放。

刷入／读回校验：PASS。启动观察：PASS。Build: PASS；Host tests: PASS，沿用未变更的完整门禁产物。Device tests: NOT RUN，指 FIDO 功能验收矩阵。Unverified：屏幕／PIN／账号保留、旧站点登录及迁移、同前缀注册／删除隔离、完整名称滚动、断电持久化与音频行为。已请求用户反馈；此前未写入设备的交接状态由本次单独授权刷入记录补充更新。

用户反馈缺少兼容 BLE FIDO 的客户端／设备，无法进行该项测试。因此本次交接暂缓 FIDO 端到端验收，不记为通过，也不将此作为完成已授权刷入的前置条件。该回复没有确认屏幕／按键／账号保留结果。
