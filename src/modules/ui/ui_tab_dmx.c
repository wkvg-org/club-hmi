/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX tab — colour picker, dimmer, blackout/home, pan/tilt, fixture.
 *
 * Layout:
 *   ┌───────────────────────────────┐
 *   │  [MH 1]  [MH 2]   (selector) │
 *   ├──────────┬───┬───────────────┤
 *   │ Pan Arc  │Tlt│ Color Picker  │
 *   │ (circle) │ ↑ │ W R O Y       │
 *   │          │   │ G C B P       │
 *   ├──────────┴───┴───────────────┤
 *   │ Dim/Str  [BLK] [Home] [Fix]  │
 *   └──────────────────────────────┘
 */

#include "ui_tab_dmx.h"
#include "../net/dmx_zbus.h"

#include <stdio.h>
#include <stdlib.h>

#include <lvgl.h>
#include <ix_lvgl/theme.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ui_dmx, LOG_LEVEL_INF);

#define MH_ADDR_1     1
#define MH_ADDR_2     10
#define FIXTURE_ADDR  19

static uint16_t g_pan_deg  = 270;
static uint8_t  g_tilt_deg = 100;
static uint8_t  g_mh_addr  = MH_ADDR_1;
static uint8_t  g_color = 5, g_dimmer = 100, g_strobe;
static bool     g_fixture_on;

static lv_obj_t *g_pantilt_label;
static lv_obj_t *g_color_label;
static lv_obj_t *g_fixture_btn;
static lv_obj_t *g_mh1_btn;
static lv_obj_t *g_mh2_btn;
static const struct lvgl_ix_palette *g_pal;

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
	struct dmx_cmd cmd = { .type = type, .addr = g_mh_addr, .val = val };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
}

static void send_pantilt_cmd(void)
{
	struct dmx_cmd cmd = { .type = DMX_CMD_PANTILT, .addr = g_mh_addr,
			       .val = g_pan_deg, .extra = g_tilt_deg };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
}

static void update_pantilt_label(void)
{
	char buf[40];
	snprintf(buf, sizeof(buf), "MH%u Pan:%u Tilt:%u",
		 g_mh_addr == MH_ADDR_1 ? 1 : 2, g_pan_deg, g_tilt_deg);
	lv_label_set_text(g_pantilt_label, buf);
}

static void update_mh_selector_style(void)
{
	lv_color_t sel = g_pal->accent;
	lv_color_t unsel = g_pal->surface_alt;

	lv_obj_set_style_bg_color(g_mh1_btn, g_mh_addr == MH_ADDR_1 ? sel : unsel, 0);
	lv_obj_set_style_bg_color(g_mh2_btn, g_mh_addr == MH_ADDR_2 ? sel : unsel, 0);
}

/* ── Event handlers ─────────────────────────────────────────────────── */

static void on_blackout(lv_event_t *e)
{
	ARG_UNUSED(e);
	dmx_pub(DMX_CMD_BLACKOUT, 0);
	LOG_INF("blackout MH%u", g_mh_addr == MH_ADDR_1 ? 1 : 2);
}

static void on_home(lv_event_t *e)
{
	ARG_UNUSED(e);
	g_pan_deg = 270; g_tilt_deg = 100;
	send_pantilt_cmd();
	update_pantilt_label();
	LOG_INF("home MH%u", g_mh_addr == MH_ADDR_1 ? 1 : 2);
}

static void on_fixture_toggle(lv_event_t *e)
{
	ARG_UNUSED(e);
	g_fixture_on = !g_fixture_on;

	struct dmx_cmd cmd = {
		.type  = DMX_CMD_FIXTURE,
		.addr  = FIXTURE_ADDR,
		.extra = g_fixture_on ? 255 : 0,
	};
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);

	lv_obj_set_style_bg_color(g_fixture_btn,
				  g_fixture_on ? g_pal->success : g_pal->surface_alt, 0);
	LOG_INF("fixture %s (addr %u)", g_fixture_on ? "on" : "off", FIXTURE_ADDR);
}

static void on_mh1(lv_event_t *e)
{
	ARG_UNUSED(e);
	g_mh_addr = MH_ADDR_1;
	update_mh_selector_style();
	update_pantilt_label();
	LOG_INF("select MH1");
}

static void on_mh2(lv_event_t *e)
{
	ARG_UNUSED(e);
	g_mh_addr = MH_ADDR_2;
	update_mh_selector_style();
	update_pantilt_label();
	LOG_INF("select MH2");
}

static void on_pan_change(lv_event_t *e)
{
	g_pan_deg = (uint16_t)lv_arc_get_value(lv_event_get_target(e));
	send_pantilt_cmd();
	update_pantilt_label();
}

