/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * App-facing API for the ESP32-P4 DSI DPI display driver.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DISPLAY_ESP32_DPI_H_
#define ZEPHYR_INCLUDE_DRIVERS_DISPLAY_ESP32_DPI_H_

#include <zephyr/device.h>

/**
 * @brief Configure the content rotation (degrees clockwise, 0/90/180/270).
 *
 * The driver rotates flush pixels into the physical (portrait) framebuffer
 * accordingly. The app must also set the matching LVGL display rotation so
 * the LVGL logical resolution matches the rotated orientation.
 */
void display_esp32_dpi_set_rotation(const struct device *dev, uint16_t rotation);

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_ESP32_DPI_H_ */
