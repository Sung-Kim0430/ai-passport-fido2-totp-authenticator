<p align="right"><a href="rtc-calibration-design.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# RTC Hybrid Calibration Implementation Plan

> **For agentic workers:** Use `subagent-driven-development` or `executing-plans` to implement the tasks below, with tests and review between tasks. Do not commit or flash without authorization.

**Goal:** Improve the authenticator's sleep-clock accuracy using board-specific temperature learning and periodic RTC-safe frequency measurements without losing user wakes or changing security deadlines.

**Architecture:** Keep SDK RTC/HRT timekeeping intact. Maintain a separate retained, fractional, piecewise sleep-time ledger and reconcile UTC once at normal wake. Collect temperature/model observations in the existing application task; run only audited RTC/ROM code in timer-maintenance wakes.

**Tech stack:** ESP32-C3, ESP-IDF 5.5.6, C11 host tests, ESP-IDF temperature sensor, RTC fast memory, GPIO0 button wake, existing NVS and application event loop.

## Status and authorization

The implementation boundary was approved on 2026-11-10. The software is now implemented in the working tree, with verification results below; this does not establish hardware accuracy, current or pulse coverage. Periodic wake-stub maintenance remains disabled by default. Preserve the existing dirty application tree. The SDK-only commit is `bd5f43c`; the previously flashed application bundle is `31d4234484ff561d541924c609444ea4961ad37938cfb2e7fffcd73774769bdc`.

No application/design commit, push, eFuse programming, Flash erase, new wiring, or new firmware flash is authorized by this plan. The previous flash approval applies only to its exact image. Keep SDK source outside the project and do not modify managed components.

## Verified constraints

- C3 slow RC is approximately 136 kHz. A 1024-cycle measurement takes approximately 7.53 ms before other work; 2–3 ms total and less than 0.15 microampere average overhead are unverified claims.
- `rtc_clk_cal()` and the ordinary temperature driver are not stock C3 wake-stub-safe APIs. Ordinary IRAM is not retained RTC fast memory.
- Normal SDK deep-sleep entry installs a new 10-cycle calibration. Installing a longer factor immediately before sleep does not defeat that overwrite.
- SDK rate changes preserve accumulated elapsed time in normal runtime. Directly writing the global factor from a stub bypasses the required boundaries and is prohibited here.
- The stub's raw wake-cause bits differ from normal application wake-cause bits. Only an exactly timer-only maintenance cause may re-sleep.
- External 32 kHz is not a software-only option: its GPIO0/1 pins are already used by buttons/LCD CS. Keep the internal slow RC initially. RC_FAST/256 is an alternative requiring a separate measured power/accuracy decision.
- The root incremental `sdkconfig` and `build/project_description.json` can be stale. Validate the delivered image using the gate's fresh defaults configuration and exact archived ELF, not those old files.

Source references are the pinned SDK's `docs/en/api-guides/deep-sleep-stub.rst`, `docs/en/api-reference/system/system_time.rst`, `components/esp_hw_support/port/esp32c3/rtc_time.c`, `sleep_wake_stub.c`, `sleep_modes.c`, and `esp_clk.c`. The [official documentation entry](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/) may describe a newer release; implementation follows 5.5.6.

## Clock domains and model

Awake UTC stays on SDK HRT. Model fitting is task-only and may use floating point; no such routine is reachable from the stub. Temperature is die temperature. Use bounded aggregates, at least 12 valid observations in three occupied temperature bins, a span of at least 3 degrees Celsius, a nonsingular fit, and an RMS residual no greater than 100 ppm. These are initial model-admission rules, not an accuracy guarantee. Predict only within the qualified observed temperature range and the sensor's configured -10 to 80 degree range.

The pure selection model can evaluate `p_pred = p_a + M(T_now) - M(T_a)` for a directly measured anchor `(T_a, p_a)` and qualified polynomial `M`, with temperature age no greater than one second and anchor age no greater than 600 seconds. This is an analytical candidate, not permission to use it as the UTC sleep rate. After the reported fast-clock observation, the application boundary was tightened: every sleep preparation takes a fresh 1024-cycle direct measurement, even if a cached direct rate or qualified prediction exists. Model learning and its storage budget remain active, but predictions do not replace this measurement. If measurement fails, leave compensation disabled for that sleep, restore the SDK's default stub, and do not prevent ordinary GPIO-only sleep. Never resurrect a cached/model rate or add a model delta to the new direct rate.

While asleep there is no new temperature input: hold the accepted period constant until another direct measurement commits. Do not claim model-based knowledge of the intervening thermal trajectory. Successful direct samples are prospective; this initial implementation does not retroactively interpolate past intervals. Tests cover the analytical predictor separately and prove that both a same-temperature cached rate and a qualified prediction cannot bypass the application's fresh sleep-entry measurement. This risk reduction does not prove the cause of the reported error or remove drift caused by oscillator changes during sleep.

Use the existing application task with deadline-gated sensor/model work outside LVGL locks, no permanent task, no unbounded history and no per-sample Flash write.

## Numeric ledger and sample commits

The C3 raw RTC counter is 48 bits. Store anchors masked to that width; compute modular delta and reject a delta at or above `2^47`. A valid Q19 period is between `2 * 2^19` and `20 * 2^19` microseconds-per-tick, inclusive. This broad implementation domain is not a chip tolerance claim. Fractional remainder is below `2^19`; maximum accumulated session duration is 366 days. Validate these semantics even after a checksum passes. Invalid/corrupt active compensation records clear compensated UTC trust and return to normal runtime without touching credentials.

Convert with `whole = delta >> 19`, `low = delta & ((1 << 19) - 1)`, `tail = low * period + remainder`, `advance_us = whole * period + (tail >> 19)`, and `new_remainder = tail & ((1 << 19) - 1)`. Check the duration/UTC addition before publishing state. This avoids `delta * period` overflow. Integer ledger helpers used by the stub are explicitly always-inlined into its RTC translation unit or placed in RTC text, with no `-lm` dependency.

A completed measurement is not committed until its rate passes validation, its ready tick is sampled, and pending user-wake evidence is checked. Settle to that ready tick with the previous period, then atomically publish the new prospective rate/checkpoint. Before this boundary, cancellation settles through a freshly sampled current tick with the old rate and discards the candidate. After it, cancellation retains the committed rate and settles only the remaining tail. Advance each anchor once. The normal-boot request is sticky and wins over publishing a candidate.

## UTC bridge and security domains

A retained, pointer-free session contains validity/version/check fields, a raw-tick anchor, accumulated/fractional elapsed time, last period, trusted absolute UTC anchor and sticky wake evidence. Capture the final UTC/HRT/raw-RTC correspondence immediately before SDK sleep entry, after terminal BSP handoffs. An earlier prepare step may collect the model/rate; it must not activate a sleep session that was subsequently aborted by a held key.

At normal application startup, before slow BSP setup, copy wake evidence into normal-boot-owned state and settle a copy of the retained checkpoint through a current raw tick `c`. The absolute target is `UTC_anchor_us + ledger_elapsed_us(c)`, not current SDK wall time plus elapsed sleep. Account for HRT time spent between sampling `c` and the checked microsecond-capable `settimeofday()` call. The raw interval covers SDK shutdown, maintenance work, bootloader and early application startup; normal HRT handles time after the bridge.

Retained states are prepared, active, and consumed. Reconciliation uses a copy until application succeeds. On `settimeofday()` failure, do not mark UTC trusted/applied or consume the active checkpoint; a same-boot retry recomputes an absolute target from it. On success, a task-owned applied flag prevents re-entry and the retained session becomes consumed. Reset between applying and consuming is a non-deep-sleep boot: it invalidates the session/UTC trust, so cannot apply the correction twice. Corrupt active state requests resync rather than applying an arbitrary phase shift.

Do not rebase awake deadlines using `esp_timer_get_time()` or retained PIN lockout using `esp_rtc_get_time_us()` from corrected UTC. Do not silently move them to the private ledger. No stub call changes SDK global calibration/private clock state. Test positive/negative UTC corrections with an outstanding lockout, repeated maintenance, failure/retry and cold recovery. A cold/hardware-power restart restores NVS credentials/attempt counts but requires UTC synchronization.

## Read-only reconciliation evidence

After a successful bridge, cache the complete ledger elapsed value at the RTC capture and an optional signed ledger-minus-SDK UTC difference aligned using HRT. The SDK sample must fit a 1 ms window. Failure of this optional diagnostic must not block a valid independent restoration, consume a failed session or alter its target. Publish diagnostic success only after the checked time-setting operation succeeds; explicit successful synchronization clears the cached evidence. This adds normal RAM only and does not change the retained-session or NVS schema.

The existing `GET /api/status` publishes these numbers through the application-owned snapshot, with a live SDK UTC/HRT sample, its window and validity flag. HTTP does not read mutable RTC/model state, calibrate, allocate a temperature handle, synchronize time or write NVS. Its fixed response buffer is 1024 bytes; the handler regression checks full-width integer formatting and invalid/unset/slow samples. `reconciled_elapsed_us` includes shutdown and boot through capture; `sdk_correction_us` is not error against an independent UTC reference. Radio/power conditions and network round-trip uncertainty must be recorded for comparisons. Share only clock/time fields, never the complete response containing the existing AP-session token.

