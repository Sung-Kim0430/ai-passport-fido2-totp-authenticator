<p align="right"><a href="light-sleep-design.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Automatic Light Sleep

## Approved behavior

The user approved automatic light sleep on 2026-11-17. Keep the existing idle
deep-sleep policy. Let ESP-IDF select short idle intervals using tickless idle;
do not suspend the application explicitly or repurpose the ADC button pin.
Retain the 40–160 MHz CPU range. Keep the backlight PWM running from RC_FAST
during light sleep. Increase the LVGL tick period from 2 ms to 5 ms so it does
not prevent every idle interval; keep the button sampling period unchanged.
SDK peripheral and radio power locks continue to decide when sleep is possible.
Enable `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` so the USB connection holds a
no-light-sleep lock; unplugged operation remains eligible for automatic sleep.
An enabled configuration alone is not evidence of actual sleep or saved power.

An alternative explicit screen-off sleep would require reversible ADC/GPIO
handoffs and application task coordination. The approved automatic approach
uses existing SDK scheduling and preserves button polling.

## Clock accounting

During light sleep the SDK reconstructs high-resolution timer time from RTC.
That contribution must not enter the existing independent awake-time coordinate.
Use the pinned ESP-IDF 5.5.6 sleep context to obtain the exact RTC interval that
the SDK substitutes, its success result, and the SDK calibration period. Only
successful intervals enter a bounded RAM accumulator. Rejected requests do not
advance it. Convert each interval with the latest independently measured direct
period and retain fractional microseconds across intervals. Capture the SDK's
integer contribution separately. Keep the period prospective when it changes.
Also capture the SDK's libc RTC/HRT offset before and after each successful
sleep: `esp_set_time_from_rtc()` replaces `s_microseconds_offset` on wake.
The pinned adapter reads that private scalar under its critical section; it
does not call wall-clock functions or take libc locks from idle callbacks.

Sleep callbacks only collect numbers: no allocation, blocking, logging, LVGL,
wall-clock changes, NVS writes, or temperature/calibration work. The application
task drains a consistent cumulative snapshot before event handling, reference
application and deep-sleep preparation. It subtracts the SDK contribution from
the awake coordinate, adds raw/corrected sleep exposure to the existing ledger,
and applies corrected-minus-SDK UTC minus the SDK libc-offset change, once.
Keep PIN and protocol monotonic
deadlines on their existing SDK clocks. Existing retained/NVS schemas stay intact.

A delayed time reference whose timestamp precedes the last drained sleep exit
may still set UTC, but must not train a coefficient using ambiguous coordinates.
Include conservative RTC boundary uncertainty in learning windows. Keep the
last qualified coefficient when a window is rejected. Overflow, source changes,
unavailable calibration or capture failures inhibit further automatic sleep;
failed UTC reconciliation requests synchronization rather than claiming trust.
Do not modify SDK source or managed components. The private sleep-context API
is isolated in the platform adapter and requires review when upgrading ESP-IDF.

## Boundaries and verification

Application policy and pure arithmetic stay in `main`; reusable backlight and
LVGL configuration stay in the BSP. No new menu, permanent worker, persistent
setting, partition, radio service, or automatic Flash write is required.

Host regressions must cover successful/rejected sleeps, fractional accumulation,
counter wrap, invalid periods, overflow, repeated draining, UTC correction signs,
stale references, failure handling and light-sleep-to-deep-sleep continuation.
Exercise the actual platform callback adapter with SDK stubs as well as pure
arithmetic. Run the complete repository gate with ESP-IDF 5.5.6 and verify the
archive. Report build, host tests and device tests separately.

After a separately approved flash, check all three buttons, long presses,
backlight at several brightness levels, TOTP time, deep-sleep wake, radio/audio
activity and sleep counters. A USB host connection inhibits automatic sleep;
radios may also inhibit sleep. Test an appropriate unplugged idle interval as
well. Current savings and RTC accuracy
require instrument/reference measurements and remain unverified until measured.

## Implementation validation: 2026-11-17

- Build: PASS. The complete `./tools/validate.sh` gate ran with ESP-IDF 5.5.6;
  partition bounds, merged bytes and the matching debug archive were verified.
- Host tests: PASS. Coverage includes 14 platform-adapter scenarios, fractional
  accumulation, UTC correction with positive/negative SDK libc offset changes,
  repeated draining, stale references, faults and deep-sleep continuation.
  The RTC owner suite also passed UndefinedBehaviorSanitizer.
- Device tests: PASS for segmented-write verification, startup, USB continuity
  during a 40-second observation, and the user's computer-USB battery-icon
  regression check. This is not full unplugged sleep acceptance.
- Unverified: actual sleep entry, backlight continuity, buttons, wake behavior,
  radio/audio interaction, current savings and absolute UTC accuracy.

Verified archive: `build/firmware/7d26d8360de6f9e2459c7bd6f4fb0f9e9a76199ffa4b0eb26a79c68f8a49114f/`.
The directory name is the merged image SHA-256. Its matching ELF SHA-256 is
`efed78c7ad6165a9b6840661a9c0d36d5cee61a9abd0e3e043e38898a2564e95`.
The application image occupies 1,878,960 bytes of the 8,257,536-byte factory
partition. Preserve the compatible NVS, PHY and RTC-history regions when using
segmented flashing for device acceptance; a merged write from `0x0` can reset
stored data in padded gaps.

The verified image was flashed with user approval. All three image hashes
were verified; write ranges
excluded NVS, PHY and RTC history. Startup ELF prefix `efed78c7a` matched the
archive. Five captured samples reported `count=0 rejected=0 sdk_us=0 fault=0`
while connected to the computer, as expected with the USB sleep lock. The
40-second log window contained no panic, assertion, watchdog or RTC
reconciliation fault. The serial monitor was closed after observation.

The USB connection sleep lock prevents native USB loss during automatic sleep
and keeps the USB-derived status input stable while connected to a computer.
The user reported that the earlier blue/neutral/blue battery-icon transition
after idle and a key press no longer occurs on computer USB. Code review also
covered the shared USB-derived `+`/`*` markers, 80% reminder rearming and SOC
cache edge refresh; these share the corrected USB input. Wi-Fi/BLE indicators
use radio lifecycle state rather than USB detection, so this USB failure path
does not drive them. Battery-format/color and cache-poll host tests passed.
Independent wall-charger detection is not established by a USB host signal,
and remaining unplugged/button/backlight/radio acceptance is still pending.
