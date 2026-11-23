<p align="right">
  <a href="rtc-calibration-refinement-plan.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# RTC Calibration Refinement Plan (Estimator v3 / RECENT_READY)

## 1. Context and Operating Model
This plan specifies the architecture and implementation tasks for improving residual-rate adaptation on the ESP32-C3 RTC controller. The current baseline is commit `0745cf8`, where the complete gate passed with no new hardware test results. This increment is limited to residual-rate refinement and calibration diagnostics.

## 2. Architecture and Selection Rules

### 2.1 Decoupling Active Evidence from Estimator Candidates
The system maintains an absolute distinction between the currently active calibration evidence (retained across sleep cycles) and candidate adjustments proposed by the estimator. Active quality remains partitioned into four retained states:
- `NONE` (0): No valid calibration evidence available.
- `LEGACY` (1): An enabled coefficient whose supporting metadata is unavailable.
- `STABLE` (2): Multi-window consistent calibration.
- `RECOVERY` (3): Recovery compensation with empirical uncertainty; physical accuracy remains unverified.

Estimator v3 introduces an ephemeral evaluation quality enum: `RECENT_READY` (4). `RECENT_READY` represents a prospective refinement candidate that has passed independent holdout replay validation. It is never treated as initial qualification evidence and cannot activate an uncalibrated device directly.

### 2.2 Estimator v3 Triple-Window Selection and Validation
When the global strict consistency check evaluates to `INCONSISTENT`, Estimator v3 executes the following deterministic procedure:
1. **Window Selection**: Identifies strictly the newest three eligible, same-source, non-overlapping windows whose age is `<= 7 days`. Skipping newer excluded rows or cherry-picking older favorable triples is strictly prohibited.
2. **Candidate Fitting**: The first two chronologically ordered windows are evaluated for mutual consistency under an allowance of `max(200 ppm, sum(reference allowances))`. If consistent, their weighted linear combination yields a rounded integer candidate `C_cand` (ppm).
3. **Independent Holdout Replay**: The third (newest) window acts as an independent holdout and is never included in the candidate fit. The candidate `C_cand` is replayed across the holdout window.
4. **Acceptance Criteria**: Replay must satisfy:
   - Baseline residual error reduction `>= 20%`.
   - Absolute error improvement `>= 1 ppm-equivalent`.
   - The reduction in absolute replay error must strictly exceed the uncertainty margin: `Margin_us = 2 * holdout.combined_uncertainty_us + uncertainty_ppm * bridge_us / (1000000 + candidate_ppm)`.
   - No `sqrt(N)` uncertainty shrinkage is permitted.
5. **Active Rate Refinement Only**: `RECENT_READY` can only refine an already-qualified active rate (including an enabled `LEGACY` coefficient). It cannot override pending/gap/source gates, cause same-triple repeat drift, or introduce steps in UTC or security clocks.

### 2.3 Metadata, NVS, and API Decoupling
- **RTC Retention**: Retained RTC memory layout is version-bumped safely. It stores active quality (0..3), estimator version, latest supporting sequence number, UTC timestamp, milli-ppm uncertainty, and validity flags across deep sleep and shadow recomputations.
- **Flash Persistence**: NVS stores integer ppm coefficients only. Cold-boot coefficients default to `LEGACY` unknown until fresh supporting evidence is validated. Record persistence is strictly separated from coefficient persistence.
- **Export / History API**: Emits an allowlisted calibration payload including distinct fields: `coefficient_saved` (active equals valid saved ppm), `coefficient_save_blocked`, `saved_ppm`, and `coefficient_save_budget_us`. Malformed metadata is rejected, and missing metadata on legacy firmware renders as unknown.
- **Invariants**: 24h/6h flash write budgets, 8 MB partition layouts, account/PIN formats, hardware clocksource, 120s/900s wake timers, and thermal models remain completely untouched.

## 3. Alternatives and Tradeoffs
- **Option A (Adopted - 2-Fit + 1-Holdout with Hard Margin)**: Reserves a separate newest holdout and limits adaptation to consistent evidence without reducing uncertainty via artificial `sqrt(N)` factors.
- **Option B (Rejected - 3-Window Regression)**: Fitting across all three windows risks chasing short-term thermal excursions without out-of-sample rejection.
- **Option C (Rejected - Direct Bootstrapping via Recent Windows)**: Allowing `RECENT_READY` to qualify uncalibrated units risks latching noisy initial crystals without long-interval proof.

