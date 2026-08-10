/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UI state, ported from the Arduino bar-dashboard. The time/date
 * and act data are fed by the (optional) backend shim; without a backend
 * the demo data in ui_globals.c keeps the GUI verifiable on hardware.
 */

#ifndef UI_GLOBALS_H_
#define UI_GLOBALS_H_

#include <stdbool.h>
#include <stdint.h>

#define MAX_ACTS 16
typedef struct {
	char name[32];
	uint16_t start_min;
	uint16_t end_min;
} act_t;

extern char g_ui_time_str[16];
extern char g_ui_date_str[16];
extern volatile uint16_t g_now_minutes;
extern volatile bool g_alarm_notification;

extern act_t g_day_acts[MAX_ACTS];
extern volatile uint8_t g_day_acts_count;

/*
 * Backend hooks:
 * - time_update_gui_globals(): updates the time/date globals (NTP-backed)
 *   once per second; returns false until the clock is synchronised.
 * - lamp_set_power/lamp_set_brightness(): MQTT-backed lamp commands.
 */
bool time_update_gui_globals(void);
void lamp_set_power(bool on);
void lamp_set_brightness(uint8_t percent);

#endif /* UI_GLOBALS_H_ */
