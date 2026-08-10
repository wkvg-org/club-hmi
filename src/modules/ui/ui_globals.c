/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UI state (see ui_globals.h). time_update_gui_globals() is
 * provided by the NTP backend; the lamp commands delegate to MQTT.
 * Demo act data keeps the overview tab populated until the Directus
 * backend has delivered real data.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../net/mqtt.h"
#include "ui_globals.h"

LOG_MODULE_REGISTER(ui_globals, LOG_LEVEL_INF);

char g_ui_time_str[16] = "--:--:--";
char g_ui_date_str[16] = "--.--.----";

volatile uint16_t g_now_minutes = 0;
volatile bool g_alarm_notification = false;

act_t g_day_acts[MAX_ACTS];
volatile uint8_t g_day_acts_count = 0;

/* Demo day schedule (used until Directus delivers real acts). */
static const act_t demo_acts[] = {
	{"Morgenshow", 8 * 60, 10 * 60},
	{"Mittagsshow", 12 * 60, 14 * 60},
	{"Abendshow", 18 * 60, 22 * 60},
};

static int ui_demo_data_init(void)
{
	g_day_acts_count = 0;
	for (size_t i = 0; i < ARRAY_SIZE(demo_acts); i++) {
		g_day_acts[i] = demo_acts[i];
		g_day_acts_count++;
	}
	return 0;
}

SYS_INIT(ui_demo_data_init, APPLICATION, 90);

void lamp_set_power(bool on)
{
	mqtt_lamp_set_power(on);
}

void lamp_set_brightness(uint8_t percent)
{
	mqtt_lamp_set_brightness(percent);
}
