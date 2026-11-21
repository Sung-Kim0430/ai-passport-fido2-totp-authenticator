**English** · [简体中文](README.zh_CN.md)

# Passport Auth — FIDO8TOTP

An offline authenticator application for FoloToy AI Passport: local OTP codes,
on-demand Wi-Fi account management, and experimental BLE FIDO. This fork uses
its own application UI rather than the upstream hardware-test menu.

**Target:** ESP32-C3, 8 MB Flash, no PSRAM, 240 × 320 display, three buttons,
ESP-IDF **5.5.6**. See the [upstream platform documentation](docs/README.md)
and [agent instructions](AGENTS.md).

## Start here

1. Unlock with the button sequence **UP, UP, DOWN, DOWN, OK** on a new device.
   Change this public default under **Settings → Security → Change PIN**.
   Existing stored PINs are preserved on a compatible upgrade. Completed failed
   submissions are now saved in NVS and survive non-deep-sleep resets and power loss;
   successful unlocking or a completed automatic wipe clears the count. Only changes
   are written, not individual key presses. The existing 5-failure lockout and
   **more-than-8-failure auto-wipe policy** continue across restarts when enabled.
   Do not test repeated wrong PINs with valuable tokens. Failures already lost by
   older firmware cannot be recovered.
2. Press **OK** on the token screen for settings. **UP/DOWN** select an item;
   a long **OK** normally returns. On the token screen, a long **OK** locks.
   Under **Display & Audio → OTP View**, cycle **Card**, **List 3**, or **List 4**.
   Lists show three or four accounts vertically, not narrow horizontal columns.
   In list mode, UP/DOWN directly changes pages and wraps at the ends; card mode
   still changes one account at a time. Each list item has a rounded rectangular
   border without a first-row selection/highlight: navigation changes whole pages,
   not individual rows. The preference is saved separately from account data.
   Settings reuse a five-row menu panel: a group heading, at most three current
   items, and a position hint. UP/DOWN still reach all 15 logical settings across
   groups without retaining a long settings-widget list. Long names are clipped without wrapping over the code. The
   unsynchronized-time hint stays inside the content panel above the footer, so **OK:Set** remains visible in every view.
3. Open **Settings → Network → Wi-Fi Hotspot**. This separate page shows the
   full SSID and 8-character key. Use UP/DOWN to select the hotspot switch or Back,
   and OK to act; a long OK returns without changing the hotspot. Join the shown
   hotspot and open `http://192.168.4.1/`.
4. Keep the device unlocked to manage accounts or change settings from the web.
   Synchronize time before using OTP codes, especially after a complete power loss.
5. Turn the hotspot off when finished. Wi-Fi and BLE are intentionally mutually
   exclusive in this application to control internal-RAM consumption.

### Device-specific hotspot

The factory chip MAC is read as a 12-character device identifier. **No OTP/eFuse
bits are programmed.** The hotspot name is
`Passport-<last-six-ID-characters>`; it no longer includes a password suffix.

A stable 8-character uppercase hexadecimal WPA2 key is derived from the chip
identifier using HMAC-SHA1 with the public domain label `Passport-Auth WiFi v1`.
It is not stored as a shared hardcoded password, and does not change when tokens
are wiped. It is nevertheless **predictable from the public chip identifier**,
not strong access control. Eight characters is WPA2's minimum length and favors
convenience. The SSID no longer reveals any password characters. This build does
not add a QR canvas or decoder. The Wi-Fi page reuses the Lab menu widgets, with
the same font and 30 px rows for the switch, SSID, key, web address, and Back.
Use this AP only for nearby, trusted-device management. The web UI
shows the device identifier and SSID, never the hotspot password or saved OTP
secrets in API responses.

## OTP accounts

Up to **16 accounts** are stored locally. Each account has its own algorithm,
digit count, and period:

