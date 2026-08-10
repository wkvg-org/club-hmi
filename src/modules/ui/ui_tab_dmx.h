/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX colour tab: a grid of named-colour buttons that send DMX colour-
 * wheel commands over UDP to the club-ctrl moving-head controller.
 */

#ifndef UI_TAB_DMX_H_
#define UI_TAB_DMX_H_

#include <lvgl.h>

lv_obj_t *ui_tab_dmx_create(lv_obj_t *parent);

#endif /* UI_TAB_DMX_H_ */
