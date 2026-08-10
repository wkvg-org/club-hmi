/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Message queue for DMX commands — single-producer (UI), single-consumer (MQTT).
 */

#include "dmx_zbus.h"

K_MSGQ_DEFINE(dmx_msgq, sizeof(struct dmx_cmd), DMX_MSGQ_MAX, 4);
