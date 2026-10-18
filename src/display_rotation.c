/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software rotation for the landscape-mounted portrait panel. See the
 * header for details.
 */

#include "display_rotation.h"

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

LOG_MODULE_REGISTER(display_rot, LOG_LEVEL_INF);

static const struct device *g_display;
#define ROT_FRAMEBUFFER_COUNT 2
static uint16_t *g_fb[ROT_FRAMEBUFFER_COUNT]; /* physical portrait RGB565 frames */
static uint16_t g_phys_w;
static uint16_t g_phys_h;
static lv_display_rotation_t g_rotation;
static uint8_t g_draw_fb;
static int8_t g_front_fb = -1;
static uint32_t g_flush_total;
static uint32_t g_flushes_window;
static uint32_t g_writes_window;
static uint32_t g_successful_writes_window;
static uint32_t g_write_errors_window;
static uint32_t g_write_total;
static int64_t g_stats_start;
static bool g_frame_ready;

static void log_frame_samples(const uint16_t *fb)
{
	const uint16_t background = 0x0021; /* RGB565 for theme #000609 */
	uint16_t x0 = 0;
	uint16_t xm = g_phys_w / 2U;
	uint16_t xr = g_phys_w - 1U;
	uint16_t y0 = 0;
	uint16_t ym = g_phys_h / 2U;
	uint16_t yb = g_phys_h - 1U;
	uint32_t grid_total = 0;
	uint32_t grid_non_background = 0;
	uint16_t step_x = g_phys_w / 32U;
	uint16_t step_y = g_phys_h / 32U;
	if (step_x == 0) {
		step_x = 1;
	}
	if (step_y == 0) {
		step_y = 1;
	}

	for (uint16_t y = step_y / 2U; y < g_phys_h; y += step_y) {
		for (uint16_t x = step_x / 2U; x < g_phys_w; x += step_x) {
			grid_total++;
			if (fb[y * g_phys_w + x] != background) {
				grid_non_background++;
			}
		}
	}

	LOG_INF("RGB565 samples TL=%04x T=%04x TR=%04x L=%04x C=%04x R=%04x BL=%04x B=%04x BR=%04x",
		fb[y0 * g_phys_w + x0], fb[y0 * g_phys_w + xm],
		fb[y0 * g_phys_w + xr], fb[ym * g_phys_w + x0],
		fb[ym * g_phys_w + xm], fb[ym * g_phys_w + xr],
		fb[yb * g_phys_w + x0], fb[yb * g_phys_w + xm],
		fb[yb * g_phys_w + xr]);
	LOG_INF("frame content grid: %u/%u samples differ from theme background RGB565=%04x",
		grid_non_background, grid_total, background);
}

