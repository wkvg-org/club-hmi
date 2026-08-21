/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Directus acts channel — producer (Directus thread), consumer (LVGL UI).
 */

#include "directus_zbus.h"

ZBUS_SUBSCRIBER_DEFINE(directus_sub, 4);
ZBUS_CHAN_DEFINE(directus_chan, struct directus_msg, NULL, NULL,
		 ZBUS_OBSERVERS(directus_sub),
		 ZBUS_MSG_INIT(.count = 0));
