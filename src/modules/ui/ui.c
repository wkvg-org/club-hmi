/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bar-dashboard UI shell — Siemens iX-style collapsible left navigation
 * rail. Each entry shows an icon plus a text label; when collapsed only
 * the icons remain. Pages are overlaid in the main content area; only the
 * active tab's page is visible.
 *
 * Tab indices (enum):
 *   TAB_OVERVIEW (0) — bar overview / day acts
 *   TAB_PAGER    (1) — pager (placeholder)
 *   TAB_LIGHT    (2) — DMX moving-head control (colour grid, dimmer)
 *   TAB_ALARM    (3) — alarm (placeholder)
 *   TAB_SETTINGS (4) — backlight / settings
 *
 * Layout (270° rotation, landscape 1280×800 logical):
 *   ┌─────────┬────────────────────────────────┐
 *   │  nav    │   pages (flex-grow: 1)         │
 *   │  rail   │   g_pages[tab]                 │
 *   │  64/240 │                                │
 *   └─────────┴────────────────────────────────┘
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <ix_lvgl/icons.h>
#include <ix_lvgl/theme.h>

#include "assets/free_mono_bold_24.h"
#include "assets/free_mono_bold_48.h"
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

/* ── Navigation geometry (iX sizing tokens) ────────────────────────── */

#define NAV_COLLAPSED_W 64
#define NAV_EXPANDED_W  240
#define NAV_ITEM_H      48
#define NAV_TOGGLE_DEBOUNCE_MS 250

/* ── Global state ───────────────────────────────────────────────────── */

static lv_obj_t   *g_root;
static lv_obj_t   *g_pages[TAB_COUNT];
static lv_obj_t   *g_nav;
static lv_obj_t   *g_nav_items[TAB_COUNT];
static lv_obj_t   *g_nav_labels[TAB_COUNT];
static lv_obj_t   *g_box_time;
static lv_obj_t   *g_label_time;
static lv_obj_t   *g_label_date;
static lv_obj_t   *g_btn_collapse;
static lv_obj_t   *g_icon_collapse;
static lv_timer_t *g_timer_1s;
static uint8_t     g_active_tab = TAB_OVERVIEW;
static bool        g_nav_collapsed;
static uint32_t    g_nav_toggle_tick;
static bool        g_nav_toggle_seen;
static bool        g_ui_built;

static const struct lvgl_ix_palette *g_pal;
static lv_style_t st_nav_base;
static lv_style_t st_nav_active;
static lv_style_t st_nav_blink;

static void nav_set_collapsed(bool collapsed);

static const enum lvgl_ix_icon tab_icons[TAB_COUNT] = {
	[TAB_OVERVIEW] = LVGL_IX_ICON_HOME,
	[TAB_PAGER]    = LVGL_IX_ICON_MAIL,
	[TAB_LIGHT]    = LVGL_IX_ICON_SUN,
	[TAB_ALARM]    = LVGL_IX_ICON_ALARM_BELL,
	[TAB_SETTINGS] = LVGL_IX_ICON_COGWHEEL,
};

static const char *const tab_titles[TAB_COUNT] = {
	[TAB_OVERVIEW] = "Übersicht",
	[TAB_PAGER]    = "Pager",
	[TAB_LIGHT]    = "Lichtsteuerung",
	[TAB_ALARM]    = "Alarmanlage",
	[TAB_SETTINGS] = "Einstellungen",
};

/* ── Tab switching ──────────────────────────────────────────────────── */

static void tab_set_active(uint8_t tab)
{
	if (tab >= TAB_COUNT) {
		return;
	}
	if (tab == g_active_tab) {
		return;
	}
	if (g_active_tab == TAB_SETTINGS) {
		ui_tab_settings_hide_keyboard();
	}

	if (g_pages[g_active_tab] != NULL) {
		lv_obj_set_hidden(g_pages[g_active_tab], true);
	}
	if (g_nav_items[g_active_tab] != NULL) {
		lv_obj_remove_style(g_nav_items[g_active_tab], &st_nav_active, 0);
	}

	g_active_tab = tab;
	if (g_pages[tab]) {
		lv_obj_set_hidden(g_pages[tab], false);
	}
	if (g_nav_items[tab]) {
		lv_obj_add_style(g_nav_items[tab], &st_nav_active, 0);
	}
}

