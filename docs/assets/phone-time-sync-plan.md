<p align="right"><a href="phone-time-sync-plan.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Phone Time Synchronization Implementation Plan

> **For agentic workers:** Use the executing-plans workflow, with independent plan and final code review. Preserve the current checkout and do not commit, push, or flash this increment without the corresponding user authorization.

**Goal:** Improve explicit phone synchronization using millisecond timestamps, repeated delay measurements, and one final time update.

**Architecture:** A small authenticated probe endpoint records device receive/send monotonic timestamps. The browser collects a bounded batch, subtracts device processing, and estimates the UTC-to-device offset from consistent low-delay samples. The existing application owner advances the selected reference to its actual execution time.

**Tech Stack:** ESP-IDF 5.5.6 C, browser JavaScript, host C/Python/Node tests.

## Approved design and boundaries

The user approved millisecond timestamps, 8–16 measurements, removal of device processing time, low-delay filtering, and one final update. Implement 12 sequential probes in a three-second sampling budget, requiring at least six valid samples. Select the fastest third (at least three, at most five), take the median UTC-minus-device offset, and reject a selected-group maximum absolute deviation from its median above 20 ms. Use the newest selected probe as the reference anchor. Compare wall elapsed and monotonic elapsed both per sample and across the batch; reject steps rather than accepting them into calibration.

The four timestamps bracket the HTTP handler and browser request, not physical radio transmission. Subtracting handler time leaves transport, scheduling, serialization, and browser overhead. Half the remaining round trip estimates one-way delay under a symmetry assumption; asymmetry remains uncertainty.

Keep phone UTC uncertainty (one second, an assumed bound) separate from transport/quantization/sample-spread uncertainty. Do not lower the phone bound merely because transport is fast. Display transport uncertainty and the phone limitation separately. A batch is one reference event. History estimation stays shadow-only.

Use a separate 16-entry one-use probe cache, isolated from legacy status polling. Require the current session and an unlocked snapshot for probes; retain POST authentication and owner-side unlocked checks. Expire references after five seconds. Invalid version-2 requests must fail rather than fall back to integer seconds. Old pages retain the original legacy protocol. During sampling, own the existing busy flag, pause polling/other mutations, reject device/session changes, and release controls on every exit. No qualified sample means no time-setting command.

## Tasks

### Wire and numerical contract

`GET /api/time-probe` uses `X-Passport-Session` and returns `{version:2,id,received_us,sent_us,valid}`. Device timestamps are nonnegative safe JS integers in microseconds; handler elapsed is at most 500,000 us. The version-2 POST adds `reference_version:2`, `reference_probe_id`, `reference_utc_us`, `reference_rtt_us` (1..500,000), and `reference_spread_us` (0..20,000) to `SYNC_TIME` and its legacy `value`. All fields are integers. UTC is bounded to 2020..2100. The device recomputes `network = rtt - (sent - received)` and rejects negative results. The cached anchor is `received + floor((sent-received)/2)`. The browser maps that anchor to UTC using its selected-group median offset. Transport uncertainty is `ceil(network/2) + 1000 + spread`; add the separate 1,000,000 us phone bound for the existing learning interface. Example: 30,001 us RTT, 20,000 us handler time, and 200 us spread produce 6,201 us transport uncertainty.

Use a 500 ms abortable deadline per probe, limited by the remaining three-second batch budget, covering both fetch and JSON body. Timed-out/invalid individual samples may be skipped; session/lock changes and wall-clock steps abort the batch. A request deadline uses AbortController plus a Promise race so even stalled bodies release busy. No automatic final-POST retry, including ambiguous network failures and result timeouts. Set `max_age_us=5000000` on version-2 owner references and recheck immediately before the actual wall setter; old references retain their current legacy semantics.

- [x] **1. Write failing protocol tests.** Add `tests/test_auth_time_sync.c` for cache isolation/lifecycle, wrap, expiration, one-use consumption, integer bounds, processing subtraction, and uncertainty accounting. Exercise the real probe handler and versioned parser in `tests/test_rtc_http_status.py`. Run focused tests and record the expected missing-feature failures.
- [x] **2. Implement bounded device support.** Add pure `main/auth_time_sync.h`; integrate the cache, lightweight `GET /api/time-probe`, explicit version-2 parsing, and one-use dispatch in `main/auth_wifi.c`. Extend command metadata only as needed. Keep legacy parsing intact and register the ninth HTTP route. Verify host tests.
- [x] **3. Write failing browser tests.** Replace the old single-probe scenarios in `tests/test_web_companion.js` with numerical four-timestamp fixtures, handler-delay exclusion, queue outliers, asymmetry uncertainty, clock steps, insufficient/invalid samples, lock/session changes, busy ownership, concurrent clicks, and exactly one final POST. Include real command-result polling and failure cleanup.
- [x] **4. Implement browser sampling.** Update `tools/web_auth_companion.html` with pure sample/selection helpers and bounded collection. Refactor command submission only enough to hold busy across sampling and final execution. Show sampling progress and separate uncertainty descriptions. Regenerate `main/auth_web_page.h` using `python3 tools/embed_web_page.py`. Verify focused Node and Python tests.
- [x] **5. Integrate documentation and gates.** Register C/Node tests in `tools/validate.sh`, update the root README and paired application design documentation, and index this pair in `docs/README.md` and its translation. Run `./tools/validate.sh --static`, then activate IDF 5.5.6 and run `./tools/validate.sh`.
- [x] **6. Review and hand off.** Resolve independent review findings, verify the exact firmware archive, report build/host/device status separately, discover USB devices read-only, and offer authorized device testing. Check real phone behavior, millisecond offset, Wi-Fi jitter, and RTC continuity on hardware before claiming accuracy.