## Model storage budget

Use a separate versioned `rtc_cal` model record: schema/version, slow-clock source, XTAL reference, fit domain/qualification, finite coefficients and checksum. It is independent from the account/config blob. Missing storage starts unqualified; unsupported, invalid or unreadable storage starts unqualified with automatic replacement blocked, without an erase or rewrite loop.

Allow the first qualified model to be committed once when the record is absent. Subsequent automatic attempts require at least 24 hours of elapsed runtime including sleep, and a meaningful change: at least 10 ppm prediction change plus either a 20 percent residual improvement or at least 2 degrees of qualified-domain expansion. A cold boot with an existing record restarts a conservative 24-hour update wait; repeated boots cannot accelerate writes. No automatic retry before the next budget window, including after commit failure. Mark persisted only after successful NVS commit and retain the last good stored model on failure. Explicit time sync may request a checkpoint but does not bypass this budget. Test unchanged fits, repeated boots, unsupported records, failed/interrupted updates and bounded attempts.

## RTC maintenance and cancellation

Initial maintenance interval: 300 seconds, as an engineering starting point rather than a measured optimum. Use a bounded XTAL-referenced measurement with explicit peripheral setup; the Timer Group constructor has not run in the stub. Audit every callee, constant, arithmetic helper and mutable object for RTC/ROM accessibility. No allocator, logging, NVS, UI, ordinary temperature-driver calls, or FreeRTOS locking is allowed in the stub.

Only enter maintenance with a valid session and exactly timer-only wake. GPIO evidence, combined/unexpected causes or invalid state requests normal boot. Snapshot wake evidence before clearing status. Check buttons throughout all waits and before sleep handoff, not just at entry/exit. An accepted user-wake request is one-way for the session: later release or successful measurement cannot clear it.

Calibration timeout/failure must have a bounded fallback, preserve the last valid accounting and avoid a rapid-wake/full-boot loop. Record a fault for normal runtime; an unproven low-level path is not enabled merely because it compiles.

The final CRC/sleep-enable tail is part of the race analysis. The experimental implementation polls after CRC and performs no further retained-data write on its successful sleep path. The 8 ms software gap threshold includes one raw-counter tick and uses the broad 20-microsecond period upper bound; it is not a measured physical pulse guarantee. `pre_tail_gap_upper_us` omits the final CRC on successful re-sleep and may contain later checkpoints on an aborted path, so is not an end-to-end peak. Ordinary edge latching and the post-WFI clear-SLEEP_EN/return-to-ROM recovery still require target proof. Keep `CONFIG_AUTH_RTC_WAKE_STUB` disabled by default. Measure the full detection interval and minimum covered low pulse width; never claim arbitrary-pulse capture.

## Both button groups

### GPIO0 function buttons

UP/DOWN/OK share an ADC ladder. Digital low detects a wake request, not which key was pressed. Preserve the existing ADC-to-digital sleep handoff and external pull-up circuit.

The existing guard releases on RELEASE, but the button component can emit SINGLE_CLICK later. Append one BSP aggregate-idle event without changing existing event values. The owned BSP timer context sets a three-key active-gesture mask on PRESS_DOWN and clears a key only on component PRESS_END, after its CLICK/DOUBLE tail. Notify aggregate idle only when the mask is empty and valid released ADC readings have remained stable for at least the configured button debounce interval. An invalid ADC reading or a new press resets release qualification. The stock power-save callback is not used: enabling power save on these ADC drivers would stop polling without a wake ISR.

Arm the guard before button initialization, for every boot including a stub-requested wake whose hardware cause remains TIMER. After initialization begins, only the button timer callback mutates guard state. Remove the racing main-task single-voltage-sample disarm. Consume every event until aggregate idle, consume the idle notification itself, and discard future idle notifications from application dispatch. A timeout while held never disarms. This covers an early release with no component RELEASE, long holds, bounce, multi-key ladder handoffs and a second press within an unfinished first gesture; that second press remains quarantined until the complete grouped gesture is idle. The next gesture after the boundary is accepted normally. Test the actual BSP callbacks and initialization ordering, not only the pure guard.

### Dedicated hardware power button

Product specifications define hardware power control: about 0.5 seconds to start and about 2 seconds to shut down. It is not an exposed MCU input. A stub cannot intercept physical power removal. Test safe cold recovery, model validity, NVS persistence, and unset UTC; do not invent a GPIO mapping or promise RTC retention after power-off.

## Bounded power/stability work

- Preserve radio stop ownership, display holds, shared-I2C shutdown and audio ON/STANDBY/OFF behavior.
- C3's temperature driver acquires sensor power at install and releases it at uninstall. Reuse one owned handle per awake episode, disable between reads without claiming electrical power-off, and uninstall before deep sleep. Do not cycle allocations per sample or enable that driver in a stub; verify coexistence with the shared ADC buttons.
- Check `settimeofday()` before claiming sync succeeded or rebasing clock state.
- Report terminal peripheral shutdown failures without returning to live UI after irreversible handoffs.
- Rate-limit battery I2C polling independently of countdown redraws if implemented; refresh USB/status transitions promptly and measure the benefit.
- Report RTC reservation, heap/largest block, application and tick stack margins. Keep watchdogs and stack checks enabled.
- Do not enable automatic light sleep with unsupported BLE clock assumptions or change RF buffers solely to improve a memory number.

## Executable boundaries and test registration

| Owned boundary | Required executable evidence |
| --- | --- |
| Task-only polynomial/selection in `main/rtc_drift.c` | `tests/test_rtc_drift.c`: qualification, eligible prediction, precedence and rejection |
| Integer-only ledger declarations/always-inline core in `main/rtc_drift.h` | Same test: 48-bit wrap, half-wrap rejection, period/fraction/duration bounds, fractions, cancellation before/after commit and once-only consumption |
| Task adapter `main/rtc_clock.c`, shared retained-state private header | `tests/test_rtc_clock.c` with narrow platform/NVS/sensor stubs: fresh-only sleep rate, absolute microsecond bridge, failed-set retry, nonzero SDK sample-phase alignment, optional/slow diagnostic failure, phase reset, persistence budgets, cold boot and unchanged security clocks |
| Read-only HTTP evidence in `main/auth_wifi.c` | `tests/test_rtc_http_status.py`: actual handler, snapshot-only model access, full-width integer response bounds, unset/failed/slow timestamp flags |
| RTC policy `main/rtc_wake_policy.h` and `main/rtc_wake_stub.c` | `tests/test_rtc_wake_policy.c`: timer-only, simultaneous GPIO, sticky request, phase cancellation, invalid session and bounded failure; final ELF/map/disassembly for actual low-level code |
| Actual BSP/guard | `tests/test_bsp_button.c`, `tests/test_key_guard.c`: delayed CLICK/DOUBLE, PRESS_END grouping, stable idle, initialization callback ordering, long hold, read failures and next gesture |
| Application synchronization/wiring | `tests/test_auth_storage.c`, `tests/test_key_wake_contract.py`, `tests/test_rtc_clock_contract.py`: failed synchronization and real call-order/terminal-sleep contracts |
| Placement audit | `tools/check_rtc_stub.py` against the actual firmware ELF, with parser fixtures in `tests/test_rtc_stub_checker.py` if a checker is implemented; otherwise retain equivalent explicit review evidence |

Register each new C/Python host target in `tools/validate.sh --static`; a file's existence does not make it a gate. Use a host C compiler with `-std=c11 -Wall -Wextra -Werror`, `-lm` only for normal-task math, and available ASan/UBSan. The existing real-LVGL rendering and web checks remain required.

No out-of-line task-model function may be called from the stub. Every retained object is defined in RTC data, not an ordinary RAM static. Audit compiler arithmetic helpers and all constants in the final image, not only source attributes. Future coding must not import math/logging/storage dependencies into the integer RTC core.

## Implementation tasks

### 1. Whole-gesture wake guard

Files: `main/key_guard.h`, `components/bsp/include/bsp_button.h`, `components/bsp/src/bsp_button.c`, `tests/test_key_guard.c`, button stubs/tests, `main/main.c`.

- [x] Write a failing PRESS → RELEASE → delayed CLICK regression and long-hold/early-release cases.
- [x] Inspect the component's existing PRESS_END and idle semantics without editing it.
- [x] Add the smallest owned-BSP/guard contract that suppresses the whole wake gesture and accepts the next independent one.
- [x] Run `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_key_guard.c -o "$COMMANDCODE_SCRATCHPAD/test_key_guard"` and execute it; run key/button contract tests.

### 2. Pure model and sleep ledger

Files: new `main/rtc_drift.h`, `main/rtc_drift.c`, `tests/test_rtc_drift.c`.

- [x] Define period/temperature/ledger units and invalid-result semantics.
- [x] Write failing tests for model qualification, fractional accumulation, cancellation, exactly-once consumption, overflow and wrap.
- [x] Implement bounded model and segment math without SDK/LVGL dependencies or heap allocation.
- [x] Compile/run with the host C compiler, sanitizers where available, and `-lm` where required.

### 3. Normal-runtime adapter

