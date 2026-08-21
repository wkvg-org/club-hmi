/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Directus CMS backend, ported from the Arduino bar-dashboard
 * (directus.cpp): fetches the day's acts over HTTPS and feeds them to
 * the overview tab. TLS verification is disabled (the reference uses
 * setInsecure()), see PLAN.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/poll.h>
#include <zephyr/data/json.h>
#include <errno.h>

#include "../config/bar_config.h"
#include "directus.h"
#include "directus_zbus.h"
#include "net.h"

LOG_MODULE_REGISTER(directus, LOG_LEVEL_INF);

#define DIRECTUS_COLLECTION  "acts"
#define DIRECTUS_SYNC_PERIOD_MS (60UL * 60UL * 1000UL)
#define DIRECTUS_HTTP_TIMEOUT_MS 15000

static char g_acts_buf[8192];
static size_t g_acts_len;

/* Directus response: { "data": [ { "name": ..., "start": ..., "end": ... } ] } */
struct act_json {
	char name[32];
	char start[32];
	char end[32];
};

struct acts_file {
	struct act_json data[MAX_ACTS];
	size_t data_len;
};

static const struct json_obj_descr act_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct act_json, name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct act_json, start, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct act_json, end, JSON_TOK_STRING),
};

static const struct json_obj_descr acts_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY(struct acts_file, data, MAX_ACTS, data_len, act_descr,
				 ARRAY_SIZE(act_descr)),
};

static uint8_t g_recv_buf[512];

static int http_response_cb(struct http_response *rsp, enum http_final_call final_data,
			    void *user_data)
{
	if (rsp->body_frag_len > 0 &&
	    g_acts_len + rsp->body_frag_len < sizeof(g_acts_buf)) {
		memcpy(g_acts_buf + g_acts_len, rsp->body_frag_start, rsp->body_frag_len);
		g_acts_len += rsp->body_frag_len;
	}

	(void)final_data;
	(void)user_data;
	return 0;
}

static bool build_url(char *url, size_t url_sz)
{
	/* The config prefers a literal IP over the DNS name. */
	const char *host = get_directus_server_domain();
	const char *ip = get_directus_server_ip();
	uint16_t port = get_directus_port();

	if (ip[0] != '\0') {
		snprintf(url, url_sz, "https://%s:%u/items/%s?sort=start", ip, port,
			 DIRECTUS_COLLECTION);
		return true;
	}

	snprintf(url, url_sz, "https://%s:%u/items/%s?sort=start", host, port,
		 DIRECTUS_COLLECTION);
	return true;
}

static bool parse_acts(void)
{
	struct acts_file file;
	act_t out[MAX_ACTS];
	int ret;
	uint8_t count = 0;

	memset(&file, 0, sizeof(file));
	ret = json_obj_parse(g_acts_buf, g_acts_len, acts_descr, ARRAY_SIZE(acts_descr), &file);
	if (ret < 0) {
		LOG_WRN("acts JSON parse failed: %d", ret);
		return false;
	}

	count = (uint8_t)file.data_len;
	if (count > MAX_ACTS) {
		count = MAX_ACTS;
	}

	for (int i = 0; i < count; i++) {
		uint16_t smin = 0;
		uint16_t emin = 0;
		/*
		 * Parse ISO 8601 timestamps by hard-coded byte offset.
		 * Directus returns "YYYY-MM-DDTHH:MM:SS" — the hour and
		 * minute are at fixed positions [11..15].
		 */
		int sh = (file.data[i].start[11] - '0') * 10 + (file.data[i].start[12] - '0');
		int sm = (file.data[i].start[14] - '0') * 10 + (file.data[i].start[15] - '0');
		int eh = (file.data[i].end[11] - '0') * 10 + (file.data[i].end[12] - '0');
		int em = (file.data[i].end[14] - '0') * 10 + (file.data[i].end[15] - '0');

		if (sh < 0 || sh > 23 || sm < 0 || sm > 59 || eh < 0 || eh > 23 || em < 0 ||
		    em > 59) {
			continue;
		}
		smin = (uint16_t)(sh * 60 + sm);
		emin = (uint16_t)(eh * 60 + em);

		strncpy(out[i].name, file.data[i].name, sizeof(out[i].name) - 1);
		out[i].name[sizeof(out[i].name) - 1] = '\0';
		out[i].start_min = smin;
		out[i].end_min = emin;
	}

	LOG_INF("acts: %u entries", count);
	struct directus_msg msg = { .count = count };
	for (uint8_t i = 0; i < count; i++) {
		msg.acts[i] = out[i];
	}
	zbus_chan_pub(&directus_chan, &msg, K_NO_WAIT);
	return count > 0;
}

