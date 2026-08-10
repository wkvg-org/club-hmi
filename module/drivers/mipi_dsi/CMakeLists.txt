# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MIPI_DSI_ESP32 dsi_esp32.c)
