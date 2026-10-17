/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime IPv4/hostname configuration for the wired Ethernet interface.
 * See net_config.h.
 */

#include "net_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/data/json.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/hostname.h>

LOG_MODULE_REGISTER(net_config, LOG_LEVEL_INF);

#define NET_CONFIG_PATH  "/SD:/network.json"
#define HOSTNAME_MAX_LEN 63
#define IPV4_TEXT_LEN    16

struct net_config_file {
	char hostname[HOSTNAME_MAX_LEN + 1];
	char mode[7]; /* "dhcp" or "static" */
	char address[IPV4_TEXT_LEN];
	char netmask[IPV4_TEXT_LEN];
	char gateway[IPV4_TEXT_LEN];
};

static const struct json_obj_descr cfg_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct net_config_file, hostname, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct net_config_file, mode, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct net_config_file, address, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct net_config_file, netmask, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct net_config_file, gateway, JSON_TOK_STRING_BUF),
};

static struct net_config_file g_cfg = {
	.hostname = "bar-dashboard",
	.mode = "dhcp",
	.address = "192.168.2.130",
	.netmask = "255.255.255.0",
	.gateway = "192.168.2.1",
};

static struct net_if *g_iface;

static bool mode_is_dhcp(const struct net_config_file *cfg)
{
	return strcmp(cfg->mode, "static") != 0;
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
	if (src != NULL && src[0] != '\0') {
		strncpy(dst, src, dst_sz - 1);
		dst[dst_sz - 1] = '\0';
	}
}

static int apply_ipv4_config(bool use_dhcp, const char *address_text,
			     const char *netmask_text, const char *gateway_text)
{
	struct net_in_addr address, netmask, gateway;
	struct net_in_addr zero = { 0 };
	struct net_in_addr *current;

	if (g_iface == NULL) {
		return -ENODEV;
	}

	net_dhcpv4_stop(g_iface);
	current = net_if_ipv4_get_global_addr(g_iface, NET_ADDR_PREFERRED);
	if (current != NULL) {
		struct net_in_addr old = *current;

		net_if_ipv4_addr_rm(g_iface, &old);
	}

	if (use_dhcp) {
		net_if_ipv4_set_gw(g_iface, &zero);
		net_dhcpv4_start(g_iface);
		return 0;
	}

	if (net_addr_pton(AF_INET, address_text, &address) ||
	    net_addr_pton(AF_INET, netmask_text, &netmask) ||
	    net_addr_pton(AF_INET, gateway_text, &gateway)) {
		return -EINVAL;
	}

	if (!net_if_ipv4_addr_add(g_iface, &address, NET_ADDR_MANUAL, 0)) {
		return -EADDRINUSE;
	}
	if (!net_if_ipv4_set_netmask_by_addr(g_iface, &address, &netmask)) {
		net_if_ipv4_addr_rm(g_iface, &address);
		return -EINVAL;
	}
	net_if_ipv4_set_gw(g_iface, &gateway);
	return 0;
}

static int store_config(void)
{
	char buf[512];
	int len;
	struct fs_file_t f;
	size_t off = 0;

	len = snprintf(buf, sizeof(buf),
		       "{\"hostname\":\"%s\",\"mode\":\"%s\",\"address\":\"%s\","
		       "\"netmask\":\"%s\",\"gateway\":\"%s\"}\n",
		       g_cfg.hostname, g_cfg.mode, g_cfg.address, g_cfg.netmask,
		       g_cfg.gateway);
	if (len < 0 || len >= sizeof(buf)) {
		return -ENOBUFS;
	}

	fs_file_t_init(&f);
	if (fs_open(&f, NET_CONFIG_PATH, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE) < 0) {
		return -EIO;
	}
	while (off < (size_t)len) {
		ssize_t w = fs_write(&f, buf + off, (size_t)len - off);

		if (w < 0) {
			fs_close(&f);
			return -EIO;
		}
		off += w;
	}
	fs_sync(&f);
	fs_close(&f);
	return 0;
}

