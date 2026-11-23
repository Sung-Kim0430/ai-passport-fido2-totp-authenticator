#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auth_ui.h"

static void test_settings_window(void)
{
    const uint8_t starts[] = {0, 3, 6, 9, 10, 13, 15, 16};
    assert(AUTH_SETTINGS_ITEM_COUNT == 16);
    for (uint8_t selected = 0; selected < AUTH_SETTINGS_ITEM_COUNT; selected++) {
        auth_settings_window_t window = auth_ui_settings_window(selected);
        assert(window.group < 7);
        assert(window.first == starts[window.group]);
        assert(window.count == starts[window.group + 1] - starts[window.group]);
        assert(window.count <= 3);
        assert(selected >= window.first && selected < window.first + window.count);
    }
    auth_settings_window_t invalid = auth_ui_settings_window(UINT8_MAX);
    assert(invalid.first == 15 && invalid.count == 1 && invalid.group == 6);
}

static void test_clock_modes_navigation(void)
{
    assert(AUTH_SETTINGS_CLOCK_MODES_INDEX == 14 && AUTH_SETTINGS_EXIT_INDEX == 15);
    assert(AUTH_CLOCK_MODES_BACK_INDEX == 3 && AUTH_CLOCK_MODES_ITEM_COUNT == 4);
    assert(AUTH_CLOCK_MODES_PAGE_LIST == 0 && AUTH_CLOCK_MODES_PAGE_BEHAVIOR == 1);
    assert(AUTH_CLOCK_MODES_PAGE_STANDBY == 2 && AUTH_CLOCK_MODES_PAGE_ACCURACY == 3);
    assert(AUTH_CLOCK_MODES_PAGE_APPLY == 4);
    for (uint8_t selected = 0; selected < AUTH_CLOCK_MODES_ITEM_COUNT; selected++) {
        uint8_t next = auth_ui_clock_modes_next_selection(selected, true);
        assert(next < AUTH_CLOCK_MODES_ITEM_COUNT);
        assert(auth_ui_clock_modes_next_selection(next, false) == selected);
    }
    assert(auth_ui_clock_modes_next_selection(AUTH_CLOCK_MODES_BACK_INDEX, true) == 0);
    assert(auth_ui_clock_modes_next_selection(0, false) == AUTH_CLOCK_MODES_BACK_INDEX);
    assert(auth_ui_clock_modes_next_selection(UINT8_MAX, true) == 0);
    assert(auth_ui_clock_modes_next_selection(UINT8_MAX, false) == 0);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_LIST, false) == 0);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_LIST, true) == 0);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_BEHAVIOR, false) == 4);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_BEHAVIOR, true) == 2);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_STANDBY, false) == 1);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_STANDBY, true) == 3);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_APPLY, false) == 3);
    assert(auth_ui_clock_modes_next_page(AUTH_CLOCK_MODES_PAGE_APPLY, true) == 1);
    assert(auth_ui_clock_modes_next_page(UINT8_MAX, false) == 4);
    assert(auth_ui_clock_modes_next_page(UINT8_MAX, true) == 4);
    for (uint8_t page = AUTH_CLOCK_MODES_PAGE_BEHAVIOR; page <= AUTH_CLOCK_MODES_PAGE_APPLY; page++) {
        uint8_t next = auth_ui_clock_modes_next_page(page, true);
        assert(next >= AUTH_CLOCK_MODES_PAGE_BEHAVIOR && next <= AUTH_CLOCK_MODES_PAGE_APPLY);
        assert(auth_ui_clock_modes_next_page(next, false) == page);
    }
}

