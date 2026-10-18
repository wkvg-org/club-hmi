/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jadard JD9365 MIPI-DSI panel driver, for the Waveshare 10.1" DSI touch
 * panel used with the ESP32-P4-NANO board.
 *
 * Mirrors the sequencing used by display_st7701.c: attach() to the MIPI-DSI
 * host first (with MIPI_DSI_MODE_VIDEO and MIPI_DSI_MODE_LPM set so the
 * upstream host drives the DCS init in low-power mode), then send the
 * panel's DCS init sequence. The upstream ESP32 DSI host starts video mode
 * and the scan-out itself as part of attach(), so there is no separate
 * "start video" step here.
 *
 * This driver has no Zephyr-subsystem API of its own: it is purely an
 * init-time actor. The zephyr,display device is the sibling
 * espressif,esp-dsi-display node (upstream drivers/display/display_esp32_dsi.c).
 */

#define DT_DRV_COMPAT jadard_jd9365

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

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
	int64_t init_start = k_uptime_get();
	int ret;

	printk("jd9365: init start dsi=%s ready=%d channel=%u lanes=%u panel=%ux%u\n",
	       cfg->mipi_dsi->name, device_is_ready(cfg->mipi_dsi), cfg->channel,
	       cfg->data_lanes, cfg->width, cfg->height);
	LOG_INF("JD9365 init start: DSI=%s ready=%d channel=%u lanes=%u panel=%ux%u",
		cfg->mipi_dsi->name, device_is_ready(cfg->mipi_dsi), cfg->channel,
		cfg->data_lanes, cfg->width, cfg->height);

	mdev.data_lanes = cfg->data_lanes;
	mdev.pixfmt = MIPI_DSI_PIXFMT_RGB565;
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;
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
		printk("jd9365: MIPI-DSI attach failed ret=%d channel=%u mode=0x%x\n",
		       ret, cfg->channel, mdev.mode_flags);
		LOG_ERR("MIPI-DSI attach failed (%d): channel=%u lanes=%u mode=0x%x",
			ret, cfg->channel, cfg->data_lanes, mdev.mode_flags);
		return ret;
	}
	printk("jd9365: MIPI-DSI attached channel=%u mode=0x%x lanes=%u\n",
	       cfg->channel, mdev.mode_flags, cfg->data_lanes);
	LOG_INF("MIPI-DSI attached: channel=%u RGB565 mode=0x%x timings h=%u/%u/%u v=%u/%u/%u",
		cfg->channel, mdev.mode_flags, mdev.timings.hactive, mdev.timings.hbp,
		mdev.timings.hfp, mdev.timings.vactive, mdev.timings.vbp,
		mdev.timings.vfp);

	for (size_t i = 0; i < ARRAY_SIZE(jd9365_init_cmds); i++) {
		const struct jd9365_init_cmd *c = &jd9365_init_cmds[i];

		ret = mipi_dsi_dcs_write(cfg->mipi_dsi, cfg->channel, c->cmd, &c->param, 1);
		if (ret < 0) {
			printk("jd9365: DCS init command %u failed cmd=0x%02x val=0x%02x ret=%d\n",
			       (unsigned int)(i + 1), c->cmd, c->param, ret);
			LOG_ERR("DCS init command %zu/%zu 0x%02x=0x%02x failed (%d) after %lld ms",
				i + 1, ARRAY_SIZE(jd9365_init_cmds), c->cmd, c->param,
				ret, k_uptime_get() - init_start);
			return ret;
		}
		if ((i + 1) % 32 == 0 || i + 1 == ARRAY_SIZE(jd9365_init_cmds)) {
			printk("jd9365: DCS init progress %u/%u last=0x%02x\n",
			       (unsigned int)(i + 1),
			       (unsigned int)ARRAY_SIZE(jd9365_init_cmds), c->cmd);
			LOG_INF("JD9365 init progress: %zu/%zu commands, last=0x%02x, elapsed=%lld ms",
				i + 1, ARRAY_SIZE(jd9365_init_cmds), c->cmd,
				k_uptime_get() - init_start);
		}
		if (c->delay_ms != 0) {
			k_msleep(c->delay_ms);
		}
	}

	printk("jd9365: panel initialized %ux%u lanes=%u commands=%u\n",
	       cfg->width, cfg->height, cfg->data_lanes,
	       (unsigned int)ARRAY_SIZE(jd9365_init_cmds));
	LOG_INF("JD9365 panel initialised: %ux%u, %u data lane(s), %zu commands, %lld ms",
		cfg->width, cfg->height, cfg->data_lanes,
		ARRAY_SIZE(jd9365_init_cmds), k_uptime_get() - init_start);

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
	DEVICE_DT_INST_DEFINE(inst, jd9365_init, NULL, NULL, &jd9365_cfg_##inst, POST_KERNEL, 90, \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(JD9365_DEFINE)