static void rot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	const uint16_t *px = (const uint16_t *)px_map;
	uint16_t w = (uint16_t)lv_area_get_width(area);
	uint16_t h = (uint16_t)lv_area_get_height(area);
	uint16_t x = (uint16_t)area->x1;
	uint16_t y = (uint16_t)area->y1;
	uint32_t flush_no = ++g_flush_total;
	uint16_t *draw_fb = g_fb[g_draw_fb];
	int ret = 0;

	if (flush_no == 1) {
		LOG_INF("first LVGL flush: area=(%d,%d)-(%d,%d), px_map=%p, %ux%u",
			area->x1, area->y1, area->x2, area->y2, px_map, w, h);
	}
	if (area->x1 < 0 || area->y1 < 0 || area->x2 >=
		    lv_display_get_horizontal_resolution(disp) ||
	    area->y2 >= lv_display_get_vertical_resolution(disp)) {
		LOG_ERR("invalid LVGL flush area: (%d,%d)-(%d,%d), logical=%ux%u",
			area->x1, area->y1, area->x2, area->y2,
			lv_display_get_horizontal_resolution(disp),
			lv_display_get_vertical_resolution(disp));
		lv_display_flush_ready(disp);
		return;
	}

	for (uint16_t dy = 0; dy < h; dy++) {
		for (uint16_t dx = 0; dx < w; dx++) {
			uint32_t fx;
			uint32_t fy;

			if (g_rotation == LV_DISPLAY_ROTATION_90) {
				fx = y + dy;
				fy = (uint32_t)(g_phys_h - 1U) - (x + dx);
			} else if (g_rotation == LV_DISPLAY_ROTATION_270) {
				fx = (uint32_t)(g_phys_w - 1U) - (y + dy);
				fy = x + dx;
			} else {
				/* Only 90/270 are supported here. */
				continue;
			}
			draw_fb[fy * g_phys_w + fx] = px[(uint32_t)dy * w + dx];
		}
	}

	if (lv_display_flush_is_last(disp)) {
		struct display_buffer_descriptor desc = {
			.buf_size = (size_t)g_phys_w * g_phys_h * 2U,
			.width = g_phys_w,
			.pitch = g_phys_w,
			.height = g_phys_h,
		};

		sys_cache_data_flush_range(draw_fb, (size_t)g_phys_w * g_phys_h * 2U);
		ret = display_write(g_display, 0, 0, &desc, draw_fb);
		g_write_total++;
		g_writes_window++;
		if (ret < 0) {
			g_write_errors_window++;
			LOG_ERR("display_write #%u failed: %d, framebuffer=%p %ux%u pitch=%u",
				flush_no, ret, draw_fb, desc.width, desc.height, desc.pitch);
		} else {
			g_successful_writes_window++;
			g_frame_ready = true;
			int8_t old_front = g_front_fb;
			g_front_fb = (int8_t)g_draw_fb;
			g_draw_fb = old_front >= 0 ? (uint8_t)old_front
							 : (uint8_t)(g_front_fb ^ 1);
			/* Seed the now-inactive buffer so later partial LVGL updates
			 * never modify the frame currently being scanned out.
			 */
			memcpy(g_fb[g_draw_fb], draw_fb,
			       (size_t)g_phys_w * g_phys_h * sizeof(*draw_fb));
			sys_cache_data_flush_range(g_fb[g_draw_fb],
					  (size_t)g_phys_w * g_phys_h * sizeof(*draw_fb));
			if (g_write_total == 1) {
				LOG_INF("first display_write succeeded: %ux%u, %zu bytes",
					desc.width, desc.height, desc.buf_size);
				log_frame_samples(draw_fb);
			}
		}
		if (k_uptime_get() - g_stats_start >= 60000) {
			LOG_INF("display output health: %u LVGL flushes/min, %u display_write calls/min, %u succeeded, %u failed (total writes=%u)",
				g_flushes_window, g_writes_window,
				g_successful_writes_window, g_write_errors_window,
				g_write_total);
			if (g_write_total > 0 && g_front_fb >= 0) {
				log_frame_samples(g_fb[(uint8_t)g_front_fb]);
			}
			g_flushes_window = 0;
			g_writes_window = 0;
			g_successful_writes_window = 0;
			g_write_errors_window = 0;
			g_stats_start = k_uptime_get();
		}
	}
	g_flushes_window++;

	lv_display_flush_ready(disp);
}

bool display_rotation_frame_ready(void)
{
	return g_frame_ready;
}

int display_rotation_init(const struct device *display, lv_display_t *lv_disp,
			  lv_display_rotation_t rotation)
{
	struct display_capabilities caps;
	size_t fb_size;

	if (rotation != LV_DISPLAY_ROTATION_90 && rotation != LV_DISPLAY_ROTATION_270) {
		LOG_ERR("unsupported rotation %d", rotation);
		return -EINVAL;
	}

	display_get_capabilities(display, &caps);
	LOG_INF("display capabilities: ready=%d resolution=%ux%u, rotation=%d",
		device_is_ready(display), caps.x_resolution, caps.y_resolution, rotation);
	g_phys_w = (uint16_t)caps.x_resolution;
	g_phys_h = (uint16_t)caps.y_resolution;
	g_rotation = rotation;
	g_display = display;

	fb_size = (size_t)g_phys_w * g_phys_h * 2U;
	for (uint8_t i = 0; i < ROT_FRAMEBUFFER_COUNT; i++) {
		g_fb[i] = shared_multi_heap_aligned_alloc(
			SMH_REG_ATTR_EXTERNAL, CONFIG_ESP32_CACHE_L2_LINE_SIZE, fb_size);
		if (g_fb[i] == NULL) {
			LOG_ERR("failed to allocate portrait framebuffer %u (%zu bytes)", i,
				fb_size);
			while (i > 0) {
				shared_multi_heap_free(g_fb[--i]);
				g_fb[i] = NULL;
			}
			return -ENOMEM;
		}
		memset(g_fb[i], 0, fb_size);
		sys_cache_data_flush_range(g_fb[i], fb_size);
	}
	g_draw_fb = 0;
	g_front_fb = -1;
	LOG_INF("allocated %u ping-pong display framebuffers: %zu bytes each at %p, %p (external RAM)",
		ROT_FRAMEBUFFER_COUNT, fb_size, g_fb[0], g_fb[1]);
	g_stats_start = k_uptime_get();

	lv_display_set_flush_cb(lv_disp, rot_flush_cb);
	LOG_INF("installed LVGL rotation flush callback: logical=%ux%u physical=%ux%u",
		lv_display_get_horizontal_resolution(lv_disp),
		lv_display_get_vertical_resolution(lv_disp), g_phys_w, g_phys_h);

	LOG_INF("software rotation %d enabled: %ux%u portrait framebuffer @ %p",
		rotation, g_phys_w, g_phys_h, (void *)g_fb);

	return 0;
}
