/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * club-hmi — bar-dashboard for the Waveshare ESP32-P4-NANO 10.1"
 * MIPI-DSI panel (TLK110 Ethernet, GT911 touch, 800x1280 portrait
 * mounted landscape @ 270° rotation).
 *
 * Architecture
 * ============
 * main() initialises the display, settings, and UI, then starts
 * three backend threads (NTP, MQTT, Directus) and enters
 * the LVGL event loop.
 *
 *   ┌──────────┐   ┌─────┐   ┌──────────┐
 *   │ NTP sync │   │ MQTT│   │ Directus │  ← backend threads
 *   │ (prio 6) │   │(prio│   │ (prio 4) │
 *   └────┬─────┘   └──┬──┘   └────┬─────┘
 *        │            │           │
 *   ┌────┴────────────┴───────────┴────┐
 *   │          Ethernet (EMAC)         │
 *   └─────────────────────────────────┘
 *        │
 *   ┌────┴────────────────────────────┐
 *   │   LVGL UI loop (main thread)    │
 *   │   • time/date display sidebar   │
 *   │   • overview / DMX tabs         │
 *   │   • settings / backlight        │
 *   └─────────────────────────────────┘
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <ix_lvgl/theme.h>

#include "display_rotation.h"
#include "modules/config/bar_config.h"
#include "modules/net/directus.h"
#include "modules/net/mqtt.h"
#include "modules/net/net.h"
#include "modules/net/net_config.h"
#include "modules/time/ntp.h"
#include "modules/config/settings.h"
#include "modules/ui/ui.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/*
 * Backend thread stacks.  Plain arrays placed in external RAM (PSRAM)
 * corrupted per-CPU state during the SNTP/DNS path on early P4 silicon,
 * so K_THREAD_STACK_DEFINE guards are used throughout.
 */
static K_THREAD_STACK_DEFINE(ntp_stack,      16384);
static K_THREAD_STACK_DEFINE(mqtt_stack,     20480);
static K_THREAD_STACK_DEFINE(directus_stack, 32768);
static struct k_thread ntp_thread;
static struct k_thread mqtt_thread;
static struct k_thread directus_thread;

int main(void)
{
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int ret;

	if (!device_is_ready(display_dev)) {
		LOG_ERR("display device not ready");
		return 0;
	}

	/*
	 * The 800×1280 panel is mounted landscape — render the UI in
	 * landscape (LVGL ROTATION_270 → logical 1280×800) and rotate the
	 * flushed pixels into the physical portrait framebuffer (see
	 * display_rotation.c) so content appears upright.
	 */
	lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
	ret = display_rotation_init(display_dev, lv_display_get_default(),
				    LV_DISPLAY_ROTATION_270);
	if (ret < 0) {
		LOG_ERR("failed to enable display rotation (%d)", ret);
		return 0;
	}

	/* Peripherals and UI */
	init_config();
	net_config_init();
	settings_load();
	settings_apply_backlight();

	/*
	 * Install the Siemens iX-inspired dark theme before building the UI;
	 * widgets created afterwards pick it up automatically.
	 */
	lvgl_ix_theme_init(lv_display_get_default(), LVGL_IX_COLOR_SCHEME_DARK, NULL);

	ui_init_and_build();

	/*
	 * The upstream DSI host enables video mode and the bridge DPI output
	 * as part of the panel attach(), so the panel is already streaming by
	 * the time main() runs - there is no separate "unblank" step.
	 */

	/* Start backend threads (wait on net_is_up() internally) */
	net_init();
	k_thread_create(&ntp_thread, ntp_stack, K_THREAD_STACK_SIZEOF(ntp_stack),
			(k_thread_entry_t)ntp_service, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&ntp_thread, "ntp");
	k_thread_create(&mqtt_thread, mqtt_stack, K_THREAD_STACK_SIZEOF(mqtt_stack),
			(k_thread_entry_t)mqtt_service, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_name_set(&mqtt_thread, "mqtt");
	k_thread_create(&directus_thread, directus_stack, K_THREAD_STACK_SIZEOF(directus_stack),
			(k_thread_entry_t)directus_service, NULL, NULL, NULL, 4, 0, K_NO_WAIT);
	k_thread_name_set(&directus_thread, "directus");

	LOG_INF("display ready, entering LVGL loop");

	/* LVGL runs cooperatively on the main thread at ~200 Hz */
	while (1) {
		lv_timer_handler();
		k_sleep(K_MSEC(5));
	}

	return 0;
}
