/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal API between the ESP32 MIPI-DSI host driver and panel drivers in
 * this same module. Not a devicetree/Zephyr-subsystem API: the panel driver
 * already has the host device pointer via its "mipi-dsi" phandle.
 */

#ifndef CLUB_DISPLAY_DSI_ESP32_H
#define CLUB_DISPLAY_DSI_ESP32_H

#include <zephyr/device.h>

/**
 * @brief Switch the DSI host into continuous video mode and start the
 * bridge's DPI output.
 *
 * Must be called only after mipi_dsi_attach() and after the panel's own
 * command-mode DCS init sequence has completed - see PLAN.md and the
 * comment in dsi_esp32_attach() for why video mode is not started
 * automatically as part of attach() on this SoC/panel combination.
 *
 * @param dev The MIPI-DSI host device (espressif,esp-mipi-dsi).
 * @return 0 on success, -EAGAIN if the host has not been attached yet.
 */
int esp32_mipi_dsi_start_video(const struct device *dev);

#endif /* CLUB_DISPLAY_DSI_ESP32_H */
