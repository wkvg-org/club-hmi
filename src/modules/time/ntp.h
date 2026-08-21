/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * NTP time sync, ported from the Arduino bar-dashboard (ntp.cpp).
 * SNTP-synchronised UTC is converted to local time (CET/CEST, the
 * "CET-1CEST,M3.5.0/02,M10.5.0/03" zone) and published to the UI
 * globals every second by time_update_gui_globals().
 */

#ifndef NTP_H_
#define NTP_H_

#include <stdbool.h>

void ntp_init(void);
void ntp_force_resync(void);

/* Periodic re-sync service thread (10 min interval). */
void ntp_service(void);

/* True once the clock has been synchronised at least once. */
bool ntp_synced(void);

/* Fills the UI globals (g_ui_time_str/g_ui_date_str/g_now_minutes) from
 * the synced clock. Returns false when not yet synced. */
bool time_update_gui_globals(void);

#endif /* NTP_H_ */
