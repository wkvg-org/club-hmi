# SPDX-License-Identifier: Apache-2.0
#
# club-display out-of-tree module: ESP32-P4 MIPI-DSI host, DPI/framebuffer
# scan-out, JD9365 panel and Waveshare panel-power/backlight drivers.

add_subdirectory(drivers/mipi_dsi)
add_subdirectory(drivers/display)
add_subdirectory(drivers/input)

# Public module include dir: <zephyr/drivers/display/display_esp32_dpi.h>
zephyr_include_directories(include)
