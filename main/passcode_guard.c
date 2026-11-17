/* 可在主机测试的 PIN 状态机：只计算输入、计数和锁定时间，不直接擦除存储。
 * 超限仅返回擦除请求，计数保留到应用完成持久化清零；饱和计数避免回绕。
 */
#include "passcode_guard.h"
#include <string.h>
#include <stdio.h>

void passcode_guard_init(passcode_guard_t *guard, uint32_t pattern, uint8_t len)
{
    if (!guard) return;
    memset(guard, 0, sizeof(*guard));

    if (len >= PASSCODE_MIN_LEN && len <= PASSCODE_MAX_LEN) {
        guard->target_pattern = pattern;
        guard->target_len = len;
    } else {
        /* Default: UP UP DOWN DOWN (len = 4, bits: 1, 1, 0, 0 -> 0b0011 = 3 in LSB first) */
        guard->target_pattern = 0x03;
        guard->target_len = 4;
    }

    guard->state = PASSCODE_STATE_LOCKED;
    guard->failed_attempts = 0;
    guard->lockout_end_ms = 0;
}

void passcode_guard_restore_attempts(passcode_guard_t *guard, uint8_t failed_attempts,
                                     uint64_t current_ms, uint32_t lockout_remaining_ms)
{
    if (!guard) return;
    passcode_guard_clear(guard);
    guard->failed_attempts = failed_attempts;
    bool locked_out = failed_attempts >= PASSCODE_MAX_ATTEMPTS && lockout_remaining_ms > 0;
    guard->state = locked_out ? PASSCODE_STATE_LOCKOUT : PASSCODE_STATE_LOCKED;
    guard->lockout_end_ms = locked_out ? current_ms + lockout_remaining_ms : 0;
}

bool passcode_guard_set_target(passcode_guard_t *guard, uint32_t pattern, uint8_t len)
{
    if (!guard || len < PASSCODE_MIN_LEN || len > PASSCODE_MAX_LEN) {
        return false;
    }
    /* Mask out bits beyond len */
    if (len < 32) {
        pattern &= ((1U << len) - 1);
    }
    guard->target_pattern = pattern;
    guard->target_len = len;
    return true;
}

bool passcode_guard_is_locked_out(passcode_guard_t *guard, uint64_t current_ms, uint32_t *remaining_sec)
{
    if (!guard) return false;

    if (guard->state == PASSCODE_STATE_LOCKOUT) {
        if (current_ms < guard->lockout_end_ms) {
            if (remaining_sec) {
                *remaining_sec = (uint32_t)((guard->lockout_end_ms - current_ms + 999) / 1000);
            }
            return true;
        } else {
            /* Lockout expired */
            guard->state = PASSCODE_STATE_LOCKED;
            guard->lockout_end_ms = 0;
            if (remaining_sec) *remaining_sec = 0;
            return false;
        }
    }

    if (remaining_sec) *remaining_sec = 0;
    return false;
}

passcode_result_t passcode_guard_input_bit(passcode_guard_t *guard, bool bit, uint64_t current_ms)
{
    if (!guard) return PASSCODE_RESULT_NONE;

    if (passcode_guard_is_locked_out(guard, current_ms, NULL)) {
        return PASSCODE_RESULT_LOCKED_OUT;
    }

    if (guard->current_len >= PASSCODE_MAX_LEN) {
        return PASSCODE_RESULT_INPUT_UPDATED; /* Max reached */
    }

    if (bit) {
        guard->current_pattern |= (1U << guard->current_len);
    } else {
        guard->current_pattern &= ~(1U << guard->current_len);
    }
    guard->current_len++;

    return PASSCODE_RESULT_INPUT_UPDATED;
}

passcode_result_t passcode_guard_backspace(passcode_guard_t *guard)
{
    if (!guard) return PASSCODE_RESULT_NONE;
    if (guard->state == PASSCODE_STATE_LOCKOUT) return PASSCODE_RESULT_LOCKED_OUT;
    if (guard->current_len == 0) return PASSCODE_RESULT_NONE;

    guard->current_len--;
    guard->current_pattern &= ~(1U << guard->current_len);
    return PASSCODE_RESULT_INPUT_UPDATED;
}

