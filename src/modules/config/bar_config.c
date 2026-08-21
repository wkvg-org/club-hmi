/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend configuration, ported from the Arduino bar-dashboard
 * (bar_config.cpp). Loads bar_dashboard.json + creds.json from the SD
 * card when present; falls back to the built-in defaults otherwise.
 */

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/data/json.h>

#include "bar_config.h"

LOG_MODULE_REGISTER(bar_config, LOG_LEVEL_INF);

#define CONFIG_PATH "/SD:/bar_dashboard.json"
#define CREDS_PATH  "/SD:/creds.json"

static const char *DEFAULT_MQTT_HOST   = "mqtt.moai.sk";
static const char *DEFAULT_MQTT_IP     = "192.168.2.128";
static const uint16_t DEFAULT_MQTT_PORT = 1883;

static const char *DEFAULT_DIRECTUS_HOST = "directus.moai.sk";
static const char *DEFAULT_DIRECTUS_IP   = "192.168.2.128";
static const uint16_t DEFAULT_DIRECTUS_PORT = 443;

static const char *DEFAULT_NTP_1 = "192.53.103.108";
static const char *DEFAULT_NTP_2 = "192.53.103.104";
static const char *DEFAULT_NTP_3 = "192.53.103.103";

static char g_device_name[64] = "bar-dashboard";
static char g_mqtt_host[128] = {0};
static char g_mqtt_ip[16] = {0};
static uint16_t g_mqtt_port = 1883;

static char g_directus_host[128] = {0};
static char g_directus_ip[16] = {0};
static uint16_t g_directus_port = 443;

static char g_ntp_hosts[3][128] = {{0}, {0}, {0}};

static char g_mqtt_user[64];
static char g_mqtt_pass[64];
static char g_directus_token[256];

/* bar_dashboard.json: { "data": { ... } } */
struct bar_config_data {
	char device_name[64];
	char mqtt_server_ip[16];
	char mqtt_server_domain[128];
	uint16_t mqtt_server_port;
	char directus_server_ip[16];
	char directus_server_domain[128];
	uint16_t directus_server_port;
	char ntp_server_domain_1[128];
	char ntp_server_domain_2[128];
	char ntp_server_domain_3[128];
	uint16_t ntp_server_port;
};

struct bar_config_file {
	struct bar_config_data data;
};

static const struct json_obj_descr cfg_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, device_name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, mqtt_server_ip, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, mqtt_server_domain, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, mqtt_server_port, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, directus_server_ip, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, directus_server_domain, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, directus_server_port, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, ntp_server_domain_1, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, ntp_server_domain_2, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, ntp_server_domain_3, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct bar_config_data, ntp_server_port, JSON_TOK_NUMBER),
};

static const struct json_obj_descr file_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct bar_config_file, data, cfg_descr),
};

/* creds.json: { "mqtt": { "username", "password" }, "directus": { "bearer_token" } } */
struct creds_mqtt {
	char username[64];
	char password[64];
};

struct creds_directus {
	char bearer_token[256];
};

struct creds {
	struct creds_mqtt mqtt;
	struct creds_directus directus;
};

static const struct json_obj_descr mqtt_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct creds_mqtt, username, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct creds_mqtt, password, JSON_TOK_STRING),
};

static const struct json_obj_descr directus_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct creds_directus, bearer_token, JSON_TOK_STRING),
};

static const struct json_obj_descr creds_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct creds, mqtt, mqtt_descr),
	JSON_OBJ_DESCR_OBJECT(struct creds, directus, directus_descr),
};

static bool read_json_file(const char *path, char *buf, size_t buf_sz)
{
	struct fs_file_t f;
	ssize_t n;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) < 0) {
		return false;
	}
	n = fs_read(&f, buf, buf_sz - 1);
	fs_close(&f);
	if (n <= 0) {
		return false;
	}
	buf[n] = '\0';
	return true;
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
	if (src != NULL && src[0] != '\0') {
		strncpy(dst, src, dst_sz - 1);
		dst[dst_sz - 1] = '\0';
	}
}

