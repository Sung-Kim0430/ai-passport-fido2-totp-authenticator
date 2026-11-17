#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "passcode_guard.h"

static void test_passcode_basic(void)
{
    passcode_guard_t guard;
    /* Default: UP UP DOWN DOWN (len=4, 0b0011 = 3 in LSB first: bit0=1, bit1=1, bit2=0, bit3=0) */
    passcode_guard_init(&guard, 0x03, 4);

    assert(guard.state == PASSCODE_STATE_LOCKED);
    assert(guard.target_len == 4);
    assert(guard.target_pattern == 0x03);

    /* Empty submit should be PASSCODE_RESULT_NONE and not increment failed_attempts */
    assert(passcode_guard_submit(&guard, 900) == PASSCODE_RESULT_NONE);
    assert(guard.failed_attempts == 0);

    /* Input: UP UP DOWN DOWN */
    assert(passcode_guard_input_bit(&guard, true, 1000) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(passcode_guard_input_bit(&guard, true, 1050) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(passcode_guard_input_bit(&guard, false, 1100) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(passcode_guard_input_bit(&guard, false, 1150) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(guard.current_len == 4);

    char str[32];
    passcode_guard_get_display_str(&guard, str, sizeof(str), false);
    assert(strcmp(str, "^ ^ v v ") == 0);

    /* Submit correct passcode */
    passcode_result_t res = passcode_guard_submit(&guard, 1200);
    assert(res == PASSCODE_RESULT_SUCCESS);
    assert(guard.state == PASSCODE_STATE_UNLOCKED);
    assert(guard.failed_attempts == 0);
    assert(guard.current_len == 0);

    printf("Passcode basic unlock: PASS\n");
}

static void test_passcode_backspace_clear(void)
{
    passcode_guard_t guard;
    passcode_guard_init(&guard, 0x03, 4);

    passcode_guard_input_bit(&guard, true, 1000);
    passcode_guard_input_bit(&guard, false, 1050);
    assert(guard.current_len == 2);

    /* Backspace */
    assert(passcode_guard_backspace(&guard) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(guard.current_len == 1);
    assert(guard.current_pattern == 0x01);

    /* Clear */
    assert(passcode_guard_clear(&guard) == PASSCODE_RESULT_CLEARED);
    assert(guard.current_len == 0);
    assert(guard.current_pattern == 0);

    printf("Passcode backspace and clear: PASS\n");
}

static void test_passcode_lockout(void)
{
    passcode_guard_t guard;
    passcode_guard_init(&guard, 0x03, 4);

    uint64_t now = 10000;
    for (int i = 0; i < 4; i++) {
        passcode_guard_input_bit(&guard, false, now); /* wrong */
        assert(passcode_guard_submit(&guard, now) == PASSCODE_RESULT_FAILED);
        assert(guard.state == PASSCODE_STATE_LOCKED);
        assert(guard.failed_attempts == i + 1);
    }

    /* 5th wrong attempt */
    passcode_guard_input_bit(&guard, false, now);
    assert(passcode_guard_submit(&guard, now) == PASSCODE_RESULT_LOCKED_OUT);
    assert(guard.state == PASSCODE_STATE_LOCKOUT);
    assert(guard.failed_attempts == 5);

    /* While in lockout, inputs are rejected */
    uint32_t rem_sec = 0;
    assert(passcode_guard_is_locked_out(&guard, now + 5000, &rem_sec));
    assert(rem_sec == 25);
    assert(passcode_guard_input_bit(&guard, true, now + 5000) == PASSCODE_RESULT_LOCKED_OUT);
    assert(passcode_guard_submit(&guard, now + 5000) == PASSCODE_RESULT_LOCKED_OUT);

    /* After 30s lockout expires */
    assert(!passcode_guard_is_locked_out(&guard, now + 30001, &rem_sec));
    assert(rem_sec == 0);
    assert(guard.state == PASSCODE_STATE_LOCKED);
    assert(guard.failed_attempts == 5);

    /* Attempt 6: Locked out again */
    passcode_guard_input_bit(&guard, false, now + 30002);
    assert(passcode_guard_submit(&guard, now + 30002) == PASSCODE_RESULT_LOCKED_OUT);
    assert(guard.failed_attempts == 6);
    assert(!passcode_guard_is_locked_out(&guard, now + 60003, &rem_sec));

    /* Attempt 7: Locked out again */
    passcode_guard_input_bit(&guard, false, now + 60004);
    assert(passcode_guard_submit(&guard, now + 60004) == PASSCODE_RESULT_LOCKED_OUT);
    assert(guard.failed_attempts == 7);
    assert(!passcode_guard_is_locked_out(&guard, now + 90005, &rem_sec));

    /* Attempt 8: 8th failed attempt */
    passcode_guard_input_bit(&guard, false, now + 90006);
    assert(passcode_guard_submit(&guard, now + 90006) == PASSCODE_RESULT_LOCKED_OUT);
    assert(guard.failed_attempts == 8);
    assert(!passcode_guard_is_locked_out(&guard, now + 120007, &rem_sec));

    /* Attempt 9: Exceeded 8 attempts (>8) -> Wiped! */
    passcode_guard_input_bit(&guard, false, now + 120008);
    assert(passcode_guard_submit(&guard, now + 120008) == PASSCODE_RESULT_WIPED);
    assert(guard.failed_attempts == 9);
    assert(guard.state == PASSCODE_STATE_LOCKOUT);
    assert(!passcode_guard_is_locked_out(&guard, now + 150009, &rem_sec));
    passcode_guard_input_bit(&guard, false, now + 150010);
    assert(passcode_guard_submit(&guard, now + 150010) == PASSCODE_RESULT_WIPED);
    assert(guard.failed_attempts == 10);
    guard.failed_attempts = UINT8_MAX;
    assert(!passcode_guard_is_locked_out(&guard, now + 180011, &rem_sec));
    passcode_guard_input_bit(&guard, false, now + 180012);
    assert(passcode_guard_submit(&guard, now + 180012) == PASSCODE_RESULT_WIPED);
    assert(guard.failed_attempts == UINT8_MAX);

    printf("Passcode lockout and retained wipe-threshold attempts: PASS\n");
}

static void test_passcode_32bit_and_change(void)
{
    passcode_guard_t guard;
    passcode_guard_init(&guard, 0x03, 4);
    guard.state = PASSCODE_STATE_UNLOCKED;

    /* Start change */
    passcode_guard_start_change(&guard);
    assert(guard.state == PASSCODE_STATE_SETTING_NEW);

    /* Input 32-bit pattern: alternate 1 and 0 (0xAAAAAAAA) */
    uint32_t target_32 = 0xAAAAAAAAU;
    for (int i = 0; i < 32; i++) {
        bool bit = (target_32 & (1U << i)) != 0;
        passcode_guard_input_bit(&guard, bit, 1000);
    }
    assert(guard.current_len == 32);

    /* Cannot add 33rd bit */
    passcode_guard_input_bit(&guard, true, 1000);
    assert(guard.current_len == 32);

    /* Submit step 1 */
    assert(passcode_guard_submit(&guard, 1000) == PASSCODE_RESULT_INPUT_UPDATED);
    assert(guard.state == PASSCODE_STATE_SETTING_CONFIRM);

    /* Re-enter same 32 bits to confirm */
    for (int i = 0; i < 32; i++) {
        bool bit = (target_32 & (1U << i)) != 0;
        passcode_guard_input_bit(&guard, bit, 1000);
    }
    assert(passcode_guard_submit(&guard, 1000) == PASSCODE_RESULT_NEW_SET);
    assert(guard.state == PASSCODE_STATE_UNLOCKED);
    assert(guard.target_len == 32);
    assert(guard.target_pattern == target_32);

    /* Lock and verify new 32-bit passcode */
    passcode_guard_lock(&guard);
    assert(guard.state == PASSCODE_STATE_LOCKED);

    for (int i = 0; i < 32; i++) {
        bool bit = (target_32 & (1U << i)) != 0;
        passcode_guard_input_bit(&guard, bit, 1000);
    }
    assert(passcode_guard_submit(&guard, 1000) == PASSCODE_RESULT_SUCCESS);
    assert(guard.state == PASSCODE_STATE_UNLOCKED);

    printf("Passcode 32-bit sequence and change: PASS\n");
}

static void test_restore_attempts(void)
{
    passcode_guard_t guard;
    passcode_guard_init(&guard, 3, 4);
    passcode_guard_restore_attempts(&guard, 4, 1000, 0);
    assert(guard.failed_attempts == 4 && guard.state == PASSCODE_STATE_LOCKED);
    passcode_guard_input_bit(&guard, false, 1000);
    assert(passcode_guard_submit(&guard, 1000) == PASSCODE_RESULT_LOCKED_OUT);
    assert(guard.failed_attempts == 5);
    passcode_guard_init(&guard, 3, 4);
    passcode_guard_restore_attempts(&guard, 5, 500, 30000);
    assert(guard.failed_attempts == 5 && guard.state == PASSCODE_STATE_LOCKOUT);
    assert(passcode_guard_is_locked_out(&guard, 30499, NULL));
    assert(!passcode_guard_is_locked_out(&guard, 30500, NULL));
    passcode_guard_restore_attempts(&guard, 8, 40000, 0);
    passcode_guard_input_bit(&guard, false, 40000);
    assert(passcode_guard_submit(&guard, 40000) == PASSCODE_RESULT_WIPED);
    assert(guard.failed_attempts == 9);
    passcode_guard_init(&guard, 3, 4);
    passcode_guard_restore_attempts(&guard, 9, 100, 1000);
    assert(guard.failed_attempts == 9 && guard.target_pattern == 3);
    assert(!passcode_guard_is_locked_out(&guard, 1100, NULL));
    passcode_guard_input_bit(&guard, true, 1100);
    passcode_guard_input_bit(&guard, true, 1100);
    passcode_guard_input_bit(&guard, false, 1100);
    passcode_guard_input_bit(&guard, false, 1100);
    assert(passcode_guard_submit(&guard, 1100) == PASSCODE_RESULT_SUCCESS);
    assert(guard.failed_attempts == 0);
}

int main(void)
{
    test_passcode_basic();
    test_passcode_backspace_clear();
    test_passcode_lockout();
    test_passcode_32bit_and_change();
    test_restore_attempts();
    printf("All passcode guard host tests: PASS\n");
    return 0;
}