static void tab_btn_event_cb(lv_event_t *e)
{
	lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
	uint8_t   tab = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
	lv_point_t point;
	lv_indev_get_point(lv_indev_active(), &point);

	LOG_INF("nav click: tab=%u collapsed=%d touch=(%d,%d)", tab, g_nav_collapsed,
		point.x, point.y);
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
	g_pal = lvgl_ix_palette_get(LVGL_IX_COLOR_SCHEME_DARK);

	lv_style_init(&st_nav_base);
	lv_style_set_bg_opa(&st_nav_base, LV_OPA_TRANSP);
	lv_style_set_border_width(&st_nav_base, 0);
	lv_style_set_radius(&st_nav_base, 0);
	lv_style_set_text_color(&st_nav_base, g_pal->text);

	lv_style_init(&st_nav_active);
	lv_style_set_bg_color(&st_nav_active, g_pal->surface_active);
	lv_style_set_bg_opa(&st_nav_active, LV_OPA_COVER);
	lv_style_set_border_width(&st_nav_active, 0);
	lv_style_set_text_color(&st_nav_active, g_pal->text);

	lv_style_init(&st_nav_blink);
	lv_style_set_bg_color(&st_nav_blink, g_pal->warning);
	lv_style_set_bg_opa(&st_nav_blink, LV_OPA_COVER);
	lv_style_set_text_color(&st_nav_blink, g_pal->on_warning);
}

/* ── Simple placeholder tab ─────────────────────────────────────────── */

static lv_obj_t *build_simple_page(lv_obj_t *parent, const char *title)
{
	lv_obj_t *page = lv_obj_create(parent);
	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lvgl_ix_style_surface(page);

	lv_obj_t *label = lv_label_create(page);
	lv_label_set_text(label, title);
	lv_obj_center(label);
	return page;
}

/* ── Navigation ─────────────────────────────────────────────────────── */

static lv_obj_t *make_nav_item(lv_obj_t *parent, enum lvgl_ix_icon icon, const char *txt,
			       uint8_t tab)
{
	lv_obj_t *btn = lv_btn_create(parent);

	lv_obj_set_size(btn, LV_PCT(100), NAV_ITEM_H);
	lv_obj_set_user_data(btn, (void *)(uintptr_t)tab);
	lv_obj_add_event_cb(btn, tab_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_style(btn, &st_nav_base, 0);
	lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_PRESSED);
	lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_HOVERED);
	lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(btn, 12, 0);

	lvgl_ix_icon_create(btn, icon, g_pal->text);

	lv_obj_t *text_label = lv_label_create(btn);

	lv_label_set_text(text_label, txt);
	lv_obj_set_style_text_font(text_label,
				    tab == TAB_OVERVIEW ? &free_mono_bold_24 :
							 &lv_font_montserrat_16, 0);

	g_nav_labels[tab] = text_label;

	return btn;
}