Files: new `main/rtc_clock.h`, `main/rtc_clock.c`, optional private shared header; `main/auth_storage.c`, `main/main.c`, `main/CMakeLists.txt`.

- [x] Add low-rate task-owned sensor/calibration lifecycle and independent model storage.
- [x] Test sync failure/rebase, no writes per sample, invalid model and cold recovery using narrow stubs/contracts.
- [x] Integrate prepare/wake reconciliation before slow BSP startup, with no changes to account data or security timing.

### 4. RTC-safe maintenance

Files: new `main/rtc_wake_stub.c`, retained-state private header, wake policy tests, optional `tools/check_rtc_stub.py`.

- [x] Test timer/GPIO routing, cancellation at every phase and invalid-session fallback in pure logic first.
- [x] Implement only C3/5.5.6-supported RTC/ROM paths with bounded, pollable calibration.
- [x] Review final map/disassembly for instruction/data dependencies and RTC footprint; reject unsafe paths.
- [x] Include the CRC/sleep tail and GPIO status-clear order in the race review.

### 5. Integration and delivery

Files: `tools/validate.sh`, actual sleep/key/time contract tests, paired root README and this design.

- [x] Run smallest relevant checks during development, then `./tools/validate.sh --static`, `./tools/validate.sh --firmware`, and the complete gate in IDF 5.5.6.
- [x] Run maintained real-LVGL drawing and web regressions; inspect memory changes without claiming long-term stability.
- [x] Review the complete diff and keep application/design work uncommitted unless requested.
- [ ] Offer a separately authorized app-only device update preserving NVS, then execute the hardware matrix.

## Acceptance and honest limits

Host proof must cover units, model rejection, cancellation, retention, exactly-once accounting, security isolation, persistence and gesture tails. Build proof must cover the actual defaults configuration, RTC/ROM placement and retained-memory footprint.

Hardware proof requires timer/GPIO phase and pulse-width sweeps, all three ADC buttons, long holds, hardware power-off during maintenance, repeated sleep/wake, temperature ramps, independent reference timing and integrated whole-board wake charge. Report raw measured ppm and `seconds/day = ppm * 0.0864`; an x-day extrapolation must state its constant-rate assumption and uncertainty. No short startup interval proves a long-term accuracy or current bound.

### Previous implementation verification

Build: PASS. The previous default complete-gate bundle is `69fae9c6161ae8f4dfc22d916ca1e68159c142b9fd3f711b6ec9058bf688bdf5`. The separately built experimental bundle is `00dcb4e58a62ce03bb1c28a00768bf9a7b6d01dd83cf78e69c5c705e9a91bdda`; its strict ELF audit found 11 reachable RTC functions with only integer division, delay and RTC CRC ROM calls. Both archives and matching ELF identities were reverified.

Host tests: PASS, including the complete static gate, all 15 drift/model groups, UTC/lifecycle/storage-budget and wake-policy tests, actual NVS boundary tests, sanitizer runs, maintained real-LVGL drawing and web regressions. Experimental RTC text is 2448 bytes, the occupied lower RTC span is 2756 bytes, and a further 24 bytes are SDK-reserved; this is not a board heap or power measurement.

Device tests: NOT RUN for the complete acceptance matrix. The default `69fae9c6…` bundle was subsequently flashed app-only and verified. A clock about 100 seconds fast was reported after sleep; usable period/model evidence was not captured. A later `USB_UART_CHIP_RESET` (reason 11) invalidated the retained session and required synchronization. This does not establish an initial-offset-corrected drift rate or its cause. Experimental hardware acceptance remains unexecuted.

### Fast-clock hardening follow-up

The fresh-direct-only sleep boundary and read-only HTTP evidence described above are new working-tree changes. They are risk reduction and instrumentation, not a demonstrated fix for the reported 100-second observation. The current device is not changed while the Never Sleep control is pending. Do not open serial, reset, flash, resynchronize on its behalf or derive coefficients from the old observation. Obtain an explicitly reported initial signed offset, elapsed reference interval and later signed offset before calculating drift.

Build: PASS, using the complete ESP-IDF 5.5.6 gate. The new default archive is `build/firmware/82bffa0af901a8860bca234d9515bb67e594cd1b5804a61f5248216deebfe5ac/`; the full-image SHA-256 is `82bffa0af901a8860bca234d9515bb67e594cd1b5804a61f5248216deebfe5ac`, and its matching/embedded ELF SHA-256 is `08d9c21f7034a7e84877a38fabdc61ea7f25eb3cda2e176fd3a3c67c8f8fc106`. The application is 1,824,400 bytes and the merged image is 1,889,936 bytes. Archive verification passed. The actual default ELF reports no maintenance entry; periodic wake-stub maintenance remains disabled. This does not revalidate the old experimental image against the new source.

Host tests: PASS. The complete static gate was rerun after adding nonzero/overlong SDK sample-phase coverage. RTC/model ASan/UBSan, the real-LVGL drawing-pressure test and web regressions passed. Independent scoped review found no blocking issues; its minor phase-coverage gap was addressed without changing production code after the firmware build.

Device tests: NOT RUN for this new archive. No serial open, device reset, time synchronization or flash was performed during this follow-up. No new commit or push was made.

Unverified: actual source of the previous error, long-term awake/sleep accuracy, board-specific model training, physical key pulse coverage, WFI/re-sleep recovery, whole-board current and remaining UI/radio/sleep acceptance. New firmware requires a separate exact-artifact flash approval; no commit or push is authorized.

### Subsequent controlled observation and authorized update

On the previously flashed `69fae9c6…` default, the reported aligned baseline was November 12 at 08:46:11. The final accurate pre-sleep observation was 21:51:30, after 13 hours, 5 minutes and 19 seconds with Never Sleep; no obvious offset was reported. Around 23:32, the first display after wake was about 33 seconds fast. The offset remained about +33 seconds while awake rather than continuing to grow. These are reported observations, not instrument-captured raw ticks. The pre-sleep time is not the exact deep-sleep-entry instant, and the minute-resolution final reference does not establish an exact frequency coefficient. The evidence concentrates investigation on sleep accounting/wake reconciliation but does not distinguish accumulated oscillator error from a wake-time replacement jump.

Following the earlier approval for the exact `82bffa0a…` artifact after the control measurements, the archive was reverified without rebuilding. The target was confirmed as ESP32-C3 with 8 MB Flash, and its 3072-byte partition table at `0x8000` matched the archived table byte-for-byte. Only the archived 1,824,400-byte application was written at `0x10000`; esptool reported sector erasure limited to `0x10000–0x1CDFFF` and successful data-hash verification. Bootloader, partition table and NVS were not flash targets; no full-chip erase or eFuse programming was performed.

Flash verification: PASS. Device tests: NOT RUN for functional/clock acceptance of the new image. Normal startup, retained account visibility and new time synchronization still require screen confirmation; a successful write is not proof of them. No serial monitor was opened after flashing because this USB path can reset the board. The port was released, and subsequent clock evidence should use the read-only HTTP fields after a new user-aligned baseline. The build-stage no-device-access statement above predates this separately authorized update. No commit or push was made.

## Next revision: divided fast RC, learned ratio and normal SDK maintenance

### Confirmed scope and status

The requested revision excludes adjacent TOTP-window displays. The selected scope is RC_FAST/256, source-specific proportional sleep correction, and normal-SDK maintenance approximately every 900 seconds without display/audio/radio startup. The selected learning reference is explicit synchronization from an accurately set phone browser; ordinary manual time entry remains available but does not train ppm. These are design requirements, not implemented or hardware-validated behavior yet. No new commit, push or flash is authorized.

The reported `82bffa0a…` result after an accurate November 13 00:00:00 baseline was a display about 326 seconds fast at a reference around 10:50. The retained comparison reported a +13.102641-second private-minus-SDK replacement, source period 3740736 Q19, no maintenance samples and no qualified temperature model. This is not a coefficient for the new clock source. The HTTP timestamp and display observation were not established as simultaneous. Do not save the original response, AP-session token or device identifiers.

### Clock source and records

Select `CONFIG_RTC_CLK_SRC_INT_8MD256=y`, `CONFIG_RTC_CLK_CAL_CYCLES=3000`, normal maintenance enabled, and `CONFIG_AUTH_RTC_WAKE_STUB=n`. The legacy symbol means the C3's nominal 17.5 MHz source divided by 256. Measure the actually selected source using the appropriate MUX/8MD256 calibration selector, with 3000-cycle private calibration and source checks before/after it. This does not remove the SDK's separate short sleep-entry calibration or prove a power/accuracy number.

Keep the private fractional 48-bit/Q19 ledger and the existing HRT/UTC capture checks. Tag every active session, coefficient and temperature record with source/reference/schema identity; separate retained-session versioning from persisted model versioning. Preserve old source records without relabelling, overwriting or applying them to the new source. New-source correction is zero until qualified references establish it. Temperature fitting remains diagnostic/learning-only, not another UTC correction. No account/PIN blob or partition-layout changes.

### Proportional correction and exactly-once accounting

