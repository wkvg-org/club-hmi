/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr shell commands for the club-hmi dashboard.
 * Provides serial-console DMX control via MQTT, LCD backlight,
 * and network status.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../net/dmx_zbus.h"
#include "../config/settings.h"
#include "../net/net.h"
#include "../ui/ui.h"

/* ── colour name table (matches the PicoSpot 45 fixture) ─────────────────── */

struct color_name {
	const char *name;
	uint8_t     value;
};

static const struct color_name colors[] = {
	{"white",   5},
	{"red",    16},
	{"orange", 27},
	{"yellow", 38},
	{"green",  49},
	{"blue",   60},
	{"cyan",   71},
	{"purple", 82},
};

/* ── DMX colour ──────────────────────────────────────────────────────────── */

static int cmd_dmx_color(const struct shell *sh, size_t argc, char **argv)
{
	struct dmx_cmd cmd = { .type = DMX_CMD_COLOR, .addr = 1 };

	/* name lookup */
	for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
		if (strcmp(argv[1], colors[i].name) == 0) {
		cmd.val = colors[i].value;
		zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
		shell_print(sh, "dmx color: %s (%u)",
				    colors[i].name, colors[i].value);
			return 0;
		}
	}

	/* numeric value 0..255 */
	char *end;
	errno = 0;
	long v = strtol(argv[1], &end, 0);

	if (errno || *end != '\0' || v < 0 || v > 255) {
		shell_error(sh, "unknown colour; use name or 0..255");
		return -EINVAL;
	}
	cmd.val = (uint8_t)v;
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
	shell_print(sh, "dmx color: %ld", v);
	return 0;
}

/* ── DMX dimmer ──────────────────────────────────────────────────────────── */

static int cmd_dmx_dimmer(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	errno = 0;
	long v = strtol(argv[1], &end, 0);

	if (errno || *end != '\0' || v < 0 || v > 255) {
		shell_error(sh, "dimmer must be 0..255");
		return -EINVAL;
	}
	struct dmx_cmd cmd = { .type = DMX_CMD_DIMMER, .addr = 1, .val = (uint8_t)v };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
	shell_print(sh, "dmx dimmer: %ld", v);
	return 0;
}

/* ── DMX blackout / home ─────────────────────────────────────────────────── */

static int cmd_dmx_blackout(const struct shell *sh, size_t argc, char **argv)
{
	struct dmx_cmd cmd = { .type = DMX_CMD_BLACKOUT, .addr = 1 };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
	shell_print(sh, "dmx blackout");
	return 0;
}

static int cmd_dmx_home(const struct shell *sh, size_t argc, char **argv)
{
	struct dmx_cmd cmd = { .type = DMX_CMD_HOME, .addr = 1 };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
	shell_print(sh, "dmx home");
	return 0;
}

static int cmd_dmx_fixture(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: dmx fixture <on|off>");
		return -EINVAL;
	}
	uint8_t val;
	if (strcmp(argv[1], "on") == 0) {
		val = 255;
	} else if (strcmp(argv[1], "off") == 0) {
		val = 0;
	} else {
		shell_error(sh, "use on or off");
		return -EINVAL;
	}
	struct dmx_cmd cmd = { .type = DMX_CMD_FIXTURE, .addr = 19, .extra = val };
	zbus_chan_pub(&dmx_chan, &cmd, K_NO_WAIT);
	shell_print(sh, "dmx fixture: %s", argv[1]);
	return 0;
}

/* ── Backlight ───────────────────────────────────────────────────────────── */

static int cmd_backlight(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	errno = 0;
	long v = strtol(argv[1], &end, 0);

	if (errno || *end != '\0' || v < 0 || v > 100) {
		shell_error(sh, "backlight must be 0..100 %%");
		return -EINVAL;
	}
	settings_set_backlight_percent((uint8_t)v);
	shell_print(sh, "backlight: %ld %%", v);
	return 0;
}

/* ── Network status ──────────────────────────────────────────────────────── */

#include <zephyr/net/net_if.h>

static int cmd_net_status(const struct shell *sh, size_t argc, char **argv)
{
	bool up = net_is_up();
	shell_print(sh, "network: %s", up ? "UP" : "DOWN");
	return 0;
}

/* ── UI control ──────────────────────────────────────────────────────────── */

static const char *tab_names[] = {
	"overview", "pager", "light", "alarm", "settings", "dmx", NULL
};

static int cmd_ui_tab(const struct shell *sh, size_t argc, char **argv)
{
	for (int i = 0; tab_names[i]; i++) {
		if (strcmp(argv[1], tab_names[i]) == 0) {
			ui_switch_tab(i);
			shell_print(sh, "ui tab: %s", tab_names[i]);
			return 0;
		}
	}
	shell_error(sh, "unknown tab; use: %s",
		    "overview pager light alarm settings dmx");
	return -EINVAL;
}

static int cmd_ui_alarm(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: ui alarm <on|off>");
		return -EINVAL;
	}
	if (strcmp(argv[1], "on") == 0) {
		ui_set_alarm_notification(true);
		shell_print(sh, "ui alarm: on");
	} else if (strcmp(argv[1], "off") == 0) {
		ui_set_alarm_notification(false);
		shell_print(sh, "ui alarm: off");
	} else {
		shell_error(sh, "use on or off");
		return -EINVAL;
	}
	return 0;
}

/* ── Subcommand registration ─────────────────────────────────────────────── */

SHELL_STATIC_SUBCMD_SET_CREATE(dmx_subcmds,
	SHELL_CMD_ARG(color,    NULL, "<name|0..255>",  cmd_dmx_color,    2, 0),
	SHELL_CMD_ARG(dimmer,   NULL, "<0..255>",        cmd_dmx_dimmer,   2, 0),
	SHELL_CMD(    blackout, NULL, "dimmer=0",        cmd_dmx_blackout),
	SHELL_CMD(    home,     NULL, "home position",   cmd_dmx_home),
	SHELL_CMD_ARG(fixture,  NULL, "<on|off>",        cmd_dmx_fixture,  2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(dmx, &dmx_subcmds, "DMX moving-head control", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(net_subcmds,
	SHELL_CMD(status, NULL, "show network status", cmd_net_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(net, &net_subcmds, "Network info", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(ui_subcmds,
	SHELL_CMD_ARG(tab,   NULL, "<overview|pager|light|alarm|settings|dmx>",
		      cmd_ui_tab,   2, 0),
	SHELL_CMD_ARG(alarm, NULL, "<on|off>",
		      cmd_ui_alarm, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ui, &ui_subcmds, "UI control", NULL);

SHELL_CMD_ARG_REGISTER(backlight, NULL, "<0..100>", cmd_backlight, 2, 0);
