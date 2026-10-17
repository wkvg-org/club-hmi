/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime IPv4/hostname configuration for the wired Ethernet interface.
 * The values are persisted as JSON on the SD card (/SD:/network.json) and
 * applied to the interface at boot and whenever the user changes them.
 */

#ifndef NET_CONFIG_H_
#define NET_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Load the persisted network config and apply it to the interface.
 *
 * Call once at boot, after the SD mount and before starting the backends.
 *
 * @return 0 on success, negative on error.
 */
int net_config_init(void);

/** @return true if the interface uses DHCP, false for a static address. */
bool net_config_uses_dhcp(void);

/** @return the configured hostname (never NULL). */
const char *net_config_get_hostname(void);

/**
 * @brief Copy the configured static IPv4 address/netmask/gateway strings.
 */
void net_config_get_ipv4(char *address, size_t address_len,
			 char *netmask, size_t netmask_len,
			 char *gateway, size_t gateway_len);

/**
 * @brief Persist and apply a new hostname.
 *
 * @return 0 on success, negative on error.
 */
int net_config_set_hostname(const char *hostname);

/**
 * @brief Persist and apply the IPv4 configuration.
 *
 * @param use_dhcp  true for DHCP, false for a static address.
 * @param address   static IPv4 address (ignored when @p use_dhcp).
 * @param netmask   static netmask (ignored when @p use_dhcp).
 * @param gateway   static gateway (ignored when @p use_dhcp).
 *
 * @return 0 on success, negative on error.
 */
int net_config_set_ipv4(bool use_dhcp, const char *address,
			const char *netmask, const char *gateway);

#endif /* NET_CONFIG_H_ */