Positive `ppm` means the uncorrected bridge runs fast. Apply `corrected_us = raw_us * 1000000 / (1000000 + ppm)`, with a positive denominator, a software admission range of -20000 through +20000 ppm, checked quotient/remainder arithmetic and explicit fractional carry. The range is a rejection guard, not a chip tolerance. Never multiply a long microsecond duration by one million before checking overflow. Freeze the coefficient for an active segment; changes affect later segments only. Keep cumulative uncorrected bridge time separate from corrected UTC and measured awake HRT.

Use one successful absolute UTC replacement per armed segment, consume it once, then create a new anchor from the resulting HRT-backed UTC before maintenance re-sleep. Do not add a complete elapsed value to SDK UTC, apply ppm twice or retroactively recalculate an interval with a new rate/coefficient. Retain chain-level raw/corrected totals and maintenance count for diagnostics. Pair HRT at bridge closure and at the next anchor so maintenance/NVS/calibration dwell is counted once as awake HRT. The opaque SDK entry tail and ROM/pre-capture startup remain inside the RTC bridge; do not label it pure physical sleep or claim unknown boot time was removed. If sleep is cancelled/rejected after preparing a new anchor, abort that unentered segment and roll back its provisional accounting before normal startup. A failed setter must leave its old active segment retryable.

### Browser reference and learning admission

Reuse the status HTTP sample as a bounded HRT probe; provide an explicit HRT-valid marker even before UTC is set and a bounded probe identifier/cache tied to the existing AP session. The browser records UTC and monotonic time around that request and submits an explicit synchronization with the probe identity, paired reference UTC and uncertainty. Reject stale/replayed/cross-session probes for learning. Measure device queue age with HRT, advance the reference to execution before setting time, and check the setter before publishing a reference or coefficient. Legacy commands without quality metadata and manual edits still synchronize normally but do not train.

Browser system time is an external assumption, not firmware-verified UTC: training requires an accurately configured phone and a declared conservative reference-clock bound. Use one second as the initial declared phone/quantization bound plus measured transport, probe-window and capture uncertainty; include a conservative 50 ppm budget for measured awake HRT. An unknown bound, client clock step or inconsistent monotonic timing makes the sample ineligible. Do not call this an absolute accuracy guarantee.

For two eligible same-source references compute `true_bridge = reference_utc_delta - measured_awake_hrt_delta` and `candidate_ppm = (raw_bridge_delta - true_bridge) * 1000000 / true_bridge`. Never learn from the already-compensated wall error or from the SDK-versus-private comparison. Initial software gates are positive/ordered intervals, at least six hours of raw bridge exposure, at most 30 days per observation, uninterrupted valid retention, and combined uncertainty no greater than 100 ppm of exposure. Require two nonoverlapping eligible observations whose candidates agree within the larger of 200 ppm and their combined uncertainty before publishing a coefficient. These are conservative admission rules, not promised residual UTC accuracy. Insufficient data continues from the last eligible baseline; a consumed, rejected or duplicate observation must not be retried as new evidence.

Cold/software/brownout reset, corruption, source change or arithmetic failure invalidates learning continuity without erasing credentials or the last valid saved coefficient. Validate source/version/CRC/bounds before use. Persist only qualified significant changes, with at most one automatic write attempt per conservative 24-hour budget; failed writes consume the budget and maintenance boots never commit NVS. First missing-record save and cold-boot budget semantics must be explicit in tests.

### Fifteen-minute normal maintenance and key handoff

Before any BSP initialization classify the complete wake-cause mask, not only the singular SDK cause which may prioritize timer over simultaneous GPIO. Only deep-sleep reset, an intended armed timer, a valid matching session/trusted UTC and no user evidence permit silent maintenance. Keep the standard SDK stub; do not use the experimental calibration/re-sleep stub. Initialize only required SDK/NVS/clock resources, restore the previous segment once, take the new direct measurement, and arm a 900-second timer plus GPIO0 low wake. No I2C, ADC button component, LVGL/display, audio, Wi-Fi, BLE, temperature sampling or NVS commit in this route; no new permanent task or unbounded history.

Install the GPIO0 digital falling-edge observer as early as application ownership permits, before NVS work or private measurement. A tiny ISR only sets a sticky flag; check the flag, pending hardware status and current low level around all potentially blocking work and before re-sleep. Never clear accepted user evidence because the key was released. Use the stock rejection-capable deep-sleep API. Setup failure, mixed/unknown cause, invalid state, observed key, cancellation or rejected sleep selects full normal startup, not a maintenance retry loop. Abort provisional sleep accounting on return. Remove only observer resources owned by this route before handing GPIO0 to BSP ADC/buttons; retain the existing entire-gesture quarantine and button-timer ownership. Hardware power removal remains a cold restart.

There are unproven physical limits before the earliest observer (including ROM/SDK startup calibration) and in the final SDK/CRC tail after the last decision. A released short pulse in those intervals is not guaranteed captured. Do not equate the private measurement window with the full blind interval or claim arbitrary short-press coverage. Physical pulse sweeps, held keys, mixed wakes and power-off must be tested; a missed valid target gesture is a failed acceptance item, not a reason to weaken the criterion. SDK rejection covers an asserted level, not every completed short pulse.

### Normative boundary refinements

- A setter returning success is an irreversible application boundary even if its subsequent 1 ms timing check fails. Mark that segment consumed/applied-uncertain, invalidate learning continuity, record the fault, select full startup with UTC untrusted, and prohibit a second replacement in the same boot. Only a setter returning failure without changing wall time leaves the old active segment retryable. A non-deep-sleep reset invalidates retention as usual.
- After segment N is restored, a prepared N+1 is provisional until sleep. Cancellation/rejection discards only N+1's anchor, arming and provisional closure. Preserve N's consumed state, accepted raw/corrected totals, fractional state and restored UTC. Continue the awake interval from N's captured HRT through cancellation; do not restore N+1 or resurrect N on later startup/re-arm. Save and restore the post-N checkpoint around a provisional commit; cumulative awake time is reevaluated through the current HRT rather than lost or counted twice.
- Retain raw cumulative time as whole microseconds plus Q19 fraction. Retain the corrected UTC phase fraction in Q19 separately from the proportional divider remainder and its denominator. Raw segment deltas are added to cumulative raw coordinates once; a new segment does not reapply the old cumulative fraction. Integer UTC setters publish only the whole part; the residual phase fraction survives consume/re-arm and is attached to the new anchor, not added twice as a historical correction. Quotient/remainder scaling returns whole/Q19 output plus a remainder less than `D = 1000000 + ppm`; intermediate products must fit checked 64-bit arithmetic. If D changes, convert `r_new = floor(r_old * D_new / D_old)` before use; represented-time loss is less than `1/(D_new * 2^19)` microseconds per conversion and is included in uncertainty. Never reinterpret an old numerator under a new D. Explicit accepted time setting resets the phase fraction deliberately; raw learning snapshots remain independent. Oracle tests cover segmentation invariance, coefficient changes and abort rollback.
- Maintain a cumulative awake coordinate across boots; never subtract HRT values from different boots. Each completed bridge covers its raw-tick anchor through the paired closure tick. Each known-awake interval covers that closure's HRT through the next anchor, including partial intervals at browser probes. At any reference HRT, atomically snapshot cumulative raw bridge time and cumulative known-awake time, deriving the latter from the current boot's closure/base plus HRT advancement. No interval belongs to both coordinates. For a probe processed later in the same boot, subtract the measured HRT age from the execution-time awake coordinate. A manual setter terminates learning continuity; a successful eligible browser setter snapshots the old coordinates before changing UTC, consumes any old observation once and starts the next reference window at the paired point. Insufficient exposure extends the existing reference window, whereas an invalid/rejected complete observation starts a new baseline rather than reusing its failed interval. Boot count alone must not change the learned ratio for equivalent raw/awake/reference traces.
- The HTTP probe cache has four numeric entries, nonzero monotonically advancing IDs and existing AP-session binding; invalidate it on AP restart. UTC at the probe is the midpoint of browser UTC observations surrounding the GET, paired with the device's returned HRT midpoint. Device execution target is that reference UTC plus `execution_hrt - probe_hrt`. Client monotonic RTT must be positive and at most 2 seconds; UTC-versus-monotonic elapsed disagreement must be at most 100 ms. Device sampling window must be at most 1 ms. Total probe-to-execution HRT age, including browser dwell and queueing, must be between zero and 5 seconds for learning. Reference uncertainty includes the declared one-second clock/quantization bound, half RTT, half device window and capture/awake budgets. The execution HRT must belong to the same valid boot. Consume a probe when a valid submission is accepted for dispatch, including queue rejection or later setter failure; retry requires a new probe. Replayed, stale or inconsistent quality cannot create a fresh learning observation. An otherwise valid legacy/explicit synchronization may still set time without training, but invalid HRT pairing must never be used to advance a target. Keep all effects conditional on checked setter success.
- Public cache IDs advance monotonically, but the internal reference identity also contains an AP nonce and is opaque across sessions/boots. Reject equal identity or non-increasing independent-reference UTC with a separate UTC watermark; never order nonce-bearing IDs numerically. Advance both watermarks for consumed references, including insufficient exposure and deliberate rebaseline, but not malformed/source-mismatched/arithmetic-failed input. A backward reference correction requires a new continuity. Only a completed boot reconciliation can train; an accepted explicit reference after an unresolved retryable bridge sets time but discards that incomplete learning interval. Accumulate a conservative 2000 microseconds per completed bridge into the active baseline's uncertainty until that window is consumed/rebased, without changing the UTC target. CRC-check retained accounting bytes before decoding booleans.
- Quantize each admitted candidate to the nearest integer ppm with ties toward zero, then publish the raw-exposure-weighted mean of the two consistent integer candidates using the same rounding. Each quantization is bounded by 0.5 ppm; combined coefficient-arithmetic quantization is at most 1 ppm, separately from the 100-ppm reference/accounting admission bound. The 24-hour +1000.4-ppm and 12-hour +1000.8-ppm regression therefore publishes 1000 ppm, not the rounded mean of unquantized ratios. This bound is not a hardware-accuracy claim. A coefficient is persistently significant only when qualified and changed by at least 10 ppm from the last valid saved coefficient. All automatic coefficient persistence, including the first missing-record save, waits for at least 24 hours of valid internal accounting since a cold boot or the last write attempt, whichever is later. A cold boot always restarts the full wait, so losing retained failure state cannot reopen an immediate allowance. A failed attempt consumes the next full budget; absent/corrupt elapsed continuity blocks automatic writes until a new full interval is established. A missing record may receive one qualified first save only after that wait; an invalid/unsupported existing record remains blocked without overwrite. No maintenance commit, no per-sync write and no persistent attempt marker that itself creates a reboot write loop.

