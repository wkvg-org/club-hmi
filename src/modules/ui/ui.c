/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bar-dashboard UI shell — tab-based layout with a right-hand sidebar
 * showing the current time/date and tab navigation buttons.  Pages
 * are overlaid in the main content area; only the active tab's page
 * is visible.
 *
 * Tab indices (enum):
 *   TAB_OVERVIEW (0) — bar overview / day acts
 *   TAB_PAGER    (1) — pager (placeholder)
 *   TAB_LIGHT    (2) — DMX moving-head control (colour grid, dimmer)
 *   TAB_ALARM    (3) — alarm (placeholder)
 *   TAB_SETTINGS (4) — backlight / settings
 *
 * Layout (270° rotation, landscape 1280×800 logical):
 *   ┌──────────────────────────┬──────────┐
 *   │   pages (flex-grow: 1)   │ sidebar  │
 *   │   g_left_pages[tab]      │ 320 px   │
 *   │                          │ ┌──────┐ │
 *   │                          │ │ time │ │
 *   │                          │ │ date │ │
 *   │                          │ ├──────┤ │
 *   │                          │ │ tab  │ │
 *   │                          │ │ btns │ │
 *   │                          │ └──────┘ │
 *   └──────────────────────────┴──────────┘
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "assets/free_mono_bold_24.h"
#include "assets/free_mono_bold_48.h"
#include "assets/oswald_regular_36.h"
#include "ui.h"
#include "ui_globals.h"
#include "ui_tab_dmx.h"
#include "ui_tab_overview.h"
#include "ui_tab_settings.h"
#include "../net/directus_zbus.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

/* ── Tab indices ────────────────────────────────────────────────────── */

enum {
	TAB_OVERVIEW = 0,
	TAB_PAGER,
	TAB_LIGHT,
	TAB_ALARM,
	TAB_SETTINGS,
	TAB_COUNT
};

/* ── Global state ───────────────────────────────────────────────────── */

static lv_obj_t   *g_root;
static lv_obj_t   *g_left_pages[TAB_COUNT];
static lv_obj_t   *g_btn_tab[TAB_COUNT];
static lv_obj_t   *g_label_time;
static lv_obj_t   *g_label_date;
static lv_timer_t *g_timer_1s;
static uint8_t     g_active_tab = TAB_OVERVIEW;

static lv_style_t st_panel;
static lv_style_t st_box;
static lv_style_t st_btn;
static lv_style_t st_btn_active;
static lv_style_t st_btn_blink;

/* ── Tab switching ──────────────────────────────────────────────────── */

static void tab_set_active(uint8_t tab)
{
	if (tab >= TAB_COUNT) {
		return;
	}
	g_active_tab = tab;

	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		if (g_left_pages[i]) {
			lv_obj_add_flag(g_left_pages[i], LV_OBJ_FLAG_HIDDEN);
		}
		if (g_btn_tab[i]) {
			lv_obj_remove_style(g_btn_tab[i], &st_btn_active, 0);
		}
	}
	if (g_left_pages[tab]) {
		lv_obj_remove_flag(g_left_pages[tab], LV_OBJ_FLAG_HIDDEN);
	}
	if (g_btn_tab[tab]) {
		lv_obj_add_style(g_btn_tab[tab], &st_btn_active, 0);
	}
}

static void tab_btn_event_cb(lv_event_t *e)
{
	lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
	uint8_t   tab = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
	tab_set_active(tab);
}

void ui_switch_tab(int tab)
{
	if (tab >= 0 && tab < TAB_COUNT) {
		tab_set_active((uint8_t)tab);
	}
}

/* ── Styles ─────────────────────────────────────────────────────────── */

static void styles_init(void)
{
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

	lv_style_init(&st_btn);
	lv_style_set_bg_color(&st_btn, lv_color_black());
	lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
	lv_style_set_border_color(&st_btn, lv_color_hex(0x777777));
	lv_style_set_border_width(&st_btn, 2);
	lv_style_set_radius(&st_btn, 0);
	lv_style_set_pad_all(&st_btn, 12);

	lv_style_init(&st_btn_active);
	lv_style_set_bg_color(&st_btn_active, lv_color_hex(0x4f4f4f));
	lv_style_set_bg_opa(&st_btn_active, LV_OPA_COVER);

	lv_style_init(&st_btn_blink);
	lv_style_set_bg_color(&st_btn_blink, lv_color_hex(0xFFE7A3));
	lv_style_set_bg_opa(&st_btn_blink, LV_OPA_COVER);
}

/* ── Simple placeholder tab ─────────────────────────────────────────── */

static lv_obj_t *build_simple_page(lv_obj_t *parent, const char *title)
{
	lv_obj_t *page = lv_obj_create(parent);
	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lv_obj_add_style(page, &st_panel, 0);

	lv_obj_t *label = lv_label_create(page);
	lv_label_set_text(label, title);
	lv_obj_center(label);
	return page;
}

/* ── Sidebar tab button ─────────────────────────────────────────────── */

