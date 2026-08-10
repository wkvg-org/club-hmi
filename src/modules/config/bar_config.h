/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend configuration, ported from the Arduino bar-dashboard
 * (bar_config.cpp). Loads bar_dashboard.json + creds.json from the SD
 * card when present; falls back to the built-in defaults otherwise.
 */

#ifndef BAR_CONFIG_H_
#define BAR_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

void init_config(void);

const char *get_device_name(void);

const char *get_mqtt_server_domain(void);
const char *get_mqtt_server_ip(void);
uint16_t get_mqtt_port(void);
const char *get_mqtt_username(void);
const char *get_mqtt_password(void);

const char *get_directus_server_domain(void);
const char *get_directus_server_ip(void);
uint16_t get_directus_port(void);
const char *get_directus_bearer_token(void);

const char *get_ntp_server(uint8_t idx);

#endif /* BAR_CONFIG_H_ */
