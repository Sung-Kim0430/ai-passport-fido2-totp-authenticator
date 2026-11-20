#ifndef RTC_HISTORY_H
#define RTC_HISTORY_H

#include "rtc_drift.h"

#define RTC_HISTORY_WIRE_SIZE 80
#define RTC_HISTORY_CAPACITY 100
#define RTC_HISTORY_PAGE_SIZE 5
#define RTC_HISTORY_UTC_MIN_US UINT64_C(1577836800000000)
#define RTC_HISTORY_UTC_MAX_US UINT64_C(4102444800000000)
#define RTC_HISTORY_MIN_RAW_US UINT64_C(21600000000)
#define RTC_HISTORY_MAX_WINDOW_US UINT64_C(2592000000000)

enum {
    RTC_HISTORY_TEMP_VALID = 1,
    RTC_HISTORY_BATTERY_VALID = 2,
    RTC_HISTORY_AWAKE_ENDPOINT_ONLY = 4,
    RTC_HISTORY_POWER_OBSERVED = 8
};

typedef struct {
    uint16_t flags;
    uint32_t source;
    uint64_t sequence;
    uint64_t epoch;
    uint64_t utc_end_us;
    rtc_drift_time_t raw;
    uint64_t awake_delta_us;
    uint64_t utc_delta_us;
    uint64_t combined_uncertainty_us;
    int16_t temperature_centi_c;
    uint16_t battery_mv;
    uint16_t temp_age_s;
    uint16_t battery_age_s;
} rtc_history_record_t;

typedef struct {
    rtc_history_record_t rows[RTC_HISTORY_CAPACITY];
    uint64_t generation;
    uint64_t evicted_utc_end_us;
    size_t count;
    size_t next;
} rtc_history_t;

typedef struct {
    uint64_t generation;
    size_t total;
    size_t cursor;
    size_t count;
    rtc_history_record_t rows[RTC_HISTORY_PAGE_SIZE];
} rtc_history_page_t;

bool rtc_history_record_valid(const rtc_history_record_t *record);
bool rtc_history_rate_ppm(const rtc_history_record_t *record, double *ppm);
bool rtc_history_encode(const rtc_history_record_t *record, uint8_t *wire, size_t size);
bool rtc_history_decode(const uint8_t *wire, size_t size, rtc_history_record_t *record);
void rtc_history_init(rtc_history_t *history);
bool rtc_history_append(rtc_history_t *history, const rtc_history_record_t *record);
bool rtc_history_get(const rtc_history_t *history, size_t index, rtc_history_record_t *record);
bool rtc_history_page(const rtc_history_t *history, uint64_t generation, size_t cursor,
    rtc_history_page_t *page);

#define RTC_HISTORY_ESTIMATE_VERSION 3
#define RTC_HISTORY_EVIDENCE_MAX_AGE_US (UINT64_C(90) * 86400 * 1000000)
#define RTC_HISTORY_RECENCY_HALF_LIFE_US (UINT64_C(30) * 86400 * 1000000)

typedef enum {
    RTC_HISTORY_ESTIMATE_PENDING = 0,
    RTC_HISTORY_ESTIMATE_INCONSISTENT,
    RTC_HISTORY_ESTIMATE_READY,
    /* Independently validated recovery of an existing rate, not a stable fit. */
    RTC_HISTORY_ESTIMATE_RECOVERY,
    /* Recent consistent training pair with an independent newest holdout. */
    RTC_HISTORY_ESTIMATE_RECENT_READY
} rtc_history_estimate_result_t;

typedef struct {
    double ppm;
    double weight;
    double uncertainty_ppm;
    size_t row_index;
} rtc_history_estimate_sample_t;

typedef struct {
    rtc_history_estimate_sample_t samples[RTC_HISTORY_CAPACITY];
} rtc_history_estimate_workspace_t;

typedef struct {
    uint32_t version;
    uint32_t source;
    rtc_history_estimate_result_t result;
    size_t samples;
    size_t inliers;
    size_t excluded;
    double ppm;
    double uncertainty_ppm;
    double holdout_baseline_error_us;
    double holdout_candidate_error_us;
    bool holdout_present;
    bool holdout_improved;
    uint64_t evidence_sequence;
    uint64_t evidence_utc_us;
} rtc_history_estimate_t;

bool rtc_history_estimate(const rtc_history_t *history, uint32_t source,
    uint64_t now_utc_us, int32_t baseline_ppm, rtc_history_estimate_workspace_t *workspace,
    rtc_history_estimate_t *estimate);

#endif
