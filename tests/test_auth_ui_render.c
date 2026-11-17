/* 原生主机绘制回归：使用合成账号与固定保留量，复现四项列表进入设置时的分配失败。
 * 主机使用 64 KiB 池以适配 64 位指针；本测试不是设备 32 KiB 池或无线组合的替代验收。
 * 必须真正执行逐行绘制并保留断言，不能只做位置计算后宣称绘制稳定。
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../main/auth_ui.c"

static uint8_t pixels[240 * 20 * 2] LV_ATTRIBUTE_MEM_ALIGN;
static auth_totp_account_t fixtures[AUTH_MAX_ACCOUNTS];

static void flush_ready(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

static void print_log(lv_log_level_t level, const char *message)
{
    (void)level;
    fputs(message, stderr);
}

static void draw_frame(lv_display_t *display, const char *stage, unsigned index)
{
    lv_tick_inc(20);
    lv_obj_invalidate(s_root_screen);
    lv_refr_now(display);
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    printf("%s %u: free=%zu largest=%zu max_used=%zu\n", stage, index,
           monitor.free_size, monitor.free_biggest_size, monitor.max_used);
    assert(lv_mem_test() == LV_RESULT_OK);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    size_t reserve = argc > 1 ? strtoul(argv[1], NULL, 10) : 15728;
    lv_init();
    lv_log_register_print_cb(print_log);
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    auth_ui_init();
    for (unsigned i = 0; i < AUTH_MAX_ACCOUNTS; i++) {
        snprintf(fixtures[i].issuer, sizeof(fixtures[i].issuer), "Account provider %02u", i);
        snprintf(fixtures[i].account, sizeof(fixtures[i].account), "user%02u@example.test", i);
        fixtures[i].period = 30;
        fixtures[i].digits = 8;
    }
    draw_frame(display, "warm-lock", 0);
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    assert(reserve < monitor.free_biggest_size);
    void *filler = lv_malloc(reserve);
    assert(filler);
    draw_frame(display, "reserved-lock", 0);
    auth_totp_list_item_t items[AUTH_TOTP_LIST_MAX];
    lv_obj_t *rows[] = {s_lbl_totp_issuer, s_lbl_totp_index, s_lbl_totp_code, s_lbl_totp_seconds};
    for (unsigned repeat = 0; repeat < 3; repeat++) {
        for (unsigned i = 0; i < AUTH_TOTP_LIST_MAX; i++) {
            items[i].account = &fixtures[i];
            items[i].code = "12345678";
            items[i].remaining = 15;
        }
        auth_ui_set_view(AUTH_VIEW_TOTP);
        auth_ui_update_totp_list(items, 0, 4, 16, AUTH_TOTP_VIEW_LIST4);
        auth_ui_update_top_clock(1772983200ULL, 8, true);
        for (unsigned i = 0; i < 4; i++) {
            assert(lv_label_get_text(rows[i])[0] != '>');
            assert(!lv_color_eq(lv_obj_get_style_text_color(rows[i], 0), color_cyan));
            assert(lv_obj_get_style_border_width(rows[i], 0) == 1);
        }
        assert(lv_obj_has_flag(s_bar_totp_countdown, LV_OBJ_FLAG_HIDDEN));
        draw_frame(display, "list4", repeat);
        auth_ui_set_view(AUTH_VIEW_SETTINGS);
        for (unsigned selected = 0; selected < AUTH_SETTINGS_ITEM_COUNT; selected++) {
            auth_ui_update_settings(selected, 80, true, AUTH_TOTP_VIEW_LIST4, 60,
                                    false, 4, false, 1772983200ULL, 8, true, true, false);
            auth_settings_window_t window = auth_ui_settings_window(selected);
            for (unsigned slot = 0; slot < 3; slot++) {
                assert(lv_obj_has_flag(s_lbl_lab_items[slot + 1], LV_OBJ_FLAG_HIDDEN) ==
                       (slot >= window.count));
            }
            assert(lv_label_get_text(s_lbl_lab_items[selected - window.first + 1])[0] == '>');
            draw_frame(display, "settings", selected);
        }
    }
    lv_free(filler);
    lv_deinit();
    puts("Raster scrolling regression: PASS");
    return 0;
}
