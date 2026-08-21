/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_H_
#define MQTT_H_

#include <stdbool.h>
#include <stdint.h>

void mqtt_service(void);

void mqtt_lamp_set_power(bool on);
void mqtt_lamp_set_brightness(uint8_t brightness);

void mqtt_dmx_color(uint8_t addr, uint8_t val);
void mqtt_dmx_dimmer(uint8_t addr, uint8_t val);
void mqtt_dmx_pan(uint8_t addr, uint16_t deg);
void mqtt_dmx_tilt(uint8_t addr, uint8_t deg);
void mqtt_dmx_strobe(uint8_t addr, uint8_t val);
void mqtt_dmx_home(uint8_t addr);
void mqtt_dmx_blackout(uint8_t addr);
void mqtt_dmx_addr(uint16_t addr);
void mqtt_dmx_fixture(uint8_t addr, uint8_t val);

void mqtt_pub(const char *topic, const char *payload);

#endif /* MQTT_H_ */
