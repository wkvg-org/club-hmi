/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Overview tab ("Meldungen" + "Show Management"), ported from the Arduino
 * bar-dashboard ui_tab_overview.cpp.
 */

#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "ui_globals.h"
#include "ui_tab_overview.h"
#include "../net/directus_zbus.h"

LOG_MODULE_REGISTER(ui_tab_overview, LOG_LEVEL_INF);

typedef struct {
	lv_obj_t *row;
	lv_obj_t *lbl_act;
	lv_obj_t *lbl_time;
} act_row_ui_t;

static act_row_ui_t g_act_rows[MAX_ACTS] = {0};
static uint8_t g_act_rows_built = 0;

static act_t  g_acts[MAX_ACTS];
static uint8_t g_acts_count;
static lv_style_t st_panel;
static lv_style_t st_box;
static lv_style_t st_title;
static lv_style_t st_row;
static lv_style_t st_row_active;

static uint16_t g_last_minute = UINT16_MAX;

static void fmt_time_range(char *out, size_t out_sz, uint16_t start_min, uint16_t end_min)
{
	uint16_t sh = start_min / 60;
	uint16_t sm = start_min % 60;
	uint16_t eh = end_min / 60;
	uint16_t em = end_min % 60;

	snprintf(out, out_sz, "%02u:%02u - %02u:%02u", sh, sm, eh, em);
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *txt)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, txt);
	lv_obj_add_style(label, &st_title, 0);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	return label;
}

static lv_obj_t *make_box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_add_style(obj, &st_box, 0);
	lv_obj_set_size(obj, w, h);
	return obj;
}

static void ui_tab_overview_styles_init(void)
{
	static bool inited = false;

	if (inited) {
		return;
	}

	lv_style_init(&st_panel);
	lv_style_set_bg_color(&st_panel, lv_color_black());
	lv_style_set_bg_opa(&st_panel, LV_OPA_COVER);
	lv_style_set_border_color(&st_panel, lv_color_hex(0x777777));
	lv_style_set_border_width(&st_panel, 2);
	lv_style_set_radius(&st_panel, 0);
	lv_style_set_pad_all(&st_panel, 12);

	lv_style_init(&st_box);
	lv_style_set_bg_color(&st_box, lv_color_black());
	lv_style_set_bg_opa(&st_box, LV_OPA_COVER);
	lv_style_set_border_color(&st_box, lv_color_hex(0x777777));
	lv_style_set_border_width(&st_box, 1);
	lv_style_set_radius(&st_box, 0);
	lv_style_set_pad_all(&st_box, 10);

	lv_style_init(&st_title);
	lv_style_set_text_color(&st_title, lv_color_black());
	lv_style_set_text_align(&st_title, LV_TEXT_ALIGN_CENTER);

	lv_style_init(&st_row);
	lv_style_set_bg_color(&st_row, lv_color_white());
	lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
	lv_style_set_border_color(&st_row, lv_color_hex(0x777777));
	lv_style_set_border_width(&st_row, 1);
	lv_style_set_radius(&st_row, 0);
	lv_style_set_pad_all(&st_row, 10);

	lv_style_init(&st_row_active);
	lv_style_set_bg_color(&st_row_active, lv_color_hex(0x40a832));
	lv_style_set_bg_opa(&st_row_active, LV_OPA_COVER);
	lv_style_set_border_color(&st_row_active, lv_color_hex(0x40a832));
	lv_style_set_border_width(&st_row_active, 1);
	lv_style_set_radius(&st_row_active, 0);
	lv_style_set_pad_all(&st_row_active, 10);

	inited = true;
}

