/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT client — connects to the broker (192.168.2.128:1883) and
 * publishes lamp control topics.  A dedicated thread handles the
 * CONNECT / CONNACK handshake, keep-alive pings (mqtt_live), and
 * incoming data (mqtt_input).
 *
 * Connection lifecycle
 * ====================
 *   1. mqtt_do_connect()  — TCP socket + MQTT CONNECT
 *   2. poll() for CONNACK — event handler sets g_connected
 *   3. mqtt_live() loop   — PINGREQ / PINGRESP, reconnects on loss
 *
 * Broker:         192.168.2.128:1883  (hard-coded, no DNS)
 * Client ID:      bar-dashboard
 * Protocol:       MQTT 3.1.1, QoS 0, clean session
 * Credentials:    none (broker has allow_anonymous=true)
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>

#include "../config/bar_config.h"
#include "dmx_zbus.h"
#include "mqtt.h"
#include "net.h"

LOG_MODULE_REGISTER(mqtt, LOG_LEVEL_INF);

/* ── Constants ──────────────────────────────────────────────────────── */

#define MQTT_CLIENT_ID         "bar-dashboard"
#define MQTT_KEEPALIVE         60
#define MQTT_CONNECT_TIMEOUT_MS 5000

#define TOPIC_LAMP_POWER      "node-red/lamp/power/set"
#define TOPIC_LAMP_BRIGHTNESS "node-red/lamp/brightness/set"

/* ── Global state ───────────────────────────────────────────────────── */

static struct mqtt_client g_client;
static struct sockaddr_in g_broker_addr;
static struct mqtt_utf8   g_client_id;
static bool               g_connected;

static char    g_publish_buf[64];
static uint8_t g_mqtt_rx_buf[512];
static uint8_t g_mqtt_tx_buf[512];

/* ── Event handler (CONNACK / DISCONNECT) ───────────────────────────── */

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);
	if (evt->type == MQTT_EVT_CONNACK && evt->result == 0) {
		g_connected = true;
		LOG_INF("MQTT connected");
	} else if (evt->type == MQTT_EVT_DISCONNECT) {
		g_connected = false;
	}
}

/* ── Connect to the broker ──────────────────────────────────────────── */

static void mqtt_do_connect(void)
{
	mqtt_client_init(&g_client);

	/* Hard-coded broker IP — the SD-card config may hold an empty
	 * mqtt_server_ip that would fall through to DNS (unreliable
	 * without a functioning gateway). */
	g_broker_addr.sin_family = AF_INET;
	g_broker_addr.sin_port   = htons(get_mqtt_port());
	inet_pton(AF_INET, "192.168.2.128", &g_broker_addr.sin_addr);

	g_client.broker           = &g_broker_addr;
	g_client.evt_cb           = mqtt_evt_handler;
	g_client.user_name        = NULL;  /* anonymous */
	g_client.password         = NULL;
	g_client.protocol_version = MQTT_VERSION_3_1_1;
	g_client.keepalive        = MQTT_KEEPALIVE;
	g_client.clean_session    = 1;
	g_client.rx_buf           = g_mqtt_rx_buf;
	g_client.rx_buf_size      = sizeof(g_mqtt_rx_buf);
	g_client.tx_buf           = g_mqtt_tx_buf;
	g_client.tx_buf_size      = sizeof(g_mqtt_tx_buf);

	g_client_id.utf8     = (uint8_t *)MQTT_CLIENT_ID;
	g_client_id.size     = strlen(MQTT_CLIENT_ID);
	g_client.client_id   = g_client_id;

	LOG_INF("MQTT broker: 192.168.2.128:%u", get_mqtt_port());

	int ret = mqtt_connect(&g_client);
	if (ret < 0) {
		LOG_WRN("MQTT connect failed: %d", ret);
		return;
	}
	/* g_connected will be set by the CONNACK event handler.
	 * Don't set it here — the TCP handshake may succeed but the
	 * MQTT-level CONNACK hasn't arrived yet. */
}

/* ── Publish a message (QoS 0) ──────────────────────────────────────── */

static void mqtt_do_publish(const char *topic, const char *payload)
{
	if (!g_connected) {
		return;
	}

	struct mqtt_publish_param param = {
		.message = {
			.topic = {
				.qos   = MQTT_QOS_0_AT_MOST_ONCE,
				.topic = {
					.utf8 = (uint8_t *)topic,
					.size = strlen(topic),
				},
			},
			.payload = {
				.data = (uint8_t *)payload,
				.len  = strlen(payload),
			},
		},
		.message_id = 1,
	};

	if (mqtt_publish(&g_client, &param) < 0) {
		LOG_WRN("MQTT publish %s failed", topic);
	}
}

