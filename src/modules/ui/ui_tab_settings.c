/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings tab: LCD backlight brightness slider. The value is persisted
 * on the SD card (settings.c) and applied at startup.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include "../config/settings.h"
#include "assets/oswald_regular_36.h"
#include "ui_tab_settings.h"

static lv_obj_t *g_backlight_label;

static void backlight_slider_cb(lv_event_t *e)
{
	lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
	int percent = lv_slider_get_value(slider);
	char text[24];

	settings_set_backlight_percent((uint8_t)percent);

	snprintf(text, sizeof(text), "Hintergrund: %d%%", percent);
	lv_label_set_text(g_backlight_label, text);
}

lv_obj_t *build_settings_tab_page(lv_obj_t *parent)
{
	lv_obj_t *page = lv_obj_create(parent);

	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(page, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(page, lv_color_hex(0x777777), 0);
	lv_obj_set_style_border_width(page, 2, 0);
	lv_obj_set_style_pad_all(page, 16, 0);
	lv_obj_set_style_pad_row(page, 24, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *title = lv_label_create(page);

	lv_label_set_text(title, "Einstellungen");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &oswald_regular_36, 0);
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(title, LV_PCT(100));

	g_backlight_label = lv_label_create(page);
	lv_label_set_text(g_backlight_label, "Hintergrund: --%");
	lv_obj_set_style_text_color(g_backlight_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_backlight_label, &lv_font_montserrat_24, 0);

	lv_obj_t *slider = lv_slider_create(page);

	lv_obj_set_width(slider, LV_PCT(100));
	lv_slider_set_range(slider, 0, 100);
	lv_slider_set_value(slider, settings_get_backlight_percent(), LV_ANIM_OFF);
	lv_obj_add_event_cb(slider, backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	return page;
}
