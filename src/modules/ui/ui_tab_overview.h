/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_TAB_OVERVIEW_H_
#define UI_TAB_OVERVIEW_H_

#include <lvgl.h>
#include <stdint.h>

#include "ui_globals.h"

lv_obj_t *ui_tab_overview_build(lv_obj_t *parent);
void ui_tab_overview_refresh(void);
void ui_tab_overview_set_day_acts(const act_t *acts, uint8_t count);

#endif /* UI_TAB_OVERVIEW_H_ */
