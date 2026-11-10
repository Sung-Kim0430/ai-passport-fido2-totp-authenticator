<p align="right">
  <a href="esp-idf-5.5.6-upgrade.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# ESP-IDF 5.5.6 Upgrade

## Selected baseline

This checkout selects ESP-IDF 5.5.6 for ESP32-C3 and cmake_utilities 1.3.0.
The BSP requires `>=5.5.6,<5.6.0`. LVGL remains 9.5.0, esp_lvgl_port 2.9.0,
button 4.2.0, and esp_codec_dev 1.6.2; this upgrade does not imply an update of
those runtime components.

The decision follows the official patch-release notes:

- [ESP-IDF 5.5.4](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
  includes a fix for ESP32-C3 NimBLE host connection loss, IDFGH-17350.
- [ESP-IDF 5.5.5](https://github.com/espressif/esp-idf/releases/tag/v5.5.5)
  contains the intervening stable-branch fixes.
- [ESP-IDF 5.5.6](https://github.com/espressif/esp-idf/releases/tag/v5.5.6)
  includes non-ESP32-chip Flash reads without disabling cache and Wi-Fi
  supplicant fixes. These are relevant fixes, not a measured application speed
  or power-consumption claim.

An SDK patch upgrade does not repair insufficient application LVGL memory or
prove radio/UI combinations stable. Keep application fixes and their acceptance
criteria separate.

## Keep SDK installations outside this repository

The local validation used an independent checkout at
`$HOME/esp/esp-idf-v5.5.6` and tools under `$HOME/.espressif-idf-5.5.6`.
The previous 5.5.3 checkout and environment were not replaced. Those paths are
observations, not mandatory installation locations for another developer.

For a new Unix-like installation, first inspect the chosen paths and obtain
any required network/host-write approval. Reuse a verified existing checkout;
do not clone over it or replace another SDK environment. The official source
route is:

```bash
export AI_PASSPORT_IDF_ROOT="$HOME/esp/esp-idf-v5.5.6"
export IDF_TOOLS_PATH="$HOME/.espressif-idf-5.5.6"
git clone --branch v5.5.6 --recursive \
    https://github.com/espressif/esp-idf.git "$AI_PASSPORT_IDF_ROOT"
"$AI_PASSPORT_IDF_ROOT/install.sh" esp32c3
```

On native Windows, select 5.5.6 in the official installer and use its terminal;
do not copy Unix shell syntax into PowerShell. No 5.5.6 mirror/tag availability
is established by the historical 5.5.3 mirror observations.

For the installation above, activate it in the current shell only:

```bash
export IDF_PATH="$HOME/esp/esp-idf-v5.5.6"
export IDF_TOOLS_PATH="$HOME/.espressif-idf-5.5.6"
unset IDF_PYTHON_ENV_PATH
. "$IDF_PATH/export.sh"
idf.py --version
```

Use the [environment guide](environment-setup.md) for platform prerequisites.
Do not copy the SDK, toolchains, Python environments, managed components,
firmware archives, or debug logs into a dependency-upgrade commit. No project
submodule for ESP-IDF is required. A local commit is not permission to push.

## cmake_utilities comparison

The comparison used the same SDK and application source digest:
`a2737cb25b567410b70a1c8e6b95e61bb60d0bbc32bb3c3d7da21e9b014bd57d`.
Only the helper version and its dependency-lock metadata changed between runs.

| Check | A: 1.1.1 | B: 1.3.0 |
| --- | --- | --- |
| SDK | 5.5.6 | 5.5.6 |
| Complete repository/host/firmware gate | PASS | PASS |
| Verified merged image and matching ELF | PASS | PASS |
| Application image size | 1,809,024 bytes | 1,809,024 bytes |

The additional real-LVGL drawing-pressure regression and web tests passed with
the selected 1.3.0 configuration. No other locked component changed during the
A/B comparison. The application image size was unchanged; different build
hashes do not by themselves demonstrate different runtime behavior.

These are integration results from the development working tree. The
SDK/dependency commit intentionally excludes the unfinished application, audio,
and UI changes. Firmware/ELF hashes belong to their exact archived bundles and
must not be substituted after rebuilding.

## Acceptance still required

Run `./tools/validate.sh` in the selected environment before delivering a new
application image. Keep Build, Host tests, Device tests, and Unverified separate.
The version-contract test checks the manifest, lock, CI tags, and maintained
setup/build guidance. It is not a hardware test.

This upgrade alone does not establish BLE + four-item-list stability, Wi-Fi +
four-item-list stability, ES8311 standby current, whole-board deep-sleep current,
or long-term RTC drift. Do not turn short startup or host tests into those claims.
Flashing needs a separately approved target, exact image, and data scope; it does
not authorize a full-chip erase or OTP/eFuse programming.