/* ── Public API ─────────────────────────────────────────────────────── */

void mqtt_lamp_set_power(bool on)
{
	const char *payload = on ? "ON" : "OFF";
	LOG_INF("lamp power -> %s", payload);
	mqtt_do_publish(TOPIC_LAMP_POWER, payload);
}

void mqtt_lamp_set_brightness(uint8_t brightness)
{
	if (brightness > 100) {
		brightness = 100;
	}
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", brightness);
	LOG_INF("lamp brightness -> %s", g_publish_buf);
	mqtt_do_publish(TOPIC_LAMP_BRIGHTNESS, g_publish_buf);
}

void mqtt_pub(const char *topic, const char *payload)
{
	mqtt_do_publish(topic, payload);
}

void mqtt_dmx_color(uint8_t val)
{
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", val);
	mqtt_do_publish("dmx/color", g_publish_buf);
}

void mqtt_dmx_dimmer(uint8_t val)
{
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", val);
	mqtt_do_publish("dmx/dimmer", g_publish_buf);
}

void mqtt_dmx_pan(uint16_t deg)
{
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", deg);
	mqtt_do_publish("dmx/pan", g_publish_buf);
}

void mqtt_dmx_tilt(uint8_t deg)
{
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", deg);
	mqtt_do_publish("dmx/tilt", g_publish_buf);
}

void mqtt_dmx_strobe(uint8_t val)
{
	snprintf(g_publish_buf, sizeof(g_publish_buf), "%u", val);
	mqtt_do_publish("dmx/strobe", g_publish_buf);
}

void mqtt_dmx_home(void)
{
	mqtt_do_publish("dmx/home", "");
}

void mqtt_dmx_blackout(void)
{
	mqtt_do_publish("dmx/blackout", "");
}

/* ── Service thread (connect / keep-alive / reconnect) ──────────────── */

static void forward_dmx_cmd(const struct dmx_cmd *cmd)
{
	switch (cmd->type) {
	case DMX_CMD_COLOR:    mqtt_dmx_color(cmd->val);            break;
	case DMX_CMD_DIMMER:   mqtt_dmx_dimmer((uint8_t)cmd->val);  break;
	case DMX_CMD_PANTILT:
		mqtt_dmx_pan(cmd->val);
		mqtt_dmx_tilt(cmd->extra);
		break;
	case DMX_CMD_STROBE:   mqtt_dmx_strobe((uint8_t)cmd->val);  break;
	case DMX_CMD_HOME:     mqtt_dmx_home();                     break;
	case DMX_CMD_BLACKOUT: mqtt_dmx_blackout();                 break;
	}
}

void mqtt_service(void)
{
	struct pollfd fds[1];
	struct dmx_cmd cmd;

	LOG_INF("mqtt service started");

	while (1) {
		int drained = 0;
		while (drained < 4 && k_msgq_get(&dmx_msgq, &cmd, K_NO_WAIT) == 0) {
			forward_dmx_cmd(&cmd);
			drained++;
		}

		if (!net_is_up()) {
			k_sleep(K_MSEC(100));
			continue;
		}

		if (!g_connected) {
			/* Connect and wait for CONNACK */
			mqtt_do_connect();
			if (g_client.transport.tcp.sock >= 0) {
				int64_t deadline = k_uptime_get() + MQTT_CONNECT_TIMEOUT_MS;
				fds[0].fd     = g_client.transport.tcp.sock;
				fds[0].events = POLLIN;
				while (!g_connected && k_uptime_get() < deadline) {
					if (poll(fds, 1, 100) > 0) {
						mqtt_input(&g_client);
					}
				}
			}
			if (!g_connected) {
				mqtt_abort(&g_client);
				LOG_WRN("MQTT connect timed out");
			}
			k_sleep(K_SECONDS(5));
		} else {
			/* Connected: poll for incoming data and run keep-alive */
			fds[0].fd     = g_client.transport.tcp.sock;
			fds[0].events = POLLIN;
			if (poll(fds, 1, 1000) > 0) {
				mqtt_input(&g_client);
			}
			int ret = mqtt_live(&g_client);
			if (ret < 0 && ret != -EAGAIN) {
				LOG_WRN("MQTT connection lost");
				g_connected = false;
			}
		}
		k_sleep(K_MSEC(10));
	}
}
