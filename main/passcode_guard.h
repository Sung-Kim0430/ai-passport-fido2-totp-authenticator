/* PIN 纯状态机：方向序列按低位优先编码，时间由调用方提供，不直接访问 NVS 或 UI。
 * failed_attempts 的持久化由应用负责；WIPE 结果是请求，不代表已经擦除凭据。
 * 重启恢复使用累计次数和剩余锁定时长，不能把 UTC 校时当作锁定计时依据。
 */
#ifndef PASSCODE_GUARD_H
#define PASSCODE_GUARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PASSCODE_MIN_LEN       4
#define PASSCODE_MAX_LEN       32
#define PASSCODE_MAX_ATTEMPTS  5
#define PASSCODE_WIPE_ATTEMPTS 8
#define PASSCODE_LOCKOUT_MS    30000

typedef enum {
    PASSCODE_STATE_LOCKED = 0,
    PASSCODE_STATE_UNLOCKED,
    PASSCODE_STATE_LOCKOUT,
    PASSCODE_STATE_SETTING_NEW,
    PASSCODE_STATE_SETTING_CONFIRM
} passcode_state_t;

typedef enum {
    PASSCODE_RESULT_NONE = 0,
    PASSCODE_RESULT_INPUT_UPDATED,
    PASSCODE_RESULT_CLEARED,
    PASSCODE_RESULT_SUCCESS,
    PASSCODE_RESULT_FAILED,
    PASSCODE_RESULT_LOCKED_OUT,
    PASSCODE_RESULT_WIPED,
    PASSCODE_RESULT_NEW_SET,
    PASSCODE_RESULT_MISMATCH
} passcode_result_t;

typedef struct {
    uint32_t target_pattern;    /* 目标方向序列，第一个输入保存在 bit 0 */
    uint8_t  target_len;        /* 4 to 32 */

    uint32_t current_pattern;   /* Currently entered bit pattern */
    uint8_t  current_len;       /* Number of bits entered */

    uint32_t new_pattern_temp;  /* Temporary storage when setting new passcode */
    uint8_t  new_len_temp;

    passcode_state_t state;
    uint8_t  failed_attempts;
    uint64_t lockout_end_ms;
} passcode_guard_t;

/**
 * @brief 使用已保存的方向序列初始化，长度无效时退回 UP、UP、DOWN、DOWN。
 * 默认按低位优先编码为 0x03；随后仍需恢复 NVS 错误计数和剩余锁定时长。
 */
void passcode_guard_init(passcode_guard_t *guard, uint32_t pattern, uint8_t len);
void passcode_guard_restore_attempts(passcode_guard_t *guard, uint8_t failed_attempts,
                                     uint64_t current_ms, uint32_t lockout_remaining_ms);

/**
 * @brief Set the target passcode (4 to 32 bits).
 */
bool passcode_guard_set_target(passcode_guard_t *guard, uint32_t pattern, uint8_t len);

/**
 * @brief Add a bit: 1 for UP, 0 for DOWN.
 */
passcode_result_t passcode_guard_input_bit(passcode_guard_t *guard, bool bit, uint64_t current_ms);

/**
 * @brief Backspace last entered bit.
 */
passcode_result_t passcode_guard_backspace(passcode_guard_t *guard);

/**
 * @brief Clear all entered bits.
 */
passcode_result_t passcode_guard_clear(passcode_guard_t *guard);

/**
 * @brief Submit currently entered bits for verification.
 */
passcode_result_t passcode_guard_submit(passcode_guard_t *guard, uint64_t current_ms);

/**
 * @brief Manually lock the device.
 */
void passcode_guard_lock(passcode_guard_t *guard);

/**
 * @brief Begin changing passcode workflow.
 */
void passcode_guard_start_change(passcode_guard_t *guard);

/**
 * @brief Check lockout status and update state if lockout expired.
 */
bool passcode_guard_is_locked_out(passcode_guard_t *guard, uint64_t current_ms, uint32_t *remaining_sec);

/**
 * @brief Format input sequence into display string (e.g. "▲ ▲ ▼ ▲" or "• • • •").
 * @param masked If true, shows "• " for entered bits; if false, shows "▲ " or "▼ ".
 */
void passcode_guard_get_display_str(const passcode_guard_t *guard, char *buf, size_t max_len, bool masked);

#ifdef __cplusplus
}
#endif

#endif /* PASSCODE_GUARD_H */
