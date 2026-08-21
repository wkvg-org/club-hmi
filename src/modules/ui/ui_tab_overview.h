/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_TAB_OVERVIEW_H_
#define UI_TAB_OVERVIEW_H_

#include <lvgl.h>
#include <zephyr/zbus/zbus.h>

lv_obj_t *ui_tab_overview_build(lv_obj_t *parent);
void ui_tab_overview_refresh(void);
void ui_tab_overview_drain_acts(const struct zbus_channel *chan);

#endif /* UI_TAB_OVERVIEW_H_ */
