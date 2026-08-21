/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jadard JD9365 MIPI-DSI panel driver, for the Waveshare 10.1" DSI touch
 * panel used with the ESP32-P4-NANO board.
 *
 * Mirrors the sequencing used by display_st7701.c: attach() to the MIPI-DSI
 * host first (with MIPI_DSI_MODE_VIDEO already set in mode_flags), then send
 * the panel's DCS init sequence - real video-mode DSI hosts interleave LP
 * commands into blanking intervals even while nominally "in video mode" (see
 * drivers/mipi_dsi/dsi_esp32.c and PLAN.md 2.4).
 *
 * This driver has no Zephyr-subsystem API of its own (matching
 * display_waveshare_dsi2dpi.c's pattern): it is purely an init-time actor.
 * The actual zephyr,display device is the sibling
 * espressif,esp32-mipi-dsi-dpi node (drivers/display/display_esp32_dpi.c),
 * which must be ready *before* this driver's init() runs so that the DMA is
 * already feeding the bridge (see PLAN.md 4 for the init-priority chain).
 */

#define DT_DRV_COMPAT jadard_jd9365

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/logging/log.h>

#include "../mipi_dsi/dsi_esp32.h"

LOG_MODULE_REGISTER(jd9365, CONFIG_DISPLAY_LOG_LEVEL);

struct jd9365_init_cmd {
	uint8_t cmd;
	uint8_t param;
	uint16_t delay_ms;
};

/*
 * Ported verbatim (programmatically extracted, not hand-transcribed) from
 * the working Arduino/ESP-IDF reference implementation for this exact panel:
 * arduino-display/displays/displays_config.h,
 * vendor_specific_init_default[] for SCREEN_10_1_DSI_TOUCH_A (200 single
 * data-byte DCS writes: JD9365 page-select preamble, page 1 power/gamma,
 * page 2 GOA/GIP mapping, page 4 timing tweaks, then sleep-out (+120ms) and
 * display-on (+20ms)). See PLAN.md 2.1.
 */
static const struct jd9365_init_cmd jd9365_init_cmds[] = {
#include "display_jd9365_cmds.inc"
};

struct jd9365_config {
	const struct device *mipi_dsi;
	uint8_t channel;
	uint8_t data_lanes;
	uint16_t width;
	uint16_t height;
	uint32_t hbp;
	uint32_t hfp;
	uint32_t hsync;
	uint32_t vbp;
	uint32_t vfp;
	uint32_t vsync;
};

static int jd9365_init(const struct device *dev)
{
	const struct jd9365_config *cfg = dev->config;
	struct mipi_dsi_device mdev = {0};
	int ret;

	mdev.data_lanes = cfg->data_lanes;
	mdev.pixfmt = MIPI_DSI_PIXFMT_RGB565;
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	mdev.timings.hactive = cfg->width;
	mdev.timings.hbp = cfg->hbp;
	mdev.timings.hfp = cfg->hfp;
	mdev.timings.hsync = cfg->hsync;
	mdev.timings.vactive = cfg->height;
	mdev.timings.vbp = cfg->vbp;
	mdev.timings.vfp = cfg->vfp;
	mdev.timings.vsync = cfg->vsync;

	ret = mipi_dsi_attach(cfg->mipi_dsi, cfg->channel, &mdev);
	if (ret < 0) {
		LOG_ERR("MIPI-DSI attach failed (%d)", ret);
		return ret;
	}

	for (size_t i = 0; i < ARRAY_SIZE(jd9365_init_cmds); i++) {
		const struct jd9365_init_cmd *c = &jd9365_init_cmds[i];

		ret = mipi_dsi_dcs_write(cfg->mipi_dsi, cfg->channel, c->cmd, &c->param, 1);
		if (ret < 0) {
			LOG_ERR("DCS write 0x%02x=0x%02x failed (%d)", c->cmd, c->param, ret);
			return ret;
		}
		if (c->delay_ms != 0) {
			k_msleep(c->delay_ms);
		}
	}

	/*
	 * TEST (see PLAN.md): the panel currently renders the LVGL content
	 * with inverted polarity (dark background shows as bright, NCC ~ -0.6
	 * vs a blank baseline photo). The Arduino reference never sends DCS
	 * 0x20/0x21, so the inversion is expected to come from the host/bridge
	 * data-path config, not the panel - but this is the cheapest possible
	 * empirical test of the panel-side hypothesis (0x20 = Display
	 * Inversion OFF).
	 */
	{
		uint8_t inv_off = 0x00;

		mipi_dsi_dcs_write(cfg->mipi_dsi, cfg->channel, 0x20, &inv_off, 1);
		k_msleep(20);
	}

	ret = esp32_mipi_dsi_start_video(cfg->mipi_dsi);
	if (ret < 0) {
		LOG_ERR("failed to start DSI video mode (%d)", ret);
		return ret;
	}

	LOG_INF("JD9365 panel initialised: %ux%u, %u data lane(s)", cfg->width, cfg->height,
		cfg->data_lanes);

	return 0;
}

#define JD9365_DEFINE(inst)                                                                       \
	static const struct jd9365_config jd9365_cfg_##inst = {                                  \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_BUS(inst)),                                     \
		.channel = DT_INST_REG_ADDR(inst),                                                \
		.data_lanes = DT_INST_PROP_BY_IDX(inst, data_lanes, 0),                           \
		.width = DT_INST_PROP(inst, width),                                               \
		.height = DT_INST_PROP(inst, height),                                             \
		.hbp = DT_PROP(DT_INST_CHILD(inst, display_timings), hback_porch),                \
		.hfp = DT_PROP(DT_INST_CHILD(inst, display_timings), hfront_porch),               \
		.hsync = DT_PROP(DT_INST_CHILD(inst, display_timings), hsync_len),                \
		.vbp = DT_PROP(DT_INST_CHILD(inst, display_timings), vback_porch),                \
		.vfp = DT_PROP(DT_INST_CHILD(inst, display_timings), vfront_porch),               \
		.vsync = DT_PROP(DT_INST_CHILD(inst, display_timings), vsync_len),                \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, jd9365_init, NULL, NULL, &jd9365_cfg_##inst, POST_KERNEL, 70, \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(JD9365_DEFINE)
