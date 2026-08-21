/*
 * Copyright (c) 2026 club-hmi
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIPI-DSI host driver for the Espressif ESP32-P4.
 *
 * Owns the DSI host + bridge register banks (one HAL context spans both,
 * see hal/mipi_dsi_hal.h) and implements the Zephyr mipi_dsi_driver_api:
 *   - attach(): called by the panel driver once, with the panel's timings.
 *     Configures the D-PHY PLL divider follow-up, DPI/bridge horizontal and
 *     vertical timing, colour coding on both host and bridge, low-power
 *     command support during blanking, and finally switches the host into
 *     video mode. This mirrors the sequencing used by display_st7701.c.
 *   - transfer(): DCS/generic read and write, over the low-power command
 *     generator. Works both before and after video mode is enabled: real
 *     DSI video-mode hosts interleave LP commands into blanking intervals.
 *
 * The actual framebuffer + DW-GDMA scan-out is a *separate* device/driver
 * (espressif,esp32-mipi-dsi-dpi, see drivers/display/display_esp32_dpi.c),
 * since it is a logically distinct role even though some of the registers
 * it touches (the DSI bridge bank) overlap with this node's "bridge" reg.
 *
 * See club-hmi/PLAN.md section 2.3/2.4/4 for the research this is based
 * on, including how the DW-GDMA base address and the host/bridge split were
 * derived.
 */

#define DT_DRV_COMPAT espressif_esp_mipi_dsi

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>

/*
 * __DECLARE_RCC_ATOMIC_ENV (referenced by mipi_dsi_ll.h's clock-gating
 * helper macros, normally supplied by ESP-IDF's PERIPH_RCC_ATOMIC()
 * critical-section helper) is already provided as a global compile
 * definition by the hal_espressif Zephyr glue (see the build command line),
 * so no local placeholder is needed here. We still take our own
 * Zephyr-native critical section (irq_lock/irq_unlock) around the handful
 * of register read-modify-writes at init time, since the global define
 * expands to a plain constant, not a real lock/unlock pair.
 *
 * mipi_dsi_ll.h/mipi_dsi_brg_ll.h call abort() in a few "invalid enum
 * value" default cases without including <stdlib.h> themselves (fine under
 * ESP-IDF, where it is pulled in transitively; not fine in this Zephyr
 * module build) - <stdlib.h> is included above to provide the declaration.
 */
#include <stdlib.h>
#include <hal/mipi_dsi_hal.h>
#include <hal/mipi_dsi_ll.h>
#include <hal/mipi_dsi_types.h>
#include <hal/lcd_types.h>
#include <hal/dw_gdma_ll.h>

#include "dsi_esp32.h"

LOG_MODULE_REGISTER(dsi_esp32, CONFIG_MIPI_DSI_LOG_LEVEL);

/* Matches the Arduino/ESP-IDF reference implementation for this exact panel
 * (club-hmi/PLAN.md 2.1/2.4): DPI clock source PLL_F240M, PHY
 * configuration/PLL-reference clock source PLL_F20M - the SOC_MOD_CLK
 * *_DEFAULT selections for this SoC.
 */
#define DSI_ESP32_DPI_CLK_SRC_MHZ   240.0f
#define DSI_ESP32_PHY_CFG_CLK_MHZ   20000000U

struct dsi_esp32_config {
	uint32_t dpi_clock_freq_mhz;
	const struct device *phy_supply;
};

struct dsi_esp32_data {
	mipi_dsi_hal_context_t hal;
	struct k_mutex lock;
	bool attached;
};

