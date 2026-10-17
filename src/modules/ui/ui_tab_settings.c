/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings tab: LCD backlight brightness and network (DHCP / static IPv4 +
 * hostname) configuration. Values are persisted on the SD card.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <ix_lvgl/theme.h>

#include "../config/settings.h"
#include "../net/net_config.h"
#include "assets/oswald_regular_36.h"
#include "ui_tab_settings.h"

LOG_MODULE_REGISTER(ui_tab_settings, LOG_LEVEL_INF);

static lv_obj_t *g_backlight_label;

static lv_obj_t *g_kb;
static lv_obj_t *g_dhcp_sw;
static lv_obj_t *g_ta_hostname;
static lv_obj_t *g_ta_ip;
static lv_obj_t *g_ta_mask;
static lv_obj_t *g_ta_gw;
static lv_obj_t *g_net_status;
static lv_timer_t *g_dhcp_debounce;

/* Debounce period for the DHCP switch (touch bounce / rapid toggling). */
#define DHCP_DEBOUNCE_MS 150

/* ── Backlight ───────────────────────────────────────────────────────── */

static void backlight_slider_cb(lv_event_t *e)
{
	lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
	int percent = lv_slider_get_value(slider);
	char text[24];

	settings_set_backlight_percent((uint8_t)percent);

	snprintf(text, sizeof(text), "Hintergrund: %d%%", percent);
	lv_label_set_text(g_backlight_label, text);
}

/* ── Network: on-screen keyboard ────────────────────────────────────── */

static void open_keyboard(lv_obj_t *ta)
{
	if (g_kb == NULL || lv_obj_has_state(ta, LV_STATE_DISABLED)) {
		return;
	}

	lv_keyboard_set_mode(g_kb, ta == g_ta_hostname ? LV_KEYBOARD_MODE_TEXT_LOWER :
						    LV_KEYBOARD_MODE_NUMBER);
	lv_keyboard_set_textarea(g_kb, ta);
	lv_obj_set_hidden(g_kb, false);
	lv_obj_move_foreground(g_kb);
	LOG_INF("keyboard opened for %s", ta == g_ta_hostname ? "hostname" :
		 ta == g_ta_ip ? "ip" : ta == g_ta_mask ? "netmask" : "gateway");
}

void ui_tab_settings_hide_keyboard(void)
{
	if (g_kb != NULL) {
		lv_obj_set_hidden(g_kb, true);
		lv_keyboard_set_textarea(g_kb, NULL);
	}
}

static void kb_done_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	ui_tab_settings_hide_keyboard();
}

static void ta_open_cb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_current_target(e);

	open_keyboard(ta);
}

static void dhcp_update_visual(void)
{
	const struct lvgl_ix_palette *pal =
		lvgl_ix_palette_get(LVGL_IX_COLOR_SCHEME_DARK);
	bool dhcp = lv_obj_has_state(g_dhcp_sw, LV_STATE_CHECKED);
	lv_color_t tc = dhcp ? pal->text_disabled : pal->text;

	lv_obj_set_style_text_color(g_ta_ip, tc, 0);
	lv_obj_set_style_text_color(g_ta_mask, tc, 0);
	lv_obj_set_style_text_color(g_ta_gw, tc, 0);

	if (dhcp) {
		lv_obj_add_state(g_ta_ip, LV_STATE_DISABLED);
		lv_obj_add_state(g_ta_mask, LV_STATE_DISABLED);
		lv_obj_add_state(g_ta_gw, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(g_ta_ip, LV_STATE_DISABLED);
		lv_obj_remove_state(g_ta_mask, LV_STATE_DISABLED);
		lv_obj_remove_state(g_ta_gw, LV_STATE_DISABLED);
	}
}

static void dhcp_debounce_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	lv_timer_pause(g_dhcp_debounce);
	dhcp_update_visual();
}

static void dhcp_switch_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* Debounce: restart the one-shot timer; the visual update runs once
	 * the switch has settled (touch bounce / rapid toggling). */
	lv_timer_reset(g_dhcp_debounce);
	lv_timer_resume(g_dhcp_debounce);
}

static void apply_net_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	ui_tab_settings_hide_keyboard();
	bool dhcp = lv_obj_has_state(g_dhcp_sw, LV_STATE_CHECKED);
	const char *hostname = lv_textarea_get_text(g_ta_hostname);
	const char *ip = lv_textarea_get_text(g_ta_ip);
	const char *mask = lv_textarea_get_text(g_ta_mask);
	const char *gw = lv_textarea_get_text(g_ta_gw);
	int ret;

	ret = net_config_set_hostname(hostname);
	if (ret < 0) {
		lv_label_set_text(g_net_status, "Hostname ungueltig");
		return;
	}

	ret = net_config_set_ipv4(dhcp, ip, mask, gw);
	if (ret < 0) {
		lv_label_set_text(g_net_status, "IPv4-Adresse ungueltig");
		return;
	}

	lv_label_set_text(g_net_status, "Netzwerk gespeichert");
}

/* ── Build helpers ──────────────────────────────────────────────────── */

static lv_obj_t *make_input_row(lv_obj_t *parent, const char *label_text,
				lv_obj_t **ta_out, const char *accepted, uint32_t max_len)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_t *lbl;
	lv_obj_t *ta;

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 12, 0);

	lbl = lv_label_create(row);
	lv_label_set_text(lbl, label_text);
	lv_obj_set_width(lbl, 150);

	ta = lv_textarea_create(row);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_max_length(ta, max_len);
	if (accepted != NULL) {
		lv_textarea_set_accepted_chars(ta, accepted);
	}
	lv_obj_set_flex_grow(ta, 1);
	lv_obj_add_event_cb(ta, ta_open_cb, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(ta, ta_open_cb, LV_EVENT_PRESSED, NULL);

	*ta_out = ta;
	return row;
}

