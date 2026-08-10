/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ethernet bring-up, ported from the Arduino bar-dashboard (net.cpp):
 * waits for the wired interface to come up with an IPv4 address (DHCP),
 * then starts the backends (NTP, MQTT, Directus).
 */

#ifndef NET_H_
#define NET_H_

#include <stdbool.h>

void net_init(void);
bool net_is_up(void);
void net_wait_up(void);

#endif /* NET_H_ */
