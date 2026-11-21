#ifndef RTC_CLOCK_INTERNAL_H
#define RTC_CLOCK_INTERNAL_H

#include "rtc_clock.h"
#include "rtc_drift.h"

#define RTC_CLOCK_MAGIC UINT32_C(0x52434c31)
#define RTC_CLOCK_RETENTION_MAGIC UINT32_C(0x52434d31)
#define RTC_CLOCK_VERSION UINT32_C(5)
#define RTC_CLOCK_MODEL_VERSION UINT32_C(1)
#define RTC_CLOCK_SAVE_INTERVAL_US (UINT64_C(86400) * 1000000)
#define RTC_CLOCK_COOLDOWN_WAKE_US (UINT64_C(120) * 1000000)
#define RTC_CLOCK_STEADY_WAKE_US (UINT64_C(900) * 1000000)
#define RTC_CLOCK_MAINTENANCE_US (UINT64_C(300) * 1000000)

typedef enum {
    RTC_CLOCK_FAULT_NONE = 0,
    RTC_CLOCK_FAULT_SOURCE,
    RTC_CLOCK_FAULT_CALIBRATION,
    RTC_CLOCK_FAULT_LEDGER,
    RTC_CLOCK_FAULT_GAP,
    RTC_CLOCK_FAULT_CAPTURE,
    RTC_CLOCK_FAULT_SLEEP_RETURNED
} rtc_clock_fault_t;

/* 只存数值，不保存堆指针或驱动句柄；平台层将整个对象放进 RTC data。 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    rtc_drift_ledger_t ledger;
    int64_t utc_anchor_us;
    uint32_t utc_trusted;
    uint32_t user_wake;
    uint32_t fault;
    uint32_t samples;
    /* 重睡成功时不含最终 CRC 尾部；取消时可含更晚检查点，不是全路径峰值。 */
    uint32_t pre_tail_gap_upper_us;
    uint32_t maintenance_enabled;
    uint32_t source;
    int32_t ppm;
    uint32_t anchor_phase_q19;
    uint32_t continuity;
    uint32_t checksum;
} rtc_clock_session_t;

typedef struct {
    uint32_t version;
    uint32_t source;
    uint32_t continuity;
    rtc_drift_time_t raw_total;
    rtc_drift_time_t corrected_total;
    uint64_t awake_total_us;
    rtc_drift_ratio_t ratio;
    uint32_t utc_phase_q19;
    uint32_t maintenance_count;
    uint32_t maintenance_attempts;
    uint32_t maintenance_failures;
    uint32_t last_maintenance_reason;
    rtc_drift_learning_t learning;
    int32_t active_ppm;
    uint32_t active_qualified;
    rtc_clock_calibration_t calibration;
    uint64_t coefficient_budget_us;
    uint32_t learning_reason;
    uint32_t reference_status;
    uint32_t checksum;
} rtc_clock_accounting_t;

#define RTC_CLOCK_HISTORY_INTERVAL_US (UINT64_C(21600) * 1000000)

typedef struct {
    uint8_t rows[2][80];
    uint64_t epoch;
    uint64_t accepted_sequence;
    uint64_t durable_sequence;
    uint64_t dropped;
    uint64_t budget_us;
    uint64_t generation;
    uint32_t version;
    uint32_t count;
    uint32_t flags;
    uint32_t checksum;
} rtc_clock_history_retained_t;

_Static_assert(sizeof(rtc_clock_history_retained_t) == 224, "retained history budget");

typedef struct {
    rtc_clock_session_t session;
    rtc_clock_accounting_t accounting;
    rtc_drift_training_t training;
    rtc_drift_model_t model;
    uint64_t next_save_rtc_us;
    uint32_t magic;
    uint32_t maintenance_blocked;
    rtc_clock_history_retained_t history;
} rtc_clock_retained_t;

extern rtc_clock_retained_t rtc_clock_retained;

typedef enum {
    RTC_CLOCK_STORE_MISSING = 0,
    RTC_CLOCK_STORE_VALID,
    RTC_CLOCK_STORE_BLOCKED
} rtc_clock_store_result_t;

/* 薄平台接口可在主机测试替换，生产实现仍使用 SDK 的真实返回值。 */
uint64_t rtc_clock_platform_history_epoch(void);
uint64_t rtc_clock_platform_ticks(void);
int64_t rtc_clock_platform_uptime_us(void);
uint64_t rtc_clock_platform_rtc_us(void);
bool rtc_clock_platform_get_wall_us(int64_t *wall_us);
bool rtc_clock_platform_set_wall_us(int64_t wall_us);
bool rtc_clock_platform_temperature(int32_t *temp_mc);
bool rtc_clock_platform_sensor_stop(void);
uint32_t rtc_clock_platform_measure(void);
uint32_t rtc_clock_platform_source(void);
bool rtc_clock_platform_supported(void);
rtc_clock_store_result_t rtc_clock_platform_load(rtc_drift_model_t *model);
bool rtc_clock_platform_save(const rtc_drift_model_t *model);
rtc_clock_store_result_t rtc_clock_platform_load_coefficient(int32_t *ppm);
bool rtc_clock_platform_save_coefficient(int32_t ppm);
bool rtc_clock_platform_arm(bool request_maintenance);
bool rtc_clock_platform_arm_interval(uint64_t interval_us);
bool rtc_clock_stub_install(void);

/* 会话校验函数只做整数运算；最终固件还必须审计实际 RTC/ROM 调用图。 */
uint32_t rtc_clock_checksum(const void *data, size_t size);
void rtc_clock_session_seal(rtc_clock_session_t *session);
bool rtc_clock_session_valid(const rtc_clock_session_t *session);

#endif
