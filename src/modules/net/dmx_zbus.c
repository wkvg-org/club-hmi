/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX command channel — single-producer (UI/shell), single-consumer (MQTT).
 */

#include "dmx_zbus.h"

ZBUS_SUBSCRIBER_DEFINE(dmx_sub, 8);
ZBUS_CHAN_DEFINE(dmx_chan, struct dmx_cmd, NULL, NULL,
		 ZBUS_OBSERVERS(dmx_sub),
		 ZBUS_MSG_INIT(.type = 0, .val = 0, .extra = 0));