static lv_obj_t *make_tab_button(lv_obj_t *parent, const char *txt,
				 uint8_t tab, lv_coord_t h)
{
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_add_style(btn, &st_btn, 0);
	lv_obj_set_width(btn, LV_PCT(100));
	lv_obj_set_height(btn, h);
	lv_obj_set_user_data(btn, (void *)(uintptr_t)tab);
	lv_obj_add_event_cb(btn, tab_btn_event_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, txt);
	lv_obj_set_style_text_font(label, &oswald_regular_36, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_center(label);

	return btn;
}

/* ── 1-second timer callback ────────────────────────────────────────── */

static void refresh_datetime_labels(void)
{
	if (g_label_time) {
		lv_label_set_text(g_label_time, g_ui_time_str);
	}
	if (g_label_date) {
		lv_label_set_text(g_label_date, g_ui_date_str);
	}
}

static void refresh_alarm_blink(void)
{
	static bool blink_on;

	if (!g_btn_tab[TAB_ALARM]) {
		return;
	}
	bool should_blink = g_alarm_notification && (g_active_tab != TAB_ALARM);

	if (should_blink) {
		blink_on = !blink_on;
		if (blink_on) {
			lv_obj_add_style(g_btn_tab[TAB_ALARM], &st_btn_blink, 0);
		} else {
			lv_obj_remove_style(g_btn_tab[TAB_ALARM], &st_btn_blink, 0);
		}
	} else {
		blink_on = false;
		lv_obj_remove_style(g_btn_tab[TAB_ALARM], &st_btn_blink, 0);
	}
}

static void ui_timer_cb(lv_timer_t *t)
{
	(void)t;
	const struct zbus_channel *chan;
	while (zbus_sub_wait(&directus_sub, &chan, K_NO_WAIT) == 0) {
		ui_tab_overview_drain_acts(chan);
	}
	time_update_gui_globals();
	refresh_datetime_labels();
	ui_tab_overview_refresh();
	refresh_alarm_blink();
}

/* ── Build the full UI ──────────────────────────────────────────────── */

void ui_init_and_build(void)
{
	static bool inited;

	if (!inited) {
		styles_init();
		inited = true;
	}

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(scr, 12, 0);

	/* Root flex container: pages (flex-grow) | sidebar (320 px) */
	g_root = lv_obj_create(scr);
	lv_obj_remove_style_all(g_root);
	lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
	lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(g_root, 12, 0);

	/* Main content area (fills available width) */
	lv_obj_t *pages = lv_obj_create(g_root);
	lv_obj_remove_style_all(pages);
	lv_obj_set_flex_grow(pages, 1);
	lv_obj_set_height(pages, LV_PCT(100));

	/* Sidebar (fixed 320 px wide, right edge) */
	const lv_coord_t sidebar_w = 320;
	lv_obj_t *sidebar = lv_obj_create(g_root);
	lv_obj_remove_style_all(sidebar);
	lv_obj_set_size(sidebar, sidebar_w, LV_PCT(100));
	lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(sidebar, 12, 0);

	/* Tab pages (overlaid, only active one visible) */
	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		g_left_pages[i] = lv_obj_create(pages);
		lv_obj_remove_style_all(g_left_pages[i]);
		lv_obj_set_size(g_left_pages[i], LV_PCT(100), LV_PCT(100));
	}
	g_left_pages[TAB_OVERVIEW]  = ui_tab_overview_build(pages);
	g_left_pages[TAB_PAGER]     = build_simple_page(pages, "Pager");
	g_left_pages[TAB_LIGHT]     = ui_tab_dmx_create(pages);
	g_left_pages[TAB_ALARM]     = build_simple_page(pages, "Alarmanlage");
	g_left_pages[TAB_SETTINGS]  = build_settings_tab_page(pages);

	/* Date/time display in the sidebar */
	lv_obj_t *box_time = lv_obj_create(sidebar);
	lv_obj_add_style(box_time, &st_box, 0);
	lv_obj_set_size(box_time, LV_PCT(100), 120);

	g_label_time = lv_label_create(box_time);
	lv_label_set_text(g_label_time, g_ui_time_str);
	lv_obj_set_style_text_font(g_label_time, &free_mono_bold_48, 0);
	lv_obj_set_style_text_align(g_label_time, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(g_label_time, lv_color_white(), 0);
	lv_obj_align(g_label_time, LV_ALIGN_TOP_MID, 0, 12);

	g_label_date = lv_label_create(box_time);
	lv_label_set_text(g_label_date, g_ui_date_str);
	lv_obj_set_style_text_font(g_label_date, &free_mono_bold_24, 0);
	lv_obj_set_style_text_align(g_label_date, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(g_label_date, lv_color_white(), 0);
	lv_obj_align(g_label_date, LV_ALIGN_BOTTOM_MID, 0, -12);

	/* Sidebar tab buttons */
	g_btn_tab[TAB_OVERVIEW]  = make_tab_button(sidebar, "\u00DCbersicht",    TAB_OVERVIEW,  105);
	g_btn_tab[TAB_PAGER]     = make_tab_button(sidebar, "Pager",             TAB_PAGER,     105);
	g_btn_tab[TAB_LIGHT]     = make_tab_button(sidebar, "Lichtsteuerung",    TAB_LIGHT,     105);
	g_btn_tab[TAB_ALARM]     = make_tab_button(sidebar, "Alarmanlage",       TAB_ALARM,     115);
	g_btn_tab[TAB_SETTINGS]  = make_tab_button(sidebar, "Einstellungen",     TAB_SETTINGS,  115);

	tab_set_active(TAB_OVERVIEW);

	/* 1-second timer for clock and overview refresh */
	if (g_timer_1s) {
		lv_timer_del(g_timer_1s);
	}
	g_timer_1s = lv_timer_create(ui_timer_cb, 1000, NULL);

	refresh_datetime_labels();
	ui_tab_overview_refresh();
	refresh_alarm_blink();
}

/* ── Public setters ─────────────────────────────────────────────────── */

void ui_set_datetime(const char *hhmm, const char *ddmmyyyy)
{
	if (hhmm) {
		strncpy(g_ui_time_str, hhmm, sizeof(g_ui_time_str) - 1);
	}
	if (ddmmyyyy) {
		strncpy(g_ui_date_str, ddmmyyyy, sizeof(g_ui_date_str) - 1);
	}
}

void ui_set_alarm_notification(bool on)
{
	g_alarm_notification = on;
}
