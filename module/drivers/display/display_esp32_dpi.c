/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * DSI DPI framebuffer scan-out driver for the Espressif ESP32-P4.
 *
 * Owns a PSRAM framebuffer and a single dedicated DW-GDMA channel that
 * continuously scans it into the DSI bridge's push FIFO via a *circular*,
 * single-node link list (the node's "next" pointer points back to itself),
 * so the hardware refreshes the panel forever with no CPU/interrupt
 * involvement once armed. This mirrors the real ESP-IDF esp_lcd_dpi_panel
 * driver's use of dw_gdma_channel_use_link_list() with
 * DW_GDMA_LINKED_LIST_TYPE_CIRCULAR (confirmed by inspecting the compiled
 * esp_lcd_panel_dpi.c.obj in the ESP-IDF release bundled under
 * arduino-display/ - see PLAN.md 2.3).
 *
 * This is the zephyr,display chosen device: it implements display_write()
 * (copy + cache flush into the framebuffer) and blanking_on/off. It is a
 * separate device/driver from the DSI host (drivers/mipi_dsi/dsi_esp32.c)
 * even though it touches the same "bridge" register bank, because it owns a
 * genuinely distinct resource (memory + DMA) - see PLAN.md section 4 for the
 * full rationale, including the DW-GDMA base address
 * (0x50081000, confirmed against the real ESP-IDF linker script) which the
 * HAL resolves automatically via the "DW_GDMA" linker-provided symbol.
 *
 * No interrupts are used in this first version; FIFO-underrun recovery is
 * left for a later hardening pass (see PLAN.md section 6).
 */

#define DT_DRV_COMPAT espressif_esp32_mipi_dsi_dpi

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/dt-bindings/mipi_dsi/mipi_dsi.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/logging/log.h>

/*
 * __DECLARE_RCC_ATOMIC_ENV is already provided as a global compile
 * definition by the hal_espressif Zephyr glue (see the build command line);
 * no local placeholder needed here.
 *
 * dw_gdma_ll.h calls abort() in a few "invalid enum value" default cases
 * without including <stdlib.h> itself (fine under ESP-IDF, where it is
 * pulled in transitively; not fine in this Zephyr module build) - included
 * above to provide the declaration.
 */
#include <hal/dw_gdma_ll.h>
#include <hal/mipi_dsi_brg_ll.h>

LOG_MODULE_REGISTER(display_esp32_dpi, CONFIG_DISPLAY_LOG_LEVEL);

#define DPI_ESP32_DMA_CHANNEL     0
#define DPI_ESP32_TRANS_WIDTH     DW_GDMA_TRANS_WIDTH_64
#define DPI_ESP32_TRANS_ITEM_SIZE 8U /* bytes per 64-bit transfer item */
#define DPI_ESP32_SRC_BURST_ITEMS DW_GDMA_BURST_ITEMS_512
#define DPI_ESP32_DST_BURST_ITEMS DW_GDMA_BURST_ITEMS_256
#define DPI_ESP32_BURST_LEN       16U

struct display_esp32_dpi_config {
	uintptr_t bridge_base;
	uint16_t width;
	uint16_t height;
};

struct display_esp32_dpi_data {
	uint8_t *framebuffer;
	size_t framebuffer_size;
	bool blanked;
	bool dma_armed;
	const struct display_esp32_dpi_config *dpi_cfg;
	uint16_t logical_width;
	uint16_t logical_height;
	uint16_t rotation; /* 0/90/180/270, clockwise; uint16_t so 270 does not truncate */
};

static inline dsi_brg_dev_t *dpi_bridge(const struct display_esp32_dpi_config *cfg)
{
	return (dsi_brg_dev_t *)cfg->bridge_base;
}

/*
 * Continuous-frame DMA design (the fix for the ~1 Hz tear sweep, see
 * PLAN.md section 8.7):
 *
 * The DW-GDMA channel runs in RELOAD multi-block mode with the channel's
 * shadow-valid marker set. In this mode the channel re-loads the same
 * block from its shadow registers when the previous block completes and
 * keeps running FOREVER - no LLI, no llp0 writes, no re-arms, no
 * transfer boundaries in the data stream. The bridge keeps consuming the
 * FIFO at the video rate, and the DMA keeps refilling it.
 *
 * The only per-frame action left is re-loading the bridge's frame
 * counter (raw_num_total_set), without which the bridge stops consuming
 * after one frame. A low-priority polling thread watches the channel's
 * BLOCK_TFR_DONE status bit: it clears it and reloads the counter within
 * ~1 ms of each block end (the FIFO's ~50 us cover is not needed - the
 * counter reload only gates the bridge's *request* signal; the data in
 * the FIFO keeps flowing to the host regardless, and the DMA is already
 * delivering the next block by the time the reload lands).
 *
 * This eliminates the transfer-boundary/panel-frame phase drift that
 * made the tear band sweep the panel once per ~1.2 s (the "flashing").
 */
static uint32_t dpi_frame_items;

static void display_esp32_dpi_arm_dma(struct display_esp32_dpi_data *data)
{
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);
	const struct display_esp32_dpi_config *cfg = data->dpi_cfg;

	/*
	 * One-time arm: reload the bridge frame counter and start the
	 * channel. After this the channel auto-reloads its block forever;
	 * the polling thread only re-loads the bridge counter on each
	 * block completion.
	 */
	mipi_dsi_brg_ll_set_num_pixel_bits(dpi_bridge(cfg),
					    (uint32_t)cfg->width * cfg->height * 16U);
	dw_gdma_ll_channel_clear_intr(dma, DPI_ESP32_DMA_CHANNEL, 0xFFFFFFFF);
	dw_gdma_ll_channel_enable(dma, DPI_ESP32_DMA_CHANNEL, true);
}