| Type | Code | Period | Secret input |
| --- | --- | --- | --- |
| TOTP / HMAC-SHA1 | 6 or 8 digits | 1–255 seconds; normally 30 | Base32 or a standard `otpauth://totp/` URI |
| TOTP / HMAC-SHA256 | 6 or 8 digits | 1–255 seconds | Base32 or URI |
| TOTP / HMAC-SHA512 | 6 or 8 digits | 1–255 seconds | Base32 or URI |
| Steam Guard | 5 characters | Fixed 30 seconds | Base32, or existing `shared_secret` in Base64 converted locally by the web page |

The web UI adds, edits, searches, deletes, and imports accounts. Editing with an
empty secret preserves that account's existing secret. URI import supports
`algorithm`, `digits`, `period`, `issuer`, and `encoder=steam`. Imports are
validated first, then saved sequentially; on a later failure the page reports
how many were saved. HOTP, Google migration QR payloads, and extracting Steam
secrets from another application are not implemented. Steam login passwords,
cookies, and `identity_secret` are not needed.

Numeric codes are grouped **3 + 3** or **4 + 4** on the device; Steam codes are
shown as five characters. With no accounts the home screen shows an empty state
without a page index or countdown. The device's bundled font is primarily Latin;
non-Latin account-name rendering needs separate font coverage verification.

### Time

TOTP uses UTC epoch seconds. Timezone and 12/24-hour settings affect display only.
Manual date/time entry includes **seconds**: **UP/DOWN** change the current field,
**OK** advances, a long **OK** saves, and a long **UP/DOWN** cancels. The web page
synchronizes browser time and displays the approximate device/browser offset.
The target uses ESP-IDF's RTC + high-resolution system clock. Deep-sleep wake
retains the synchronized clock; a cold boot or non-deep-sleep restart requires
synchronization again. Until then the device displays `SYNC TIME`, not OTP codes.
Synchronizing time does not write NVS.

When USB is connected at boot and the application log console is enabled, the
existing tick task emits up to six `CLOCK_COMPARE` / `MEMORY` / `LIGHT_SLEEP` samples, then stops this
startup trace. It adds no monitor task or on-device history. `hrt_us` and `rtc_us`
are elapsed microseconds from the same baseline; `diff_us` is RTC minus HRT.
Relative drift is `diff_us / hrt_us × 1,000,000` ppm, or
`ppm × 0.0864 × days` seconds. These awake samples do not establish deep-sleep
accuracy; temperature changes and long-term extrapolation require separate board
measurements. The SDK's high-resolution-clock specification is not this board's
measured absolute UTC accuracy. If `LIGHT_SLEEP` counts advance during a sample,
HRT includes RTC-reconstructed intervals and is not an independent awake reference.

### Idle power management

Default builds enable automatic light sleep with tickless idle alongside
40–160 MHz dynamic frequency scaling. The SDK sleeps only when tasks, timer
deadlines and peripheral power locks allow it. LVGL ticks every 5 ms, button
sampling stays unchanged, and RC_FAST keeps the backlight PWM alive during sleep.
The existing automatic/manual deep-sleep policy remains available.

Successful shallow-sleep intervals enter a separate fractional RTC ledger.
Their SDK contribution is removed from independent awake time, and the
application task applies only the UTC correction difference, including removal
of the SDK's additional libc RTC/HRT offset adjustment. PIN/protocol
deadlines remain on their existing monotonic clocks. Stale reference timestamps
cannot train against ambiguous sleep coordinates. A capture/accounting fault
inhibits further light sleep and requires time synchronization before trusted
continuation; the last qualified stored coefficient is retained.

The bounded startup trace reports actual sleep/rejection counts and SDK elapsed
sleep time. `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` keeps native USB available
by preventing automatic light sleep while connected to a computer. Unplugged
operation may sleep; radio/audio activity can also inhibit sleep.
Enabling the configuration does not prove that the board entered it. Backlight
continuity, buttons, sleep current and time accuracy still require device tests.
The [design and acceptance checks](docs/assets/light-sleep-design.md) describe
the pinned ESP-IDF sleep-context dependency and the remaining measurements.

### Fast-RC sleep correction: implementation, not hardware acceptance

