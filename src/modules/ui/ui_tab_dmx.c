/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX tab — colour picker, dimmer, blackout/home, joystick, DMX address.
 *
 * Layout:
 *   ┌─────────────┬─────────────┐
 *   │  Joystick   │ Color Picker│
 *   │  Pan/Tilt   │ W R O Y     │
 *   │             │ G C B P     │
 *   ├─────────────┴─────────────┤
 *   │ Dim: [==slider==] [BLK] [Home] │
 *   │ Addr: [-] 1 [+]              │
 *   └──────────────────────────────┘
 */

#include "ui_tab_dmx.h"
#include "../net/color_mqtt.h"
#include "../net/dmx_zbus.h"

#include <stdio.h>
#include <stdlib.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ui_dmx, LOG_LEVEL_INF);

#define JOY_THROTTLE_MS 50

static uint16_t g_pan_deg  = 270;
static uint8_t  g_tilt_deg = 100;
static uint16_t g_dmx_addr = 1;
static uint8_t  g_color = 5, g_dimmer = 100, g_strobe;

static lv_obj_t *g_joy_thumb;
static lv_obj_t *g_joy_label;
static lv_obj_t *g_addr_label;
static lv_obj_t *g_color_label;
static int64_t  g_joy_last_ms;

struct color_preset {
	const char *name;
	uint8_t     dmx_val;
	uint32_t    rgb888;
};

static const struct color_preset color_presets[] = {
	{"White",    5, 0xFFFFFF},
	{"Red",     16, 0xFF0000},
	{"Orange",  27, 0xFF8000},
	{"Yellow",  38, 0xFFFF00},
	{"Green",   49, 0x00FF00},
	{"Cyan",    71, 0x00FFFF},
	{"Blue",    60, 0x0000FF},
	{"Purple",  82, 0x8000FF},
};
#define N_COLORS (sizeof(color_presets) / sizeof(color_presets[0]))

static void dmx_pub(uint8_t type, uint16_t val)
{
	struct dmx_cmd cmd = { .type = type, .val = val };
	k_msgq_put(&dmx_msgq, &cmd, K_NO_WAIT);
}

/* ── Event handlers ─────────────────────────────────────────────────── */

static void on_blackout(lv_event_t *e)
{
	ARG_UNUSED(e);
	dmx_pub(DMX_CMD_BLACKOUT, 0);
	LOG_INF("blackout");
}

static void on_home(lv_event_t *e)
{
	ARG_UNUSED(e);
	g_pan_deg = 270; g_tilt_deg = 100;
	dmx_pub(DMX_CMD_HOME, 0);
	lv_obj_set_pos(g_joy_thumb, 0, 0);
	LOG_INF("home");
}

/* ── DMX controls ──────────────────────────────────────────────────── */

struct slider_ctx {
	uint8_t type;
	uint8_t *val;
};

static struct slider_ctx ctx_dimmer = {DMX_CMD_DIMMER, &g_dimmer};
static struct slider_ctx ctx_strobe = {DMX_CMD_STROBE, &g_strobe};

static void on_dmx_slider(lv_event_t *e)
{
	struct slider_ctx *ctx = lv_event_get_user_data(e);
	*ctx->val = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
	dmx_pub(ctx->type, *ctx->val);

	char buf[16];
	if (ctx->type == DMX_CMD_DIMMER) snprintf(buf, sizeof(buf), "Dim:%u", g_dimmer);
	else                            snprintf(buf, sizeof(buf), "Str:%u", g_strobe);
	lv_label_set_text(g_color_label, buf);
}

static void on_color_btn(lv_event_t *e)
{
	const struct color_preset *cp = lv_event_get_user_data(e);
	if (!cp) return;
	g_color = cp->dmx_val;
	dmx_pub(DMX_CMD_COLOR, cp->dmx_val);

	char buf[16];
	snprintf(buf, sizeof(buf), "C:%u", cp->dmx_val);
	lv_label_set_text(g_color_label, buf);
}

