/*
 * SPDX-FileCopyrightText: 2026 FoloToy
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

/* Matches BSP_LCD_BL in components/bsp/include/bsp_pins.h (U0TXD / GPIO21). */
#define BL_GUARD_GPIO 21

static void force_backlight_low(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
    /* Latch low and clear UART0 reset pull-up before enabling GPIO output so
     * GPIO21 stays dark from 2nd-stage bootloader entry until LEDC takes over. */
    gpio_ll_pullup_dis(&GPIO, BL_GUARD_GPIO);
    gpio_ll_pulldown_dis(&GPIO, BL_GUARD_GPIO);
    gpio_ll_input_disable(&GPIO, BL_GUARD_GPIO);
    gpio_ll_matrix_out_default(&GPIO, BL_GUARD_GPIO);
    gpio_ll_set_level(&GPIO, BL_GUARD_GPIO, 0);
    gpio_ll_func_sel(&GPIO, BL_GUARD_GPIO, PIN_FUNC_GPIO);
    gpio_ll_output_enable(&GPIO, BL_GUARD_GPIO);
#endif
}

/* Force-linked by ESP-IDF bootloader main (-u bootloader_hooks_include). */
void bootloader_hooks_include(void)
{
}

void bootloader_before_init(void)
{
    force_backlight_low();
}

void bootloader_after_init(void)
{
    /* bootloader_console_init() disables output on U0TXD (GPIO21), leaving the
     * reset pull-up or floating input active; re-clamp low through app load. */
    force_backlight_low();
}