static void nav_set_collapsed(bool collapsed)
{
	lv_coord_t width = collapsed ? NAV_COLLAPSED_W : NAV_EXPANDED_W;

	g_nav_collapsed = collapsed;

	/* Keep the sidebar fixed when flex recalculates the row after page changes. */
	lv_obj_set_width(g_nav, width);
	lv_obj_set_style_min_width(g_nav, width, 0);
	lv_obj_set_style_max_width(g_nav, width, 0);

	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		if (g_nav_items[i] == NULL) {
			continue;
		}
		lv_obj_set_width(g_nav_items[i], LV_PCT(100));
		if (collapsed) {
			lv_obj_set_hidden(g_nav_labels[i], true);
			lv_obj_set_flex_align(g_nav_items[i], LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		} else {
			lv_obj_set_hidden(g_nav_labels[i], false);
			lv_obj_set_flex_align(g_nav_items[i], LV_FLEX_ALIGN_START,
					      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		}
	}

	if (g_box_time != NULL) {
		lv_obj_set_hidden(g_box_time, collapsed);
	}

	if (g_icon_collapse != NULL) {
		lv_image_set_src(g_icon_collapse,
				 lvgl_ix_icon_get(collapsed ? LVGL_IX_ICON_CHEVRON_RIGHT :
						   LVGL_IX_ICON_CHEVRON_LEFT));
	}
}

static void collapse_btn_event_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
	    lv_event_get_current_target(e) != g_btn_collapse ||
	    lv_event_get_target(e) != g_btn_collapse) {
		return;
	}
	if (g_nav_toggle_seen &&
	    lv_tick_elaps(g_nav_toggle_tick) < NAV_TOGGLE_DEBOUNCE_MS) {
		return;
	}

	g_nav_toggle_tick = lv_tick_get();
	g_nav_toggle_seen = true;
	lv_point_t point;
	lv_area_t area;
	lv_indev_get_point(lv_indev_active(), &point);
	lv_obj_get_coords(g_btn_collapse, &area);
	LOG_INF("nav collapse click: %d -> %d touch=(%d,%d) button=(%d,%d)-(%d,%d)",
		g_nav_collapsed, !g_nav_collapsed, point.x, point.y,
		area.x1, area.y1, area.x2, area.y2);
	nav_set_collapsed(!g_nav_collapsed);
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

	if (!g_nav_items[TAB_ALARM]) {
		return;
	}
	bool should_blink = g_alarm_notification && (g_active_tab != TAB_ALARM);

	if (should_blink) {
		blink_on = !blink_on;
		if (blink_on) {
			lv_obj_add_style(g_nav_items[TAB_ALARM], &st_nav_blink, 0);
		} else {
			lv_obj_remove_style(g_nav_items[TAB_ALARM], &st_nav_blink, 0);
		}
	} else {
		blink_on = false;
		lv_obj_remove_style(g_nav_items[TAB_ALARM], &st_nav_blink, 0);
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
	if (g_ui_built) {
		return;
	}
	styles_init();

	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, g_pal->background, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(scr, 12, 0);

	/* Root flex container: navigation rail | pages. */
	g_root = lv_obj_create(scr);
	lv_obj_remove_style_all(g_root);
	lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
	lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(g_root, 12, 0);

	/* Left navigation rail / sidebar. */
	g_nav = lv_obj_create(g_root);
	lv_obj_remove_style_all(g_nav);
	lv_obj_set_size(g_nav, NAV_EXPANDED_W, LV_PCT(100));
	lv_obj_set_style_min_width(g_nav, NAV_EXPANDED_W, 0);
	lv_obj_set_style_max_width(g_nav, NAV_EXPANDED_W, 0);
	lv_obj_set_flex_flow(g_nav, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(g_nav, 8, 0);

	/* Collapse / expand toggle. */
	g_btn_collapse = lv_btn_create(g_nav);
	lv_obj_set_size(g_btn_collapse, LV_PCT(100), NAV_ITEM_H);
	lv_obj_add_style(g_btn_collapse, &st_nav_base, 0);
	lv_obj_set_style_outline_width(g_btn_collapse, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_obj_set_style_border_width(g_btn_collapse, 0, LV_PART_MAIN | LV_STATE_PRESSED);
	lv_obj_set_style_border_width(g_btn_collapse, 0, LV_PART_MAIN | LV_STATE_HOVERED);
	lv_obj_add_event_cb(g_btn_collapse, collapse_btn_event_cb, LV_EVENT_CLICKED, NULL);
	g_icon_collapse = lvgl_ix_icon_create(g_btn_collapse, LVGL_IX_ICON_CHEVRON_LEFT,
					      g_pal->text);
	if (g_icon_collapse != NULL) {
		lv_obj_center(g_icon_collapse);
	}

	/* Date/time display (visible only when expanded). */
	g_box_time = lv_obj_create(g_nav);
	lvgl_ix_style_surface(g_box_time);
	lv_obj_set_size(g_box_time, LV_PCT(100), 112);
	lv_obj_set_flex_flow(g_box_time, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(g_box_time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(g_box_time, 8, 0);
	lv_obj_set_style_pad_row(g_box_time, 4, 0);

	g_label_time = lv_label_create(g_box_time);
	lv_label_set_text(g_label_time, g_ui_time_str);
	lv_obj_set_width(g_label_time, LV_PCT(100));
	lv_label_set_long_mode(g_label_time, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_font(g_label_time, &free_mono_bold_48, 0);
	lv_obj_set_style_text_align(g_label_time, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(g_label_time, g_pal->text, 0);

	g_label_date = lv_label_create(g_box_time);
	lv_label_set_text(g_label_date, g_ui_date_str);
	lv_obj_set_width(g_label_date, LV_PCT(100));
	lv_label_set_long_mode(g_label_date, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_font(g_label_date, &free_mono_bold_24, 0);
	lv_obj_set_style_text_align(g_label_date, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(g_label_date, g_pal->text, 0);

	/* Navigation items. */
	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		g_nav_items[i] = make_nav_item(g_nav, tab_icons[i], tab_titles[i], i);
	}

	/* Main content area (fills remaining width). */
	lv_obj_t *pages = lv_obj_create(g_root);

	lv_obj_remove_style_all(pages);
	lv_obj_set_flex_grow(pages, 1);
	lv_obj_set_height(pages, LV_PCT(100));

	/* Tab pages (overlaid, only the active one is visible). */
	g_pages[TAB_OVERVIEW]  = ui_tab_overview_build(pages);
	g_pages[TAB_PAGER]     = build_simple_page(pages, "Pager");
	g_pages[TAB_LIGHT]     = ui_tab_dmx_create(pages);
	g_pages[TAB_ALARM]     = build_simple_page(pages, "Alarmanlage");
	g_pages[TAB_SETTINGS]  = build_settings_tab_page(pages);

	/* Page builders create visible objects; keep only the selected page on top. */
	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		lv_obj_set_hidden(g_pages[i], i != g_active_tab);
	}
	for (uint8_t i = 0; i < TAB_COUNT; i++) {
		if (g_nav_items[i] != NULL) {
			if (i == g_active_tab) {
				lv_obj_add_style(g_nav_items[i], &st_nav_active, 0);
			} else {
				lv_obj_remove_style(g_nav_items[i], &st_nav_active, 0);
			}
		}
	}

	nav_set_collapsed(g_nav_collapsed);

	/* 1-second timer for clock and overview refresh. */
	if (g_timer_1s) {
		lv_timer_del(g_timer_1s);
	}
	g_timer_1s = lv_timer_create(ui_timer_cb, 1000, NULL);
	g_ui_built = true;

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