Fresh default builds select the C3's **RC_FAST/256** source with
`CONFIG_RTC_CLK_SRC_INT_8MD256=y`, startup calibration of 3000 cycles, and
`CONFIG_AUTH_RTC_MAINTENANCE=y`. An existing root `sdkconfig` does not
necessarily adopt changed defaults: check the actual generated configuration or
use the isolated firmware gate. The legacy 8M symbol names RC_FAST/256 on C3.
Private measurement uses the selected RTC MUX and a new 3000-cycle calibration,
with source checks before and after; it does not change the SDK's separate
short sleep-entry calibration. Source-specific versioned/CRC-checked keys keep
slow-RC models and coefficients separate from the new source and account data.

A 48-bit/Q19 ledger retains raw bridge time, corrected time, fractional phase
and proportional divider carry. **Positive ppm means fast:** corrected bridge
time is `raw × 1000000 / (1000000 + ppm)`. A new source starts at **0 ppm**;
neither a previous fast-clock observation nor a temperature prediction becomes
a hardcoded correction. Each armed segment freezes its coefficient. Wake applies
one absolute UTC replacement, then consumes the segment; known awake HRT is
counted separately. Cancellation/rejected sleep discards only the new provisional
anchor. A failed setter remains retryable; a successful but timing-uncertain
setter is consumed, cannot run twice, and requires resynchronization. PIN timing
and credential schemas are unchanged.

Use the Wi-Fi page's **explicit time-sync button with an accurately configured
phone** to provide learning references. One click collects up to 12 sequential
millisecond-resolution exchanges within a three-second sampling budget; each
probe has a 500 ms deadline covering its response body. At least six valid
samples are required. The fastest third (three to five samples) supplies a
median UTC-to-device offset; a selected-group maximum absolute deviation from its median above 20 ms is rejected.
The four timestamps remove measured device-handler time from the round trip.
Remaining browser, transport and scheduling overhead, including asymmetric
paths, stays in the uncertainty estimate. Timestamp resolution is not measured
absolute UTC accuracy.

The authenticated `/api/time-probe` endpoint has an independent 16-entry one-use
cache, isolated from status polling. Sampling pauses page polling and mutations.
Clock steps, session/device changes, lock, and inadequate samples abort without
setting time. The final version-2 command is submitted once and never retried
automatically; invalid or expired references do not fall back to seconds. Both
HTTP admission and the application owner enforce the five-second reference age.
Legacy clients retain their original single-probe/integer-second behavior.

Transport uncertainty is `ceil((RTT - handler_time) / 2) + 1000 us + spread`.
The phone's separate assumed UTC bound remains **one second**, not a firmware
measurement of phone accuracy. Both terms enter the learning uncertainty; a
fast Wi-Fi exchange does not prove accurate phone UTC. The web result displays
transport uncertainty and the phone limitation separately. Status polling never
synchronizes or trains, and one multi-probe batch creates only one reference.
Manual entry and legacy/invalid-quality commands do not train. Learning
subtracts measured awake HRT from independent reference elapsed time and
compares it with **uncorrected** bridge history, not already-compensated wall
error. Windows require at least six hours of raw bridge exposure, at most
30 days and at most 100 ppm reference/accounting uncertainty.
Insufficient evidence leaves correction unchanged; the software rejection range
is ±20000 ppm, not a hardware tolerance or promised accuracy.

The RTC calibration retention area now uses `RTC_NOINIT` with layout versioning. When the magic
number, version, source, semantic checks, and CRC all pass, software or USB resets preserve
qualified active coefficients, their source, accumulated write budgets, and maintenance diagnostics;
UTC, learning baselines, and elapsed time continuity are not restored, meaning time
resynchronization is still required. If the retention area is invalid or after complete power loss,
ppm is loaded from NVS as LEGACY and the budget is re-accumulated. The 24-hour coefficient write
budget remains unchanged, and background maintenance now also executes due coefficient saves, with
failed writes consuming budget. When scheduled maintenance fails while the button watcher is normal
and no buttons are pressed, the device keeps the screen off, disables timed retries, revokes time-trusted status, and enters button-only-wake deep sleep; faults in buttons, the watcher, or GPIO
configurations still allow normal startup. Maintenance attempt counts, failure counts, and the last
maintenance outcome are exposed via `/api/status` and preserved across time syncs. Note that
overnight accuracy improvements have not yet been demonstrated on physical hardware.