## 4. Implementation Tasks
- [x] 1. Add `RTC_HISTORY_ESTIMATE_RECENT_READY = 4` to core calibration types and safely bump the RTC retention memory layout version.
- [x] 2. Implement the strict three-window selector: newest 3 same-source, non-overlapping windows `<= 7 days` old without row skipping.
- [x] 3. Implement weighted two-window fitting and independent third-window holdout replay with `>= 20%`, `>= 1 ppm`, and non-shrunk uncertainty verification.
- [x] 4. Enforce that `RECENT_READY` only refines existing qualified rates and blocks same-triple re-adaptation.
- [x] 5. Decouple record persistence from coefficient flash persistence; track separate save status, blocked status, and elapsed write budgets.
- [x] 6. Update serialization and history export with allowlisted calibration fields and backward-compatible parser guards.

## 5. Verification Commands and Gate Delivery
Write the focused regressions first and observe their expected failures before implementation. Then execute the following checks in the activated ESP-IDF 5.5.6 environment; set `PASSPORT_RTC_ARCHIVE` to the exact archive emitted by the successful gate:
```bash
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_history_estimator.c main/rtc_history.c main/rtc_drift.c -lm -o /tmp/passport-rtc-estimator
/tmp/passport-rtc-estimator
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_rtc_clock.c main/rtc_trace.c main/rtc_history.c main/rtc_history_store.c main/rtc_drift.c -lm -o /tmp/passport-rtc-clock
/tmp/passport-rtc-clock
python3 tests/test_rtc_history_http.py
node tests/test_web_companion.js
./tools/validate.sh
python3 tools/archive_firmware.py verify "$PASSPORT_RTC_ARCHIVE"
```

Delivery status:
```text
Build: PASS
Host tests: PASS
Device tests: PASS (limited to flash hash verification, startup, and loading existing history: app SHA bf4a7d37f747292feb76184a98043626c19281d87986db496a8a4131efef92cf at 0x10000 verified from archive 5dbe4d770819244220c56e81a259b2611360d499ffde50f11cea1cdfa24b9912; startup succeeded; loaded 3 history rows with ready=1, active -7165ppm, no crashes observed)
Unverified: Pending web interface new metadata and export verification, and overnight battery drift checks (no 30-day +/-3s accuracy or account rendering/count verification claimed).
```
*Flashing requires explicit user consent; no git pushes are performed.*

## 6. Physical Overnight Battery Protocol (Unverified)
1. Synchronize device time accurately via reference.
2. Disconnect USB and allow device to enter automatic screen-off and sleep overnight.
3. The following morning, measure reference time delta before the next explicit time synchronization; normal sleep compensation remains enabled.
4. Collect newly formed independent windows for post-run evaluation without reusing fitted windows.

## 7. Deferred Scope
Persistent provenance audit logs, differentiated light/deep sleep regimes, and dynamic thermal/battery modeling are reserved for subsequent milestones.

## 8. Reviewed Acceptance Contract

The deployed integer candidate must agree with the newest holdout under `max(200 ppm, training_uncertainty_ppm + holdout_uncertainty_ppm)`. Define training uncertainty as the maximum of each training reference allowance plus its absolute deviation from the rounded candidate. The final empirical allowance also covers the newest holdout reference allowance plus its deviation; it is not a confidence interval or a future physical bound. Retained milli-ppm values round upward. Candidate replay preserves Q19 fractions and uses the production integer scaler. The one-ppm-equivalent threshold is `holdout_bridge_us / 1000000`; 20% and one-ppm thresholds are inclusive and the uncertainty margin is strict. Neither training window may worsen beyond twice its own reference allowance plus one ppm-equivalent.

Every history-driven activation, across READY, RECENT_READY and RECOVERY, requires an evidence sequence newer than the active tuple and a nondecreasing supporting UTC endpoint. The legacy publication path also records the accepted endpoint when available; unknown legacy uncertainty stays invalid. READY and RECENT_READY map to active STABLE, RECOVERY maps to active RECOVERY. Recomputed shadow candidates, learning resets and save failures cannot replace the active tuple. Same-rate metadata upgrades are deferred: an unchanged Flash-loaded coefficient may remain LEGACY indefinitely. Retained provenance and its watermark survive deep sleep, not complete power loss; ppm-only NVS cannot promise lifetime evidence deduplication.

The API emits `saved_ppm: null` when no valid saved coefficient exists. `coefficient_saved` requires a qualified active coefficient and equality with a valid stored value. Metadata absence permits old-firmware fallback; present null, partial, fractional, out-of-range or inconsistent metadata is rejected by the browser. Test maximum field widths through the real HTTP formatter and retain the existing 768-byte chunk and 1024-byte mailbox bounds. Tests cover ring wrap, exact seven-day age, fractions and integer rounding, noisy or contradictory holdouts, repeat refresh and age-out without a new endpoint, pending/durable gaps, both publication paths, malformed retained metadata with a valid checksum, cold boot and save failure.
