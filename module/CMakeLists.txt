# SPDX-License-Identifier: Apache-2.0
#
# club-hmi out-of-tree module: JD9365 panel, Waveshare panel-power/backlight
# and GT911 touch drivers. The MIPI-DSI host and the DPI scan-out controller
# are the upstream Zephyr drivers (drivers/mipi_dsi/dsi_esp32.c and
# drivers/display/display_esp32_dsi.c).

add_subdirectory(drivers/display)
add_subdirectory(drivers/input)

# Public module include dir: <zephyr/drivers/display/waveshare_panel_ctl.h>
zephyr_include_directories(include)