Temperature-model learning still uses 12 fixed observations, three bins, 3 °C
span and fit RMS ≤100 ppm, but never supplies another UTC rate correction.
Its separate model-save policy remains distinct. Sampling uses the existing
application task outside LVGL locks; the sensor is fully uninstalled before sleep.

Trusted sleep retains the **120-second initial cooldown / 900-second steady**
maintenance cadence. GPIO0's early digital observer latches falling edges,
pending status and held-low evidence; the existing guard discards the entire
wake gesture before normal input. The old calibration/re-sleep wake stub remains
**off** (`CONFIG_AUTH_RTC_WAKE_STUB`).

Pre-observer ROM/SDK startup and the final SDK/CRC tail still have unproven
short-pulse coverage. Minimum physical key pulses, repeated wake/re-sleep,
startup stability, whole-board current and long-term UTC accuracy require device
acceptance. No 20-ms wake, microampere bound or daily 1–2-second accuracy follows
from a successful build. No previous/next TOTP windows are added. Hardware power
loss remains a cold restart requiring time sync. `RTC_CAL` is diagnostic, not
an absolute reference; `pre_tail_gap_upper_us` is not an end-to-end blind-window
measurement. See the [design and acceptance plan](docs/assets/rtc-calibration-design.md).

### Read-only clock evidence without serial access

`GET /api/status` retains the existing fields and adds `time_set`, `time_us`,
`time_sample_hrt_us`, `time_sample_window_us`, `time_sample_valid`, and an `rtc`
object. Only use the microsecond timestamp when `time_sample_valid` is true;
failed reads, an unset clock or a sampling window above 1 ms invalidate it.
The HRT value is the sampling-window midpoint, not an independent UTC reference.
Network round-trip uncertainty still matters when comparing external clocks.

`time_probe` provides a one-use ID, paired HRT, window and HRT-valid marker
independently of whether UTC is set; it is used only by an explicit browser sync.
`rtc.policy` is `selected_source_ratio`; `source=2` identifies RC_FAST/256 on C3.
`ppm` and `ppm_qualified` describe the active ratio; a source without a saved
qualified value stays zero/unqualified during this shadow increment. `reconciled_elapsed_us` is the raw last
bridge, including shutdown and early boot, not pure physical sleep;
`corrected_elapsed_us` is its applied corrected advance. `raw_chain_us`,
`corrected_chain_us`, `awake_total_us` and `maintenance_count` expose retained
accounting; `learning_reason` and `reference_status` distinguish rejection,
insufficient evidence and uncertain setters. `sdk_correction_us` remains the
signed corrected-target-minus-SDK difference at a paired phase, meaningful only
with `sdk_sample_valid`, **not measured error against real UTC**. Temperature-model
qualification is analytical, not proof it was applied. Successful explicit sync
clears the historical reconciliation snapshot.

The GET does not synchronize time, reset the board, sample the temperature
sensor or write NVS. Clock state is copied by the application task into the
existing bounded snapshot; HTTP does not inspect its mutable model. Keep power
and radio conditions consistent: enabling the hotspot changes thermal conditions.
Do not open the serial port during a retained-clock experiment; a serial-induced
reset invalidates that session. Share only the clock/time fields, not the whole
response, which also contains the existing AP-session token.

### Bounded calibration history and shadow adaptation

The application retains at most **100 valid independent external-reference windows**, not 100 sync clicks or internal frequency samples. Rates are recomputed from raw fractional bridge time and reference elapsed time minus awake HRT; errors in seconds are not averaged. A valid awake-only repeated reference preserves pending learning and reports reason **13** (`no new sleep`).

