/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * UI shell header — tab-based dashboard with time/date sidebar.
 */

#ifndef UI_H_
#define UI_H_

#include <lvgl.h>
#include <stdint.h>

#include "ui_globals.h"

void ui_init_and_build(void);
void ui_set_datetime(const char *hhmm, const char *ddmmyyyy);
void ui_set_alarm_notification(bool on);
void ui_set_day_acts(const act_t *acts, uint8_t count);

/* Switch to a tab by index:
 *   0 = Overview, 1 = Pager, 2 = Light (DMX), 3 = Alarm, 4 = Settings */
void ui_switch_tab(int tab);

#endif /* UI_H_ */