static lv_obj_t *make_section_title(lv_obj_t *parent, const char *txt)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, txt);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
	return label;
}

static void build_network_section(lv_obj_t *page)
{
	char buf[64];
	const char *hostname;
	const struct lvgl_ix_palette *pal =
		lvgl_ix_palette_get(LVGL_IX_COLOR_SCHEME_DARK);

	make_section_title(page, "Netzwerk");

	/* DHCP toggle */
	lv_obj_t *row = lv_obj_create(page);

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *lbl = lv_label_create(row);

	lv_label_set_text(lbl, "DHCP");
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

	g_dhcp_sw = lv_switch_create(row);
	lv_obj_set_size(g_dhcp_sw, 48, 24);
	if (net_config_uses_dhcp()) {
		lv_obj_add_state(g_dhcp_sw, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(g_dhcp_sw, dhcp_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

	/* One-shot debounce timer for the DHCP switch. */
	g_dhcp_debounce = lv_timer_create(dhcp_debounce_cb, DHCP_DEBOUNCE_MS, NULL);
	lv_timer_pause(g_dhcp_debounce);

	/* Hostname + static IPv4 fields */
	hostname = net_config_get_hostname();
	make_input_row(page, "Hostname", &g_ta_hostname, NULL, 63);
	lv_textarea_set_text(g_ta_hostname, hostname);

	net_config_get_ipv4(buf, sizeof(buf), NULL, 0, NULL, 0);
	make_input_row(page, "IP-Adresse", &g_ta_ip, "0123456789.", 15);
	lv_textarea_set_text(g_ta_ip, buf);

	net_config_get_ipv4(NULL, 0, buf, sizeof(buf), NULL, 0);
	make_input_row(page, "Netzmaske", &g_ta_mask, "0123456789.", 15);
	lv_textarea_set_text(g_ta_mask, buf);

	net_config_get_ipv4(NULL, 0, NULL, 0, buf, sizeof(buf));
	make_input_row(page, "Gateway", &g_ta_gw, "0123456789.", 15);
	lv_textarea_set_text(g_ta_gw, buf);

	/* Apply button + status */
	lv_obj_t *apply = lv_btn_create(page);
	lv_obj_t *albl = lv_label_create(apply);

	lv_label_set_text(albl, "Netzwerk anwenden");
	lv_obj_center(albl);
	lv_obj_set_width(apply, LV_PCT(100));
	lv_obj_add_event_cb(apply, apply_net_cb, LV_EVENT_CLICKED, NULL);

	g_net_status = lv_label_create(page);
	lv_label_set_text(g_net_status, "");
	lv_obj_set_style_text_color(g_net_status, pal->text_secondary, 0);
	lv_obj_set_style_text_font(g_net_status, &lv_font_montserrat_16, 0);
	lv_obj_set_width(g_net_status, LV_PCT(100));

	/* Initial enabled/disabled state of the IPv4 fields. */
	dhcp_update_visual();
}

/* ── Tab ────────────────────────────────────────────────────────────── */

lv_obj_t *build_settings_tab_page(lv_obj_t *parent)
{
	lv_obj_t *page = lv_obj_create(parent);

	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lvgl_ix_style_surface(page);
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
	lv_label_set_text_fmt(g_backlight_label, "Hintergrund: %u%%",
			      settings_get_backlight_percent());
	lv_obj_set_style_text_color(g_backlight_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_backlight_label, &lv_font_montserrat_24, 0);

	lv_obj_t *slider = lv_slider_create(page);

	lv_obj_set_width(slider, LV_PCT(100));
	lv_obj_set_height(slider, 24);
	lv_slider_set_range(slider, 0, 100);
	lv_slider_set_value(slider, settings_get_backlight_percent(), LV_ANIM_OFF);
	lv_obj_add_event_cb(slider, backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	build_network_section(page);

	/* The top layer stays above the page tree and uses display coordinates. */
	g_kb = lv_keyboard_create(lv_layer_top());
	lv_obj_set_size(g_kb, lv_display_get_horizontal_resolution(lv_display_get_default()),
			lv_display_get_vertical_resolution(lv_display_get_default()) * 35 / 100);
	lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	const struct lvgl_ix_palette *pal = lvgl_ix_palette_get(LVGL_IX_COLOR_SCHEME_DARK);

	lv_obj_set_style_bg_color(g_kb, pal->surface, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(g_kb, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(g_kb, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all(g_kb, 8, LV_PART_MAIN);
	lv_obj_set_style_pad_row(g_kb, 6, LV_PART_MAIN);
	lv_obj_set_style_pad_column(g_kb, 6, LV_PART_MAIN);
	lv_obj_set_style_bg_color(g_kb, pal->surface_alt, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(g_kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_text_color(g_kb, pal->text, LV_PART_ITEMS);
	lv_obj_set_style_radius(g_kb, 4, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(g_kb, pal->surface_active,
				  LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_hidden(g_kb, true);
	lv_obj_add_event_cb(g_kb, kb_done_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(g_kb, kb_done_cb, LV_EVENT_CANCEL, NULL);

	return page;
}