passcode_result_t passcode_guard_clear(passcode_guard_t *guard)
{
    if (!guard) return PASSCODE_RESULT_NONE;
    guard->current_pattern = 0;
    guard->current_len = 0;
    return PASSCODE_RESULT_CLEARED;
}

passcode_result_t passcode_guard_submit(passcode_guard_t *guard, uint64_t current_ms)
{
    if (!guard) return PASSCODE_RESULT_NONE;

    if (passcode_guard_is_locked_out(guard, current_ms, NULL)) {
        return PASSCODE_RESULT_LOCKED_OUT;
    }

    if (guard->state == PASSCODE_STATE_LOCKED) {
        if (guard->current_len == 0) {
            return PASSCODE_RESULT_NONE; /* Don't penalize empty submission */
        }
        if (guard->current_len == guard->target_len &&
            guard->current_pattern == guard->target_pattern) {
            guard->state = PASSCODE_STATE_UNLOCKED;
            guard->failed_attempts = 0;
            passcode_guard_clear(guard);
            return PASSCODE_RESULT_SUCCESS;
        } else {
            if (guard->failed_attempts < UINT8_MAX) guard->failed_attempts++;
            passcode_guard_clear(guard);
            if (guard->failed_attempts > PASSCODE_WIPE_ATTEMPTS) {
                guard->state = PASSCODE_STATE_LOCKOUT;
                guard->lockout_end_ms = current_ms + PASSCODE_LOCKOUT_MS;
                return PASSCODE_RESULT_WIPED;
            }
            if (guard->failed_attempts >= PASSCODE_MAX_ATTEMPTS) {
                guard->state = PASSCODE_STATE_LOCKOUT;
                guard->lockout_end_ms = current_ms + PASSCODE_LOCKOUT_MS;
                return PASSCODE_RESULT_LOCKED_OUT;
            }
            return PASSCODE_RESULT_FAILED;
        }
    } else if (guard->state == PASSCODE_STATE_SETTING_NEW) {
        if (guard->current_len < PASSCODE_MIN_LEN) {
            return PASSCODE_RESULT_FAILED;
        }
        guard->new_pattern_temp = guard->current_pattern;
        guard->new_len_temp = guard->current_len;
        guard->state = PASSCODE_STATE_SETTING_CONFIRM;
        passcode_guard_clear(guard);
        return PASSCODE_RESULT_INPUT_UPDATED;
    } else if (guard->state == PASSCODE_STATE_SETTING_CONFIRM) {
        if (guard->current_len == guard->new_len_temp &&
            guard->current_pattern == guard->new_pattern_temp) {
            passcode_guard_set_target(guard, guard->new_pattern_temp, guard->new_len_temp);
            guard->state = PASSCODE_STATE_UNLOCKED;
            passcode_guard_clear(guard);
            return PASSCODE_RESULT_NEW_SET;
        } else {
            /* Mismatch, reset state */
            guard->state = PASSCODE_STATE_UNLOCKED;
            passcode_guard_clear(guard);
            return PASSCODE_RESULT_MISMATCH;
        }
    }

    return PASSCODE_RESULT_NONE;
}

void passcode_guard_lock(passcode_guard_t *guard)
{
    if (!guard) return;
    guard->state = PASSCODE_STATE_LOCKED;
    passcode_guard_clear(guard);
}

void passcode_guard_start_change(passcode_guard_t *guard)
{
    if (!guard) return;
    guard->state = PASSCODE_STATE_SETTING_NEW;
    passcode_guard_clear(guard);
}

void passcode_guard_get_display_str(const passcode_guard_t *guard, char *buf, size_t max_len, bool masked)
{
    if (!guard || !buf || max_len == 0) return;
    buf[0] = '\0';

    size_t offset = 0;
    for (uint8_t i = 0; i < guard->current_len; i++) {
        const char *glyph;
        if (masked) {
            glyph = "* ";
        } else {
            bool bit = (guard->current_pattern & (1U << i)) != 0;
            glyph = bit ? "^ " : "v ";
        }
        size_t glen = strlen(glyph);
        if (offset + glen + 1 < max_len) {
            memcpy(&buf[offset], glyph, glen);
            offset += glen;
            buf[offset] = '\0';
        } else {
            break;
        }
    }
}
