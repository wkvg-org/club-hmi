/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persisted device settings (backlight brightness), stored as JSON on
 * the SD card (/SD/settings.json) and applied at startup.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display/waveshare_panel_ctl.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/data/json.h>

#include "settings.h"

LOG_MODULE_REGISTER(settings, LOG_LEVEL_INF);

#define SETTINGS_PATH "/SD:/settings.json"
#define DEFAULT_BACKLIGHT_PERCENT 100

struct settings_file {
	uint8_t backlight;
};

static const struct json_obj_descr settings_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct settings_file, backlight, JSON_TOK_NUMBER),
};

static uint8_t g_backlight_percent = DEFAULT_BACKLIGHT_PERCENT;

void settings_load(void)
{
	struct fs_file_t f;
	struct settings_file s;
	char buf[256];
	ssize_t n;

	fs_file_t_init(&f);
	if (fs_open(&f, SETTINGS_PATH, FS_O_READ) < 0) {
		LOG_INF("no %s, using default backlight %u%%", SETTINGS_PATH,
			DEFAULT_BACKLIGHT_PERCENT);
		return;
	}
	n = fs_read(&f, buf, sizeof(buf) - 1);
	fs_close(&f);
	if (n <= 0) {
		LOG_WRN("could not read %s", SETTINGS_PATH);
		return;
	}
	buf[n] = '\0';

	memset(&s, 0, sizeof(s));
	if (json_obj_parse(buf, strlen(buf), settings_descr, ARRAY_SIZE(settings_descr), &s) < 0) {
		LOG_WRN("failed to parse %s", SETTINGS_PATH);
		return;
	}

	if (s.backlight > 100) {
		s.backlight = 100;
	}
	g_backlight_percent = s.backlight;
	LOG_INF("stored backlight %u%%", g_backlight_percent);
}

static void settings_store(void)
{
	struct fs_file_t f;
	char buf[64];
	int len;

	len = snprintf(buf, sizeof(buf), "{\"backlight\": %u}\n", g_backlight_percent);

	fs_file_t_init(&f);
	if (fs_open(&f, SETTINGS_PATH, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE) < 0) {
		LOG_WRN("could not open %s for write", SETTINGS_PATH);
		return;
	}
	if (fs_write(&f, buf, len) < 0) {
		LOG_WRN("could not write %s", SETTINGS_PATH);
	}
	fs_close(&f);
	LOG_INF("stored backlight %u%% to %s", g_backlight_percent, SETTINGS_PATH);
}

uint8_t settings_get_backlight_percent(void)
{
	return g_backlight_percent;
}

void settings_apply_backlight(void)
{
	const struct device *panel_ctl = DEVICE_DT_GET(DT_NODELABEL(panel_ctl));

	if (!device_is_ready(panel_ctl)) {
		LOG_ERR("panel-ctl not ready");
		return;
	}

	uint8_t raw = (uint8_t)((uint32_t)g_backlight_percent * 255U / 100U);

	if (waveshare_panel_ctl_set_backlight(panel_ctl, raw) < 0) {
		LOG_ERR("failed to apply backlight");
		return;
	}
	LOG_INF("backlight applied: %u%%", g_backlight_percent);
}

void settings_set_backlight_percent(uint8_t percent)
{
	if (percent > 100) {
		percent = 100;
	}

	if (percent == g_backlight_percent) {
		return;
	}

	g_backlight_percent = percent;
	settings_apply_backlight();
	settings_store();
}