static void dpi_poll_entry(void *arg1, void *arg2, void *arg3)
{
	struct display_esp32_dpi_data *data = arg1;
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);

	while (1) {
		uint32_t st = dw_gdma_ll_channel_get_intr_status(dma, DPI_ESP32_DMA_CHANNEL);

		if (data->dma_armed && (st & DW_GDMA_LL_CHANNEL_EVENT_BLOCK_TFR_DONE)) {
			dw_gdma_ll_channel_clear_intr(dma, DPI_ESP32_DMA_CHANNEL,
						      DW_GDMA_LL_CHANNEL_EVENT_BLOCK_TFR_DONE);
			mipi_dsi_brg_ll_set_num_pixel_bits(dpi_bridge(data->dpi_cfg),
							  dpi_frame_items);
		}
		k_sleep(K_MSEC(1));
	}
}

#define DPI_ESP32_POLL_STACK_SIZE 1024

static K_THREAD_STACK_DEFINE(dpi_poll_stack, DPI_ESP32_POLL_STACK_SIZE);
static struct k_thread dpi_poll_thread;

static void dpi_start_poll_thread(struct display_esp32_dpi_data *data)
{
	k_thread_create(&dpi_poll_thread, dpi_poll_stack, DPI_ESP32_POLL_STACK_SIZE,
			dpi_poll_entry, data, NULL, NULL, 6, 0, K_NO_WAIT);
}

