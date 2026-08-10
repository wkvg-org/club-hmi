/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * NTP time synchronisation.
 *
 * Clock model
 * ===========
 * A 64-bit UTC epoch base (g_base_utc_secs) is paired with the boot
 * uptime at which it was set (g_base_uptime_ms).  The current UTC is
 * computed as  base + (uptime_now - base_uptime) / 1000.
 *
 * This keeps the clock running between SNTP re-syncs without libc
 * time/clock integration.
 *
 * Seed time
 * ---------
 * At boot the clock is seeded to 2025-01-01 00:00:00 UTC so the UI
 * shows a moving wall-clock immediately.  Once the first SNTP reply
 * arrives the seed is overwritten with the real epoch.
 *
 * SNTP transport
 * --------------
 * The P4's Zephyr stack does not support UDP connect(), which the
 * Zephyr sntp_simple() / sntp_simple_addr() library relies on.  We
 * therefore use raw socket() / sendto() / poll() / recvfrom() to the
 * hard-coded PTB (Physikalisch-Technische Bundesanstalt) NTP servers.
 *
 * Sync interval
 * -------------
 * First sync:   3 s after boot (network initialisation delay).
 * Subsequent: every 10 minutes (NTP_SYNC_INTERVAL_MS).
 *
 * Time zone
 * ---------
 * CET  (UTC+1): last Sunday of October 01:00 UTC →
 *               last Sunday of March   01:00 UTC
 * CEST (UTC+2): last Sunday of March   01:00 UTC →
 *               last Sunday of October 01:00 UTC
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/sys/mutex.h>

#include "ntp.h"

#include "../config/bar_config.h"
#include "../net/net.h"
#include "../ui/ui_globals.h"

LOG_MODULE_REGISTER(ntp, LOG_LEVEL_INF);

/* ── Timing constants ───────────────────────────────────────────────── */

#define NTP_SYNC_INTERVAL_MS  (10UL * 60UL * 1000UL)  /* 10 minutes */
#define NTP_QUERY_TIMEOUT_MS   2000                    /* socket poll timeout */
#define NTP_STARTUP_DELAY_MS   3000                    /* wait for net stack */

/* ── Clock state (mutex-protected against the NTP thread) ───────────── */

static volatile bool g_synced;        /* true after first successful SNTP */
static volatile bool g_seeded;        /* true after clock_seed() */
static int64_t       g_base_utc_secs; /* epoch base */
static uint32_t      g_base_uptime_ms;/* uptime at which base was set */
static uint32_t      g_last_sync_ms;  /* uptime of last sync attempt */
static K_MUTEX_DEFINE(g_clock_mutex);

/* ── Raw NTP packet ─────────────────────────────────────────────────── */

static const uint8_t ntp_request[48] = {
	0x1b, /* LI=0, VN=4, Mode=3 (client) */
	/* remaining 47 bytes are zero */
};

/* ── Seed the clock ─────────────────────────────────────────────────── */

static void clock_seed(void)
{
	k_mutex_lock(&g_clock_mutex, K_FOREVER);
	if (!g_seeded) {
		g_base_utc_secs  = 1735689600; /* 2025-01-01 00:00:00 UTC */
		g_base_uptime_ms = k_uptime_get_32();
		g_seeded = true;
	}
	k_mutex_unlock(&g_clock_mutex);
}

/* ── Current UTC epoch (thread-safe) ────────────────────────────────── */

static int64_t utc_now(void)
{
	int64_t  base;
	uint32_t base_ms;

	k_mutex_lock(&g_clock_mutex, K_FOREVER);
	base    = g_base_utc_secs;
	base_ms = g_base_uptime_ms;
	k_mutex_unlock(&g_clock_mutex);

	return base + (k_uptime_get_32() - base_ms) / 1000;
}

/* ── Single NTP query (raw UDP) ─────────────────────────────────────── */

static void ntp_query(const char *server_ip)
{
	struct sockaddr_in addr;
	uint8_t            resp[48];
	int                sock, ret;
	int64_t            epoch;

	/* Build the destination address */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(123);
	inet_pton(AF_INET, server_ip, &addr.sin_addr);

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_WRN("NTP socket(%s) failed: %d", server_ip, sock);
		return;
	}

	ret = sendto(sock, ntp_request, sizeof(ntp_request), 0,
		     (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_WRN("NTP sendto(%s) failed: %d", server_ip, ret);
		close(sock);
		return;
	}

	/* Wait for the reply with a timeout */
	struct pollfd fds = { .fd = sock, .events = POLLIN };
	ret = poll(&fds, 1, NTP_QUERY_TIMEOUT_MS);
	if (ret <= 0) {
		LOG_DBG("NTP %s timeout", server_ip);
		close(sock);
		return;
	}

	ret = recvfrom(sock, resp, sizeof(resp), 0, NULL, NULL);
	close(sock);

	if (ret < (int)sizeof(resp)) {
		LOG_DBG("NTP %s short response: %d bytes", server_ip, ret);
		return;
	}

	/* NTP transmit timestamp is at bytes 40–43 (big-endian seconds
	 * since 1900-01-01).  Subtract 2 208 988 800 to get UNIX epoch. */
	epoch  = ((int64_t)(uint32_t)resp[40] << 24)
	       | ((int64_t)(uint32_t)resp[41] << 16)
	       | ((int64_t)(uint32_t)resp[42] << 8)
	       |  (int64_t)(uint32_t)resp[43];
	epoch -= 2208988800U;

	k_mutex_lock(&g_clock_mutex, K_FOREVER);
	g_base_utc_secs  = epoch;
	g_base_uptime_ms = k_uptime_get_32();
	g_synced = true;
	k_mutex_unlock(&g_clock_mutex);

	LOG_INF("NTP sync: %s → UTC %lld", server_ip, (long long)epoch);
}