Global READY enables calibration initially when at least two training inliers are present and initial uncertainty is ≤100 ppm, requiring independent holdout improvement once already calibrated; global inconsistency allows RECOVERY (via the two most recent independent windows) to restore existing coefficients. v3 introduces RECENT_READY—requiring the latest three non-overlapping, single-source windows each ending within ≤7 days without skipping newer excluded windows (the first two for training and the third for independent validation)—which strictly refines an existing calibration. All history-based enablements require pending queue persistence with accepted=durable, a consistent clock source, a sequence number strictly newer than active coefficients, and non-regressing UTC. Active sources remain NONE, LEGACY, STABLE, and RECOVERY, with empirical uncertainty tracked independently of shadow states; RECENT_READY is not an active source enum and maps to STABLE. Reported uncertainty does not represent a physical bound on future drift. Deep sleep preserves active sources, whereas a cold boot from NVS transitions only the ppm source to LEGACY. The web UI displays record versus coefficient persistence states separately; NVS compatibility alongside the 24-hour coefficient and 6-hour record write budgets remains unchanged. Without overnight on-battery real-world validation for this iteration, 30-day ±3 s accuracy is not promised; reported temperature and voltage are timestamped awake endpoint observations rather than sleep averages or active thermal compensations.

Two retained pending rows distinguish accepted from durable evidence. A full queue never overwrites its rows; additional admitted windows advance the baseline and increment an explicit dropped count. History writes have an independent six-hour attempt budget, including failures, restarted on cold boot. The allowed manifest transaction checkpoints accepted/dropped counters; cold loss can still lose newer pending rows and counters, and continuity is marked incomplete. Silent maintenance never loads, estimates, exports or writes history. Unavailable or corrupt history does not replace the active correction or erase security storage.

While unlocked, use the web page's RTC history view/refresh and JSON export controls. Authenticated `GET /api/rtc-history` returns at most five rows, with `generation` and 64-bit measurements encoded as decimal strings. Continue with `?generation=<returned-generation>&cursor=<next_cursor>`; a changed generation returns **409**, requiring a new traversal. Export is bounded to 20 pages/100 rows and aborts rather than downloading partial data. The new endpoint carries the history summary; existing `/api/status` remains compatible. The exported allowlist excludes account data, device identity, AP/probe tokens, reference identities and internal epochs. HTTP requests use a bounded mailbox; only the existing application task copies the mutable history.

This fork uses a custom 8 MiB partition table:

| Partition | Offset | Size |
| --- | ---: | ---: |
| `nvs` | `0x9000` | `0x6000` |
| `phy_init` | `0xF000` | `0x1000` |
| `factory` | `0x10000` | `0x7E0000` |
| `rtc_hist` | `0x7F0000` | `0x10000` |

History uses only `rtc_hist`; account/PIN storage and application start offsets do not move. An old installed table lacks this partition: an app-only update cannot enable durable history and falls back to coefficient-only operation. A later **separately authorized partition-table plus application update** is needed, using verified segmented images to preserve the original NVS/PHY/bootloader. No automatic erase, merged-image overwrite of user data, or eFuse operation is part of this change. Native tests use the actual IDF 5.5.6 NVS engine; real-board power cuts, heap/stack margins and long-term UTC precision remain separate acceptance checks. See the [implementation and validation record](docs/assets/rtc-calibration-design.md).

### Observed startup comparison

On 2026-11-09, verified image `bb33c1f2a107` (ELF `8201249e03a2`) was flashed
app-only, retaining the matching partition table and NVS. With USB power and BLE
disabled, HRT measured 31.476992 s while the calibrated internal-RC RTC measured
31.428623 s: RTC was 48.369 ms behind HRT, an interval-average −1536.65 ppm.
The six startup interval averages changed from −801.41 to −1536.65 ppm. This is
not a thermally settled coefficient or a Deep-sleep measurement. Assuming the
last short-interval coefficient stayed constant would give 132.77 seconds/day
of relative lag, but that is an illustration, not a measured daily guarantee.
No absolute UTC reference or board-current instrument was used.

