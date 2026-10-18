/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare panel power-sequencing and backlight controller driver.
 *
 * Found on the Waveshare 10.1" MIPI-DSI touch panel used with the
 * ESP32-P4-NANO board: a small on-board microcontroller on I2C address
 * 0x45, *not* a display bridge chip. Register 0x95 sequences the panel's
 * power rails; register 0x96 sets backlight brightness (0-255).
 *
 * Sequence ported verbatim from the working Arduino/ESP-IDF reference
 * (arduino-display/displays/displays_config.cpp: display_init() /
 * set_display_backlight()) - see PLAN.md 2.1. Must run before the JD9365
 * panel driver sends any DCS commands (lowest init priority of the DSI
 * stack), since it powers up the panel itself.
 */

#define DT_DRV_COMPAT waveshare_panel_ctl

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/display/waveshare_panel_ctl.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(waveshare_panel_ctl, CONFIG_DISPLAY_LOG_LEVEL);

#define WAVESHARE_PANEL_CTL_REG_POWER      0x95
#define WAVESHARE_PANEL_CTL_REG_BACKLIGHT  0x96

struct waveshare_panel_ctl_config {
	struct i2c_dt_spec i2c;
};

static int waveshare_panel_ctl_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct waveshare_panel_ctl_config *cfg = dev->config;
	int ret = i2c_reg_write_byte_dt(&cfg->i2c, reg, val);

	if (ret < 0) {
		printk("panelctl: I2C write failed addr=0x%02x reg=0x%02x val=0x%02x ret=%d\n",
		       cfg->i2c.addr, reg, val, ret);
		LOG_ERR("panel controller I2C write failed: addr=0x%02x reg=0x%02x val=0x%02x ret=%d",
			cfg->i2c.addr, reg, val, ret);
	} else {
		printk("panelctl: I2C write addr=0x%02x reg=0x%02x val=0x%02x ok\n",
		       cfg->i2c.addr, reg, val);
	}
	return ret;
}

int waveshare_panel_ctl_set_backlight(const struct device *dev, uint8_t brightness)
{
	return waveshare_panel_ctl_write(dev, WAVESHARE_PANEL_CTL_REG_BACKLIGHT, brightness);
}

static int waveshare_panel_ctl_init(const struct device *dev)
{
	const struct waveshare_panel_ctl_config *cfg = dev->config;
	static const struct {
		uint8_t reg;
		uint8_t val;
	} power_seq[] = {
		{WAVESHARE_PANEL_CTL_REG_POWER, 0x11},
		{WAVESHARE_PANEL_CTL_REG_POWER, 0x17},
		{WAVESHARE_PANEL_CTL_REG_BACKLIGHT, 0x00},
	};
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		printk("panelctl: I2C bus not ready addr=0x%02x\n", cfg->i2c.addr);
		LOG_ERR("I2C bus not ready: controller addr=0x%02x", cfg->i2c.addr);
		return -ENODEV;
	}
	printk("panelctl: init start bus=%s addr=0x%02x\n",
	       cfg->i2c.bus->name, cfg->i2c.addr);
	LOG_INF("panel controller init: I2C bus=%s addr=0x%02x",
		cfg->i2c.bus->name, cfg->i2c.addr);

	for (size_t i = 0; i < ARRAY_SIZE(power_seq); i++) {
		printk("panelctl: power step %u/%u reg=0x%02x val=0x%02x\n",
		       (unsigned int)(i + 1), (unsigned int)ARRAY_SIZE(power_seq),
		       power_seq[i].reg, power_seq[i].val);
		LOG_INF("panel power sequence %zu/%zu: reg=0x%02x val=0x%02x",
			i + 1, ARRAY_SIZE(power_seq), power_seq[i].reg, power_seq[i].val);
		ret = waveshare_panel_ctl_write(dev, power_seq[i].reg, power_seq[i].val);
		if (ret < 0) {
			LOG_ERR("power sequence write 0x%02x=0x%02x failed (%d)",
				power_seq[i].reg, power_seq[i].val, ret);
			return ret;
		}
		k_msleep(100);
	}

	printk("panelctl: init complete backlight=0 (held off until first frame)\n");
	LOG_INF("Waveshare panel power controller ready: backlight held off until first frame");

	return 0;
}

#define WAVESHARE_PANEL_CTL_DEFINE(inst)                                                          \
	static const struct waveshare_panel_ctl_config waveshare_panel_ctl_cfg_##inst = {         \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, waveshare_panel_ctl_init, NULL, NULL,                          \
			      &waveshare_panel_ctl_cfg_##inst, POST_KERNEL, 55, NULL);

DT_INST_FOREACH_STATUS_OKAY(WAVESHARE_PANEL_CTL_DEFINE)
