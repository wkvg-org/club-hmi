/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX command channel (zbus) — decouples UI events from MQTT publish.
 */

#ifndef DMX_ZBUS_H_
#define DMX_ZBUS_H_

#include <zephyr/zbus/zbus.h>
#include <stdint.h>

enum dmx_cmd_type {
	DMX_CMD_COLOR = 0,
	DMX_CMD_DIMMER,
	DMX_CMD_PANTILT,
	DMX_CMD_STROBE,
	DMX_CMD_HOME,
	DMX_CMD_BLACKOUT,
	DMX_CMD_ADDR,
	DMX_CMD_FIXTURE,
};

struct dmx_cmd {
	uint8_t  type;
	uint8_t  addr;    /* target DMX address (1/10 for MH, 19 for fixture) */
	uint16_t val;     /* pan for PANTILT, value for others */
	uint8_t  extra;   /* tilt for PANTILT, on/off for FIXTURE */
};

ZBUS_CHAN_DECLARE(dmx_chan);
ZBUS_OBS_DECLARE(dmx_sub);

#endif /* DMX_ZBUS_H_ */