static int dsi_esp32_attach(const struct device *dev, uint8_t channel,
			     const struct mipi_dsi_device *mdev)
{
	const struct dsi_esp32_config *cfg = dev->config;
	struct dsi_esp32_data *data = dev->data;
	mipi_dsi_hal_context_t *hal = &data->hal;
	uint32_t dpi_div;

	if (mdev->pixfmt != MIPI_DSI_PIXFMT_RGB565) {
		LOG_ERR("only MIPI_DSI_PIXFMT_RGB565 is currently supported (got %u)",
			mdev->pixfmt);
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	dpi_div = mipi_dsi_hal_host_dpi_calculate_divider(hal, DSI_ESP32_DPI_CLK_SRC_MHZ,
							   (float)cfg->dpi_clock_freq_mhz);
	unsigned int key = irq_lock();
	/* Toggle the DPI clock gate off/on around the divider change: it was
	 * already enabled (at whatever divider was latched at reset) back in
	 * dsi_esp32_init(), well before the real target divider is known
	 * here. Re-latching via a fresh enable edge is cheap insurance
	 * against a divider implementation that only samples a new value
	 * when the gate transitions, rather than updating on the fly.
	 */
	mipi_dsi_ll_enable_dpi_clock(0, false);
	mipi_dsi_ll_set_dpi_clock_div(0, dpi_div);
	mipi_dsi_ll_enable_dpi_clock(0, true);
	irq_unlock(key);

	mipi_dsi_host_ll_dpi_set_vcid(hal->host, channel);

	/* Colour coding on both host and bridge, matching the ESP-IDF
	 * esp_lcd_dpi_panel driver exactly (in/out color format RGB565,
	 * sub_config 0 = 16-bit config 1). The R/B-swap issue seen with
	 * config 0 during bring-up was a *data-path* symptom of the bridge
	 * stalling (no frame-counter reload), not a color-coding mismatch -
	 * the working Arduino/IDF reference uses exactly these values.
	 */
	mipi_dsi_host_ll_dpi_set_color_coding(hal->host, LCD_COLOR_FMT_RGB565, 0);
	mipi_dsi_brg_ll_set_input_color_format(hal->bridge, LCD_COLOR_FMT_RGB565);
	mipi_dsi_brg_ll_set_output_color_format(hal->bridge, LCD_COLOR_FMT_RGB565, 0);

	/* DPI input timing polarity: all ACTIVE-HIGH (the DesignWare
	 * defaults, which the ESP-IDF driver leaves untouched). The DSI
	 * bridge outputs its syncs active-high; matching this is required
	 * for the host's DPI-input sampler to frame the bridge's data -
	 * with active-low settings (an earlier assumption) the host could
	 * never frame the bridge data stream and its DPI payload buffer
	 * stayed permanently full (vid_pkt_status.dpi_buff_pld_full set).
	 */
	mipi_dsi_host_ll_dpi_set_timing_polarity(hal->host, false, false, false, false, false);

	mipi_dsi_hal_host_dpi_set_horizontal_timing(hal, mdev->timings.hsync, mdev->timings.hbp,
						     mdev->timings.hactive, mdev->timings.hfp);
	mipi_dsi_hal_host_dpi_set_vertical_timing(hal, mdev->timings.vsync, mdev->timings.vbp,
						   mdev->timings.vactive, mdev->timings.vfp);

	/* One packet per line, no null padding. */
	mipi_dsi_host_ll_dpi_set_trunks_num(hal->host, 0);
	mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(hal->host, mdev->timings.hactive);
	mipi_dsi_host_ll_dpi_set_null_packet_size(hal->host, 0);
	mipi_dsi_host_ll_dpi_set_video_burst_type(hal->host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);

	/* Allow the host to insert LP commands during blanking. Bridge/DMA
	 * output and host video mode are deliberately *not* started here -
	 * see esp32_mipi_dsi_start_video() below and PLAN.md: on this panel,
	 * switching to video mode before the panel's command-mode DCS init
	 * sequence completes (matching the sequencing several other Zephyr
	 * DSI panel drivers use) left the generic command FIFO permanently
	 * full after ~18 short writes on real hardware, consistent with the
	 * transmission engine never actually draining commands once video
	 * mode requests the link's attention. Pure command-mode init first,
	 * video mode only afterwards, avoids that.
	 */
	mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(hal->host, true, true);
	mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(hal->host, true, true, true, true);
	mipi_dsi_host_ll_dpi_enable_lp_command(hal->host, true);

	/*
	 * DSI bridge config, matching the ESP-IDF esp_lcd_panel_dpi driver
	 * exactly (verified working reference - see PLAN.md section 8): the
	 * DW-GDMA is the flow controller, burst length 256, empty threshold
	 * 768 (1024-entry FIFO), one multi-block node per frame, underrun
	 * discard count = active line width.
	 */
	mipi_dsi_brg_ll_set_num_pixel_bits(hal->bridge,
					    mdev->timings.hactive * mdev->timings.vactive * 16U);
	mipi_dsi_brg_ll_set_underrun_discard_count(hal->bridge, mdev->timings.hactive);
	mipi_dsi_brg_ll_set_flow_controller(hal->bridge, MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
	mipi_dsi_brg_ll_set_multi_block_number(hal->bridge, 1);
	mipi_dsi_brg_ll_set_burst_len(hal->bridge, 256);
	mipi_dsi_brg_ll_set_empty_threshold(hal->bridge, 1024 - 256);

	/*
	 * Commit video-mode/timing/colour-coding shadow registers - see
	 * esp32_mipi_dsi_start_video(), where this is done *after* video
	 * mode is enabled: doing it here (before video mode) did not budge
	 * the "_act" registers off zero on real hardware.
	 */

	/* Enable the bridge core now (DPI output and video mode stay
	 * deferred to esp32_mipi_dsi_start_video()). */
	mipi_dsi_brg_ll_enable(hal->bridge, true);

	data->attached = true;
	k_mutex_unlock(&data->lock);

	LOG_INF("DSI attached: %ux%u RGB565, dpi_div=%u, real dpi clk=%d.%02d MHz",
		mdev->timings.hactive, mdev->timings.vactive, dpi_div,
		(int)hal->real_dpi_clock_freq_mhz,
		(int)((hal->real_dpi_clock_freq_mhz - (int)hal->real_dpi_clock_freq_mhz) * 100));

	return 0;
}

int esp32_mipi_dsi_start_video(const struct device *dev)
{
	struct dsi_esp32_data *data = dev->data;
	mipi_dsi_hal_context_t *hal = &data->hal;

	if (!data->attached) {
		return -EAGAIN;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	mipi_dsi_brg_ll_enable(hal->bridge, true);
	mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, true);
	mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
	/*
	 * The clock lane's "request HS" bit (lpclk_ctrl.phy_txrequestclkhs)
	 * defaults to 0 and is not touched anywhere else - without it the
	 * clock lane never actually enters high speed, so no HS video burst
	 * can ever go out over the wire regardless of everything else being
	 * configured correctly. This was found on real hardware: DMA
	 * completed a full-frame transfer into the bridge, and the host's
	 * own DPI payload buffer (vid_pkt_status.dpi_pld_w_full /
	 * dpi_buff_pld_full) filled up and stayed full - data arriving with
	 * nowhere to go, exactly like the LP command FIFO symptom that led
	 * to the escape-clock-divider fix, just one layer further along.
	 * AUTO mode enters/leaves HS around each video transmission
	 * automatically, which is the normal choice for video mode.
	 */
	mipi_dsi_host_ll_set_clock_lane_state(hal->host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
	mipi_dsi_host_ll_enable_video_mode(hal->host, true);

	/*
	 * Commit the video-mode/timing/colour-coding registers configured in
	 * attach() into their "_act" (active/shadow) counterparts. Found on
	 * real hardware: even with vid_shadow_ctrl.vid_shadow_en left at its
	 * default 0 ("disabled"), the vid_mode_cfg_act/vid_pkt_size_act/
	 * vid_hline_time_act etc. registers read back as all-zero - the
	 * requested registers alone are *not* live* - until a vid_shadow_req
	 * pulse is issued. Earlier attempts wrote vid_shadow_req=1 then
	 * immediately =0 without waiting for the hardware to consume the
	 * request; the DWC MIPI DSI host samples the request asynchronously
	 * and the one-shot pulse was likely missed. Do it properly here:
	 * enable shadowing, raise the request, and poll until the hardware
	 * clears the request bit itself (indicating the shadow update
	 * completed), with a bounded timeout.
	 */
	{
		hal->host->vid_shadow_ctrl.vid_shadow_en = 1;
		hal->host->vid_shadow_ctrl.vid_shadow_req = 1;
		int wait = 0;

		while (hal->host->vid_shadow_ctrl.vid_shadow_req && wait < 10000) {
			k_busy_wait(10);
			wait++;
		}
		LOG_INF("  shadow commit: req_cleared=%d (after %d x 10us)", wait < 10000, wait);
	}

	k_mutex_unlock(&data->lock);

	LOG_INF("DSI video mode enabled");

	/* Let a few frames pass, then report bridge/host video-path status:
	 * unlike the LP command path, this is not yet validated on hardware
	 * (see PLAN.md) - the escape-clock fix only proved LP/command mode
	 * works, not the separate HS/video (bridge + DW-GDMA) datapath. */
	k_msleep(50);
	LOG_INF("  bridge: int_st=0x%08x fifo_depth=%u | host: vid_pkt_status=0x%08x "
		"mode_cfg=0x%08x",
		mipi_dsi_brg_ll_get_interrupt_status(hal->bridge),
		mipi_dsi_brg_ll_get_fifo_depth(hal->bridge), hal->host->vid_pkt_status.val,
		hal->host->mode_cfg.val);
	LOG_INF("  bridge regs: en=0x%08x dpi_misc=0x%08x dma_flow=0x%08x pixel_type=0x%08x "
		"raw_num=0x%08x",
		hal->bridge->en.val, hal->bridge->dpi_misc_config.val,
		hal->bridge->dma_flow_ctrl.val, hal->bridge->pixel_type.val,
		hal->bridge->raw_num_cfg.val);

	{
		dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);

		/* Sample repeatedly: if RELOAD is really looping, chen0/
		 * status0 should show fresh completions each ~14.6ms frame;
		 * if it transferred once and stopped, every sample will be
		 * byte-for-byte identical.
		 */
		for (int i = 0; i < 5; i++) {
			LOG_INF("  [%d] dw_gdma: chen0=0x%08x ch0.int_st=0x%08x "
				"ch0.status0=0x%08x ch0.status1=0x%08x common_int_st=0x%08x",
				i, dma->chen0.val, dw_gdma_ll_channel_get_intr_status(dma, 0),
				dma->ch[0].status0.val, dma->ch[0].status1.val,
				dw_gdma_ll_get_common_intr_status(dma));
			k_msleep(20);
		}
	}

	LOG_INF("  shadow: vid_shadow_ctrl=0x%08x vid_mode_cfg=0x%08x/act=0x%08x "
		"dpi_color_coding=0x%08x/act=0x%08x",
		hal->host->vid_shadow_ctrl.val, hal->host->vid_mode_cfg.val,
		hal->host->vid_mode_cfg_act.val, hal->host->dpi_color_coding.val,
		hal->host->dpi_color_coding_act.val);
	LOG_INF("  shadow: vid_pkt_size=0x%08x/act=0x%08x vid_hline_time=0x%08x/act=0x%08x",
		hal->host->vid_pkt_size.val, hal->host->vid_pkt_size_act.val,
		hal->host->vid_hline_time.val, hal->host->vid_hline_time_act.val);
	LOG_INF("  shadow: dpi_cfg_pol=0x%08x (no act reg) dpi_vcid=0x%08x/act=0x%08x "
		"dpi_lp_cmd_tim=0x%08x/act=0x%08x",
		hal->host->dpi_cfg_pol.val, hal->host->dpi_vcid.val, hal->host->dpi_vcid_act.val,
		hal->host->dpi_lp_cmd_tim.val, hal->host->dpi_lp_cmd_tim_act.val);

	/*
	 * TEST diagnostics: is the PHY actually entering HS in video mode?
	 * phy_status's stopstate bits must go 0 during HS bursts. Also dump
	 * the DPI clock registers (peri_clk_ctrl03: dpiclk_en/src_sel/div)
	 * and the ref clock ctrl registers, and the PHY test interface state.
	 */
	for (int i = 0; i < 5; i++) {
		LOG_INF("  [%d] phy_status=0x%08x lpclk_ctrl=0x%08x "
			"peri_clk_ctrl03=0x%08x ref_clk_ctrl1=0x%08x ref_clk_ctrl2=0x%08x",
			i, hal->host->phy_status.val, hal->host->lpclk_ctrl.val,
			HP_SYS_CLKRST.peri_clk_ctrl03.val, HP_SYS_CLKRST.ref_clk_ctrl1.val,
			HP_SYS_CLKRST.ref_clk_ctrl2.val);
		k_msleep(20);
	}

	return 0;
}

/*
 * The vendor HAL's mipi_dsi_hal_host_gen_* helpers busy-wait on FIFO
 * flags with *no timeout at all* (plain `while (fifo_full);`). During
 * hardware bring-up this turned a hardware/config bug (D-PHY never
 * reaching HS, see dsi_esp32_init()) into a silent, permanent hang with no
 * diagnostic output at all - nothing after it in the boot sequence would
 * ever run. Every FIFO wait here is bounded and logs which specific
 * condition timed out, so a future regression fails loudly instead of
 * hanging the whole boot.
 *
 * LP commands can only be inserted during blanking intervals (see
 * dsi_esp32_attach()'s lp_horizontal/vertical_timing/lp_command enables),
 * which recur once per video frame - at this panel's timings (800x1280,
 * 80 MHz DPI clock) that is ~14.6 ms. The timeout must comfortably exceed
 * that, hence a wall-clock-time bound (via k_uptime_get()) rather than a
 * raw spin-iteration count, which would be wrong on a different core clock
 * and was in fact the bug the first version of this macro had: it timed
 * out after ~1-3 ms of spinning, well inside a single frame period, and
 * misreported perfectly normal FIFO backpressure as a hardware failure.
 */
#define DSI_ESP32_FIFO_TIMEOUT_MS 100

#define DSI_ESP32_WAIT_OR_TIMEOUT(cond, what)                                                    \
	do {                                                                                       \
		int64_t _deadline = k_uptime_get() + DSI_ESP32_FIFO_TIMEOUT_MS;                  \
		bool _timed_out = false;                                                          \
		while (cond) {                                                                    \
			if (k_uptime_get() > _deadline) {                                         \
				_timed_out = true;                                                \
				break;                                                            \
			}                                                                         \
		}                                                                                 \
		if (_timed_out) {                                                                \
			LOG_ERR("timeout waiting for %s", what);                                  \
			LOG_ERR("  phy_status=0x%08x cmd_pkt_status=0x%08x int_st0=0x%08x "       \
				"int_st1=0x%08x",                                                 \
				hal->host->phy_status.val, hal->host->cmd_pkt_status.val,         \
				hal->host->int_st0.val, hal->host->int_st1.val);                  \
			LOG_ERR("  mode_cfg=0x%08x cmd_mode_cfg=0x%08x phy_if_cfg=0x%08x",        \
				hal->host->mode_cfg.val, hal->host->cmd_mode_cfg.val,             \
				hal->host->phy_if_cfg.val);                                       \
			LOG_ERR("  phy_tmr_cfg=0x%08x phy_tmr_lpclk_cfg=0x%08x "                  \
				"vid_pkt_status=0x%08x",                                          \
				hal->host->phy_tmr_cfg.val, hal->host->phy_tmr_lpclk_cfg.val,     \
				hal->host->vid_pkt_status.val);                                   \
			k_mutex_unlock(&data->lock);                                              \
			return -ETIMEDOUT;                                                        \
		}                                                                                 \
	} while (0)

static ssize_t dsi_esp32_transfer(const struct device *dev, uint8_t channel,
				   struct mipi_dsi_msg *msg)
{
	struct dsi_esp32_data *data = dev->data;
	mipi_dsi_hal_context_t *hal = &data->hal;
	ssize_t ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (msg->type) {
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_DCS_LONG_WRITE: {
		/* Reimplemented (not calling mipi_dsi_hal_host_gen_write_dcs_command)
		 * so every FIFO wait is bounded - see comment above. All
		 * jd9365 commands are 1 command byte + <=1 data byte, i.e.
		 * payload_size <= 2, which never touches the payload FIFO at
		 * all (only DCS_LONG_WRITE with >2 total bytes would); that
		 * path is included for completeness/robustness.
		 */
		const uint8_t *p = msg->tx_buf;
		uint32_t payload_size = 1 + msg->tx_len;
		mipi_dsi_data_type_t dt;
		uint8_t hdr_msb, hdr_lsb;

		if (payload_size > 2) {
			uint32_t remain = msg->tx_len;
			const uint8_t *cur = p;
			uint32_t first;
			uint32_t temp = msg->cmd;

			first = MIN(3U, remain);
			for (uint32_t i = 0; i < first; i++) {
				temp |= (uint32_t)cur[i] << (8 * (i + 1));
			}
			DSI_ESP32_WAIT_OR_TIMEOUT(
				mipi_dsi_host_ll_gen_is_write_fifo_full(hal->host),
				"gen write FIFO (DCS long write header)");
			mipi_dsi_host_ll_gen_write_payload_fifo(hal->host, temp);
			cur += first;
			remain -= first;
			while (remain >= 4) {
				memcpy(&temp, cur, sizeof(temp));
				DSI_ESP32_WAIT_OR_TIMEOUT(
					mipi_dsi_host_ll_gen_is_write_fifo_full(hal->host),
					"gen write FIFO (DCS long write payload)");
				mipi_dsi_host_ll_gen_write_payload_fifo(hal->host, temp);
				cur += 4;
				remain -= 4;
			}
			if (remain) {
				temp = 0;
				memcpy(&temp, cur, remain);
				DSI_ESP32_WAIT_OR_TIMEOUT(
					mipi_dsi_host_ll_gen_is_write_fifo_full(hal->host),
					"gen write FIFO (DCS long write tail)");
				mipi_dsi_host_ll_gen_write_payload_fifo(hal->host, temp);
			}
			dt = MIPI_DSI_DT_DCS_LONG_WRITE;
			hdr_msb = (payload_size >> 8) & 0xFF;
			hdr_lsb = payload_size & 0xFF;
		} else if (payload_size == 2) {
			dt = MIPI_DSI_DT_DCS_SHORT_WRITE_1;
			hdr_msb = p[0];
			hdr_lsb = msg->cmd;
		} else {
			dt = MIPI_DSI_DT_DCS_SHORT_WRITE_0;
			hdr_msb = 0;
			hdr_lsb = msg->cmd;
		}

		DSI_ESP32_WAIT_OR_TIMEOUT(mipi_dsi_host_ll_gen_is_cmd_fifo_full(hal->host),
					   "gen cmd FIFO (DCS write header)");
		mipi_dsi_host_ll_gen_set_packet_header(hal->host, channel, dt, hdr_msb, hdr_lsb);
		ret = msg->tx_len;
		break;
	}

	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM: {
		uint16_t header = 0;
		const uint8_t *p = msg->tx_buf;

		for (size_t i = 0; i < msg->tx_len && i < 2; i++) {
			header |= (uint16_t)p[i] << (8 * i);
		}
		DSI_ESP32_WAIT_OR_TIMEOUT(mipi_dsi_host_ll_gen_is_cmd_fifo_full(hal->host),
					   "gen cmd FIFO (generic short write)");
		mipi_dsi_hal_host_gen_write_short_packet(hal, channel,
							  (mipi_dsi_data_type_t)msg->type, header);
		ret = msg->tx_len;
		break;
	}

	case MIPI_DSI_GENERIC_LONG_WRITE:
		DSI_ESP32_WAIT_OR_TIMEOUT(mipi_dsi_host_ll_gen_is_write_fifo_full(hal->host),
					   "gen write FIFO (generic long write)");
		mipi_dsi_hal_host_gen_write_long_packet(hal, channel, MIPI_DSI_DT_GENERIC_LONG_WRITE,
							 msg->tx_buf, msg->tx_len);
		ret = msg->tx_len;
		break;

	case MIPI_DSI_DCS_READ:
		DSI_ESP32_WAIT_OR_TIMEOUT(mipi_dsi_host_ll_gen_is_cmd_fifo_full(hal->host),
					   "gen cmd FIFO (DCS read)");
		mipi_dsi_hal_host_gen_read_dcs_command(hal, channel, msg->cmd, 1, msg->rx_buf,
							msg->rx_len);
		ret = msg->rx_len;
		break;

	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM: {
		uint16_t header = 0;
		const uint8_t *p = msg->tx_buf;

		for (size_t i = 0; i < msg->tx_len && i < 2; i++) {
			header |= (uint16_t)p[i] << (8 * i);
		}
		DSI_ESP32_WAIT_OR_TIMEOUT(mipi_dsi_host_ll_gen_is_cmd_fifo_full(hal->host),
					   "gen cmd FIFO (generic read)");
		mipi_dsi_hal_host_gen_read_short_packet(hal, channel,
							 (mipi_dsi_data_type_t)msg->type, header,
							 msg->rx_buf, msg->rx_len);
		ret = msg->rx_len;
		break;
	}

	default:
		LOG_ERR("unsupported DSI message type 0x%02x", msg->type);
		ret = -ENOTSUP;
		break;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_API(mipi_dsi, dsi_esp32_api) = {
	.attach = dsi_esp32_attach,
	.transfer = dsi_esp32_transfer,
};

static int dsi_esp32_init(const struct device *dev)
{
	const struct dsi_esp32_config *cfg = dev->config;
	struct dsi_esp32_data *data = dev->data;
	mipi_dsi_hal_config_t hal_cfg = {
		.bus_id = 0,
		.num_data_lanes = DT_INST_PROP(0, data_lanes),
		.lane_bit_rate_mbps = DT_INST_PROP(0, lane_bit_rate_mbps),
	};
	int ret;

	k_mutex_init(&data->lock);

	if (cfg->phy_supply != NULL) {
		ret = regulator_enable(cfg->phy_supply);
		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("failed to enable DSI PHY supply (%d)", ret);
			return ret;
		}
	}

	unsigned int key = irq_lock();
	mipi_dsi_ll_enable_bus_clock(0, true);
	mipi_dsi_ll_reset_register(0);
	mipi_dsi_ll_enable_dpi_clock(0, true);
	mipi_dsi_ll_set_dpi_clock_source(0, MIPI_DSI_DPI_CLK_SRC_PLL_F240M);
	mipi_dsi_ll_enable_phy_config_clock(0, true);
	mipi_dsi_ll_set_phy_config_clock_source(0, MIPI_DSI_PHY_CFG_CLK_SRC_PLL_F20M);
	mipi_dsi_ll_enable_phy_pllref_clock(0, true);
	mipi_dsi_ll_set_phy_pllref_clock_source(0, MIPI_DSI_PHY_PLLREF_CLK_SRC_PLL_F20M);
	/*
	 * HW default for HP_SYS_CLKRST.ref_clk_ctrl2.reg_ref_20m_clk_en is 1
	 * (per the register doc), but Zephyr's own boot path leaves it
	 * gated off in practice (confirmed on real hardware) - without this,
	 * the shared SPLL-derived 20 MHz reference tap never actually reaches
	 * the PLL_F20M mux, so the D-PHY's config/PLL-reference clocks are
	 * dead regardless of the mipi_dsi-specific gates above, and
	 * phy_status.phy_lock never sets. No LL wrapper exists for this
	 * shared (non-DSI-specific) gate, so it's set directly here.
	 */
	HP_SYS_CLKRST.ref_clk_ctrl2.reg_ref_20m_clk_en = 1;
	irq_unlock(key);

	mipi_dsi_hal_init(&data->hal, &hal_cfg);
	mipi_dsi_hal_configure_phy_pll(&data->hal, DSI_ESP32_PHY_CFG_CLK_MHZ,
					(float)hal_cfg.lane_bit_rate_mbps);

	/*
	 * D-PHY HS<->LP switching timing, in lane-byte-clock cycles (byte
	 * clock = lane_bit_rate_mbps / 8 = ~187.5 MHz here, ~5.33 ns/cycle).
	 * Neither mipi_dsi_hal_init() nor mipi_dsi_hal_configure_phy_pll()
	 * touch these - in ESP-IDF they are set by the esp_lcd_mipi_dsi_bus
	 * driver layer, which does not exist in this Zephyr HAL port (only
	 * the lower mipi_dsi_hal.c/mipi_dsi_periph.c pieces are present).
	 * Leaving these at their power-on-reset values was a real bring-up
	 * bug found on hardware: phy_status read back as fully "locked" and
	 * all lanes in a clean stop-state, yet not a single queued LP
	 * command ever drained (gen_cmd_full stayed set forever) - the PHY
	 * was never actually completing an LP->HS->LP round trip. Values
	 * below use generous margins over the MIPI D-PHY spec's documented
	 * minimums (e.g. TCLK-ZERO >= 300 ns dominates the clock lane
	 * LP->HS figure) rather than tight numbers, since excess margin here
	 * only costs a little latency, not correctness.
	 */
	/*
	 * TX escape clock divider: LP-mode transmission (used for *every*
	 * command in this driver, see the speed-mode block below) is not
	 * clocked by the HS lane byte clock at all - it uses a much slower
	 * "escape clock" derived from it, generated by clkmgr_cfg's
	 * tx_esc_clk_division field. That field has *no* LL wrapper
	 * function anywhere in this HAL fork (only the neighbouring
	 * to_clk_division timeout-counter field does, via
	 * mipi_dsi_host_ll_set_timeout_clock_division()), and is left at
	 * its power-on-reset value by mipi_dsi_hal_init() and everything
	 * else in this driver. Divide the ~187.5 MHz lane byte clock
	 * (1500 Mbps / 8) down to a conservative ~9 MHz, well inside the
	 * MIPI D-PHY escape-mode clock range.
	 */
	data->hal.host->clkmgr_cfg.tx_esc_clk_division = 20;

	mipi_dsi_phy_ll_set_stop_wait_time(data->hal.host, 60);
	mipi_dsi_phy_ll_set_switch_time(data->hal.host, 30, 60, 30, 100);
	mipi_dsi_phy_ll_set_max_read_time(data->hal.host, 10000);

	/*
	 * Force every command/generic transaction type to LP speed. These
	 * per-transaction-type bits (cmd_mode_cfg) are never touched by
	 * mipi_dsi_hal_init() and default to 0 = MIPI_DSI_LL_TRANS_SPEED_HS.
	 * All panel init here happens in pure command mode, with the data
	 * lanes sitting in LP/stop-state (no HS burst has been requested at
	 * all) - if a command is staged expecting an HS transmission, the
	 * hardware appears to simply wait forever for HS-entry preconditions
	 * that are never satisfied, rather than erroring out. That exactly
	 * matches what was observed on real hardware: the generic command
	 * FIFO accepts entries up to its physical depth (~19-20), consistent
	 * with nothing ever actually draining/transmitting from the very
	 * first queued command, then blocks forever with zero interrupt/error
	 * flags set (phy_status still reports a clean PLL-locked, all-lanes-
	 * stop-state link throughout).
	 */
	mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(data->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(data->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(data->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(data->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(data->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(data->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_wr_speed_mode(data->hal.host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_long_wr_speed_mode(data->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(data->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(data->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_gen_short_rd_speed_mode(data->hal.host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
	mipi_dsi_host_ll_set_mrps_speed_mode(data->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);

	/* Wait for the D-PHY PLL to lock and the data lanes to reach the
	 * "stop state" (LP-11 idle, i.e. ready) before any command/video
	 * traffic is attempted. Skipping this (a real bring-up bug found
	 * during hardware testing - see PLAN.md) leaves the link
	 * non-operational: the generator FIFOs never drain and any transfer
	 * hangs forever in the vendor HAL's unbounded busy-wait loops.
	 */
	bool locked = false;
	bool lanes_ready = false;

	for (int i = 0; i < 10000; i++) {
		locked = mipi_dsi_phy_ll_is_pll_locked(data->hal.host);
		lanes_ready =
			mipi_dsi_phy_ll_are_lanes_stopped(data->hal.host, hal_cfg.num_data_lanes);
		if (locked && lanes_ready) {
			break;
		}
		k_busy_wait(100);
	}
	if (!locked || !lanes_ready) {
		LOG_ERR("D-PHY not ready after 1s: pll_locked=%d lanes_stopped=%d", locked,
			lanes_ready);
		return -EIO;
	}

	/* Bridge registers (touched by this driver's attach() as well as by
	 * the separate DPI/framebuffer driver) need their register clock
	 * force-enabled before any bridge register access is meaningful.
	 */
	mipi_dsi_brg_ll_force_enable_reg_clock(data->hal.bridge, true);

	/* Command mode until a panel attach()es and switches us to video
	 * mode; enable the LP generator's ECC/ack defaults untouched. */
	mipi_dsi_host_ll_enable_video_mode(data->hal.host, false);

	LOG_INF("ESP32-P4 MIPI-DSI host ready: %u lane(s) @ %u Mbps (real %d.%02d Mbps)",
		hal_cfg.num_data_lanes, (unsigned int)hal_cfg.lane_bit_rate_mbps,
		(int)data->hal.lane_bit_rate_mbps,
		(int)((data->hal.lane_bit_rate_mbps - (int)data->hal.lane_bit_rate_mbps) * 100));
	LOG_INF("  baseline: mode_cfg=0x%08x cmd_mode_cfg=0x%08x phy_if_cfg=0x%08x "
		"phy_tmr_cfg=0x%08x phy_tmr_lpclk_cfg=0x%08x",
		data->hal.host->mode_cfg.val, data->hal.host->cmd_mode_cfg.val,
		data->hal.host->phy_if_cfg.val, data->hal.host->phy_tmr_cfg.val,
		data->hal.host->phy_tmr_lpclk_cfg.val);

	return 0;
}

#if DT_INST_NODE_HAS_PROP(0, phy_supply)
#define DSI_ESP32_PHY_SUPPLY_GET DEVICE_DT_GET(DT_INST_PHANDLE(0, phy_supply))
#else
#define DSI_ESP32_PHY_SUPPLY_GET NULL
#endif

static const struct dsi_esp32_config dsi_esp32_cfg_0 = {
	.dpi_clock_freq_mhz = DT_INST_PROP(0, dpi_clock_freq_mhz),
	.phy_supply = DSI_ESP32_PHY_SUPPLY_GET,
};

static struct dsi_esp32_data dsi_esp32_data_0;

DEVICE_DT_INST_DEFINE(0, dsi_esp32_init, NULL, &dsi_esp32_data_0, &dsi_esp32_cfg_0, POST_KERNEL,
		       60, &dsi_esp32_api);
