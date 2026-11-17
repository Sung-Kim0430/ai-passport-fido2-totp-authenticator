<p align="right"><a href="fido-rp-storage-plan.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Full RP-ID Storage Implementation Plan

**Goal:** Isolate BLE FIDO site material using the complete supported RP ID without invalidating existing keys during migration.

**Architecture:** A host-testable store encodes a versioned blob containing the full RP ID, salt, user handle and live/deleted state. The NVS adapter uses a SHA-256-derived 15-byte key and verifies the full identity on every access, refusing hash collisions. Legacy salts remain available for lazy migration; per-site tombstones prevent deleted credentials from being restored from legacy data. Re-registration generates fresh salt and rejects the old allow-list credential. The bounded eight-row list prioritizes full identities before legacy rows. This continues the existing one-credential-per-RP experimental model and 63-byte RP-ID limit.

**Implementation:** Work inline in the existing dirty checkout, preserving earlier changes. Do not commit, push or flash this increment without fresh authorization.

- [x] Reproduce the old shared-prefix collision with a failing regression. Add `tests/test_auth_fido_store.c` for independent salts/handles, preservation of legacy material, independent deletion, tombstone re-registration, corruption, hash collisions and read/write/commit failures.
- [x] Add `main/auth_fido_store.h` and `main/auth_fido_store.c` for bounded serialization and migration through read/write/random/hash callbacks. Store salt and handle together; never regenerate after a storage error or fall back after a corrupt/new-format record.
- [x] Add `main/auth_fido_store_platform.c` for NVS and SHA-256, and wire it into `main/CMakeLists.txt`. Confirm commits for both new and readable material; preserve shared legacy keys during individual deletion. Keep seed and counter contracts in `main/auth_ble.c`.
- [x] Update `main/auth_ble.c`, `main/auth_storage.h`, `main/auth_storage.c`, `main/main.c`, `main/auth_ui.h` and `main/auth_ui.c`: all operations use full RP IDs; display complete names with scrolling; mark legacy prefix rows read-only and explain migration. Make invalidation local to the selected RP.
- [x] Register host tests in `tools/validate.sh`, update `tests/test_ble_material.py`, and run `./tools/validate.sh --static`. Test actual adapter errors and UI/control integration as applicable.
- [x] Update `README.md` and paired audit notes to replace the outstanding-prefix limitation with the migration contract. Run `./tools/validate.sh` in ESP-IDF 5.5.6, request independent code review, verify archived artifacts and offer device testing.

**Acceptance:** Two RP IDs sharing their first 12 characters must retain independent user handles; deleting and re-registering one must leave the other credential unchanged. Migrating an old entry must reproduce its existing derived credential. Existing legacy prefix collisions may already have overwritten user handles; unavailable historical values cannot be recovered. Legacy prefix rows are not individually deleted because they do not identify a complete RP. Reset FIDO remains the explicit namespace-wide erase.

**Device checks pending:** Registration/login for two same-prefix sites, pre-upgrade credential login after migration, power-cycle persistence, deletion and re-registration isolation, full-name rendering, and legacy-row controls. Host tests and builds do not establish these results.

## Validation and handoff

On 2026-11-17, the complete `./tools/validate.sh` gate passed under ESP-IDF v5.5.6 with `COMMANDCODE_SCRATCHPAD` explicitly unset. Repository checks, host tests, firmware build, actual NVS capacity test, RTC wake checks and merged-layout verification passed. The store tests also passed ASan/UBSan. Regressions first reproduced the legacy prefix collision and repeated-refresh scroll reset, then verified both fixes. Two independent reviews found no blocking issues; archive reverification passed separately.

Build: PASS. Host tests: PASS. Device tests: NOT RUN. Unverified: the device checklist above. This increment was not flashed, committed or pushed; read-only discovery found `/dev/cu.usbmodem101`. Upgrade testing should use compatible segmented images to preserve NVS and RTC history; a merged flash at 0x0 can overwrite stored data.

Full-image SHA-256: `eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a`.

ELF SHA-256: `3b9f26135a4436cb175a0b911126742dd0a87baba5b8caf0e4de6fe68f5333e5`.

`build/firmware/eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a/`: application 1,877,424 bytes; merged image 1,942,960 bytes.

## Separately authorized application flash — 2026-11-17

After the user explicitly requested flashing, the unchanged `eaf05ce6e2989cb3da69539c8b819c8590c6907a54a4a02f936b3633cb4f599a` bundle was reverified. The device was present at `/dev/cu.usbmodem101`; esptool identified ESP32-C3 and 8 MB Flash. A 3072-byte partition-metadata read at `0x8000` matched the archived table exactly.

Only the verified 1,877,424-byte application was written at `0x10000`. Esptool reported sector erasure at `0x10000–0x1DAFFF`, successful write-time data-hash verification and independent `verify_flash` digest matching before reset. Bootloader, partition table, security NVS, PHY and `rtc_hist` were not write targets. No full-chip erase, rebuild, commit or push occurred.

A bounded 15-second serial observation captured startup after `USB_UART_CHIP_RESET`, the matching `3b9f26135…` ELF prefix and ESP-IDF v5.5.6. The application reported successful initialization, display/LVGL/button setup and enabled automatic Light sleep. No panic, watchdog or repeated boot was observed in that window. Two I2S “channel has not been enabled yet” error lines occurred while audio entered low power; application initialization continued. This is not a clean-audio acceptance result. The RTC ledger reported `restored=0`; establish a new phone time reference before accuracy measurements. The serial connection was closed and released.

Flash/readback verification: PASS. Startup observation: PASS. Build: PASS; Host tests: PASS, from the unchanged complete-gate artifact. Device tests: NOT RUN for the FIDO functional matrix. Unverified: screen/PIN/account retention, legacy-site login and migration, same-prefix registration/deletion isolation, full-name scrolling, power-cycle persistence and audio behavior. User observations were requested; the earlier no-device-write handoff is superseded only by this authorized flash record.

The user reported that BLE FIDO testing is unavailable because they lack a compatible client/device. End-to-end FIDO acceptance is therefore deferred for this handoff; it is not reported as a pass or requested as a prerequisite to completing the authorized flash. Screen/button/account retention were not confirmed by that reply.