static void display_esp32_dpi_configure_dma(const struct display_esp32_dpi_config *cfg,
					     struct display_esp32_dpi_data *data)
{
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);
	size_t total_items = data->framebuffer_size / DPI_ESP32_TRANS_ITEM_SIZE;
	uint32_t src_addr = (uint32_t)(uintptr_t)data->framebuffer;
	uint32_t dst_addr = MIPI_DSI_BRG_MEM_BASE;

	unsigned int key = irq_lock();
	dw_gdma_ll_enable_bus_clock(0, true);
	dw_gdma_ll_reset_register(0);
	irq_unlock(key);

	dw_gdma_ll_reset(dma);
	dw_gdma_ll_enable_controller(dma, true);
	dw_gdma_ll_enable_intr_global(dma, true);

	/* Channel config for the continuous-stream design: RELOAD multi-
	 * block on both ends (the shadow registers re-load automatically on
	 * block completion -> infinite frames, no LLI), DMA (SELF) as flow
	 * controller, HW handshake both ends, IDF's priority/outstanding
	 * limits, and the channel's shadow-valid marker set so the block
	 * auto-reloads instead of stopping. The BLOCK_TFR_DONE event is
	 * polled by a thread (no interrupts - the level-triggered DMA
	 * interrupt storms and its register writes fault from ISR context
	 * on this silicon). */
	dw_gdma_ll_channel_set_src_multi_block_type(dma, DPI_ESP32_DMA_CHANNEL,
						     DW_GDMA_BLOCK_TRANSFER_RELOAD);
	dw_gdma_ll_channel_set_dst_multi_block_type(dma, DPI_ESP32_DMA_CHANNEL,
						     DW_GDMA_BLOCK_TRANSFER_RELOAD);
	dw_gdma_ll_channel_set_trans_flow(dma, DPI_ESP32_DMA_CHANNEL, DW_GDMA_ROLE_MEM,
					   DW_GDMA_ROLE_PERIPH_DSI, DW_GDMA_FLOW_CTRL_SELF);
	dw_gdma_ll_channel_set_src_handshake_interface(dma, DPI_ESP32_DMA_CHANNEL,
							DW_GDMA_HANDSHAKE_HW);
	dw_gdma_ll_channel_set_dst_handshake_interface(dma, DPI_ESP32_DMA_CHANNEL,
							DW_GDMA_HANDSHAKE_HW);
	dw_gdma_ll_channel_set_dst_handshake_periph(dma, DPI_ESP32_DMA_CHANNEL,
						     DW_GDMA_ROLE_PERIPH_DSI);
	dw_gdma_ll_channel_set_priority(dma, DPI_ESP32_DMA_CHANNEL, 1);
	dw_gdma_ll_channel_set_src_outstanding_limit(dma, DPI_ESP32_DMA_CHANNEL, 5);
	dw_gdma_ll_channel_set_dst_outstanding_limit(dma, DPI_ESP32_DMA_CHANNEL, 2);

	dw_gdma_ll_channel_set_src_addr(dma, DPI_ESP32_DMA_CHANNEL, src_addr);
	dw_gdma_ll_channel_set_dst_addr(dma, DPI_ESP32_DMA_CHANNEL, dst_addr);
	dw_gdma_ll_channel_set_src_master_port(dma, DPI_ESP32_DMA_CHANNEL, (intptr_t)src_addr);
	dw_gdma_ll_channel_set_dst_master_port(dma, DPI_ESP32_DMA_CHANNEL, (intptr_t)dst_addr);
	dw_gdma_ll_channel_set_src_trans_width(dma, DPI_ESP32_DMA_CHANNEL, DPI_ESP32_TRANS_WIDTH);
	dw_gdma_ll_channel_set_dst_trans_width(dma, DPI_ESP32_DMA_CHANNEL, DPI_ESP32_TRANS_WIDTH);
	dw_gdma_ll_channel_set_src_burst_mode(dma, DPI_ESP32_DMA_CHANNEL,
					       DW_GDMA_BURST_MODE_INCREMENT);
	dw_gdma_ll_channel_set_dst_burst_mode(dma, DPI_ESP32_DMA_CHANNEL, DW_GDMA_BURST_MODE_FIXED);
	dw_gdma_ll_channel_set_src_burst_items(dma, DPI_ESP32_DMA_CHANNEL,
						DPI_ESP32_SRC_BURST_ITEMS);
	dw_gdma_ll_channel_set_dst_burst_items(dma, DPI_ESP32_DMA_CHANNEL,
						DPI_ESP32_DST_BURST_ITEMS);
	dw_gdma_ll_channel_set_trans_block_size(dma, DPI_ESP32_DMA_CHANNEL, total_items);
	dw_gdma_ll_channel_enable_src_periph_status_write_back(dma, DPI_ESP32_DMA_CHANNEL, false);
	dw_gdma_ll_channel_enable_dst_periph_status_write_back(dma, DPI_ESP32_DMA_CHANNEL, false);

	/* Shadow-valid: the block auto-reloads from the shadow registers
	 * when it completes - the channel runs forever. is_last=false so
	 * there is no end-of-transfer state. */
	dw_gdma_ll_channel_set_block_markers(dma, DPI_ESP32_DMA_CHANNEL, false, false, true);

	dpi_frame_items = total_items;
}