### Implementation and acceptance sequence

Extend existing modules rather than introducing a new framework. Test first: source/MUX/record isolation; signed proportional fractions and overflow against an independent oracle; raw/awake learning and reference admission; repeated per-segment restore/re-arm, rollback and failed setter; timer-only routing, sticky GPIO evidence and gesture handoff; HTTP probe/queue age/replay/buffer bounds and web synchronization compatibility. Preserve source-independent PIN/credential regressions and secret-free snapshots. Expose active source, raw/corrected chain totals, ppm qualification, learning rejection reason, maintenance mode/count and fault in bounded diagnostics; distinguish SDK replacement from measured UTC error.

Use the complete host/firmware gate, RTC/math sanitizers, real-LVGL rendering and web tests. Measure retained/normal RAM and stack use. No claim of 20 ms wake, a microampere bound, daily 1–2-second accuracy or proven stability follows from compilation. A later exact-artifact flash requires separate approval, then independent-reference sleep tests, button phase/pulse sweeps, repeated UI/radio/sleep cycles and whole-board wake-charge measurement. The design-stage NOT RUN status is superseded by the Fast-RC revision verification record below; hardware acceptance remains NOT RUN.

### Execution checklist

Implementation is approved. Continue on the existing dirty feature branch; do not commit, push, replace unrelated changes or flash.

- [x] Pure math/learning: `main/rtc_drift.[ch]`, `tests/test_rtc_drift.c`; red/green `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_drift.c main/rtc_drift.c -lm`, independent oracle, ASan/UBSan.
- [x] Browser probe transport: `main/auth_wifi.c`, `main/fido_protocol.h`, `tools/web_auth_companion.html`, `tests/test_rtc_http_status.py`, `tests/test_web_companion.js`; red/green actual handlers and `node tests/test_web_companion.js`.
- [x] Source/record binding: `main/rtc_clock_platform.c`, `main/rtc_clock_internal.h`, `sdkconfig.defaults`, `main/Kconfig.projbuild`, `tests/test_rtc_clock_platform.py`; selected-source MUX/3000 tests and real NVS-function boundary tests.
- [x] Runtime accounting/reference: `main/rtc_clock.[ch]`, private header, time-only `main/auth_storage.[ch]`, `tests/test_rtc_clock.c`, `tests/test_auth_storage.c`; signed ratio, success-but-late terminal state, fractional/awake conservation, learning and persistence budget.
- [x] SDK maintenance and observed-key handoff: startup/sleep portions of `main/main.c`, platform hooks, `tests/test_rtc_clock_contract.py`, `tests/test_key_wake_contract.py`, wake policy tests; no BSP/radio on timer-only route, rejection/abort and next gesture.
- [x] Integration: regenerate `main/auth_web_page.h`, review spec then quality, run `./tools/validate.sh` in IDF 5.5.6 plus rendering/web/sanitizers, verify exact archive and report remaining hardware checks before offering a separately approved flash.

### Fast-RC revision verification

The complete IDF 5.5.6 gate passed and archived `build/firmware/46b6f3c816cc48eda1819b950c7d755a0d9718927a3c1bc76ab7e9728240df15/`; full-image SHA256 is `46b6f3c816cc48eda1819b950c7d755a0d9718927a3c1bc76ab7e9728240df15`, matching ELF SHA256 `b71521d320b65f67466515b924aec6a779fd6bd4adeda72c47675b65530133fb`. Archive verification binds the 1839504-byte application at `0x10000`, 1905040-byte merged image, bootloader, partition table and ELF/MAP. The partition bytes match the previously flashed `82b…` archive. The isolated generated configuration selected RC_FAST/256, 3000 cycles and normal SDK maintenance; the experimental stub is disabled.

Host validation includes 35 pure-math/learning groups, actual runtime/reference tests including 96-segment conservation, 9 wiring/observer checks, 4 platform/NVS boundary checks, 2 actual HTTP handler/parser checks, existing full host regressions, web tests and embedded-page check. Fail-fast ASan/UBSan passed for math and runtime; the real-LVGL CTest passed. Independent review's unresolved-bridge-learning and CRC-before-boolean issues were reproduced, fixed and rechecked. A successful compile is not the +326-second board symptom regression.

Actual linked allocations are 488 bytes for `rtc_clock_retained`, 312 bytes for the normal-RAM clock state, 1248 bytes for the existing public snapshot and 160 bytes for the four probes. The HTTP response uses a fixed 1792-byte stack buffer; application/tick task stacks remain 8192/4096 bytes. These static figures do not establish native free heap or task-stack high-water margins.

Build: PASS. Host tests: PASS. Device tests: NOT RUN for this revision. No device connection, reset or flash was performed; port enumeration found no ESP32 USB port. Remaining: measured long-sleep UTC accuracy and trained-coefficient behavior, pre-observer/final-tail key pulses, held/mixed wakes, repeated boot/UI/radio/sleep stability including the earlier reported freeze, and whole-board wake charge/current. A new exact-artifact flash requires approval, with a powered device and data-capable USB cable. Do not flash the merged image over retained NVS as a routine application update.

### Subsequent authorized Fast-RC application flash

After explicit approval, the exact `46b6f3c8…` archive was reverified and its 1839504-byte application was written at `0x10000` with image parameters kept and esptool data-hash verification passing. The detected target was ESP32-C3 revision 1.1, 40 MHz XTAL and 8 MB Flash; the installed 3072-byte partition table matched the archive byte-for-byte. Only application sectors `0x10000–0x1D1FFF` were erased/written, not NVS, PHY, bootloader or partition sectors. No rebuild, full-chip erase or eFuse operation occurred.

One bounded startup capture before any new time baseline showed the expected application ELF prefix `b71521d32…`, app IDF v5.5.6 and USB_UART_CHIP_RESET (reason 11). The deliberately preserved installed bootloader reports v5.5.3; it was not replaced by the bundle's bootloader. No panic/error marker appeared in this window, but no full application-initialization marker was captured, so rendering and responsiveness are not established by the log. The serial handle was closed and released; do not reopen it after establishing the next reference.

Flash/data verification and application identity: PASS. Device tests: NOT RUN for the functional RTC/key/power matrix. Screen unlock, retained accounts, startup stability, new-source sleep drift, periodic maintenance and learned correction remain pending user/instrument observations. After confirming the UI/accounts, use explicit web synchronization for the new accurate baseline; manual setting does not train ppm. Raw startup data stays only in session scratch; do not copy identities or secrets into this record.

## Approved next design: bounded history and adaptive sleep correction

### Scope, evidence and limits

The approved direction is a maximum of 100 valid external-reference windows, robust adaptive scalar correction, and temperature/battery diagnostics in shadow mode. Cover battery, USB, and transitions. This section is an implementation proposal, not a statement that new code has been delivered. Keep the current source-specific qualified coefficient until a replacement passes admission. Do not connect, reset or flash a device, create a worktree, commit, change the account/PIN blob or partitions, or add adjacent TOTP windows as part of planning.

The existing `46b6f3c8…` revision has reported on-device evidence of applied correction: a USB-powered raw bridge increment of 23904.766086 seconds received 20.815255 seconds of proportional correction, followed by a net backward synchronization of 3.493433 seconds. A battery-powered increment of 34590.434060 seconds received 30.119882 seconds of correction, followed by a net forward synchronization of 1.803122 seconds. These are single trials under uncontrolled thermal conditions, not proof that supply caused the difference. Numerical timestamp resolution is not absolute UTC accuracy. Do not store raw status responses or their tokens.

Thirty offline days within +/-3 seconds requires about 1.1574 ppm mean error before allocating initial/reference error. Existing integer candidate/publication rounding allows up to 1 ppm of arithmetic quantization. The current conservative 2 ms allowance per completed bridge totals about 5.76 seconds over 2880 fifteen-minute intervals. Neither more records nor a lower fit RMS removes these budgets. Do not reduce declared uncertainty without new evidence, promise this accuracy, or infer it from endpoint cancellation.

