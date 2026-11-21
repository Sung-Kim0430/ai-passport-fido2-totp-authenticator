#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "rtc_history_store.h"

typedef enum {
    RTC_CLOCK_CALIBRATION_NONE = 0,
    RTC_CLOCK_CALIBRATION_LEGACY,
    RTC_CLOCK_CALIBRATION_STABLE,
    RTC_CLOCK_CALIBRATION_RECOVERY
} rtc_clock_calibration_quality_t;

typedef struct {
    uint32_t quality;
    uint32_t estimator_version;
    uint64_t evidence_sequence;
    uint64_t evidence_utc_us;
    uint32_t uncertainty_millippm;
    uint32_t uncertainty_valid;
} rtc_clock_calibration_t;

typedef struct {
    uint64_t generation;
    uint64_t accepted_sequence;
    uint64_t durable_sequence;
    uint64_t dropped;
    uint64_t save_budget_us;
    uint32_t durable_count;
    uint32_t pending_count;
    uint32_t flags;
    bool ready;
    rtc_history_store_result_t store_status;
    rtc_history_store_result_t save_result;
    int32_t active_ppm;
    bool active_qualified;
    int32_t legacy_candidate_ppm;
    bool legacy_candidate_qualified;
    rtc_clock_calibration_t calibration;
    bool coefficient_saved;
    bool coefficient_save_blocked;
    int32_t saved_ppm;
    bool saved_ppm_valid;
    uint64_t coefficient_save_budget_us;
    rtc_history_estimate_t shadow;
} rtc_clock_history_summary_t;

rtc_clock_history_summary_t rtc_clock_history_owner_summary(void);
bool rtc_clock_history_owner_page(uint64_t generation, size_t cursor, rtc_history_page_t *page);
void rtc_clock_battery_cache_update(bool valid, uint16_t millivolts, int64_t stamp_us);

typedef enum {
    RTC_CLOCK_BASELINE = 0,
    RTC_CLOCK_RESTORED,
    RTC_CLOCK_NEEDS_SYNC
} rtc_clock_boot_result_t;

typedef struct {
    uint64_t id;
    uint64_t utc_us;
    uint64_t uncertainty_us;
    int64_t hrt_us;
    bool quality;
    uint32_t max_age_us; /* Zero preserves the legacy set-but-do-not-train policy. */
} rtc_clock_reference_t;

typedef enum {
    RTC_CLOCK_REFERENCE_FAILED = 0,
    RTC_CLOCK_REFERENCE_SET,
    RTC_CLOCK_REFERENCE_UNCERTAIN
} rtc_clock_reference_result_t;

rtc_clock_reference_result_t rtc_clock_apply_reference(const rtc_clock_reference_t *reference);
typedef enum {
    RTC_CLOCK_MAINTENANCE_NONE = 0,
    RTC_CLOCK_MAINTENANCE_ATTEMPT,
    RTC_CLOCK_MAINTENANCE_OBSERVER,
    RTC_CLOCK_MAINTENANCE_NOT_DUE,
    RTC_CLOCK_MAINTENANCE_KEY,
    RTC_CLOCK_MAINTENANCE_PREPARE,
    RTC_CLOCK_MAINTENANCE_GPIO,
    RTC_CLOCK_MAINTENANCE_COMMIT,
    RTC_CLOCK_MAINTENANCE_REJECT
} rtc_clock_maintenance_reason_t;
void rtc_clock_note_maintenance(rtc_clock_maintenance_reason_t reason);
bool rtc_clock_maintenance_due(bool timer_only);
bool rtc_clock_prepare_maintenance(void);
bool rtc_clock_abort_sleep(void);
void rtc_clock_begin_normal_wake(void);

/* 只读数值快照，不含凭据；checkpoint 不代表整个睡眠尾部已实测。 */
typedef struct {
    uint64_t checkpoint_elapsed_us;
    /* 含启动到采样点的经过量；修正差仅相对 SDK 估值，不是外部时钟精度。 */
    uint64_t reconciled_elapsed_us;
    int64_t sdk_correction_us;
    bool sdk_sample_valid;
    uint32_t period_q19;
    uint32_t samples;
    uint32_t pre_tail_gap_upper_us;
    uint32_t fault;
    uint32_t model_observations;
    uint32_t model_rms_millippm;
    bool utc_restored;
    bool user_wake;
    bool maintenance_blocked;
    bool model_qualified;
    uint32_t source;
    int32_t ppm;
    bool ppm_qualified;
    uint64_t corrected_elapsed_us;
    uint64_t raw_chain_us;
    uint64_t corrected_chain_us;
    uint64_t awake_total_us;
    uint32_t maintenance_count;
    uint32_t maintenance_attempts;
    uint32_t maintenance_failures;
    uint32_t last_maintenance_reason;
    uint32_t learning_reason;
    rtc_clock_reference_result_t reference_status;
} rtc_clock_diagnostics_t;

rtc_clock_diagnostics_t rtc_clock_diagnostics(void);

/* 应用任务所有：启动接续在慢 BSP 初始化前执行，失败不得继续宣称 UTC 可信。 */
rtc_clock_boot_result_t rtc_clock_boot(bool deep_wakeup, bool time_trusted);
void rtc_clock_tick(void);
/* 应用任务所有；失败时自动浅睡眠被禁止，调用方须撤销 UTC 可信标志。 */
bool rtc_clock_start_light_sleep(void);
bool rtc_clock_service_light_sleep(void);
/* 普通期准备和传感器卸载；最终锚点另在不可逆交还完成后提交。 */
bool rtc_clock_prepare_sleep(void);
bool rtc_clock_commit_sleep(bool time_trusted);
/* 仅在外部 settimeofday 已成功后调用；不修改 PIN 等单调计时域。 */
void rtc_clock_wall_time_set(void);

#endif