static int display_esp32_dpi_write(const struct device *dev, const uint16_t x, const uint16_t y,
				    const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct display_esp32_dpi_config *cfg = dev->config;
	struct display_esp32_dpi_data *data = dev->data;
	const uint8_t *src = buf;
	uint16_t pitch = desc->pitch ? desc->pitch : desc->width;
	size_t row_bytes = (size_t)desc->width * 2U;

	if (data->rotation == 90 || data->rotation == 270) {
		/* Logical (landscape) area -> physical (portrait) framebuffer.
		 * LVGL renders the logical resolution (logical_width x
		 * logical_height = 1280x800) and the flush callback passes
		 * areas in logical coordinates; the pixels are rotated here
		 * into the physical framebuffer:
		 *   ROTATION_90:  (fx, fy) = (logical y, height - 1 - logical x)
		 *   ROTATION_270: (fx, fy) = (width - 1 - logical y, logical x)
		 * Per-pixel copy (the row orientation changes), matching the
		 * Arduino reference's rotate-in-flush approach. */
		uint32_t lw = data->logical_width;
		uint32_t lh = data->logical_height;
		uint16_t *fb = (uint16_t *)data->framebuffer;
		const uint16_t *px = (const uint16_t *)src;
		uint32_t dy;
		uint32_t dx;

		if ((x + desc->width) > lw || (y + desc->height) > lh) {
			LOG_ERR("write out of bounds: (%u,%u) %ux%u on a %ux%u logical panel", x, y,
				desc->width, desc->height, lw, lh);
			return -EINVAL;
		}

		if (data->rotation == 90) {
			for (dy = 0; dy < desc->height; dy++) {
				for (dx = 0; dx < desc->width; dx++) {
					uint32_t fx = y + dy;
					uint32_t fy = (cfg->height - 1U) - (x + dx);

					fb[fy * cfg->width + fx] = px[dy * pitch + dx];
				}
			}
		} else {
			for (dy = 0; dy < desc->height; dy++) {
				for (dx = 0; dx < desc->width; dx++) {
					uint32_t fx = (cfg->width - 1U) - (y + dy);
					uint32_t fy = x + dx;

					fb[fy * cfg->width + fx] = px[dy * pitch + dx];
				}
			}
		}
		sys_cache_data_flush_range(data->framebuffer, data->framebuffer_size);
		return 0;
	}

	/* Unrotated path: plain row copy. */
	if ((x + desc->width) > cfg->width || (y + desc->height) > cfg->height) {
		LOG_ERR("write out of bounds: (%u,%u) %ux%u on a %ux%u panel", x, y, desc->width,
			desc->height, cfg->width, cfg->height);
		return -EINVAL;
	}

	for (uint16_t row = 0; row < desc->height; row++) {
		uint8_t *dst = data->framebuffer + ((size_t)(y + row) * cfg->width + x) * 2U;

		memcpy(dst, src + (size_t)row * pitch * 2U, row_bytes);
	}

	/* The DMA engine's bus master does not snoop the CPU cache: flush so
	 * it observes the pixels we just wrote. Flush the whole touched row
	 * span (simplest correct option for a first version; could be
	 * tightened to exactly the sub-rectangle later).
	 */
	sys_cache_data_flush_range(data->framebuffer + (size_t)y * cfg->width * 2U,
				    (size_t)desc->height * cfg->width * 2U);

	return 0;
}

/* Called by the app to configure the LVGL-style rotation (0/90/180/270
 * degrees clockwise). The app must also set the matching LVGL display
 * rotation so the logical resolution swaps to landscape. */
