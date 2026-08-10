/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * App-facing API for the Waveshare panel power/backlight controller.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DISPLAY_WAVESHARE_PANEL_CTL_H_
#define ZEPHYR_INCLUDE_DRIVERS_DISPLAY_WAVESHARE_PANEL_CTL_H_

#include <zephyr/device.h>

/**
 * @brief Set the LCD backlight brightness (0-255).
 *
 * Written to the on-board controller's register 0x96.
 */
int waveshare_panel_ctl_set_backlight(const struct device *dev, uint8_t brightness);

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_WAVESHARE_PANEL_CTL_H_ */