int main(void)
{
    test_settings_window();
    test_clock_modes_navigation();
    char text[16];
    auth_ui_format_battery(text, sizeof(text), 100, false, false);
    assert(strcmp(text, "100%") == 0);
    auth_ui_format_battery(text, sizeof(text), 80, true, false);
    assert(strcmp(text, "80%+") == 0);
    auth_ui_format_battery(text, sizeof(text), 79, false, true);
    assert(strcmp(text, "79%") == 0);
    auth_ui_format_battery(text, sizeof(text), 100, true, true);
    assert(strcmp(text, "100%*+") == 0);
    auth_ui_format_battery(text, sizeof(text), -1, false, true);
    assert(strcmp(text, "--") == 0);
    auth_ui_format_battery(text, sizeof(text), 79, true, true);
    assert(strcmp(text, "79%*+") == 0);
    for (int i = 0; i < 100000; i++) {
        auth_ui_format_battery(text, sizeof(text), i % 101, i % 2, i % 3 == 0);
        assert((strchr(text, '*') != NULL) == (i % 2 != 0 && i % 3 == 0));
        assert((strchr(text, '+') != NULL) == (i % 2 != 0));
    }
    assert(auth_ui_battery_color(100, false, true) == 0xA2B0C0);
    assert(auth_ui_battery_color(10, false, false) == 0xFF9B9B);
    assert(auth_ui_battery_color(0, true, true) == 0xFF9B9B);
    assert(auth_ui_battery_color(11, true, false) == 0xEDC776);
    assert(auth_ui_battery_color(30, true, true) == 0xEDC776);
    assert(auth_ui_battery_color(31, false, true) == 0xA2B0C0);
    assert(auth_ui_battery_color(31, true, true) == 0x80D6BD);
    assert(auth_ui_battery_color(50, true, false) == 0x80D6BD);
    assert(auth_ui_battery_color(79, true, true) == 0x80D6BD);
    assert(auth_ui_battery_color(80, true, true) == 0x6AAEFF);
    assert(auth_ui_battery_color(-1, true, false) == 0xFF9B9B);
    for (int soc = -1; soc <= 100; soc++) for (unsigned flags = 0; flags < 4; flags++) {
        uint32_t expected = soc <= 10 ? 0xFF9B9B : soc <= 30 ? 0xEDC776 :
                            !(flags & 1) ? 0xA2B0C0 :
                            (flags & 2) && soc >= 80 ? 0x6AAEFF : 0x80D6BD;
        assert(auth_ui_battery_color(soc, flags & 1, flags & 2) == expected);
    }
    auth_ui_format_top_clock(text, sizeof(text), 0, 8, true);
    assert(strcmp(text, "--:--") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704067200ULL, 0, true);
    assert(strcmp(text, "00:00") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704067200ULL, 0, false);
    assert(strcmp(text, "12:00 AM") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704110400ULL, 0, false);
    assert(strcmp(text, "12:00 PM") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704114300ULL, 0, false);
    assert(strcmp(text, "01:05 PM") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704067200ULL, 8, true);
    assert(strcmp(text, "08:00") == 0);
    auth_ui_format_top_clock(text, sizeof(text), 1704067200ULL, -12, false);
    assert(strcmp(text, "12:00 PM") == 0);
    assert(auth_ui_next_totp_view(AUTH_TOTP_VIEW_CARD) == AUTH_TOTP_VIEW_LIST3);
    assert(auth_ui_next_totp_view(AUTH_TOTP_VIEW_LIST3) == AUTH_TOTP_VIEW_LIST4);
    assert(auth_ui_next_totp_view(AUTH_TOTP_VIEW_LIST4) == AUTH_TOTP_VIEW_CARD);
    const auth_totp_view_t modes[] = {AUTH_TOTP_VIEW_CARD, AUTH_TOTP_VIEW_LIST3, AUTH_TOTP_VIEW_LIST4};
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        uint8_t capacity = modes[m] == AUTH_TOTP_VIEW_CARD ? 1 : (uint8_t)modes[m];
        assert(auth_ui_totp_page_count(0, 0, modes[m]) == 0);
        for (uint8_t total = 1; total <= AUTH_MAX_ACCOUNTS; total++) {
            for (uint8_t selected = 0; selected < total; selected++) {
                uint8_t start = auth_ui_totp_page_start(selected, modes[m]);
                uint8_t count = auth_ui_totp_page_count(total, selected, modes[m]);
                assert(start <= selected && selected < start + count);
                assert(count > 0 && count <= capacity);
                assert(start + count <= total);
                assert(start == selected / capacity * capacity);
            }
            assert(auth_ui_totp_page_count(total, total, modes[m]) == 0);
        }
    }
    assert(auth_ui_totp_navigate(16, 5, AUTH_TOTP_VIEW_CARD, true) == 6);
    assert(auth_ui_totp_navigate(16, 5, AUTH_TOTP_VIEW_CARD, false) == 4);
    assert(auth_ui_totp_navigate(16, 5, AUTH_TOTP_VIEW_LIST3, true) == 6);
    assert(auth_ui_totp_navigate(16, 5, AUTH_TOTP_VIEW_LIST3, false) == 0);
    assert(auth_ui_totp_navigate(16, 12, AUTH_TOTP_VIEW_LIST3, true) == 15);
    assert(auth_ui_totp_navigate(16, 15, AUTH_TOTP_VIEW_LIST3, true) == 0);
    assert(auth_ui_totp_navigate(16, 0, AUTH_TOTP_VIEW_LIST3, false) == 15);
    assert(auth_ui_totp_navigate(13, 8, AUTH_TOTP_VIEW_LIST4, true) == 12);
    assert(auth_ui_totp_navigate(13, 12, AUTH_TOTP_VIEW_LIST4, true) == 0);
    assert(auth_ui_totp_navigate(13, 0, AUTH_TOTP_VIEW_LIST4, false) == 12);
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        assert(auth_ui_totp_navigate(0, 0, modes[m], true) == 0);
        assert(auth_ui_totp_navigate(0, 0, modes[m], false) == 0);
        for (uint8_t total = 1; total <= AUTH_MAX_ACCOUNTS; total++) {
            unsigned covered = 0;
            uint8_t page = 0;
            unsigned steps = 0;
            do {
                assert(steps++ < AUTH_MAX_ACCOUNTS);
                covered += auth_ui_totp_page_count(total, page, modes[m]);
                uint8_t next = auth_ui_totp_navigate(total, page, modes[m], true);
                assert(auth_ui_totp_navigate(total, next, modes[m], false) == page);
                page = next;
            } while (page);
            assert(covered == total);
        }
    }
    puts("Battery, clock modes, settings groups, card stepping and direct list paging: PASS");
    return 0;
}