## Acceptance evidence

Deterministic tests must recover known offsets when device processing is large; queued samples must not pull the chosen offset; asymmetric transport must retain a covering uncertainty estimate. Rejected batches send zero commands. A successful batch sends one command and retains all existing authentication, reference-age, owner-queue, and stored-data boundaries. Firmware compilation and synthetic tests do not establish real phone UTC accuracy or offline drift.

## Validation and device handoff

Build: PASS. The complete gate ran with ESP-IDF v5.5.6 and the existing separate tool installation; the archive was reverified. Host tests: PASS, including the browser regression now registered in the shared gate, pure probe arithmetic/cache tests, actual HTTP parsing/dispatch, and owner expiry before the wall setter. Independent plan and code reviews approved the change; additional mid-batch lock/session/HTTP failures, failed-request clock steps, and stalled-body/deadline tests passed.

The full-image SHA-256 is `c77888d63457dbdaa9f26443cf2e7481ff101ec1322406fe8b311fc3831c3e83`; the matching embedded ELF SHA-256 is `805143e64a5f2c846c7cf2e0ac7729b18ec2598bd906cfcfa94400609615caf8`. The local bundle is `build/firmware/c77888d63457dbdaa9f26443cf2e7481ff101ec1322406fe8b311fc3831c3e83/`. The application is 1,875,344 bytes and the merged image is 1,940,880 bytes.

Device tests: NOT RUN. Read-only discovery found `/dev/cu.usbmodem101`, VID 303a/PID 1001; no serial port was opened. Flash approval for this exact artifact is pending. Use compatible segmented images to avoid writing NVS, PHY and RTC history; a merged 0x0 write can overwrite stored data. No commit or push was performed for this increment.

Unverified: real phone/browser synchronization behavior, measured absolute UTC error, Wi-Fi jitter/asymmetry, and subsequent awake/light/deep-sleep continuity and long-term drift. Synthetic millisecond fixtures and compilation do not establish those hardware results.

## Additional firmware audit

The follow-up audit covered application startup and sleep teardown, RTC
continuation and light-sleep accounting, HTTP/time synchronization, text/CBOR
parsing, BLE credential material, and account storage. It found and corrected:

- Missing, malformed, negative, overflowing or extra text-command arguments
  could become a valid mutation, including deleting OTP account zero.
- Button deletion could race an in-flight shared timer callback. Cleanup now
  disarms the component timer and waits for a one-shot sentinel on the same
  serial timer task; timeout preserves resources and aborts sleep preparation.
  Startup checks button initialization before declaring input ready.
- BLE key, user-handle and counter persistence errors could be ignored. Stable
  key reads and writes now confirm commit, errors preserve existing material,
  and assertions never create a missing RP salt. No volatile counter fallback
  or success with unavailable material remains in these paths.
- The allow-list parser retained only the final credential. BLE now owns a
  bounded copy of the whole list and matches every descriptor after confirmation.
- Incomplete/trailing CBOR, truncated RP identities, and oversized response
  builder lengths were accepted. They are rejected. Display-only labels retain
  UTF-8-safe shortening, independently of strict identity parsing.
- Three host suites required a local scratch environment variable; they now
  use the system temporary directory when the variable is absent.

The follow-up [full-RP storage fix](fido-rp-storage-plan.md) replaces the legacy
12-character namespace for active site records. It preserves old salt during
lazy migration, isolates new user handles and deletion, verifies full RP identity
against its hashed storage key, and uses tombstones to prevent legacy credential
resurrection. Ambiguous legacy rows remain read-only. Previously overwritten
handles cannot be recovered, and old firmware cannot honor the new tombstones.
The experimental BLE model still requires real browser/device acceptance.

Focused regression tests include ASan/UBSan protocol/button checks, NVS
read/size/write/commit failure injection, counter overflow, stale timer barriers,
and first/middle/last allow-list matches. Independent reviews checked the timer
sequence against IDF 5.5.6 and button 4.2.0, and checked response builders at exact
buffer boundaries. Host evidence does not prove hardware callback scheduling,
real browser FIDO compatibility, current draw, or long-term UTC accuracy.

Before the full-RP follow-up, the complete gate and archive reverification passed after the additional audit fixes, using ESP-IDF v5.5.6 with `COMMANDCODE_SCRATCHPAD` explicitly unset. The build identity above names that earlier artifact including the audit fixes; the full-RP plan records its separate validation. Build: PASS; Host tests: PASS; Device tests: NOT RUN.
