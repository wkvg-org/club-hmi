/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persisted device settings (backlight brightness), stored as JSON on
 * the SD card (/SD/settings.json) and applied at startup.
 */

#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdint.h>

/* Backlight in percent (0-100). */
uint8_t settings_get_backlight_percent(void);

/* Loads the stored value from the SD card; call after the SD mount
 * (init_config()). */
void settings_load(void);

/* Applies the stored backlight to the panel controller; called at
 * startup after the SD mount and the panel-ctl driver init. */
void settings_apply_backlight(void);

/* Sets the backlight on the panel controller and persists the value. */
void settings_set_backlight_percent(uint8_t percent);

#endif /* SETTINGS_H_ */
