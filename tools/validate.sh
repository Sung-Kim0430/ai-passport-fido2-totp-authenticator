#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
validation_tmp_root="${COMMANDCODE_SCRATCHPAD:-${TMPDIR:-/tmp}}"
export TMPDIR="${validation_tmp_root}"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() (
    local actionlint_bin
    local test_dir
    local dead_strip_flag

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_sdk_version_contract.py
    python3 tools/embed_web_page.py --check

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d "${validation_tmp_root%/}/ai-passport-host-tests.XXXXXX")"
    trap 'case "${test_dir}" in "${validation_tmp_root%/}"/ai-passport-host-tests.*) rm -rf -- "${test_dir}" ;; esac' EXIT
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_totp_crypto.c main/totp_crypto.c main/totp_sha2.c \
        -o "${test_dir}/test_totp_crypto"
    "${test_dir}/test_totp_crypto"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_auth_identity.c main/auth_identity.c main/totp_crypto.c \
        -o "${test_dir}/test_auth_identity"
    "${test_dir}/test_auth_identity"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_auth_ui.c main/auth_ui.c \
        -o "${test_dir}/test_auth_ui"
    "${test_dir}/test_auth_ui"
    "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Imain \
        tests/test_auth_storage.c main/totp_crypto.c main/totp_sha2.c \
        -o "${test_dir}/test_auth_storage"
    "${test_dir}/test_auth_storage"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passcode_guard.c main/passcode_guard.c \
        -o "${test_dir}/test_passcode_guard"
    "${test_dir}/test_passcode_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_fido_protocol.c main/fido_protocol.c \
        -o "${test_dir}/test_fido_protocol"
    "${test_dir}/test_fido_protocol"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_drift.c main/rtc_drift.c -lm \
        -o "${test_dir}/test_rtc_drift"
    "${test_dir}/test_rtc_drift"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_history.c main/rtc_history.c main/rtc_drift.c -lm \
        -o "${test_dir}/test_rtc_history"
    "${test_dir}/test_rtc_history"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_history_estimator.c main/rtc_history.c main/rtc_drift.c -lm \
        -o "${test_dir}/test_rtc_history_estimator"
    "${test_dir}/test_rtc_history_estimator"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_history_store.c main/rtc_history_store.c main/rtc_history.c main/rtc_drift.c -lm \
        -o "${test_dir}/test_rtc_history_store"
    "${test_dir}/test_rtc_history_store"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_clock.c main/rtc_history.c main/rtc_history_store.c main/rtc_drift.c -lm \
        -o "${test_dir}/test_rtc_clock"
    "${test_dir}/test_rtc_clock"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtc_wake_policy.c -o "${test_dir}/test_rtc_wake_policy"
    "${test_dir}/test_rtc_wake_policy"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_auth_lifecycle.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_poll.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_clock_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_clock_platform.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_history_platform.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_http_status.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_history_http.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_stub_checker.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_validate_temp_paths.py
    echo "Host tests: PASS"
)

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.6 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d "${validation_tmp_root%/}/ai-passport-firmware.XXXXXX")"
    trap 'case "${validation_build_dir}" in "${validation_tmp_root%/}"/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    python3 tools/check_rtc_stub.py "${validation_build_dir}/FoloToy-AI-Passport.elf" \
        --sdkconfig "${validation_build_dir}/sdkconfig"
    cmake -S tests/rtc_nvs_host -B "${validation_build_dir}/rtc-history-nvs-host" \
        -D "IDF_PATH=${IDF_PATH}" -D CMAKE_BUILD_TYPE=Debug
    cmake --build "${validation_build_dir}/rtc-history-nvs-host" --parallel 4
    ctest --test-dir "${validation_build_dir}/rtc-history-nvs-host" --output-on-failure
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