static void on_tilt_change(lv_event_t *e)
{
	g_tilt_deg = 200 - (uint8_t)lv_slider_get_value(lv_event_get_target(e));
	send_pantilt_cmd();
	update_pantilt_label();
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
			      lv_color_t color, int32_t min, int32_t max, int32_t val,
			      lv_event_cb_t cb, void *user_data)
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
	lv_slider_set_range(sl, min, max);
	lv_slider_set_value(sl, val, LV_ANIM_OFF);
	lv_obj_set_flex_grow(sl, 1);
	lv_obj_set_height(sl, 24);
	lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, user_data);

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
		lv_obj_set_style_text_color(lbl, (i == 0 || i == 3 || i == 5) ? lv_color_black() : lv_color_white(), 0);
		lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
		lv_obj_center(lbl);

		lv_obj_add_event_cb(btn, on_color_btn, LV_EVENT_CLICKED,
				    (void *)&color_presets[i]);
	}

	make_slider(col, "Dim", g_pal->text, 0, 255, 0, on_dmx_slider, &ctx_dimmer);
	make_slider(col, "Str", g_pal->text_secondary, 0, 255, 0, on_dmx_slider, &ctx_strobe);
	lv_obj_set_style_margin_top(lv_obj_get_child(col, lv_obj_get_child_count(col) - 1), 8, 0);
}

/* ── Pan arc + Tilt slider ──────────────────────────────────────────── */

