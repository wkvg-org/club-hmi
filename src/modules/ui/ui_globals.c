/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UI state (see ui_globals.h). time_update_gui_globals() is
 * provided by the NTP backend; the lamp commands delegate to MQTT.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../net/mqtt.h"
#include "ui_globals.h"

LOG_MODULE_REGISTER(ui_globals, LOG_LEVEL_INF);

char g_ui_time_str[16] = "--:--";
char g_ui_date_str[16] = "--.--.----";

volatile uint16_t g_now_minutes = 0;
volatile bool g_alarm_notification = false;

void lamp_set_power(bool on)
{
	mqtt_lamp_set_power(on);
}

void lamp_set_brightness(uint8_t percent)
{
	mqtt_lamp_set_brightness(percent);
}