/* ── Force re-sync (tries all PTB servers) ──────────────────────────── */

void ntp_force_resync(void)
{
	/* Hard-coded PTB NTP server IPs — the SD-card config may
	 * contain empty or garbage NTP hostnames. */
	static const char *servers[] = {
		"192.53.103.108", /* ptbtime1.ptb.de */
		"192.53.103.104", /* ptbtime2.ptb.de */
		"192.53.103.103", /* ptbtime3.ptb.de */
	};

	for (int i = 0; i < 3 && !g_synced; i++) {
		ntp_query(servers[i]);
	}
}

/* ── NTP service thread ─────────────────────────────────────────────── */

void ntp_service(void)
{
	clock_seed();

	/* Let the network stack fully initialise before the first query */
	k_sleep(K_MSEC(NTP_STARTUP_DELAY_MS));

	while (1) {
		LOG_DBG("ntp: up=%d seed=%d sync=%d",
			net_is_up(), (int)g_seeded, (int)g_synced);

		if (net_is_up() &&
		    (k_uptime_get_32() - g_last_sync_ms >= NTP_SYNC_INTERVAL_MS ||
		     g_last_sync_ms == 0)) {
			g_last_sync_ms = k_uptime_get_32();
			ntp_force_resync();
		}
		k_sleep(K_SECONDS(5));
	}
}

/* ── Civil-from-epoch helpers (Gregorian, 1970–2100) ────────────────── */

static int is_leap_year(int y)
{
	return ((y % 4) == 0 && (y % 100) != 0) || (y % 400) == 0;
}

static int days_in_month(int y, int m)
{
	static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return dim[m - 1] + (m == 2 && is_leap_year(y) ? 1 : 0);
}

static int day_of_week(int y, int m, int d)
{
	int days = 4; /* 1970-01-01 was Thursday */
	for (int yy = 1970; yy < y; yy++) {
		days += is_leap_year(yy) ? 366 : 365;
	}
	for (int mm = 1; mm < m; mm++) {
		days += days_in_month(y, mm);
	}
	days += d - 1;
	return days % 7;
}

static int last_sunday(int y, int m)
{
	int dim = days_in_month(y, m);
	return dim - day_of_week(y, m, dim);
}

/* ── Central-European DST detection (CET/CEST) ──────────────────────── */

static bool is_cest(int64_t utc_secs)
{
	int64_t days = utc_secs / 86400;
	int     y    = 1970;
	int64_t ds   = days;

	/* Find the year */
	while (ds >= (is_leap_year(y) ? 366 : 365)) {
		ds -= is_leap_year(y) ? 366 : 365;
		y++;
	}

	/* March: epoch of last Sunday 01:00 UTC */
	int64_t mar_days = 0;
	for (int yy = 1970; yy < y; yy++) mar_days += is_leap_year(yy) ? 366 : 365;
	for (int mm = 1; mm < 3; mm++)      mar_days += days_in_month(y, mm);
	mar_days += last_sunday(y, 3) - 1;
	int64_t mar_epoch = mar_days * 86400 + 3600;

	/* October: epoch of last Sunday 01:00 UTC */
	int64_t oct_days = 0;
	for (int yy = 1970; yy < y; yy++) oct_days += is_leap_year(yy) ? 366 : 365;
	for (int mm = 1; mm < 10; mm++)     oct_days += days_in_month(y, mm);
	oct_days += last_sunday(y, 10) - 1;
	int64_t oct_epoch = oct_days * 86400 + 3600;

	return utc_secs >= mar_epoch && utc_secs < oct_epoch;
}

/* ── Epoch → civil date (y, m, d, hh, mm, ss) ──────────────────────── */

static void epoch_to_civil(int64_t secs, int offset_h,
			   int *y, int *m, int *d, int *hh, int *mm, int *ss)
{
	secs += offset_h * 3600;
	int64_t days = secs / 86400;
	int64_t tod  = secs % 86400;

	*y = 1970;
	while (days >= (is_leap_year(*y) ? 366 : 365)) {
		days -= is_leap_year(*y) ? 366 : 365;
		(*y)++;
	}
	for (*m = 1; *m <= 12; (*m)++) {
		int dim = days_in_month(*y, *m);
		if (days < dim) break;
		days -= dim;
	}
	*d  = (int)days + 1;
	*hh = (int)(tod / 3600);
	*mm = (int)((tod % 3600) / 60);
	*ss = (int)(tod % 60);
}

/* ── Update the UI time/date globals (called every second) ──────────── */

bool time_update_gui_globals(void)
{
	int y, mo, d, hh, mm, ss;

	if (!g_seeded) {
		return false;
	}

	int64_t utc    = utc_now();
	int     offset = is_cest(utc) ? 2 : 1;

	epoch_to_civil(utc, offset, &y, &mo, &d, &hh, &mm, &ss);

	snprintf(g_ui_time_str, sizeof(g_ui_time_str),
		 "%02d:%02d:%02d", hh, mm, ss);
	snprintf(g_ui_date_str, sizeof(g_ui_date_str),
		 "%02d.%02d.%04d", d, mo, y);
	g_now_minutes = (uint16_t)(hh * 60 + mm);

	return true;
}