Native startup readings: free heap stabilized at 124,380 bytes, largest internal
block 94,208 bytes, minimum heap 119,940 bytes, and tick-task stack minimum
1,984 bytes across the samples. The 32 KiB LVGL pool initially had 3,156 bytes
free and a 1,960-byte largest block. A separate 64-bit host LVGL test used a
64 KiB pool and completed 10,000 refreshes without growing live allocations;
it is not a substitute for long-running native UI/radio/sleep acceptance.

## Status and safety

- The top bar has rounded-corner safety margins and no F8T wordmark. Outside the
  lock screen it shows HH:MM, with AM/PM after the time in 12-hour mode. The lock
  screen keeps its larger clock rather than duplicating it in the bar.
- Bluetooth icon: gray = unavailable/off, amber = enabled without a link,
  green = connected. The Wi-Fi icon indicates the hotspot state.
- Unknown battery level and 0–10% are red; 11–30% is amber. These warnings
  override the other colors. Above 30%, the battery is gray without USB,
  green with USB, and blue when USB is connected and the enabled 80%
  reminder threshold has been reached.
  The suffix **`*`** appears only with USB connected and the reminder enabled;
  unplugging hides it without disabling the saved reminder. **`+`** is the USB hint.
  USB connection is not a charge-current measurement; the 80% feature is a
  reminder, not hardware charge cutoff. Battery SOC I²C queries are cached for
  five seconds, with an immediate query on USB-state changes; token countdowns
  keep their existing refresh rate. This is not a measured current reduction.
- **Wipe All Tokens** and **Reset FIDO** require confirmation. Moving selection
  or returning cancels. A web wipe request opens a confirmation on the device;
  it does not erase data immediately.
- Wipe-all removes OTP accounts and FIDO credentials/PIN state. Reset-FIDO targets
  FIDO only. Deletions cannot be undone; retain service recovery codes elsewhere.
- Automatic wipe after repeated failed device-PIN attempts is enabled by default.
  The lockout/auto-wipe policy is separate from an experimental FIDO client PIN.
- Web mutations require local unlock and a current AP-session token. No wildcard
  CORS is provided. The UI displays a saved result only after the application task
  reports completion. The server publishes bounded, secret-free snapshots rather
  than reading mutable account data directly from its HTTP task.
- NVS errors do not trigger an automatic full-chip erase. Unsupported/corrupt
  account storage fails closed instead of silently replacing saved accounts.

## Audio power states

- **ON:** playback wakes the codec and I2S before writing PCM.
- **STANDBY:** Audio Beep is enabled but idle; retain the codec object,
  configuration and enabled bias, mute it, and stop I2S. The existing application
  tick handles the idle deadline without a new audio-monitor task.
- **OFF:** disabling Audio Beep or entering deep sleep uses the BSP software
  power-down path, not the keep-bias standby path.

The ES8311 and always-powered amplifier have no MCU-controlled supply switch on
this board. Software Off is not physical VDD removal. Transition sound, standby
current, and whole-board sleep current still require device/instrument tests.

## Lab / Experiments

**BLE FIDO defaults to OFF.** The device Lab contains the BLE transport toggle,
USB application-log toggle, and FIDO relying-party list/delete/reset controls.
Remote CTAP Reset is refused (`CTAP2_ERR_NOT_ALLOWED`); reset through the device
Lab with confirmation. It must not clear the PIN before asking for confirmation
or accidentally request an OTP wipe.

The current FIDO credential model is experimental, not certified, and is not
established as a production multi-account passkey implementation. Phone
registration/login compatibility still needs end-to-end acceptance. Unit tests
or a valid CBOR response do not establish browser compatibility.

Site records now store the complete supported RP ID (up to 63 bytes), salt and
user handle together. NVS keys use a SHA-256 fingerprint of the **entire** RP ID;
every access also verifies the stored full ID and refuses a fingerprint collision.
Sites sharing their first 12 characters have independent handles and deletion.

