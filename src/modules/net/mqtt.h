/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_H_
#define MQTT_H_

#include <stdbool.h>
#include <stdint.h>

void mqtt_service(void);

void mqtt_lamp_set_power(bool on);
void mqtt_lamp_set_brightness(uint8_t brightness);

void mqtt_dmx_color(uint8_t val);
void mqtt_dmx_dimmer(uint8_t val);
void mqtt_dmx_pan(uint16_t deg);
void mqtt_dmx_tilt(uint8_t deg);
void mqtt_dmx_strobe(uint8_t val);
void mqtt_dmx_home(void);
void mqtt_dmx_blackout(void);

void mqtt_pub(const char *topic, const char *payload);

#endif /* MQTT_H_ */