int net_config_init(void)
{
	char buf[512];
	struct fs_file_t f;
	struct net_config_file file;
	ssize_t n;
	int ret;

	g_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));

	fs_file_t_init(&f);
	if (fs_open(&f, NET_CONFIG_PATH, FS_O_READ) == 0) {
		n = fs_read(&f, buf, sizeof(buf) - 1);
		fs_close(&f);
		if (n > 0) {
			buf[n] = '\0';
			memset(&file, 0, sizeof(file));
			if (json_obj_parse(buf, strlen(buf), cfg_descr,
					   ARRAY_SIZE(cfg_descr), &file) >= 0) {
				copy_str(g_cfg.hostname, sizeof(g_cfg.hostname),
					 file.hostname);
				copy_str(g_cfg.mode, sizeof(g_cfg.mode), file.mode);
				copy_str(g_cfg.address, sizeof(g_cfg.address),
					 file.address);
				copy_str(g_cfg.netmask, sizeof(g_cfg.netmask),
					 file.netmask);
				copy_str(g_cfg.gateway, sizeof(g_cfg.gateway),
					 file.gateway);
				LOG_INF("network config loaded from %s", NET_CONFIG_PATH);
			} else {
				LOG_WRN("failed to parse %s, using defaults", NET_CONFIG_PATH);
			}
		}
	} else {
		LOG_INF("no %s, using defaults", NET_CONFIG_PATH);
	}

	ret = net_hostname_set(g_cfg.hostname, strlen(g_cfg.hostname));
	if (ret < 0) {
		LOG_WRN("failed to set hostname (%d)", ret);
	}

	if (g_iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return -ENODEV;
	}

	ret = apply_ipv4_config(mode_is_dhcp(&g_cfg), g_cfg.address, g_cfg.netmask,
				g_cfg.gateway);
	if (ret < 0) {
		LOG_ERR("failed to apply IPv4 config (%d)", ret);
		return ret;
	}

	LOG_INF("network config: %s, %s", g_cfg.hostname,
		mode_is_dhcp(&g_cfg) ? "DHCP" : "static");
	return 0;
}

bool net_config_uses_dhcp(void)
{
	return mode_is_dhcp(&g_cfg);
}

const char *net_config_get_hostname(void)
{
	return g_cfg.hostname;
}

void net_config_get_ipv4(char *address, size_t address_len,
			 char *netmask, size_t netmask_len,
			 char *gateway, size_t gateway_len)
{
	if (address != NULL) {
		snprintf(address, address_len, "%s", g_cfg.address);
	}
	if (netmask != NULL) {
		snprintf(netmask, netmask_len, "%s", g_cfg.netmask);
	}
	if (gateway != NULL) {
		snprintf(gateway, gateway_len, "%s", g_cfg.gateway);
	}
}

int net_config_set_hostname(const char *hostname)
{
	if (hostname == NULL || hostname[0] == '\0' ||
	    strlen(hostname) > HOSTNAME_MAX_LEN) {
		return -EINVAL;
	}

	copy_str(g_cfg.hostname, sizeof(g_cfg.hostname), hostname);

	int ret = net_hostname_set(g_cfg.hostname, strlen(g_cfg.hostname));

	if (ret < 0) {
		return ret;
	}
	return store_config();
}

int net_config_set_ipv4(bool use_dhcp, const char *address,
			const char *netmask, const char *gateway)
{
	struct net_in_addr parsed;
	int ret;

	if (!use_dhcp) {
		if (address == NULL || netmask == NULL || gateway == NULL ||
		    net_addr_pton(AF_INET, address, &parsed) ||
		    net_addr_pton(AF_INET, netmask, &parsed) ||
		    net_addr_pton(AF_INET, gateway, &parsed)) {
			return -EINVAL;
		}
	}

	copy_str(g_cfg.mode, sizeof(g_cfg.mode), use_dhcp ? "dhcp" : "static");
	if (!use_dhcp) {
		copy_str(g_cfg.address, sizeof(g_cfg.address), address);
		copy_str(g_cfg.netmask, sizeof(g_cfg.netmask), netmask);
		copy_str(g_cfg.gateway, sizeof(g_cfg.gateway), gateway);
	}

	ret = apply_ipv4_config(use_dhcp, g_cfg.address, g_cfg.netmask, g_cfg.gateway);
	if (ret < 0) {
		return ret;
	}
	return store_config();
}
