/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Message queue for DMX commands — decouples UI events from MQTT publish.
 */

#ifndef DMX_ZBUS_H_
#define DMX_ZBUS_H_

#include <zephyr/kernel.h>
#include <stdint.h>

enum dmx_cmd_type {
	DMX_CMD_COLOR = 0,
	DMX_CMD_DIMMER,
	DMX_CMD_PANTILT,
	DMX_CMD_STROBE,
	DMX_CMD_HOME,
	DMX_CMD_BLACKOUT,
};

struct dmx_cmd {
	uint8_t  type;
	uint16_t val;    /* pan for PANTILT, arg for others */
	uint8_t  extra;  /* tilt for PANTILT */
};

#define DMX_MSGQ_MAX 8

extern struct k_msgq dmx_msgq;

#endif /* DMX_ZBUS_H_ */