static lv_obj_t *make_slider(lv_obj_t *parent, const char *name,
			      lv_color_t color, uint8_t val, struct slider_ctx *ctx)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, LV_PCT(100), 40);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 2, 0);
	lv_obj_set_layout(row, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *lbl = lv_label_create(row);
	lv_label_set_text(lbl, name);
	lv_obj_set_style_text_color(lbl, color, 0);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
	lv_obj_set_width(lbl, 64);

	lv_obj_t *sl = lv_slider_create(row);
	lv_slider_set_range(sl, 0, 255);
	lv_slider_set_value(sl, val, LV_ANIM_OFF);
	lv_obj_set_flex_grow(sl, 1);
	lv_obj_add_event_cb(sl, on_dmx_slider, LV_EVENT_VALUE_CHANGED, ctx);

	return row;
}

static void build_color_mixer(lv_obj_t *parent)
{
	lv_obj_t *col = lv_obj_create(parent);
	lv_obj_set_size(col, LV_PCT(90), LV_PCT(85));
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(col, 0, 0);
	lv_obj_set_style_pad_all(col, 4, 0);
	lv_obj_set_style_pad_row(col, 12, 0);
	lv_obj_set_layout(col, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

	/* Color button grid */
	lv_obj_t *grid = lv_obj_create(col);
	lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, 0, 0);
	lv_obj_set_style_pad_gap(grid, 4, 0);
	lv_obj_set_style_margin_bottom(grid, 8, 0);
	lv_obj_set_layout(grid, LV_LAYOUT_GRID);
	lv_obj_set_flex_grow(grid, 1);

	static lv_coord_t cd[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
	static lv_coord_t rd[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
				  LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
	lv_obj_set_grid_dsc_array(grid, cd, rd);

	for (int i = 0; i < N_COLORS; i++) {
		lv_obj_t *btn = lv_btn_create(grid);
		lv_obj_set_style_bg_color(btn, lv_color_hex(color_presets[i].rgb888), 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(btn, 10, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);
		lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 2, 1,
				     LV_GRID_ALIGN_STRETCH, i / 2, 1);

		lv_obj_t *lbl = lv_label_create(btn);
		lv_label_set_text(lbl, color_presets[i].name);
		lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
		lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
		lv_obj_center(lbl);

		lv_obj_add_event_cb(btn, on_color_btn, LV_EVENT_CLICKED,
				    (void *)&color_presets[i]);
	}

	make_slider(col, "Dim", lv_color_hex(0xFFFFFF), 0, &ctx_dimmer);
	make_slider(col, "Str", lv_color_hex(0x888888), 0,   &ctx_strobe);
	lv_obj_set_style_margin_top(lv_obj_get_child(col, lv_obj_get_child_count(col) - 1), 8, 0);
}

/* ── Joystick ───────────────────────────────────────────────────────── */

static void update_joy_label(void)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "Pan:%u Tilt:%u", g_pan_deg, g_tilt_deg);
	lv_label_set_text(g_joy_label, buf);
}

static void send_joy_cmd(void)
{
	int64_t now = k_uptime_get();
	if (now - g_joy_last_ms < JOY_THROTTLE_MS) return;
	g_joy_last_ms = now;

	struct dmx_cmd cmd = { .type = DMX_CMD_PANTILT, .val = g_pan_deg, .extra = g_tilt_deg };
	k_msgq_put(&dmx_msgq, &cmd, K_NO_WAIT);
}

