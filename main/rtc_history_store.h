#ifndef RTC_HISTORY_STORE_H
#define RTC_HISTORY_STORE_H

#include "rtc_history.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_HISTORY_STORE_MISSING,
    RTC_HISTORY_STORE_VALID,
    RTC_HISTORY_STORE_BLOCKED,
    RTC_HISTORY_STORE_IO_ERROR
} rtc_history_store_result_t;

typedef struct {
    void *user;
    rtc_history_store_result_t (*read)(void *, const char *, void *, size_t *);
    bool (*write)(void *, const char *, const void *, size_t);
    bool (*commit)(void *);
} rtc_history_store_io_t;

typedef struct {
    uint64_t accepted_sequence;
    uint64_t dropped;
} rtc_history_store_checkpoint_t;

typedef struct {
    rtc_history_store_checkpoint_t checkpoint;
    rtc_history_store_io_t io;
    rtc_history_t *history;
    rtc_history_store_result_t status;
    uint32_t source;
    uint8_t map[10];
    uint8_t manifest;
    uint8_t scratch[832];
} rtc_history_store_t;

rtc_history_store_result_t rtc_history_store_open(rtc_history_store_t *store,
    rtc_history_t *history, rtc_history_store_io_t io, uint32_t source);
rtc_history_store_result_t rtc_history_store_append(rtc_history_store_t *store,
    const rtc_history_record_t *record, const rtc_history_store_checkpoint_t *checkpoint);
rtc_history_store_result_t rtc_history_store_platform_io(rtc_history_store_io_t *io);

#ifdef __cplusplus
}
#endif
#endif