### Record and learner contract

One observation contains nonoverlapping same-source, continuous, independently referenced coordinates. Define `R` as the uncorrected raw bridge delta including its Q19 fraction, `A` as the cumulative awake delta, `U` as the reference UTC delta, and `T = U - A`. Learn `p = 1000000 * (R / T - 1)`. Never use corrected-chain error, the active coefficient, SDK replacement difference, or internal oscillator measurements as the independent target.

Preserve current admission guards initially: positive/ordered coordinates, source/reference identity, completed restoration, at least six hours raw exposure, at most 30 days per window, +/-20000 ppm bounds and at most 100 ppm combined exposure uncertainty. Preserve the reference, awake and per-bridge uncertainty terms. An admitted row is consumed once; advance its baseline once. Invalid or unresolved continuity cannot be repaired merely by adding a history row.

For an accurately paired, increasing, same-source/same-continuity reference with unchanged raw coordinates, verify nondecreasing awake coordinates and UTC/HRT consistency within the reference bounds. Then advance replay watermarks without replacing the original learning baseline, deleting pending evidence or appending a row. This exception does not apply to backward references, manual setting, source changes, corrupt retention, unresolved bridges or successful-but-late setters. A failed setter has no new history/learner effects; retain existing retry/terminal semantics.

Capacity is 100, not a minimum training count. The existing 12-row awake temperature fit remains a separate diagnostic. For retained and stored values validate raw bytes/CRC before typed boolean or semantic decoding.

### Storage, memory and cold recovery

Use a pure `main/rtc_history.[ch]` boundary for window records, bounded ring operations, explicit serialization and estimator arithmetic. Keep NVS adaptation in `main/rtc_clock_platform.c`, ownership/reference transactions in `main/rtc_clock.c`, and existing build registration in `main/CMakeLists.txt`. Do not refactor unrelated modules.

Proposed wire row ceiling: 80 bytes, explicitly little-endian, with sequence/continuity identity, endpoint UTC, raw delta and Q19 fraction, awake/UTC deltas, uncertainty, source/reference flags, and valid/aged endpoint temperature/battery diagnostics. Unknown or awake-only measurements must not be labelled sleep averages. Store no credentials, AP/probe tokens, device identifiers, account names or raw native structs. The final field table and golden bytes must be fixed by tests before production code.

Capacity candidate: ten logical chunks of ten rows, eleven physical 832-byte chunk slots including one copy-on-write spare, plus two alternating 256-byte manifests. The live serialized ceiling is 9664 bytes; it is NOT an NVS-capacity proof. Headers/manifests contain schema, generation, compatibility, bounds and CRC. Publish a new manifest only after its chunk is committed and verified; never overwrite a chunk referenced by the current committed manifest. Recover the newest internally complete generation, an older complete generation if available, or coefficient-only operation. Do not assume every older manifest remains complete after spare reuse. Invalid/unsupported stores block automatic replacement rather than triggering erase or rewrite loops.

Before integration, prove capacity on an IDF 5.5.6 six-page NVS image with the maximum account blob, bounded FIDO/PIN/identity/configuration keys, coefficient/model records, history replacement, account replacement, fragmentation and garbage collection. A dictionary stub, payload-size sum or empty-partition fit is insufficient. Do not delete security data, enlarge partitions or silently reduce capacity if this gate fails. Report the blocker and redesign the encoding within the approved boundary.

Keep the full history in normal-task RAM, not RTC memory. Incremental normal RAM ceiling: 16 KiB including bounded estimator/serialization scratch; retained incremental ceiling: 256 bytes. These are proposed limits to verify in the final ELF and native heap/stack measurements. Load/estimate/export only on full normal startup/runtime, never sort or reload all 100 rows in silent maintenance. Use a bounded retained pending record if persistence is deferred; expose accepted versus durable sequence. Old closed delta rows may survive cold boot, but an old live coordinate baseline must not reconnect to a new boot epoch.

History persistence has an independent attempt budget, initially no more than one attempt per six hours of valid internal accounting, with failure consuming that budget and cold boot restarting the wait. Write only dirty admitted evidence, not every sync or sensor sample; never commit during silent maintenance. Keep the existing coefficient 24-hour/significance budget unchanged. Immediate cold power loss may lose an uncommitted row; report this explicitly rather than claiming durable capture. Existing qualified correction remains usable if history loading, allocation or saving fails.

### Estimation and environmental diagnostics

First deliver the estimator in shadow mode, so replay compares predictions without changing UTC. Use only compatible, nonoverlapping, admitted source/reference windows; preserve epoch boundaries for coordinate pairing and do not infer supply context from USB/JTAG attachment. User-labelled power context, when collected, is observational metadata, not a verified voltage measurement.

Start with exposure-weighted window rates after uncertainty admission, weighted-median outlier screening, and the existing two-independent-window consistency floor of 200 ppm or combined uncertainty, whichever is larger. Compute rates from stored raw/reference deltas, not rounded historical published coefficients. Keep the current coefficient when evidence is insufficient, conflicting, stale or outside supported conditions. A single new row cannot publish a replacement. Do not invent confidence improvement by dividing correlated reference error by the square root of record count. Fix estimator thresholds, evidence ageing and publication rules from deterministic replay/chronological holdout before enabling adaptation; retain them as explicit versioned parameters. Do not add arbitrary model parameters just to fit these two observed power trials.

Activation is a separate checkpoint after shadow validation: require consistent independent evidence and improvement on newer held-out windows against the unchanged scalar baseline, within their uncertainty. Bound candidate rates by existing admission rules and apply changes prospectively at segment boundaries. Do not relax security deadlines or retroactively recalculate elapsed UTC. Initially retain integer active ppm and current tested ratio arithmetic. Fractional active coefficients require a separately reviewed fixed-point/overflow/remainder contract and oracle tests; extra decimal places alone do not establish physical accuracy.

Reuse normal-runtime temperature and `bsp_battery_mv()` sampling with validity, age and coverage. CW2017 reports cell voltage, not proven VBUS, charging state or oscillator-rail voltage. The charging IC and supply telemetry remain unconfirmed. Do not put the full battery initialization into timer maintenance: it can program a profile and wait for SOC. No maintenance I2C/sensor changes in this increment. Do not fit sleep thermal trajectories from awake endpoint samples or enable a temperature/voltage correction until held-out evidence demonstrates its benefit.

### Read-only export

Keep existing `/api/status` compatibility and fixed-size snapshots. Add bounded numeric summary fields for history count, accepted/durable sequence, current/candidate qualification, last learning result, persistence result and diagnostic validity. Do not append 100 rows to the existing response.

Propose an authenticated, unlocked `GET /api/rtc-history` with at most five rows per page, generation-bound pagination and a stale-generation response. Copy a bounded page from application-owned immutable records under a short lock; format/send outside the lock. HTTP performs no NVS reads/writes, sensor sampling, learning or synchronization. Export only allowlisted calibration fields, never AP/probe IDs, internal epoch identifiers or security data. The web view distinguishes published, pending, no-new-sleep, rejected and not-durable states. Regenerate `main/auth_web_page.h` from `tools/web_auth_companion.html`; do not edit the generated payload independently.

### Test-first implementation sequence

No task below authorizes commits, branch changes or device operations. Each code task starts with a regression that fails for the intended behavior, then the minimum implementation and its focused passing check.

