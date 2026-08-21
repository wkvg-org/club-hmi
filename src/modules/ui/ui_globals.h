/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UI state. The time/date globals are updated from the main
 * thread's LVGL timer.  Acts data flows via the directus zbus channel.
 */

#ifndef UI_GLOBALS_H_
#define UI_GLOBALS_H_

#include <stdbool.h>
#include <stdint.h>

#include "../net/directus_zbus.h"

extern char g_ui_time_str[16];
extern char g_ui_date_str[16];
extern volatile uint16_t g_now_minutes;
extern volatile bool g_alarm_notification;

bool time_update_gui_globals(void);
void lamp_set_power(bool on);
void lamp_set_brightness(uint8_t percent);

#endif /* UI_GLOBALS_H_ */
