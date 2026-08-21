/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Directus acts channel (zbus) — decouples the Directus download thread
 * from the LVGL main thread that renders the overview tab.
 */

#ifndef DIRECTUS_ZBUS_H_
#define DIRECTUS_ZBUS_H_

#include <zephyr/zbus/zbus.h>
#include <stdint.h>

#define MAX_ACTS 16

typedef struct {
	char name[32];
	uint16_t start_min;
	uint16_t end_min;
} act_t;

struct directus_msg {
	act_t acts[MAX_ACTS];
	uint8_t count;
};

ZBUS_CHAN_DECLARE(directus_chan);
ZBUS_OBS_DECLARE(directus_sub);

#endif /* DIRECTUS_ZBUS_H_ */