1. **Benign repeated-reference behavior.** Modify `main/rtc_drift.[ch]`, `main/rtc_clock.c`, `tests/test_rtc_drift.c` and `tests/test_rtc_clock.c`. Reproduce pending loss after a zero-sleep valid reference. Test that baseline/pending/coefficient survive and replay watermarks advance; backward/malformed/reference-jump/manual/reset/source/fault paths still invalidate correctly; failed or late setters cannot create evidence. Do not change the active ratio.
2. **Pure history/encoding.** Create `main/rtc_history.[ch]` and `tests/test_rtc_history.c`. Fix the wire table with golden byte fixtures; cover zero/one/100/101 rows, oldest replacement, fractions, invalid duration, overlap, duplicate sequence, epoch isolation, CRC/semantic corruption, overflow and bounded iteration. Register its build in `main/CMakeLists.txt` and host execution in `tools/validate.sh`.
3. **Storage and capacity gate.** Add narrowly scoped history hooks to `main/rtc_clock_platform.c` and declarations to `main/rtc_clock_internal.h`. Extend `tests/test_rtc_clock_platform.py` or add a focused history-store test. Add an actual SDK NVS host/emulated-flash capacity test; pin its runner once the available SDK host target is confirmed. Cut power at each chunk/manifest commit boundary; test unsupported records, full storage, failed writes, post-cold generation recovery and no retry loops. If actual NVS/GC validation is unavailable, mark the capacity gate NOT RUN and stop integration, not PASS.
4. **Task-owned capture and durable cursor.** Modify `main/rtc_clock.[ch]`, `main/rtc_clock_internal.h` and `tests/test_rtc_clock.c`. Append only after successful checked reference application; maintain exact raw/awake ownership, sequence/epoch isolation, one-time consumption and bounded pending state. Test abort, cancellation, deep/cold boot, failed persistence and budgets. Verify the timer route never loads/sorts history or commits NVS; keep existing button/security invariants.
5. **Shadow estimator and environment evidence.** Extend the pure history module/tests with independent replay oracles, unequal exposure, irregular references, outliers, correlated uncertainty, context changes and chronological holdout. In `main/main.c` reuse normal-task battery/temperature cache rather than initialize drivers from HTTP or maintenance. Test missing/stale sensors and unchanged UTC while shadowing. Add no permanent task or automatic network synchronization.
6. **Bounded diagnostics and web.** Modify `main/auth_wifi.[ch]`, `main/main.c`, `tools/web_auth_companion.html` and generated `main/auth_web_page.h`; extend `tests/test_rtc_http_status.py`, `tests/test_web_companion.js` and relevant lifecycle tests. Test unauthorized/locked access, pagination bounds, generation change, integer-width buffer bounds, secret-free export, and repeated sync without pending loss. Publication and durable storage must be distinct in the UI.
7. **Adaptive activation checkpoint.** Replay deterministic fixtures and chronological holdout against the frozen baseline. After scoped design/code review, enable only the validated scalar policy with existing integer scaling, two-window consistency, prospectively frozen segment coefficients and unchanged save budgets. Regress rejected/outlier evidence, coefficient oscillation, old-source history, cold recovery, unresolved bridges and once-only accounting. Failed gates keep the previous coefficient.
8. **Complete validation and documentation.** Update this paired design and only directly affected paired README/API documentation. Run the complete IDF 5.5.6 gate plus RTC/history ASan/UBSan, real-LVGL and web tests. Report actual retained/normal RAM, history storage occupancy, stack/heap and exact artifact identity. Invite a separately approved application-only flash; do not operate the device during planning or host implementation.

Focused host commands below use session-scratch executables. The full validator also writes durable firmware/debug archives under repository `build/`, currently creates temporary work under `/tmp`, and may install missing `actionlint`; these are not scratch-only commands. Before future gate execution, narrowly route its temporary directories and cleanup through `COMMANDCODE_SCRATCHPAD` while preserving durable archive destinations and failure behavior. Do not run installs or gates during this planning change.

```sh
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_drift.c main/rtc_drift.c -lm -o "$COMMANDCODE_SCRATCHPAD/rtc-drift"
"$COMMANDCODE_SCRATCHPAD/rtc-drift"
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_clock.c main/rtc_drift.c -lm -o "$COMMANDCODE_SCRATCHPAD/rtc-clock"
"$COMMANDCODE_SCRATCHPAD/rtc-clock"
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_history.c main/rtc_history.c -lm -o "$COMMANDCODE_SCRATCHPAD/rtc-history"
"$COMMANDCODE_SCRATCHPAD/rtc-history"
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_clock_platform.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_http_status.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_rtc_clock_contract.py
node tests/test_web_companion.js
python3 tools/embed_web_page.py --check
./tools/validate.sh --static
./tools/validate.sh --firmware
./tools/validate.sh
```

New history commands run only after their files exist. Confirm the embedded-page check CLI before use; the maintained web test also verifies generated-page equality. Extend the real gate rather than treating an unregistered test file or source regex as capacity proof.

### Hardware acceptance and stop conditions

After separate flash approval, verify account/PIN retention, reference admission, bounded export, coefficient fallback, repeated maintenance and user wakes before accuracy trials. Compare fixed versus adaptive behavior using independent-reference multi-day trials, explicit battery/USB/transition conditions and observed temperature ranges. Use separate training and later validation windows, then a continuous 30-day no-corrective-sync holdover with bounded initial/reference uncertainty and intermediate excursion checks. Reading telemetry can change power/temperature; document that measurement condition. Measure board wake charge before any future cadence/sensor changes.

Stop claiming progress toward monthly accuracy if reference/accounting uncertainty cannot resolve the target, capacity would endanger security storage, held-out behavior does not improve, or key/power stability regresses. Fall back to the last validated scalar/direct-measurement policy without extrapolating environmental models or silently enabling network time. The target remains best effort; an external precision timebase may still be necessary for a hard specification.

### Review refinements: enforceable first-increment boundaries

These refinements control over any less-specific wording above.

