/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GT911 touch driver for the Waveshare ESP32-P4-NANO 10.1" DSI
 * display, polled via I2C.
 *
 * The board does not route the GT911 INT/RST pins (the Arduino reference
 * drives the controller with GPIO_NUM_NC for both), so this driver polls
 * the touch status register on a timer instead of using the GPIO
 * interrupt path of the mainline input_gt911 driver (which requires an
 * irq-gpios property).
 *
 * Coordinates are reported through input_touchscreen_report_pos(), so the
 * touchscreen-common devicetree properties (inverted-x/y, swapped-x-y,
 * screen dimensions) apply the rotation/mirroring needed to align the
 * touch plane with the LVGL logical display space.
 */

#define DT_DRV_COMPAT waveshare_gt911

#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gt911_poll, CONFIG_INPUT_LOG_LEVEL);

/* GT911 registers (16-bit addresses, sent LSB first) */
#define GT911_REG_STATUS  BSWAP_16(0x814EU)
#define GT911_REG_POINT_0 0x814FU
#define GT911_POINT_OFFSET 0x8U
#define GT911_REG_POINT_ADDR(n) BSWAP_16(GT911_REG_POINT_0 + GT911_POINT_OFFSET * (n))

#define GT911_TOUCH_STATUS_MSK (1 << 7U)
#define GT911_TOUCH_POINTS_MSK 0x0FU
#define GT911_MAX_POINTS       5U

/** Point register layout (8 bytes per point). */
struct gt911_point_reg {
	uint8_t id;        /*!< Track ID. */
	uint8_t low_x;     /*!< Low byte of x coordinate. */
	uint8_t high_x;    /*!< High byte of x coordinate. */
	uint8_t low_y;     /*!< Low byte of y coordinate. */
	uint8_t high_y;    /*!< High byte of y coordinate. */
	uint8_t low_size;  /*!< Low byte of point size. */
	uint8_t high_size; /*!< High byte of point size. */
	uint8_t reserved;  /*!< Reserved. */
};

struct gt911_poll_config {
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec bus;
	uint16_t poll_period_ms;
};

struct gt911_poll_data {
	const struct device *dev;
	struct k_timer timer;
	struct k_work work;
	bool pressed;
	uint16_t last_x;
	uint16_t last_y;
};

static int gt911_i2c_read(const struct device *dev, uint16_t reg, uint8_t *buf, uint8_t len)
{
	const struct gt911_poll_config *cfg = dev->config;
	uint16_t reg_le = sys_cpu_to_le16(reg);

	return i2c_write_read_dt(&cfg->bus, &reg_le, sizeof(reg_le), buf, len);
}

static int gt911_i2c_clear_status(const struct device *dev)
{
	const struct gt911_poll_config *cfg = dev->config;
	uint8_t buf[3] = {(uint8_t)(GT911_REG_STATUS & 0xFF),
			  (uint8_t)(GT911_REG_STATUS >> 8), 0};

	return i2c_write_dt(&cfg->bus, buf, sizeof(buf));
}

static void gt911_process(struct k_work *work)
{
	struct gt911_poll_data *data = CONTAINER_OF(work, struct gt911_poll_data, work);
	const struct device *dev = data->dev;
	uint8_t status;
	int r;

	r = gt911_i2c_read(dev, GT911_REG_STATUS, &status, 1);
	if (r < 0) {
		LOG_WRN("status read failed: %d", r);
		return;
	}

	if (!(status & GT911_TOUCH_STATUS_MSK)) {
		return;
	}

	uint8_t points = status & GT911_TOUCH_POINTS_MSK;

	if (points == 0) {
		gt911_i2c_clear_status(dev);
		if (data->pressed) {
			data->pressed = false;
			input_touchscreen_report_pos(dev, data->last_x, data->last_y, K_FOREVER);
			input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
		}
		return;
	}

	if (points > GT911_MAX_POINTS) {
		gt911_i2c_clear_status(dev);
		return;
	}

	struct gt911_point_reg point_reg;

	r = gt911_i2c_read(dev, GT911_REG_POINT_ADDR(0), (uint8_t *)&point_reg,
			   sizeof(point_reg));
	if (r < 0) {
		LOG_WRN("point read failed: %d", r);
		return;
	}

	/* Clear the touch status so the next report is a fresh one */
	gt911_i2c_clear_status(dev);

	uint16_t x = ((uint16_t)point_reg.high_x << 8U) | point_reg.low_x;
	uint16_t y = ((uint16_t)point_reg.high_y << 8U) | point_reg.low_y;

	data->last_x = x;
	data->last_y = y;

	/* Publish position before press so LVGL never hit-tests the previous tap. */
	input_touchscreen_report_pos(dev, x, y, K_FOREVER);
	if (!data->pressed) {
		data->pressed = true;
		input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
	}
}

static void gt911_timer_handler(struct k_timer *timer)
{
	struct gt911_poll_data *data = CONTAINER_OF(timer, struct gt911_poll_data, timer);

	k_work_submit(&data->work);
}

static int gt911_poll_init(const struct device *dev)
{
	const struct gt911_poll_config *cfg = dev->config;
	struct gt911_poll_data *data = dev->data;

	if (!i2c_is_ready_dt(&cfg->bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	data->dev = dev;
	k_work_init(&data->work, gt911_process);
	k_timer_init(&data->timer, gt911_timer_handler, NULL);
	k_timer_start(&data->timer, K_MSEC(cfg->poll_period_ms), K_MSEC(cfg->poll_period_ms));

	LOG_INF("GT911 touch ready (polled every %u ms)", cfg->poll_period_ms);
	return 0;
}

#define GT911_POLL_INIT(index)                                                                     \
	static const struct gt911_poll_config gt911_poll_config_##index = {                        \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index),                           \
		.bus = I2C_DT_SPEC_INST_GET(index),                                                \
		.poll_period_ms = DT_INST_PROP_OR(index, poll-period-ms, 10),                      \
	};                                                                                         \
	static struct gt911_poll_data gt911_poll_data_##index;                                     \
	DEVICE_DT_INST_DEFINE(index, gt911_poll_init, NULL, &gt911_poll_data_##index,              \
			      &gt911_poll_config_##index, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(GT911_POLL_INIT)
