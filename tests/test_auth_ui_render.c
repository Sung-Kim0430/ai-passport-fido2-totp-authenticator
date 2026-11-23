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
static uint8_t frame_rgb[240 * 320 * 3];
static const char *render_directory;
static auth_totp_account_t fixtures[AUTH_MAX_ACCOUNTS];

static void flush_ready(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    if (render_directory) {
        unsigned stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565);
        for (int32_t y = area->y1; y <= area->y2; y++) {
            for (int32_t x = area->x1; x <= area->x2; x++) {
                uint16_t pixel;
                memcpy(&pixel, data + (y - area->y1) * stride + (x - area->x1) * 2, sizeof(pixel));
                size_t offset = ((size_t)y * 240 + x) * 3;
                frame_rgb[offset] = ((pixel >> 11) & 31) * 255 / 31;
                frame_rgb[offset + 1] = ((pixel >> 5) & 63) * 255 / 63;
                frame_rgb[offset + 2] = (pixel & 31) * 255 / 31;
            }
        }
    }
    lv_display_flush_ready(display);
}

static void save_clock_frame(unsigned active, unsigned selected, unsigned page, unsigned state)
{
    if (!render_directory) return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/clock-a%u-s%u-p%u-state%u.ppm",
                          render_directory, active, selected, page, state);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *out = fopen(path, "wb");
    assert(out);
    assert(fprintf(out, "P6\n240 320\n255\n") > 0);
    assert(fwrite(frame_rgb, sizeof(frame_rgb), 1, out) == 1);
    assert(fclose(out) == 0);
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

static void assert_label_fits(lv_obj_t *label, const lv_area_t *bounds)
{
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    if (area.x1 < bounds->x1 || area.x2 > bounds->x2 ||
        area.y1 < bounds->y1 || area.y2 > bounds->y2) {
        fprintf(stderr, "Clipped label '%s': (%ld,%ld)-(%ld,%ld), bounds (%ld,%ld)-(%ld,%ld)\n",
                lv_label_get_text(label), (long)area.x1, (long)area.y1, (long)area.x2, (long)area.y2,
                (long)bounds->x1, (long)bounds->y1, (long)bounds->x2, (long)bounds->y2);
    }
    assert(area.x1 >= bounds->x1 && area.x2 <= bounds->x2);
    assert(area.y1 >= bounds->y1 && area.y2 <= bounds->y2);
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(label),
                    lv_obj_get_style_text_font(label, 0),
                    lv_obj_get_style_text_letter_space(label, 0),
                    lv_obj_get_style_text_line_space(label, 0),
                    lv_obj_get_content_width(label), LV_TEXT_FLAG_NONE);
    assert(text_size.x <= lv_obj_get_content_width(label));
    assert(text_size.y <= lv_obj_get_content_height(label));
}

static void check_clock_layout(void)
{
    lv_area_t bounds;
    lv_obj_get_content_coords(s_pnl_clock_modes, &bounds);
    assert(bounds.y1 >= 36 && bounds.y2 < 288);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_pnl_clock_modes); i++) {
        lv_obj_t *child = lv_obj_get_child(s_pnl_clock_modes, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        assert_label_fits(child, &bounds);
        if (!lv_label_get_text(child)[0]) continue;
        lv_area_t child_area;
        lv_obj_get_coords(child, &child_area);
        for (uint32_t j = i + 1; j < lv_obj_get_child_count(s_pnl_clock_modes); j++) {
            lv_obj_t *other = lv_obj_get_child(s_pnl_clock_modes, j);
            if (lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(other)[0]) continue;
            lv_area_t other_area;
            lv_obj_get_coords(other, &other_area);
            assert(child_area.y2 < other_area.y1 || other_area.y2 < child_area.y1 ||
                   child_area.x2 < other_area.x1 || other_area.x2 < child_area.x1);
        }
    }
    lv_obj_get_content_coords(s_bottom_bar, &bounds);
    assert_label_fits(s_lbl_footer_hint, &bounds);
    assert(lv_obj_has_flag(s_pnl_lab, LV_OBJ_FLAG_HIDDEN));
}