static void build_pan_arc(lv_obj_t *parent)
{
	lv_obj_t *arc = lv_arc_create(parent);
	lv_obj_set_size(arc, 180, 180);
	lv_obj_add_flag(arc, LV_OBJ_FLAG_CLICKABLE);
	lv_arc_set_range(arc, 0, 360);
	lv_arc_set_value(arc, g_pan_deg);
	lv_arc_set_rotation(arc, 135);
	lv_arc_set_bg_angles(arc, 0, 270);
	lv_obj_set_style_arc_color(arc, g_pal->accent, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(arc, g_pal->border_subtle, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(arc, g_pal->surface, LV_PART_KNOB);
	lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(arc, 12, LV_PART_KNOB);
	lv_obj_add_event_cb(arc, on_pan_change, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(arc, on_pan_change, LV_EVENT_PRESSING, NULL);
}

static void build_tilt_slider(lv_obj_t *parent)
{
	lv_obj_t *sl = lv_slider_create(parent);
	lv_obj_set_size(sl, 24, LV_PCT(100));
	lv_slider_set_range(sl, 0, 200);
	lv_slider_set_value(sl, 200 - g_tilt_deg, LV_ANIM_OFF);
	/* Vertical slider: thin track across the width, not the height. */
	lv_obj_set_style_pad_hor(sl, 10, LV_PART_MAIN);
	lv_obj_set_style_pad_ver(sl, 0, LV_PART_MAIN);
	lv_obj_add_event_cb(sl, on_tilt_change, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── Moving-head selector ───────────────────────────────────────────── */

static void build_mh_selector(lv_obj_t *parent)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 2, 0);
	lv_obj_set_layout(row, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	lv_obj_t *lbl = lv_label_create(row);
	lv_label_set_text(lbl, "Head:");
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

	g_mh1_btn = lv_btn_create(row);
	lv_obj_set_size(g_mh1_btn, 60, 36);
	lv_obj_set_style_radius(g_mh1_btn, 6, 0);
	lv_obj_t *m1 = lv_label_create(g_mh1_btn);
	lv_label_set_text(m1, "MH 1");
	lv_obj_set_style_text_color(m1, lv_color_white(), 0);
	lv_obj_set_style_text_font(m1, &lv_font_montserrat_16, 0);
	lv_obj_center(m1);
	lv_obj_add_event_cb(g_mh1_btn, on_mh1, LV_EVENT_RELEASED, NULL);

	g_mh2_btn = lv_btn_create(row);
	lv_obj_set_size(g_mh2_btn, 60, 36);
	lv_obj_set_style_radius(g_mh2_btn, 6, 0);
	lv_obj_t *m2 = lv_label_create(g_mh2_btn);
	lv_label_set_text(m2, "MH 2");
	lv_obj_set_style_text_color(m2, lv_color_white(), 0);
	lv_obj_set_style_text_font(m2, &lv_font_montserrat_16, 0);
	lv_obj_center(m2);
	lv_obj_add_event_cb(g_mh2_btn, on_mh2, LV_EVENT_RELEASED, NULL);

	update_mh_selector_style();
}

/* ── Build the tab ──────────────────────────────────────────────────── */

lv_obj_t *ui_tab_dmx_create(lv_obj_t *parent)
{
	g_pal = lvgl_ix_palette_get(LVGL_IX_COLOR_SCHEME_DARK);

	lv_obj_t *page = lv_obj_create(parent);
	lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 4, 0);
	lv_obj_set_layout(page, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

	build_mh_selector(page);

	lv_obj_t *top = lv_obj_create(page);
	lv_obj_set_size(top, LV_PCT(100), LV_PCT(65));
	lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_set_layout(top, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);

	/* Left column */
	lv_obj_t *lcol = lv_obj_create(top);
	lv_obj_set_flex_grow(lcol, 1);
	lv_obj_set_height(lcol, LV_PCT(100));
	lv_obj_set_style_bg_opa(lcol, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(lcol, 0, 0);
	lv_obj_set_style_pad_all(lcol, 0, 0);
	lv_obj_set_layout(lcol, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(lcol, LV_FLEX_FLOW_COLUMN);

	/* Pan arc + vertical tilt slider side-by-side */
	lv_obj_t *pan_tilt_row = lv_obj_create(lcol);
	lv_obj_set_size(pan_tilt_row, LV_PCT(100), LV_PCT(85));
	lv_obj_set_style_bg_opa(pan_tilt_row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(pan_tilt_row, 0, 0);
	lv_obj_set_style_pad_all(pan_tilt_row, 0, 0);
	lv_obj_set_layout(pan_tilt_row, LV_LAYOUT_FLEX);
	lv_obj_set_flex_flow(pan_tilt_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollbar_mode(pan_tilt_row, LV_SCROLLBAR_MODE_OFF);

	lv_obj_t *arc_wrap = lv_obj_create(pan_tilt_row);
	lv_obj_set_flex_grow(arc_wrap, 3);
	lv_obj_set_height(arc_wrap, LV_PCT(100));
	lv_obj_set_style_bg_opa(arc_wrap, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(arc_wrap, 0, 0);
	lv_obj_set_style_pad_all(arc_wrap, 8, 0);
	lv_obj_clear_flag(arc_wrap, LV_OBJ_FLAG_SCROLLABLE);
	build_pan_arc(arc_wrap);

	lv_obj_t *tilt_wrap = lv_obj_create(pan_tilt_row);
	lv_obj_set_flex_grow(tilt_wrap, 1);
	lv_obj_set_height(tilt_wrap, LV_PCT(100));
	lv_obj_set_style_bg_opa(tilt_wrap, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tilt_wrap, 0, 0);
	lv_obj_set_style_pad_all(tilt_wrap, 8, 0);
	lv_obj_clear_flag(tilt_wrap, LV_OBJ_FLAG_SCROLLABLE);
	build_tilt_slider(tilt_wrap);

	g_pantilt_label = lv_label_create(lcol);
	lv_label_set_text(g_pantilt_label, "MH1 Pan:270 Tilt:100");
	lv_obj_set_style_text_color(g_pantilt_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(g_pantilt_label, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_align(g_pantilt_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(g_pantilt_label, LV_PCT(100));

	/* Right column: color mixer */
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
	lvgl_ix_style_danger_button(blk);
	lv_obj_set_style_radius(blk, 6, 0);
	lv_obj_t *bl = lv_label_create(blk);
	lv_label_set_text(bl, "BLK");
	lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
	lv_obj_add_event_cb(blk, on_blackout, LV_EVENT_RELEASED, NULL);

	lv_obj_t *hm = lv_btn_create(btns);
	lvgl_ix_style_secondary_button(hm);
	lv_obj_set_style_radius(hm, 6, 0);
	lv_obj_t *hl = lv_label_create(hm);
	lv_label_set_text(hl, "Home");
	lv_obj_set_style_text_color(hl, lv_color_white(), 0);
	lv_obj_set_style_text_font(hl, &lv_font_montserrat_16, 0);
	lv_obj_add_event_cb(hm, on_home, LV_EVENT_RELEASED, NULL);

	g_fixture_btn = lv_btn_create(btns);
	lv_obj_set_style_bg_color(g_fixture_btn, g_pal->surface_alt, 0);
	lv_obj_set_style_radius(g_fixture_btn, 6, 0);
	lv_obj_t *fl = lv_label_create(g_fixture_btn);
	lv_label_set_text(fl, "Zephyr Fixture");
	lv_obj_set_style_text_color(fl, lv_color_white(), 0);
	lv_obj_set_style_text_font(fl, &lv_font_montserrat_16, 0);
	lv_obj_add_event_cb(g_fixture_btn, on_fixture_toggle, LV_EVENT_RELEASED, NULL);

	return page;
}
