/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software rotation for the landscape-mounted portrait panel.
 *
 * The 800x1280 panel is mounted landscape, so LVGL renders at 1280x800
 * (LV_DISPLAY_ROTATION_270) and the flush callback rotates each flushed
 * landscape tile into a physical portrait framebuffer, which is then
 * presented to the display controller for scan-out.
 */

#ifndef DISPLAY_ROTATION_H_
#define DISPLAY_ROTATION_H_

#include <zephyr/device.h>
#include <lvgl.h>

/**
 * @brief Enable software rotation of the LVGL output into a portrait
 * framebuffer presented to @p display.
 *
 * @param display The zephyr,display device (espressif,esp-dsi-display).
 * @param lv_disp The LVGL display.
 * @param rotation Rotation to apply (LV_DISPLAY_ROTATION_90 or 270).
 * @return 0 on success, negative on error.
 */
int display_rotation_init(const struct device *display, lv_display_t *lv_disp,
			  lv_display_rotation_t rotation);

#endif /* DISPLAY_ROTATION_H_ */
