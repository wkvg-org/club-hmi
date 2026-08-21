/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings tab: LCD backlight brightness slider. The value is persisted
 * on the SD card (settings.c) and applied at startup.
 */

#ifndef UI_TAB_SETTINGS_H_
#define UI_TAB_SETTINGS_H_

#include <lvgl.h>

lv_obj_t *build_settings_tab_page(lv_obj_t *parent);

#endif /* UI_TAB_SETTINGS_H_ */