Older prefix records migrate lazily when that site is used again, preserving the
original salt and derived credential. Shared legacy material is retained for
other, unmigrated sites. The Lab prioritizes full-ID records in its eight-row
list, scrolls the selected full name, and marks old prefix rows `Legacy:`;
those ambiguous rows are read-only until the site is used to migrate it. A
per-RP tombstone blocks legacy fallback after deletion. Re-registering a deleted
site creates a fresh salt, leaving the old credential invalid. Reset FIDO still
clears the whole FIDO namespace. User handles already overwritten by an old
prefix collision cannot be recovered automatically. Downgrading to firmware
that only understands prefix records can revive those older credentials and
ignores the new records/deletion markers; this migration does not support such
a rollback. Preserve NVS when upgrading to test migration.

Over-capacity RP IDs are rejected explicitly, while display-only labels may be
shortened at UTF-8 boundaries. BLE storage errors refuse registration/login
instead of returning success using temporary key material.

The ESP32-C3 has fixed-function USB Serial/JTAG, **not a programmable USB HID
FIDO2 endpoint**. USB Log Console only controls application logs; it does not
disable hardware JTAG. This firmware does not burn eFuses or provision
irreversible secure-boot/Flash-encryption settings. NVS is not a secure element;
do not treat this development firmware as equivalent to a hardened security key.

## Build, tests, and firmware

Activate an existing ESP-IDF 5.5.6 environment, then use the shared gate.
cmake_utilities is pinned to 1.3.0 after same-source comparison with 1.1.1;
see the [SDK upgrade record](docs/development/engineering/esp-idf-5.5.6-upgrade.md).
SDK/tool installations remain outside the project:

```sh
idf.py --version
python3 tools/embed_web_page.py
./tools/validate.sh --static
./tools/validate.sh
```

The static gate includes RFC 6238 SHA-1/SHA-256/SHA-512 vectors, Steam vectors,
legacy-account migration, account validation, and battery-marker tests, alongside
the upstream checks. The embedded web header must match its HTML source.

The firmware gate builds an isolated configuration from `sdkconfig.defaults`,
validates the merged image, and retains a matching ELF/MAP bundle under
`build/firmware/<full-image-sha256>/`. See [build and test](docs/development/engineering/build-and-test.md).

**Data preservation matters.** The verified merged image at
`build/FoloToy-AI-Passport-full.bin` is for `0x0`; padding can overwrite NVS.
For a compatible development update that must retain accounts, use verified
segmented images at their actual offsets, not the merged image. Do not erase
Flash or burn eFuses as a routine update. See the
[flashing/data policy](docs/development/engineering/firmware-layout.md#flashing-and-stored-data).
Legacy account blobs migrate to `config_v2` only after successful validation and
save; the older firmware cannot read accounts newly stored in that format.

## Memory and acceptance

OTP generation uses fixed-size buffers and does not allocate heap memory.
The hotspot uses HT20 (20 MHz) and an 11 dBm transmit-power limit, including the
PHY startup limit. This is a configuration, not a measured board-current claim.
BLE buffers, task stacks, watchdogs, and stack protection are not reduced just
to lower a memory number. The datasheet's 5 μA Deep-sleep figure is chip-only;
LCD, codec, the always-powered amplifier, regulator, and charging circuitry make
whole-board current a separate measurement.

Wi-Fi uses bounded requests/responses and releases its HTTP, DNS, socket, netif,
and driver resources on shutdown or startup failure. Radio transitions must
finish before starting the other transport. The global ESP-IDF event loop is a
shared service and may remain allocated after its first use.

The maintained `tests/auth_ui_host` regression performs real LVGL drawing:
it reserves 15,728 bytes after warm-up and draws List 4 plus all 15 settings over
three rounds, with assertions enabled. Its 64 KiB host pool accounts for 64-bit
pointers; it is not a test of the device's 32 KiB pool with BLE or Wi-Fi active.
Those native interaction combinations remain separate acceptance items.

These design constraints and passing host tests are **not a lifetime guarantee**.
Long-running board tests must still record free heap, largest free block, minimum
heap, task-stack margins, and reset reason while repeatedly changing settings,
opening/closing the hotspot, and enabling/disabling BLE. Build, host tests,
device tests, and unverified checks must be reported separately. A flash or clean
boot is not proof of leak freedom or working FIDO registration.