- **Shadow isolation:** before the activation checkpoint, gate both the existing two-window publisher and the history estimator. Keep candidate learning in separate shadow state; neither path may replace the active qualified coefficient or its stored value. Restore/use the compatible existing saved coefficient normally; candidate evaluation is not a save-significance event. Regress two publishable windows, repeated references, deep wakes and cold recovery with a preexisting qualified coefficient. Do not freeze UTC itself or disable its existing prospective ratio correction.
- **Pending queue:** reserve two 80-byte serialized rows plus at most 64 bytes of metadata/CRC, a total of at most 224 retained bytes within the 256-byte ceiling. On normal deep wake, rebuild ordinary history from the committed generation and merge valid pending rows by compatible sequence identity. If a committed row is still pending because acknowledgement was interrupted, acknowledge it without reinsertion. When both slots are occupied and no write is allowed or succeeds, do not overwrite either: consume/rebaseline the new valid reference once, advance the accepted sequence, increment an explicit dropped-evidence counter, do not feed that dropped row to the estimator, and report degraded history while retaining the current coefficient. Track accepted, durable and dropped counters in retained metadata and checkpoint them only with an allowed manifest update. Cold loss restores the last committed counters and may lose newer volatile counters/rows; expose the new boot's incomplete continuity rather than inventing exact lost counts. Test repeated failures across multiple admissions/deep wakes, full queue, and reset between manifest commit and acknowledgement.
- **One publication/copy contract:** the application owns a fixed normal-RAM history cache. All append, oldest replacement, generation/cursor updates, reload publication and invalidation participate in the same history-specific lock. Provide a bounded page-copy API that atomically copies generation, count/cursor and at most five rows into caller-owned storage. Formatting and transmission occur after unlocking; retain no application pointers. Invalidate/synchronize cache replacement or teardown with copying before memory reuse. HTTP must not call mutable learner/RTC internals or assume the existing status lock protects this cache. Test concurrent oldest replacement, reload, stale pagination and AP lifecycle; independent status/page responses need not have the same sampling instant, but each is internally coherent.
- **Validation runners:** the embedded-page `--check` CLI is confirmed. After history integration, runtime/history host links include `main/rtc_history.c` and `main/rtc_drift.c`. Use the exact sanitizer/rendering commands below after those files/tests exist. Before claiming the capacity gate runnable, discover and record the pinned IDF 5.5.6 NVS host/emulated-flash target, its exact invocation, six-page geometry and fixture keys. If unavailable, keep that gate NOT RUN and stop integration; do not substitute mocked GC or a guessed command. Narrow temporary-directory routing in `tools/validate.sh` is required before executing the later complete gate; durable `build/` artifacts remain intentional.

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Imain tests/test_rtc_history.c main/rtc_history.c main/rtc_drift.c -lm -o "$COMMANDCODE_SCRATCHPAD/rtc-history-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$COMMANDCODE_SCRATCHPAD/rtc-history-san"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Imain tests/test_rtc_clock.c main/rtc_history.c main/rtc_drift.c -lm -o "$COMMANDCODE_SCRATCHPAD/rtc-clock-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$COMMANDCODE_SCRATCHPAD/rtc-clock-san"
cmake -S tests/auth_ui_host -B "$COMMANDCODE_SCRATCHPAD/rtc-history-lvgl"
cmake --build "$COMMANDCODE_SCRATCHPAD/rtc-history-lvgl"
ctest --test-dir "$COMMANDCODE_SCRATCHPAD/rtc-history-lvgl" --output-on-failure
```

Planning-stage status: design direction approved; implementation, capacity proof and new adaptive hardware tests NOT RUN. Existing firmware remains unchanged. Source review and documentation checks do not replace build, host or device validation of the proposed increment.

### Authorized implementation amendment: isolated history partition

During implementation, inspection confirmed that `AUTH_MAX_FIDO_RPS` limits enumeration, not credential creation. An eight-RP scenario therefore cannot prove worst-case coexistence in the original six-page NVS. The user explicitly approved an independent 64 KiB history partition instead of restricting credentials or sharing that capacity.

This approval supersedes the earlier no-partition-change/shared-six-page history proposal only as follows: preserve `nvs` at `0x9000`/`0x6000`, `phy_init` at `0xF000`/`0x1000`, and the factory start at `0x10000`; reduce factory size to `0x7E0000`; add data/NVS partition `rtc_hist` at `0x7F0000`, size `0x10000`. Total Flash remains 8 MiB. Application, account/PIN blob and original security keys keep their existing locations. This is authorization to implement the layout, NOT to flash it; a later partition-table/app write needs exact-artifact approval. Never write a merged image over retained NVS for this upgrade.

History opens only the named `rtc_hist` partition, with independent initialization/error handling and no automatic erase. Validate the production codec/chunk/manifest protocol against actual unchanged IDF 5.5.6 Storage/Page/GC sources on sixteen 4096-byte emulated NOR pages. A standalone native harness may provide only portability/NOR adapters, not substitute an in-memory dictionary for NVS. Include repeated changed-data rotations, physical reclamation, readback/reopen and interruption boundaries. Partition-routing tests remain separate from engine-capacity proof; preserve and verify the original security image independently. Do not claim native capacity until the harness builds and passes.

Task 1 implementation has passed its 37-group pure suite, runtime suite and ASan/UBSan, with separate specification and code-quality approvals. It adds no-new-sleep result 13 without renumbering old results. Full feature integration, firmware gate and device acceptance are not yet complete. The running device has not been accessed or changed.

### Implementation checkpoint: history, storage and shadow arithmetic

The pure 100-row ring, explicit 80-byte codec and five-row generation-bound pagination have passed focused host tests, sanitizers, specification review and quality review. Tests include golden bytes, all 640 single-bit corruptions of a row, semantic corruption with a repaired CRC, replacement and failure atomicity. The production copy-on-write store has separately passed reopen, deduplication, rotation, interrupted-publication and error-latching tests. Full open/append and all readers must share application-task ownership; protecting only the final RAM publication is insufficient.

The actual unchanged ESP-IDF 5.5.6 NVS engine now runs through the native `tests/rtc_nvs_host` harness. On the isolated sixteen-page history fixture, 650 changed-data appends exercised 14,024 programs and 202 erases; the final retained 100 rows matched after reopening. The measured entry counts were 329 used and 1,687 free. All 148 selected protocol/torn-NOR fault cases recovered a complete old or new generation without hybrid rows in that run. A separate six-page security fixture remained unchanged. These are native storage-engine results, not board power-cut validation or proof of every possible interruption.

The shadow estimator has passed 12 focused groups and scoped specification/quality review. It uses compatible independent windows, exposure/recency/reference-quality weights and weighted-median inliers. The newest eligible window is excluded from fitting when at least three are available. Uncertainty retains reference and observed spread rather than shrinking by record count. A regression reproduced rejection of eight valid -20000 ppm rows due to floating-point averaging; median-anchored weighted differences and an observed-inlier-range clamp fixed it without relaxing integer admission. Both endpoints now pass heterogeneous-weight cases for every history size from 3 through 100. `READY` and holdout improvement are diagnostic shadow results, not permission to change the active coefficient or a monthly-accuracy claim.

The firmware delivery path now runs the native NVS CTest before merging/verifying/archiving. Static validation remains SDK-independent. Temporary validation directories use the session scratch root when available, preserve paths with spaces, and clean only their own allocated directory on failure; four routing/cleanup regressions pass. Durable firmware archives remain under `build/`.

Build: NOT RUN for the integrated increment. Host tests: PASS for the focused history/store/estimator and temporary-path checks described here; the integrated complete gate remains pending. Device tests: NOT RUN. Unverified: runtime/export integration, linked resource totals, real-board heap/stack and power-cut behavior, and independent multi-day/30-day holdover. No device access, flashing, commit or push has occurred during this implementation checkpoint.

### Integrated first-increment verification — 2026-11-15

The first shadow increment is implemented: checked reference capture, two-row retained queue, isolated durable history, current-UTC shadow refresh, and authenticated bounded HTTP/browser export. The existing saved active coefficient remains frozen across both learning paths; the adaptive activation checkpoint is deliberately not enabled. Deterministic fixtures and source review do not replace independent board holdout evidence.

Review found and reproduced three recovery defects before delivery. Manifest schema 2 now checkpoints accepted/dropped counters in the same allowed append transaction, without enlarging its 256-byte blob. Transient adapter initialization is retried under the six-hour budget rather than reopened with null callbacks. A cold start with unreadable existing storage marks its durable prefix unknown; after recovery it rebases only new-epoch pending sequences by the committed accepted watermark, with checked arithmetic and no relabelling of old records. Focused RED/GREEN and subsequent scoped review passed. A further runtime regression verifies deep reboot after manifest commit but before acknowledgment, without duplicate insertion or another write.

`auth_wifi_history_service()` is pumped by the normal application event loop outside LVGL and the FIDO-pending clock-work conditional. The HTTP task never accesses the mutable ring/store. Summary and five-row pages are copied through a bounded mailbox; JSON formatting and transmission happen outside its short lock. The new endpoint carries the history summary so the original 1792-byte status format remains unchanged. Browser export uses generation-bound traversal and an explicit calibration allowlist; no partial download, security data or internal identities are exported. Shadow estimates refresh after normal reload/append and on the normal minute diagnostic cadence, using current UTC rather than a stale row endpoint.

Build: PASS. The complete ESP-IDF 5.5.6 gate built the isolated defaults configuration, ran the actual NVS CTest before archival, verified the custom layout and merged bytes, and retained `build/firmware/240d61a4421706c2cbae883e3867cd2c54d5335bc1a6cdfba918969c97ab0427/`. Full-image SHA-256: `240d61a4421706c2cbae883e3867cd2c54d5335bc1a6cdfba918969c97ab0427`. Matching/embedded ELF SHA-256: `92d4d6be2a565f075d4175590fc2de326bf69760a471824f9ff075e159946801`. The application is 1,872,464 bytes at `0x10000`; the merged image is 1,938,000 bytes. Archive re-verification passed. The default experimental wake stub remains disabled.

Host tests: PASS. The complete static gate includes history codec/ring/estimator/store/runtime, platform and actual HTTP-handler regressions. Separate final checks passed RTC/history/store ASan/UBSan, actual SDK NVS ASan/UBSan, real LVGL drawing-pressure CTest, browser logic tests and embedded-page equality. The NVS capacity/fault results above remain unchanged after the counter-checkpoint extension. Scoped storage/runtime/export reviews have no remaining blockers.

Target ELF/DWARF reports retained state 720 bytes, versus the previous 488 (232-byte increment); the history portion is exactly 224 bytes. The ordinary heap cache for ring/store/estimator workspace is 12,112 bytes; normal clock state is 408 bytes; the mailbox is 608 bytes; its summary/page types are 144/424 bytes. The existing public snapshot remains 1248 bytes. These are linked/type allocation figures and application-buffer budget checks, not a measurement of total incremental SDK/NVS heap, largest free block, task high-water margins or whole-board power. The complete runtime 16 KiB target remains a board measurement item.

Device tests: NOT RUN. No serial connection, reset, flash, erase, eFuse action, commit or push occurred for this increment. Unverified: real-board NVS heap/stack and power-cut behavior, account/PIN retention and AP/UI/sleep/key stability after an authorized update, battery/USB/transition holdout trials, and continuous 30-day no-corrective-sync accuracy. The old installed partition table lacks `rtc_hist`; enabling durable history requires separately approved verified partition-table plus application writes while preserving original security NVS, PHY and bootloader. Do not use the merged image for this data-preserving upgrade.

### Separately authorized history partition/application flash — 2026-11-15

After explicit approval of the data-preserving partition-table plus application update, the exact `240d61a4…` archive above was reverified without rebuilding. Read-only USB discovery found one ESP device; bootloader connection confirmed ESP32-C3 and 8 MB Flash. A 3072-byte read of installed partition metadata matched the previous verified `46b6f3c8…` table. No account/PIN storage or full installed firmware was read out as a prerequisite.

Only the approved 3072-byte partition table at `0x8000` and 1,872,464-byte application at `0x10000` were written, preserving image parameters. Esptool reported erasure only at `0x8000–0x8FFF` and `0x10000–0x1D9FFF`, with both data hashes verified. Original NVS (`0x9000–0xEFFF`), PHY (`0xF000–0xFFFF`) and bootloader were not flash targets. No merged image, full-chip erase, eFuse programming, commit or push was used. This establishes the write ranges, not screen-confirmed account/PIN retention.

A single bounded startup capture before any new reference baseline observed the expected application ELF prefix `92d4d6be2…`, version `bd5f43c-dirty`, USB_UART_CHIP_RESET and the deliberately preserved ESP-IDF 5.5.3 bootloader. No panic/abort/brownout or explicit error marker appeared in the captured window, but the application-initialization-complete marker was not captured. The serial handle was closed and released; do not reopen it after establishing the next clock baseline. Raw device logs remain only in session scratch.

Build: PASS and Host tests: PASS for the unchanged verified artifact. Flash/data-hash verification and startup application identity: PASS. Device tests: NOT RUN for the functional/accuracy acceptance matrix. Unverified: visible startup/unlock, original accounts/PIN, the RTC-history web view and source-specific saved coefficient, native heap/stack, repeated maintenance/user wakes, power cuts and multi-day/30-day precision. Confirm UI/account preservation first, then explicitly synchronize from an accurately set phone to establish a new baseline; initial history may be empty. Successful flashing is not functional or monthly-precision acceptance.

## Millisecond phone synchronization follow-up

The [approved synchronization plan](phone-time-sync-plan.md) replaces the current
page's single status probe with 12 bounded four-timestamp exchanges, handler-time
subtraction and a median offset from consistent low-delay samples. A successful
batch submits one reference; failed batches do not set time. Legacy pages keep
their original protocol. The endpoint cache is separate from status polling,
and version-2 references expire in both the HTTP and owner stages. Transport
uncertainty and the assumed one-second phone UTC bound remain separate metadata,
combined conservatively for the existing learning interface. This change neither
activates shadow coefficients nor proves millisecond absolute UTC accuracy.