static void load_bar_dashboard(void)
{
	char buf[2048];
	struct bar_config_file file;

	if (!read_json_file(CONFIG_PATH, buf, sizeof(buf))) {
		LOG_INF("no %s on SD, using defaults", CONFIG_PATH);
		return;
	}

	memset(&file, 0, sizeof(file));
	if (json_obj_parse(buf, strlen(buf), file_descr, ARRAY_SIZE(file_descr), &file) < 0) {
		LOG_WRN("failed to parse %s", CONFIG_PATH);
		return;
	}

	copy_str(g_device_name, sizeof(g_device_name), file.data.device_name);
	copy_str(g_mqtt_ip, sizeof(g_mqtt_ip), file.data.mqtt_server_ip);
	copy_str(g_mqtt_host, sizeof(g_mqtt_host), file.data.mqtt_server_domain);
	if (file.data.mqtt_server_port != 0) {
		g_mqtt_port = file.data.mqtt_server_port;
	}
	copy_str(g_directus_ip, sizeof(g_directus_ip), file.data.directus_server_ip);
	copy_str(g_directus_host, sizeof(g_directus_host), file.data.directus_server_domain);
	if (file.data.directus_server_port != 0) {
		g_directus_port = file.data.directus_server_port;
	}
	copy_str(g_ntp_hosts[0], sizeof(g_ntp_hosts[0]), file.data.ntp_server_domain_1);
	copy_str(g_ntp_hosts[1], sizeof(g_ntp_hosts[1]), file.data.ntp_server_domain_2);
	copy_str(g_ntp_hosts[2], sizeof(g_ntp_hosts[2]), file.data.ntp_server_domain_3);

	LOG_INF("config loaded from %s", CONFIG_PATH);
}

static void load_creds(void)
{
	char buf[1024];
	struct creds creds;

	if (!read_json_file(CREDS_PATH, buf, sizeof(buf))) {
		LOG_INF("no %s on SD, using empty credentials", CREDS_PATH);
		return;
	}

	memset(&creds, 0, sizeof(creds));
	if (json_obj_parse(buf, strlen(buf), creds_descr, ARRAY_SIZE(creds_descr), &creds) < 0) {
		LOG_WRN("failed to parse %s", CREDS_PATH);
		return;
	}

	if (creds.mqtt.username[0]) {
		strncpy(g_mqtt_user, creds.mqtt.username, sizeof(g_mqtt_user) - 1);
	}
	if (creds.mqtt.password[0]) {
		strncpy(g_mqtt_pass, creds.mqtt.password, sizeof(g_mqtt_pass) - 1);
	}
	if (creds.directus.bearer_token[0]) {
		strncpy(g_directus_token, creds.directus.bearer_token, sizeof(g_directus_token) - 1);
	}

	LOG_INF("credentials loaded from %s", CREDS_PATH);
}

static void mount_sd(void)
{
	/* The FATFS volume is auto-mounted at /SD: by the fstab node in the
	 * board DTS (CONFIG_FS_FATFS_FSTAB_AUTOMOUNT). Verify it is
	 * reachable before claiming the SD card is present. */
	struct fs_dir_t dir;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, "/SD:") < 0) {
		LOG_WRN("no SD card present");
		return;
	}
	fs_closedir(&dir);
	LOG_INF("SD card present");
}

void init_config(void)
{
	mount_sd();
	load_bar_dashboard();
	load_creds();

	if (g_mqtt_host[0] == '\0') {
		copy_str(g_mqtt_host, sizeof(g_mqtt_host), DEFAULT_MQTT_HOST);
	}
	if (g_mqtt_ip[0] == '\0') {
		copy_str(g_mqtt_ip, sizeof(g_mqtt_ip), DEFAULT_MQTT_IP);
	}
	if (g_directus_host[0] == '\0') {
		copy_str(g_directus_host, sizeof(g_directus_host), DEFAULT_DIRECTUS_HOST);
	}
	if (g_directus_ip[0] == '\0') {
		copy_str(g_directus_ip, sizeof(g_directus_ip), DEFAULT_DIRECTUS_IP);
	}
	for (int i = 0; i < 3; i++) {
		if (g_ntp_hosts[i][0] == '\0') {
			copy_str(g_ntp_hosts[i], sizeof(g_ntp_hosts[i]),
				 i == 0 ? DEFAULT_NTP_1 : (i == 1 ? DEFAULT_NTP_2 : DEFAULT_NTP_3));
		}
	}
}

const char *get_device_name(void)
{
	return g_device_name;
}

const char *get_mqtt_server_domain(void)
{
	return g_mqtt_host;
}

const char *get_mqtt_server_ip(void)
{
	return g_mqtt_ip;
}

uint16_t get_mqtt_port(void)
{
	return g_mqtt_port;
}

const char *get_mqtt_username(void)
{
	return g_mqtt_user;
}

const char *get_mqtt_password(void)
{
	return g_mqtt_pass;
}

const char *get_directus_server_domain(void)
{
	return g_directus_host;
}

const char *get_directus_server_ip(void)
{
	return g_directus_ip;
}

uint16_t get_directus_port(void)
{
	return g_directus_port;
}

const char *get_directus_bearer_token(void)
{
	return g_directus_token;
}

const char *get_ntp_server(uint8_t idx)
{
	if (idx >= ARRAY_SIZE(g_ntp_hosts)) {
		return g_ntp_hosts[0];
	}
	return g_ntp_hosts[idx];
}
