/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ethernet bring-up — waits for the wired interface to obtain an
 * IPv4 address (DHCP or static), then signals the backends (NTP,
 * MQTT, Directus) that the network is ready via net_is_up().
 *
 * With a static IP (CONFIG_NET_DHCPV4=n) the address is assigned
 * during net_config auto-init before main() runs.  net_is_up()
 * accepts DHCP, MANUAL, and OVERRIDABLE address types so that
 * both configurations work.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

#include "net.h"

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

static bool g_net_up;
static struct net_mgmt_event_callback mgmt_cb;

static struct net_if *get_ethernet_iface(void)
{
	return net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
}

/* ── Network management event handler ───────────────────────────────── */

static void net_event_handler(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("Ethernet up, got IPv4");
		g_net_up = true;
	} else if (mgmt_event == NET_EVENT_IPV4_ADDR_DEL) {
		LOG_INF("Ethernet IPv4 lost");
		g_net_up = false;
	}
}

/* ── Is the network ready? ──────────────────────────────────────────── */

bool net_is_up(void)
{
	/*
	 * net_config auto-init may have assigned an IP before the app
	 * registered its event callback, so also poll the interface.
	 * Accept DHCP, MANUAL (static IP), and OVERRIDABLE addresses.
	 */
	struct net_if *iface = get_ethernet_iface();

	if (iface != NULL && net_if_is_up(iface)) {
		struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

		if (ipv4 != NULL &&
		    ipv4->unicast[0].ipv4.addr_state == NET_ADDR_PREFERRED &&
		    (ipv4->unicast[0].ipv4.addr_type == NET_ADDR_DHCP ||
		     ipv4->unicast[0].ipv4.addr_type == NET_ADDR_MANUAL ||
		     ipv4->unicast[0].ipv4.addr_type == NET_ADDR_OVERRIDABLE)) {
			g_net_up = true;
		}
	}
	return g_net_up;
}

/* ── Initialise the network watcher ─────────────────────────────────── */

void net_init(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD |
				     NET_EVENT_IPV4_ADDR_DEL);
	net_mgmt_add_event_callback(&mgmt_cb);

	if (get_ethernet_iface() == NULL) {
		LOG_ERR("no Ethernet network interface");
		return;
	}

	LOG_INF("Ethernet init: waiting for IPv4");
}
