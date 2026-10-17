# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_JD9365 display_jd9365.c)
zephyr_library_sources_ifdef(CONFIG_WAVESHARE_PANEL_CTL display_waveshare_panelctl.c)
