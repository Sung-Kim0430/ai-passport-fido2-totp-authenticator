<p align="right">
  <strong>简体中文</strong> · <a href="esp-idf-5.5.6-upgrade.md">English</a>
</p>

# ESP-IDF 5.5.6 升级

## 当前选择的基线

本工作区使用 ESP-IDF 5.5.6、ESP32-C3 和 cmake_utilities 1.3.0。
BSP 要求 `>=5.5.6,<5.6.0`。LVGL 保持 9.5.0、esp_lvgl_port 保持 2.9.0、
button 保持 4.2.0、esp_codec_dev 保持 1.6.2；本次升级不代表同时升级这些运行时组件。

选择依据是官方补丁版说明：

- [ESP-IDF 5.5.4](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
  包含 ESP32-C3 NimBLE 主机连接丢失修复 IDFGH-17350。
- [ESP-IDF 5.5.5](https://github.com/espressif/esp-idf/releases/tag/v5.5.5)
  包含稳定分支的中间修复。
- [ESP-IDF 5.5.6](https://github.com/espressif/esp-idf/releases/tag/v5.5.6)
  包含非 ESP32 芯片 Flash 读取不禁用缓存、Wi-Fi supplicant 等修复。
  这些是相关修复，不是本应用速度或功耗已经实测改善的声明。

SDK 补丁升级不能代替应用 LVGL 内存不足的修复，也不能证明无线与 UI 组合稳定。
应用修复及验收标准需要单独保留。

## SDK 安装保持在本仓库外

本次本地验证使用独立源码目录 `$HOME/esp/esp-idf-v5.5.6`，工具目录为
`$HOME/.espressif-idf-5.5.6`。原 5.5.3 源码和环境未被替换。
这些路径是本机实际记录，不是要求其他开发者照搬的固定位置。

在类 Unix 系统全新安装时，先检查目标路径，并取得环境要求的联网／主机写入批准。
已有且验证通过的 checkout 应复用，不要覆盖克隆，也不要替换另一套 SDK 环境。
官方源码线路为：

```bash
export AI_PASSPORT_IDF_ROOT="$HOME/esp/esp-idf-v5.5.6"
export IDF_TOOLS_PATH="$HOME/.espressif-idf-5.5.6"
git clone --branch v5.5.6 --recursive \
    https://github.com/espressif/esp-idf.git "$AI_PASSPORT_IDF_ROOT"
"$AI_PASSPORT_IDF_ROOT/install.sh" esp32c3
```

原生 Windows 应在官方安装器选择 5.5.6 并使用其终端，不把 Unix shell 语法照搬到
PowerShell。历史 5.5.3 镜像记录不能证明 5.5.6 镜像或 tag 已经可用。

对于上述安装，只在当前 shell 激活：

```bash
export IDF_PATH="$HOME/esp/esp-idf-v5.5.6"
export IDF_TOOLS_PATH="$HOME/.espressif-idf-5.5.6"
unset IDF_PYTHON_ENV_PATH
. "$IDF_PATH/export.sh"
idf.py --version
```

各平台前置条件见[环境指南](environment-setup.zh_CN.md)。依赖升级提交不得包含
SDK、工具链、Python 环境、managed components、固件归档或调试日志。
不需要给工程添加 ESP-IDF 子模块。本地提交不代表获得 push 授权。

## cmake_utilities 对照

两轮使用相同 SDK 和应用源码摘要：
`a2737cb25b567410b70a1c8e6b95e61bb60d0bbc32bb3c3d7da21e9b014bd57d`。
两轮之间只改变构建辅助组件版本及对应依赖锁元数据。

| 检查 | A：1.1.1 | B：1.3.0 |
| --- | --- | --- |
| SDK | 5.5.6 | 5.5.6 |
| 仓库、主机测试、固件完整门禁 | PASS | PASS |
| 合并镜像与匹配 ELF 校验 | PASS | PASS |
| 应用镜像大小 | 1,809,024 字节 | 1,809,024 字节 |

所选 1.3.0 配置的额外真实 LVGL 绘制压力回归和网页测试也已通过。
A/B 对照期间其他已锁定组件没有变化。应用大小未变化；构建哈希不同本身
不能证明运行时行为不同。

这些是开发工作区的集成结果。SDK／依赖提交有意不包含尚未完成的应用、音频和 UI
改动。固件／ELF 哈希仅对应各自准确的归档包，不能重新构建后混用。

## 仍然需要的验收

交付新应用镜像前，在所选环境执行 `./tools/validate.sh`，分别报告 Build、Host tests、
Device tests 和 Unverified。版本一致性测试检查 manifest、锁文件、CI 标签及维护的
安装／构建说明，不是真机测试。

本次升级本身不能证明 BLE＋4 项列表、Wi-Fi＋4 项列表、ES8311 待机电流、整板
深睡电流或长期 RTC 漂移已经通过。不能把启动短测或主机测试当成这些结果。
烧录需要另行获得目标、准确镜像和数据范围的授权，不隐含全片擦除或 OTP/eFuse 编程授权。