void display_esp32_dpi_set_rotation(const struct device *dev, uint16_t rotation)
{
	struct display_esp32_dpi_data *data = dev->data;

	data->rotation = rotation % 360;
	if (rotation == 90 || rotation == 270) {
		data->logical_width = data->dpi_cfg->height;
		data->logical_height = data->dpi_cfg->width;
	} else {
		data->logical_width = data->dpi_cfg->width;
		data->logical_height = data->dpi_cfg->height;
	}
}

static int display_esp32_dpi_blanking_on(const struct device *dev)
{
	const struct display_esp32_dpi_config *cfg = dev->config;
	struct display_esp32_dpi_data *data = dev->data;

	mipi_dsi_brg_ll_enable_dpi_output(dpi_bridge(cfg), false);
	mipi_dsi_brg_ll_update_dpi_config(dpi_bridge(cfg));
	data->blanked = true;
	return 0;
}

static int display_esp32_dpi_blanking_off(const struct device *dev)
{
	const struct display_esp32_dpi_config *cfg = dev->config;
	struct display_esp32_dpi_data *data = dev->data;

	mipi_dsi_brg_ll_enable_dpi_output(dpi_bridge(cfg), true);
	mipi_dsi_brg_ll_update_dpi_config(dpi_bridge(cfg));
	data->blanked = false;
	return 0;
}

static void display_esp32_dpi_get_capabilities(const struct device *dev,
						struct display_capabilities *capabilities)
{
	const struct display_esp32_dpi_config *cfg = dev->config;

	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->x_resolution = cfg->width;
	capabilities->y_resolution = cfg->height;
	capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
	capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int display_esp32_dpi_set_pixel_format(const struct device *dev,
					       const enum display_pixel_format format)
{
	return (format == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static DEVICE_API(display, display_esp32_dpi_api) = {
	.write = display_esp32_dpi_write,
	.blanking_on = display_esp32_dpi_blanking_on,
	.blanking_off = display_esp32_dpi_blanking_off,
	.get_capabilities = display_esp32_dpi_get_capabilities,
	.set_pixel_format = display_esp32_dpi_set_pixel_format,
};

static int display_esp32_dpi_init(const struct device *dev)
{
	const struct display_esp32_dpi_config *cfg = dev->config;
	struct display_esp32_dpi_data *data = dev->data;

	data->dpi_cfg = cfg;
	data->framebuffer_size = (size_t)cfg->width * cfg->height * 2U;
	data->framebuffer = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 64,
							     data->framebuffer_size);
	if (data->framebuffer == NULL) {
		LOG_ERR("failed to allocate %zu byte PSRAM framebuffer", data->framebuffer_size);
		return -ENOMEM;
	}

	/*
	 * Initial framebuffer contents: dark blue (RGB565 0x1030) written
	 * through the normal cached path + flush - the flush path is
	 * verified working (the quadrant test pattern reaches the panel).
	 */
	for (size_t i = 0; i < data->framebuffer_size; i += 2) {
		data->framebuffer[i] = 0x30;
		data->framebuffer[i + 1] = 0x10;
	}
	sys_cache_data_flush_range(data->framebuffer, data->framebuffer_size);

	display_esp32_dpi_configure_dma(cfg, data);

	/* Arm the DMA: the channel auto-reloads its block forever; the
	 * polling thread only reloads the bridge frame counter on each
	 * block completion. */
	data->dma_armed = true;
	display_esp32_dpi_arm_dma(data);
	dpi_start_poll_thread(data);

	LOG_INF("DSI DPI framebuffer ready: %ux%u RGB565, %zu bytes in PSRAM at %p", cfg->width,
		cfg->height, data->framebuffer_size, (void *)data->framebuffer);

	return 0;
}

static const struct display_esp32_dpi_config display_esp32_dpi_cfg_0 = {
	.bridge_base = DT_INST_REG_ADDR(0),
	.width = DT_INST_PROP(0, width),
	.height = DT_INST_PROP(0, height),
};

static struct display_esp32_dpi_data display_esp32_dpi_data_0;

DEVICE_DT_INST_DEFINE(0, display_esp32_dpi_init, NULL, &display_esp32_dpi_data_0,
		       &display_esp32_dpi_cfg_0, POST_KERNEL, 65, &display_esp32_dpi_api);