static void test_clock_modes(lv_display_t *display)
{
    assert(!s_pnl_clock_modes);
    auth_ui_update_clock_modes(RTC_MODE_ECO, 0, 0, false, false);
    assert(!s_pnl_clock_modes); /* Background refresh cannot allocate an inactive view. */
    uint32_t root_children = lv_obj_get_child_count(s_root_screen);
    for (rtc_mode_t active = RTC_MODE_ECO; active < RTC_MODE_COUNT; active++) {
        auth_ui_set_view(AUTH_VIEW_CLOCK_MODES);
        assert(s_pnl_clock_modes);
        for (uint8_t selected = 0; selected < AUTH_CLOCK_MODES_ITEM_COUNT; selected++) {
            auth_ui_update_clock_modes(active, selected, AUTH_CLOCK_MODES_PAGE_LIST, false, false);
            draw_frame(display, "clock-list", selected);
            save_clock_frame(active, selected, 0, 0);
            check_clock_layout();
            for (uint8_t row = 0; row < AUTH_CLOCK_MODES_ITEM_COUNT; row++) {
                assert(!lv_obj_has_flag(s_lbl_clock_items[row], LV_OBJ_FLAG_HIDDEN));
                assert((lv_label_get_text(s_lbl_clock_items[row])[0] == '>') == (row == selected));
            }
            if (selected == AUTH_CLOCK_MODES_BACK_INDEX) continue;
            for (uint8_t page = AUTH_CLOCK_MODES_PAGE_BEHAVIOR;
                 page <= AUTH_CLOCK_MODES_PAGE_APPLY; page++) {
                for (unsigned state = 0; state < (page == AUTH_CLOCK_MODES_PAGE_APPLY ? 4U : 1U); state++) {
                    auth_ui_update_clock_modes(active, selected, page, state & 1, state & 2);
                    draw_frame(display, "clock-detail", page);
                    save_clock_frame(active, selected, page, state);
                    check_clock_layout();
                    for (unsigned row = 0; row < AUTH_CLOCK_MODES_ITEM_COUNT; row++)
                        assert(lv_obj_has_flag(s_lbl_clock_items[row], LV_OBJ_FLAG_HIDDEN));
                    const char *body = lv_label_get_text(s_lbl_clock_body);
                    if (page == AUTH_CLOCK_MODES_PAGE_APPLY) {
                        const char *action = lv_label_get_text(s_lbl_clock_action);
                        if (selected == (uint8_t)active) {
                            assert(strcmp(action, "Active") == 0);
                            assert(body[0] == '\0');
                            assert(strstr(lv_label_get_text(s_lbl_footer_hint), "OK Back"));
                        } else {
                            assert(strcmp(body, s_clock_copy.switch_warning) == 0);
                            assert((strstr(action, s_clock_copy.save_failed) != NULL) == ((state & 2) != 0));
                            assert(strstr(action, state & 1 ? s_clock_copy.confirm : s_clock_copy.apply));
                        }
                    } else {
                        const char *expected = page == AUTH_CLOCK_MODES_PAGE_BEHAVIOR ? s_clock_intro[selected] :
                                               page == AUTH_CLOCK_MODES_PAGE_STANDBY ? s_clock_estimates[selected] :
                                               s_clock_notes[selected];
                        assert(strcmp(body, expected) == 0);
                        assert(lv_obj_has_flag(s_lbl_clock_action, LV_OBJ_FLAG_HIDDEN));
                    }
                }
            }
        }
        auth_ui_update_clock_modes(active, UINT8_MAX, UINT8_MAX, true, true);
        draw_frame(display, "clock-invalid", active);
        check_clock_layout();
        auth_ui_update_clock_modes((rtc_mode_t)UINT8_MAX, AUTH_CLOCK_MODES_BACK_INDEX,
                                   AUTH_CLOCK_MODES_PAGE_APPLY, true, true);
        draw_frame(display, "clock-invalid-active", active);
        check_clock_layout();
        assert(strstr(lv_label_get_text(s_lbl_clock_mode), "--"));
        auth_ui_set_view(AUTH_VIEW_LAB);
        assert(!s_pnl_clock_modes);
        assert(lv_obj_get_child_count(s_root_screen) == root_children);
        auth_ui_update_lab(4, false, true, false);
        draw_frame(display, "lab-after-clock", active);
        for (unsigned row = 0; row < AUTH_LAB_ITEM_COUNT; row++) {
            assert(!lv_obj_has_flag(s_lbl_lab_items[row], LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_get_style_text_font(s_lbl_lab_items[row], 0) == &lv_font_montserrat_14);
            assert(lv_label_get_long_mode(s_lbl_lab_items[row]) == LV_LABEL_LONG_DOT);
        }
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    size_t reserve = argc > 1 ? strtoul(argv[1], NULL, 10) : 15728;
    render_directory = argc > 2 ? argv[2] : NULL;
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
            if (selected == AUTH_SETTINGS_CLOCK_MODES_INDEX) {
                assert(window.group == 5 && window.count == 2);
                assert(strcmp(lv_label_get_text(s_lbl_lab_items[2]), "> Clock Mode") == 0);
            } else if (selected == AUTH_SETTINGS_EXIT_INDEX) {
                assert(window.group == 6 && window.count == 1);
                assert(strcmp(lv_label_get_text(s_lbl_lab_items[1]), "> Exit to TOTP") == 0);
            }
            draw_frame(display, "settings", selected);
        }
        test_clock_modes(display);
    }
    lv_free(filler);
    lv_deinit();
    puts("Raster scrolling regression: PASS");
    return 0;
}