bool directus_download_acts(void)
{
	struct http_request req;
	static char port_str[8];
	static const char *headers[2];
	static char auth[320];
	char url[256];
	int ret;
	int sock;

	memset(&req, 0, sizeof(req));
	build_url(url, sizeof(url));

	g_acts_len = 0;

	/*
	 * Verify the server is reachable before attempting TLS.
	 * A plain TCP connect avoids the mbedTLS crash path that
	 * occurs when TLS negotiates against an absent server.
	 */
	{
		int test = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (test < 0) {
			LOG_WRN("directus test socket failed");
			return false;
		}

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port   = htons(get_directus_port());
		const char *ip = get_directus_server_ip();
		if (ip[0] == '\0') {
			ip = get_directus_server_domain();
		}
		inet_pton(AF_INET, ip, &addr.sin_addr);

		struct zsock_pollfd pfd = { .fd = test, .events = ZSOCK_POLLOUT };
		ret = zsock_connect(test, (struct sockaddr *)&addr, sizeof(addr));
		if (ret < 0 && errno != EINPROGRESS) {
			LOG_WRN("directus server %s:%u unreachable: %d", ip,
				get_directus_port(), errno);
			zsock_close(test);
			return false;
		}
		ret = zsock_poll(&pfd, 1, 3000);
		zsock_close(test);
		if (ret <= 0) {
			LOG_WRN("directus server %s:%u timeout", ip,
				get_directus_port());
			return false;
		}
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) {
		LOG_WRN("TLS socket failed: %d", errno);
		return false;
	}

	int verify = TLS_PEER_VERIFY_NONE;

	ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (ret < 0) {
		LOG_WRN("TLS verify option failed: %d", errno);
		zsock_close(sock);
		return false;
	}

	snprintf(port_str, sizeof(port_str), "%u", get_directus_port());

	req.method = HTTP_GET;
	req.url = url;
	req.host = get_directus_server_domain();
	req.port = port_str;
	req.protocol = "HTTP/1.1";
	req.response = http_response_cb;
	req.recv_buf = g_recv_buf;
	req.recv_buf_len = sizeof(g_recv_buf);

	headers[1] = NULL;
	if (get_directus_bearer_token()[0] != '\0') {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s",
			 get_directus_bearer_token());
		headers[0] = auth;
		req.header_fields = headers;
	}

	ret = http_client_req(sock, &req, DIRECTUS_HTTP_TIMEOUT_MS, NULL);
	zsock_close(sock);
	if (ret < 0) {
		LOG_WRN("directus GET failed: %d", ret);
		return false;
	}

	return parse_acts();
}

void directus_service(void)
{
	uint32_t last_sync = 0;

	/* Defer the first download: the TLS + DNS work is heavy, and starting
	 * it at the same instant as NTP/MQTT corrupted the scheduler
	 * (store fault in ready_thread). */
	k_sleep(K_SECONDS(30));

	while (1) {
		if (net_is_up() && k_uptime_get_32() - last_sync >= DIRECTUS_SYNC_PERIOD_MS) {
			directus_download_acts();
			last_sync = k_uptime_get_32();
		}
		k_sleep(K_SECONDS(10));
	}
}