static void on_joy_pressing(lv_event_t *e)
{
	static bool in_handler;
	if (in_handler) return;
	in_handler = true;

	lv_obj_t *joy = lv_event_get_target(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	lv_area_t a;
	lv_obj_get_coords(joy, &a);
	lv_coord_t rx = p.x - a.x1, ry = p.y - a.y1;
	lv_coord_t jw = lv_obj_get_content_width(joy);
	lv_coord_t jh = lv_obj_get_content_height(joy);
	lv_coord_t tw = lv_obj_get_width(g_joy_thumb);
	lv_coord_t th = lv_obj_get_height(g_joy_thumb);
	lv_coord_t rw = (jw > tw) ? (jw - tw) : 1;
	lv_coord_t rh = (jh > th) ? (jh - th) : 1;

	if (rx < 0) rx = 0; else if (rx > rw) rx = rw;
	if (ry < 0) ry = 0; else if (ry > rh) ry = rh;

	lv_obj_set_pos(g_joy_thumb, rx, ry);
	g_pan_deg  = 540u - (uint16_t)((uint32_t)rx * 540u / (uint32_t)rw);
	g_tilt_deg = (uint8_t)((uint32_t)ry * 200u / (uint32_t)rh);
	send_joy_cmd();
	update_joy_label();

	in_handler = false;
}

static void build_joystick(lv_obj_t *parent)
{
	lv_obj_t *joy = lv_obj_create(parent);
	lv_obj_set_size(joy, LV_PCT(100), LV_PCT(75));
	lv_obj_set_style_bg_color(joy, lv_color_hex(0x222222), 0);
	lv_obj_set_style_bg_opa(joy, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(joy, lv_color_hex(0x666666), 0);
	lv_obj_set_style_border_width(joy, 2, 0);
	lv_obj_set_style_radius(joy, 8, 0);
	lv_obj_set_scrollbar_mode(joy, LV_SCROLLBAR_MODE_OFF);

	g_joy_thumb = lv_obj_create(joy);
	lv_obj_set_size(g_joy_thumb, 40, 40);
	lv_obj_set_style_bg_color(g_joy_thumb, lv_color_hex(0x0099ff), 0);
	lv_obj_set_style_bg_opa(g_joy_thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(g_joy_thumb, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(g_joy_thumb, 0, 0);

	lv_obj_add_event_cb(joy, on_joy_pressing, LV_EVENT_PRESSING, NULL);
}

/* ── Address selector ───────────────────────────────────────────────── */

static void update_addr_label(void)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%u", g_dmx_addr);
	lv_label_set_text(g_addr_label, buf);
}

static void on_addr_up(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (g_dmx_addr < 504) {
		g_dmx_addr++;
		update_addr_label();
		color_mqtt_set_addr(g_dmx_addr);
		LOG_INF("dmx addr %u", g_dmx_addr);
	}
}

static void on_addr_down(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (g_dmx_addr > 1) {
		g_dmx_addr--;
		update_addr_label();
		color_mqtt_set_addr(g_dmx_addr);
		LOG_INF("dmx addr %u", g_dmx_addr);
	}
}

static void build_addr_selector(lv_obj_t *parent)
{
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_size(c, LV_PCT(100), 48);
	lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_pad_all(c, 2, 0);
	lv_obj_set_layout(c, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *lbl = lv_label_create(c);
	lv_label_set_text(lbl, "Addr:");
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

	lv_obj_t *down = lv_btn_create(c);
	lv_obj_set_size(down, 36, 36);
	lv_obj_set_style_bg_color(down, lv_color_hex(0x444444), 0);
	lv_obj_set_style_radius(down, 6, 0);
	lv_obj_t *dl = lv_label_create(down);
	lv_label_set_text(dl, "-");
	lv_obj_set_style_text_color(dl, lv_color_white(), 0);
	lv_obj_set_style_text_font(dl, &lv_font_montserrat_24, 0);
	lv_obj_center(dl);
	lv_obj_add_event_cb(down, on_addr_down, LV_EVENT_CLICKED, NULL);

	g_addr_label = lv_label_create(c);
	lv_label_set_text(g_addr_label, "1");
	lv_obj_set_style_text_color(g_addr_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_addr_label, &lv_font_montserrat_24, 0);
	lv_obj_set_width(g_addr_label, 60);
	lv_obj_set_style_text_align(g_addr_label, LV_TEXT_ALIGN_CENTER, 0);

	lv_obj_t *up = lv_btn_create(c);
	lv_obj_set_size(up, 36, 36);
	lv_obj_set_style_bg_color(up, lv_color_hex(0x444444), 0);
	lv_obj_set_style_radius(up, 6, 0);
	lv_obj_t *ul = lv_label_create(up);
	lv_label_set_text(ul, "+");
	lv_obj_set_style_text_color(ul, lv_color_white(), 0);
	lv_obj_set_style_text_font(ul, &lv_font_montserrat_24, 0);
	lv_obj_center(ul);
	lv_obj_add_event_cb(up, on_addr_up, LV_EVENT_CLICKED, NULL);
}

/* ── Build the tab ──────────────────────────────────────────────────── */

lv_obj_t *ui_tab_dmx_create(lv_obj_t *parent)
{
	lv_obj_t *page = lv_obj_create(parent);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 4, 0);
	lv_obj_set_layout(page, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *top = lv_obj_create(page);
	lv_obj_set_size(top, LV_PCT(100), LV_PCT(65));
	lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_set_layout(top, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);

	lv_obj_t *jcol = lv_obj_create(top);
	lv_obj_set_flex_grow(jcol, 1);
	lv_obj_set_height(jcol, LV_PCT(100));
	lv_obj_set_style_bg_opa(jcol, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(jcol, 0, 0);
	lv_obj_set_style_pad_all(jcol, 0, 0);
	lv_obj_set_layout(jcol, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(jcol, LV_FLEX_FLOW_COLUMN);
	build_joystick(jcol);

	g_joy_label = lv_label_create(jcol);
	lv_label_set_text(g_joy_label, "Pan:270 Tilt:100");
	lv_obj_set_style_text_color(g_joy_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_joy_label, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_align(g_joy_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(g_joy_label, LV_PCT(100));

	lv_obj_t *rcol = lv_obj_create(top);
	lv_obj_set_flex_grow(rcol, 1);
	lv_obj_set_height(rcol, LV_PCT(100));
	lv_obj_set_style_bg_opa(rcol, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(rcol, 0, 0);
	lv_obj_set_style_pad_all(rcol, 0, 0);
	lv_obj_set_layout(rcol, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(rcol, LV_FLEX_FLOW_COLUMN);

	build_color_mixer(rcol);

	g_color_label = lv_label_create(rcol);
	lv_label_set_text(g_color_label, "C:5");
	lv_obj_set_style_text_color(g_color_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_color_label, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_align(g_color_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(g_color_label, LV_PCT(100));

	lv_obj_t *btns = lv_obj_create(page);
	lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btns, 0, 0);
	lv_obj_set_style_pad_all(btns, 2, 0);
	lv_obj_set_layout(btns, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *blk = lv_btn_create(btns);
	lv_obj_set_style_bg_color(blk, (lv_color_t){50, 50, 50}, 0);
	lv_obj_set_style_radius(blk, 6, 0);
	lv_obj_t *bl = lv_label_create(blk);
	lv_label_set_text(bl, "BLK");
	lv_obj_set_style_text_color(bl, lv_color_white(), 0);
	lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
	lv_obj_add_event_cb(blk, on_blackout, LV_EVENT_CLICKED, NULL);

	lv_obj_t *hm = lv_btn_create(btns);
	lv_obj_set_style_bg_color(hm, (lv_color_t){80, 80, 80}, 0);
	lv_obj_set_style_radius(hm, 6, 0);
	lv_obj_t *hl = lv_label_create(hm);
	lv_label_set_text(hl, "Home");
	lv_obj_set_style_text_color(hl, lv_color_white(), 0);
	lv_obj_set_style_text_font(hl, &lv_font_montserrat_16, 0);
	lv_obj_add_event_cb(hm, on_home, LV_EVENT_CLICKED, NULL);

	build_addr_selector(page);

	return page;
}
