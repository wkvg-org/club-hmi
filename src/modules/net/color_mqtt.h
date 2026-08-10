/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX sender header — see color_mqtt.c for protocol details.
 */

#ifndef COLOR_MQTT_H_
#define COLOR_MQTT_H_

#include <stdint.h>

int  color_mqtt_init(void);
void color_mqtt_set_color(uint8_t dmx_val);
void color_mqtt_set_dimmer(uint8_t val);
void color_mqtt_set_pan(uint16_t deg);
void color_mqtt_set_tilt(uint8_t deg);
void color_mqtt_set_addr(uint16_t addr);
void color_mqtt_send(const char *cmd);

#endif /* COLOR_MQTT_H_ */
