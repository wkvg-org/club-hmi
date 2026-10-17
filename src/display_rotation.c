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
static uint16_t *g_fb; /* physical (portrait) framebuffer, RGB565 */
static uint16_t g_phys_w;
static uint16_t g_phys_h;
static lv_display_rotation_t g_rotation;

static void rot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	const uint16_t *px = (const uint16_t *)px_map;
	uint16_t w = (uint16_t)lv_area_get_width(area);
	uint16_t h = (uint16_t)lv_area_get_height(area);
	uint16_t x = (uint16_t)area->x1;
	uint16_t y = (uint16_t)area->y1;

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
			g_fb[fy * g_phys_w + fx] = px[(uint32_t)dy * w + dx];
		}
	}

	if (lv_display_flush_is_last(disp)) {
		struct display_buffer_descriptor desc = {
			.buf_size = (size_t)g_phys_w * g_phys_h * 2U,
			.width = g_phys_w,
			.pitch = g_phys_w,
			.height = g_phys_h,
		};

		sys_cache_data_flush_range(g_fb, (size_t)g_phys_w * g_phys_h * 2U);
		(void)display_write(g_display, 0, 0, &desc, g_fb);
	}

	lv_display_flush_ready(disp);
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
	g_phys_w = (uint16_t)caps.x_resolution;
	g_phys_h = (uint16_t)caps.y_resolution;
	g_rotation = rotation;
	g_display = display;

	fb_size = (size_t)g_phys_w * g_phys_h * 2U;
	g_fb = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,
					       CONFIG_ESP32_CACHE_L2_LINE_SIZE, fb_size);
	if (g_fb == NULL) {
		LOG_ERR("failed to allocate %zu byte portrait framebuffer", fb_size);
		return -ENOMEM;
	}
	memset(g_fb, 0, fb_size);

	lv_display_set_flush_cb(lv_disp, rot_flush_cb);

	LOG_INF("software rotation %d enabled: %ux%u portrait framebuffer @ %p",
		rotation, g_phys_w, g_phys_h, (void *)g_fb);

	return 0;
}
