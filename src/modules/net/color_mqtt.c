/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMX sender — transmits moving-head commands as UDP datagrams to
 * port 9090.  A persistent UDP socket is opened lazily on first send,
 * because the network stack may not be ready during main() init.
 *
 * Protocol (line-oriented, '\n' terminated):
 *   C:<dmx_value>    colour wheel        (0–255)
 *   D:<0..255>       dimmer
 *   S:<0..255>       strobe
 *   P:<0..540>       pan
 *   T:<0..200>       tilt
 *   A:<1..504>       DMX start address
 *   BLACK            blackout
 *   HOME             recenter, white, full brightness
 *
 * Target:            192.168.2.128:9090
 *                    (udp2mqtt bridge or direct S3 UDP server)
 */

#include "color_mqtt.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(color_mqtt, LOG_LEVEL_INF);

/* ── Constants ──────────────────────────────────────────────────────── */

#define UDP_PORT  9090
#define TARGET_IP "192.168.2.128"
#define JOY_THROTTLE_MS 50

/* ── Persistent UDP socket ──────────────────────────────────────────── */

static struct sockaddr_in peer_addr;
static bool  peer_valid;
static int   udp_sock = -1;

int color_mqtt_init(void)
{
	memset(&peer_addr, 0, sizeof(peer_addr));
	peer_addr.sin_family = AF_INET;
	peer_addr.sin_port   = htons(UDP_PORT);

	if (inet_pton(AF_INET, TARGET_IP, &peer_addr.sin_addr) == 1) {
		peer_valid = true;
	}

	LOG_INF("DMX UDP sender: " TARGET_IP ":%u", (unsigned)UDP_PORT);
	return 0;
}

static void ensure_socket(void)
{
	if (udp_sock >= 0) {
		return;
	}
	udp_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_sock < 0) {
		return;
	}
	LOG_INF("DMX UDP socket created");
}

static void udp_send(const char *buf, size_t len)
{
	if (!peer_valid) {
		return;
	}
	ensure_socket();
	if (udp_sock < 0) {
		return;
	}
	zsock_sendto(udp_sock, buf, len, 0,
		     (const struct sockaddr *)&peer_addr, sizeof(peer_addr));
}

void color_mqtt_set_color(uint8_t dmx_val)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "C:%u\n", dmx_val);
	udp_send(buf, strlen(buf));
	printk("DMX: color %u\n", dmx_val);
}

void color_mqtt_set_dimmer(uint8_t val)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "D:%u\n", val);
	udp_send(buf, strlen(buf));
	printk("DMX: dimmer %u\n", val);
}

void color_mqtt_set_pan(uint16_t deg)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "P:%u\n", deg);
	udp_send(buf, strlen(buf));
	printk("DMX: pan %u\n", deg);
}

void color_mqtt_set_tilt(uint8_t deg)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "T:%u\n", deg);
	udp_send(buf, strlen(buf));
	printk("DMX: tilt %u\n", deg);
}

void color_mqtt_set_addr(uint16_t addr)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "A:%u\n", addr);
	udp_send(buf, strlen(buf));
	printk("DMX: addr %u\n", addr);
}

void color_mqtt_send(const char *cmd)
{
	if (strcmp(cmd, "BLACK") == 0 || strcmp(cmd, "BLACK\n") == 0) {
		udp_send("BLACK\n", 6);
		printk("DMX: blackout\n");
	} else if (strcmp(cmd, "HOME") == 0 || strcmp(cmd, "HOME\n") == 0) {
		udp_send("HOME\n", 5);
		printk("DMX: home\n");
	}
}