static lv_obj_t *build_show_row(lv_obj_t *parent, uint8_t idx)
{
	lv_obj_t *row = lv_obj_create(parent);

	lv_obj_add_style(row, &st_row, 0);
	lv_obj_set_width(row, LV_PCT(100));
	lv_obj_set_height(row, 70);

	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 12, 0);

	lv_obj_t *act_btn = lv_btn_create(row);

	lv_obj_set_style_bg_color(act_btn, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(act_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(act_btn, lv_color_hex(0x777777), 0);
	lv_obj_set_style_border_width(act_btn, 1, 0);
	lv_obj_set_style_radius(act_btn, 0, 0);
	lv_obj_set_size(act_btn, 160, 55);

	lv_obj_t *lbl_act = lv_label_create(act_btn);

	lv_label_set_text(lbl_act, "Act");
	lv_obj_center(lbl_act);

	lv_obj_t *time_box = lv_obj_create(row);

	lv_obj_add_style(time_box, &st_box, 0);
	lv_obj_set_flex_grow(time_box, 1);
	lv_obj_set_height(time_box, 55);
	lv_obj_set_style_border_width(time_box, 1, 0);

	lv_obj_t *lbl_time = lv_label_create(time_box);

	lv_label_set_text(lbl_time, "--:-- - --:--");
	lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
	lv_obj_center(lbl_time);

	if (idx < MAX_ACTS) {
		g_act_rows[idx].row = row;
		g_act_rows[idx].lbl_act = lbl_act;
		g_act_rows[idx].lbl_time = lbl_time;
	}

	return row;
}

/**
 * Build the overview tab page with two panels:
 *   TOP — "Meldungen" (alerts) with a three-stage status indicator and
 *         an "acknowledge" (quittieren) button.
 *   BOTTOM — "Show Management" with up to MAX_ACTS (16) rows, each
 *            showing an act name, time range, and green highlight for
 *            the currently active act.
 */
lv_obj_t *ui_tab_overview_build(lv_obj_t *parent)
{
	ui_tab_overview_styles_init();

	lv_obj_t *page = lv_obj_create(parent);

	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(page, 12, 0);

	lv_obj_t *p_msg = lv_obj_create(page);

	lv_obj_add_style(p_msg, &st_panel, 0);
	lv_obj_set_width(p_msg, LV_PCT(100));
	lv_obj_set_flex_grow(p_msg, 1);
	lv_obj_set_flex_flow(p_msg, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(p_msg, 12, 0);

	make_title(p_msg, "Meldungen");

	lv_obj_t *msg_row = lv_obj_create(p_msg);

	lv_obj_remove_style_all(msg_row);
	lv_obj_set_width(msg_row, LV_PCT(100));
	lv_obj_set_flex_grow(msg_row, 1);
	lv_obj_set_flex_flow(msg_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(msg_row, 12, 0);

	lv_obj_t *msg_left = lv_obj_create(msg_row);

	lv_obj_remove_style_all(msg_left);
	lv_obj_set_flex_grow(msg_left, 1);
	lv_obj_set_height(msg_left, LV_PCT(100));
	lv_obj_set_flex_flow(msg_left, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(msg_left, 12, 0);

	lv_obj_t *box_meldetext = make_box(msg_left, LV_PCT(100), 70);
	lv_obj_t *label_meldetext = lv_label_create(box_meldetext);

	lv_label_set_text(label_meldetext, "Meldetext");
	lv_obj_set_style_text_color(label_meldetext, lv_color_white(), 0);
	lv_obj_center(label_meldetext);

	lv_obj_t *box_handlung = make_box(msg_left, LV_PCT(100), LV_PCT(100));

	lv_obj_set_flex_grow(box_handlung, 1);
	lv_obj_t *label_handlung = lv_label_create(box_handlung);

	lv_label_set_text(label_handlung, "Handlungsanweisung");
	lv_obj_set_style_text_color(label_handlung, lv_color_white(), 0);
	lv_obj_center(label_handlung);

	const lv_coord_t stufe_w = 120;
	lv_obj_t *stufe = lv_obj_create(msg_row);

	lv_obj_add_style(stufe, &st_box, 0);
	lv_obj_set_size(stufe, stufe_w, LV_PCT(100));
	lv_obj_set_flex_flow(stufe, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(stufe, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(stufe, 10, 0);

	lv_obj_t *label_stufe = lv_label_create(stufe);

	lv_label_set_text(label_stufe, "Stufe");
	lv_obj_set_style_text_color(label_stufe, lv_color_white(), 0);

	lv_obj_t *stufe_indicators = lv_obj_create(stufe);

	lv_obj_remove_style_all(stufe_indicators);
	lv_obj_set_width(stufe_indicators, LV_PCT(100));
	lv_obj_set_flex_grow(stufe_indicators, 1);
	lv_obj_set_flex_flow(stufe_indicators, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(stufe_indicators, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			     LV_FLEX_ALIGN_CENTER);

	const lv_color_t indicator_colors[3] = {
		lv_color_hex(0xD32F2F),
		lv_color_hex(0xFBC02D),
		lv_color_hex(0x1976D2)};
	const lv_coord_t indicator_diameter = (stufe_w * 6) / 10;

	for (uint8_t i = 0; i < 3; i++) {
		lv_obj_t *indicator = lv_obj_create(stufe_indicators);

		lv_obj_remove_style_all(indicator);
		lv_obj_set_size(indicator, indicator_diameter, indicator_diameter);
		lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(indicator, indicator_colors[i], 0);
		lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(indicator, 2, 0);
		lv_obj_set_style_border_color(indicator, lv_color_white(), 0);
	}

	lv_obj_t *quitt = lv_btn_create(msg_row);

	lv_obj_set_style_bg_color(quitt, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(quitt, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(quitt, lv_color_hex(0x777777), 0);
	lv_obj_set_style_border_width(quitt, 2, 0);
	lv_obj_set_style_radius(quitt, 0, 0);
	lv_obj_set_style_pad_all(quitt, 12, 0);
	lv_obj_set_size(quitt, 200, LV_PCT(100));

	lv_obj_t *label_quitt = lv_label_create(quitt);

	lv_label_set_text(label_quitt, "Quittieren");
	lv_obj_set_style_text_color(label_quitt, lv_color_white(), 0);
	lv_obj_center(label_quitt);

	lv_obj_t *p_show = lv_obj_create(page);

	lv_obj_add_style(p_show, &st_panel, 0);
	lv_obj_set_width(p_show, LV_PCT(100));
	lv_obj_set_flex_grow(p_show, 1);
	lv_obj_set_flex_flow(p_show, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(p_show, 12, 0);

	make_title(p_show, "Show Management");

	lv_obj_t *show_list = lv_obj_create(p_show);

	lv_obj_remove_style_all(show_list);
	lv_obj_set_width(show_list, LV_PCT(100));
	lv_obj_set_flex_grow(show_list, 1);
	lv_obj_set_flex_flow(show_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(show_list, 12, 0);

	g_act_rows_built = 0;
	for (uint8_t i = 0; i < MAX_ACTS; i++) {
		build_show_row(show_list, i);
		g_act_rows_built++;
	}

	return page;
}

/**
 * Refresh the overview tab once per minute: update act row names and
 * time ranges, show/hide rows based on act count, and highlight
 * the currently active act in green. No-op if no minute tick occurred.
 */
void ui_tab_overview_refresh(void)
{
	if (g_act_rows_built == 0) {
		return;
	}

	if (g_last_minute == g_now_minutes) {
		return;
	}

	uint8_t cnt = g_acts_count;

	if (cnt > MAX_ACTS) {
		cnt = MAX_ACTS;
	}

	for (uint8_t i = 0; i < MAX_ACTS; i++) {
		if (!g_act_rows[i].row) {
			continue;
		}

		if (i < cnt) {
			lv_obj_remove_flag(g_act_rows[i].row, LV_OBJ_FLAG_HIDDEN);
			lv_label_set_text(g_act_rows[i].lbl_act, g_acts[i].name);

			char buf[32];

			fmt_time_range(buf, sizeof(buf), g_acts[i].start_min,
				       g_acts[i].end_min);
			lv_label_set_text(g_act_rows[i].lbl_time, buf);
		} else {
			lv_obj_add_flag(g_act_rows[i].row, LV_OBJ_FLAG_HIDDEN);
		}
	}

	uint16_t now = g_now_minutes;

	g_last_minute = now;

	for (uint8_t i = 0; i < cnt; i++) {
		bool active = (now >= g_acts[i].start_min) && (now < g_acts[i].end_min);

		lv_obj_remove_style(g_act_rows[i].row, &st_row_active, 0);
		lv_obj_remove_style(g_act_rows[i].row, &st_row, 0);
		lv_obj_add_style(g_act_rows[i].row, active ? &st_row_active : &st_row, 0);
	}
}

void ui_tab_overview_drain_acts(const struct zbus_channel *chan)
{
	if (chan != &directus_chan) {
		return;
	}
	struct directus_msg msg;
	zbus_chan_read(&directus_chan, &msg, K_NO_WAIT);
	g_acts_count = (msg.count > MAX_ACTS) ? MAX_ACTS : msg.count;
	for (uint8_t i = 0; i < g_acts_count; i++) {
		g_acts[i] = msg.acts[i];
	}
	g_last_minute = UINT16_MAX;
}
