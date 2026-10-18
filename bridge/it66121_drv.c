// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on Linux v7.0 drivers/gpu/drm/bridge/ite-it66121.c.
 * Copyright (C) 2020 BayLibre, SAS
 * Author: Phong LE <ple@baylibre.com>
 * Copyright (C) 2018-2019, Artem Mygaiev
 * Copyright (C) 2017, Fresco Logic, Incorporated.
 *
 * Firmware graph and component attachment with adapter-described I2C access,
 * parallel input timing and controller-owned reset.
 */
#include <linux/bitfield.h>
#include <linux/component.h>
#include <linux/gpio/consumer.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <sound/hdmi-codec.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/property.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/media-bus-format.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>

#include "it66121_registers.h"

/**
 * enum chip_id - Silicon variants selected from hardware identity
 * @ID_IT6610: IT6610 transmitter with chip-specific AFE/input constraints.
 * @ID_IT66121: IT66121 HDMI transmitter.
 * @ID_IT66122: IT66122 HDMI transmitter.
 */
enum chip_id {
	ID_IT6610,
	ID_IT66121,
	ID_IT66122,
};

/**
 * struct it66121_chip_info - Identity match excluding silicon revision
 * @id: Internal variant used for register-programming differences.
 * @vid: Little-endian vendor identity word.
 * @pid: Device identity word with revision bits excluded.
 */
struct it66121_chip_info {
	enum chip_id id;
	u16 vid;
	u16 pid;
};

/**
 * struct it66121_ctx - Shared bridge state for OF and component attachment
 * @regmap: Uncached eight-bit banked register access.
 * @bridge: Reference-counted DRM bridge; provider module must outlive
 *          callbacks.
 * @connector: Active atomic connector, or NULL after bridge disable/unbind.
 * @dev: Owning I2C device.
 * @client: I2C client registered by firmware or enumeration.
 * @adapter: Parent I2C adapter carrying transport properties and optional IRQ
 *           domain.
 * @irq: Component IRQ requested at bind, or zero while unbound.
 * @aligned_i2c: Four-byte aligned accesses consume adjacent register side
 *               effects.
 * @reset_by_controller: Display-controller reset requires bridge-setting
 *                       restoration.
 * @component_binding: Use encoder component bind instead of OF downstream
 *                     graph.
 * @input_clock_delay: Hardware pixel-clock selector, not nanoseconds.
 * @irq_output_mode: Polarity/drive bits restored after controller-owned
 *                   reset.
 * @hpd_enabled: Last framework HPD enable state, restored across controller
 *               reset.
 * @gpio_reset: Optional native-board reset GPIO.
 * @audio: Audio register state protected by the bridge mutex.
 * @audio.ch_enable: Configured I2S source/sample-width register value.
 * @audio.fs: Hardware sample-frequency selector.
 * @audio.swl: Sample-word-length state.
 * @audio.auto_cts: Enable measured CTS programming.
 * @bus_width: Parallel input width, 12 or 24 bits.
 * @hdmi_avi_infoframe: AVI state packed for the active mode.
 * @id: Silicon identity selected by probe.
 *
 * The register mutex protects mode, DDC, IRQ and audio sequences. Requesting
 * connector lifetime is checked independently of bridge attachment so old
 * DRM files cannot access a newly rebound display through stale callbacks.
 */
struct it66121_ctx {
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct drm_connector *connector;
	struct device *dev;
	struct i2c_client *client;
	struct i2c_adapter *adapter;
	int irq;
	bool aligned_i2c;
	bool reset_by_controller;
	bool component_binding;
	u8 input_clock_delay;
	u8 irq_output_mode;
	bool hpd_enabled;
	struct gpio_desc *gpio_reset;
	struct {
		u8 ch_enable;
		u8 fs;
		u8 swl;
		bool auto_cts;
	} audio;
	u32 bus_width;
	/** @lock: Serializes mode, DDC, IRQ and audio register sequences. */
	struct mutex lock;
	struct hdmi_avi_infoframe hdmi_avi_infoframe;
	enum chip_id id;
};

/**
 * var it66121_regmap_banks - Map two register banks with common low registers
 */
static const struct regmap_range_cfg it66121_regmap_banks[] = {
	{
		.name = "it66121",
		.range_min = 0,
		.range_max = IT66121_MAX_REGISTER,
		.selector_reg = IT66121_CLK_BANK_REG,
		.selector_mask = IT66121_CLK_BANK_SELECT,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = IT66121_REG_BANK_SIZE,
	},
};

/**
 * it66121_init_regmap() - Create the uncached banked bridge regmap
 * @ctx: Bridge instance.
 *
 * Aligned adapters use single-byte regmap operations; DDC side effects are
 * handled separately by aligned transfer helpers.
 *
 * Context: Process context. May sleep.
 * Return: Managed regmap or ERR_PTR() on failure.
 */
static struct regmap *it66121_init_regmap(struct it66121_ctx *ctx)
{
	const struct regmap_config config = {
		.val_bits = 8,
		.reg_bits = 8,
		.max_register = IT66121_MAX_REGISTER,
		.ranges = it66121_regmap_banks,
		.num_ranges = ARRAY_SIZE(it66121_regmap_banks),
		.cache_type = REGCACHE_NONE,
		.use_single_read = ctx->aligned_i2c,
		.use_single_write = ctx->aligned_i2c,
	};

	return devm_regmap_init_i2c(ctx->client, &config);
}

#define EDID_SLEEP     1000
#define EDID_TIMEOUT   200000
#define EDID_FIFO_SIZE 32
/* Aligned transport consumes three FIFO bytes before the EDID payload. */
#define IT66121_ALIGNED_FIFO_SKIP 3
#define EDID_DDC_ADDR		  IT66121_DDC_HEADER_EDID
#define IT66121_DDC_CONTROL	  IT66121_MASTER_SEL_REG
#define IT66121_DDC_SEGMENT	  IT66121_DDC_SEGMENT_REG
#define IT66121_DDC_MASTER_HOST	  IT66121_MASTER_SEL_HOST
#define DDC_CMD_FIFO_CLEAR	  IT66121_DDC_COMMAND_FIFO_CLR
#define DDC_CMD_ABORT		  IT66121_DDC_COMMAND_ABORT
#define DDC_CMD_EDID_READ	  IT66121_DDC_COMMAND_EDID_READ

/**
 * it66121_preamble_ddc() - Select host ownership of the DDC engine
 * @ctx: Bridge instance.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static inline int it66121_preamble_ddc(struct it66121_ctx *ctx)
{
	return regmap_write(ctx->regmap, IT66121_MASTER_SEL_REG, IT66121_MASTER_SEL_HOST);
}

/**
 * it66121_fire_afe() - Release the transmitter analog driver reset
 * @ctx: Bridge instance.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static inline int it66121_fire_afe(struct it66121_ctx *ctx)
{
	return regmap_write(ctx->regmap, IT66121_AFE_DRV_REG, 0);
}

/**
 * it66121_configure_input() - Program RGB bus width, delay and CSC bypass
 * @ctx: Bridge instance.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_configure_input(struct it66121_ctx *ctx)
{
	int ret;
	u8 mode = IT66121_INPUT_MODE_RGB | ctx->input_clock_delay;

	if (ctx->bus_width == 12)
		mode |= IT66121_INPUT_MODE_DDR;

	ret = regmap_write(ctx->regmap, IT66121_INPUT_MODE_REG, mode);
	if (ret)
		return ret;

	return regmap_write(ctx->regmap, IT66121_INPUT_CSC_REG, IT66121_INPUT_CSC_NO_CONV);
}

/**
 * it66121_configure_afe() - Set analog gains and resets for the pixel clock
 * @ctx: Bridge instance.
 * @mode: Display timing to inspect.
 *
 * Uses chip-specific low/high-clock and IT6610 bypass settings. The adjusted
 * mode supplies the pixel clock in kHz.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_configure_afe(struct it66121_ctx *ctx, const struct drm_display_mode *mode)
{
	bool high_clock = mode->clock > IT66121_AFE_CLK_HIGH;
	int ret;

	ret = regmap_write(ctx->regmap, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_RST);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG,
				IT66121_AFE_XP_GAINBIT | IT66121_AFE_XP_ENO,
				high_clock ? IT66121_AFE_XP_GAINBIT : IT66121_AFE_XP_ENO);
	if (ret)
		return ret;
	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG,
				IT66121_AFE_IP_GAINBIT | IT66121_AFE_IP_ER0,
				high_clock ? IT66121_AFE_IP_GAINBIT : IT66121_AFE_IP_ER0);
	if (ret)
		return ret;
	if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG, IT66121_AFE_IP_EC1,
					high_clock ? 0 : IT66121_AFE_IP_EC1);
		if (ret)
			return ret;
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_EC1_REG,
					IT66121_AFE_XP_EC1_LOWCLK,
					high_clock ? 0 : IT66121_AFE_XP_EC1_LOWCLK);
		if (ret)
			return ret;
	}

	ret = regmap_write_bits(ctx->regmap, IT66121_SW_RST_REG,
				IT66121_SW_RST_REF | IT66121_SW_RST_VID, 0);
	if (ret)
		return ret;

	if (ctx->id == ID_IT6610) {
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG, IT6610_AFE_XP_BYPASS,
					IT6610_AFE_XP_BYPASS);
		if (ret)
			return ret;
	}

	return it66121_fire_afe(ctx);
}

/**
 * it66121_ddc_snapshot() - Read a destructive four-byte DDC status snapshot
 * @priv: Bridge instance.
 * @word: Four-byte destination including status and a consumed FIFO byte.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_snapshot(struct it66121_ctx *priv, u8 *word)
{
	u8 reg = IT66121_DDC_SEGMENT;
	struct i2c_msg msgs[] = {
		{ .addr = priv->client->addr, .len = 1, .buf = &reg },
		{ .addr = priv->client->addr, .flags = I2C_M_RD, .len = 4, .buf = word },
	};
	int ret = i2c_transfer(priv->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/**
 * it66121_ddc_write() - Write one full DDC control group without RMW
 * @priv: Bridge instance.
 * @reg: Register offset.
 * @a: Byte at the starting register.
 * @b: Second control byte.
 * @c: Third control byte.
 * @d: Fourth control byte.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_write(struct it66121_ctx *priv, u8 reg, u8 a, u8 b, u8 c, u8 d)
{
	u8 word[] = { reg, a, b, c, d };
	struct i2c_msg msg = {
		.addr = priv->client->addr,
		.len = sizeof(word),
		.buf = word,
	};
	int ret = i2c_transfer(priv->adapter, &msg, 1);

	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

/**
 * it66121_ddc_collect() - Preserve FIFO bytes consumed by aligned status reads
 * @priv: Bridge using aligned four-byte register access.
 * @buf: Destination for exactly @len FIFO bytes, or NULL for status only.
 * @len: Expected FIFO byte count; ignored when @buf is NULL.
 *
 * Each snapshot includes status and a destructive FIFO byte. Keep that byte
 * instead of rereading the FIFO. Apply the readiness deadline only before
 * DONE; USB latency while draining a completed finite response is not a new
 * DDC timeout. Reject excess/missing payload and hardware error status.
 *
 * Context: Process context. Caller serializes the bridge DDC sequence.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_collect(struct it66121_ctx *priv, u8 *buf, size_t len)
{
	ktime_t deadline = ktime_add_us(ktime_get(), EDID_TIMEOUT);
	size_t received = 0;
	u8 word[4];
	int ret;

	for (;;) {
		ret = it66121_ddc_snapshot(priv, word);
		if (ret)
			return ret;
		dev_dbg(&priv->client->dev, "DDC status=%02x fifo=%02x received=%zu/%zu\n", word[2],
			word[3], received, len);
		if (word[2] & GENMASK(5, 3))
			return -EIO;
		if (buf && !(word[2] & BIT(1))) {
			if (received == len)
				return -EIO;
			buf[received++] = word[3];
		}
		if ((word[2] & BIT(7)) && (!buf || received == len))
			return 0;
		if ((word[2] & BIT(7)) && (word[2] & BIT(1)))
			return -EIO;
		if (!(word[2] & BIT(7)) && ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		if (!(word[2] & BIT(7)) || (word[2] & BIT(1)))
			usleep_range(EDID_SLEEP, EDID_SLEEP * 2);
	}
}

/**
 * it66121_wait_ddc_ready() - Wait for DDC completion without losing FIFO data
 * @ctx: Bridge instance.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static inline int it66121_wait_ddc_ready(struct it66121_ctx *ctx)
{
	int ret;
	int val;
	u32 error = IT66121_DDC_STATUS_NOACK | IT66121_DDC_STATUS_WAIT_BUS |
		    IT66121_DDC_STATUS_ARBI_LOSE;
	u32 done = IT66121_DDC_STATUS_TX_DONE;

	if (ctx->aligned_i2c)
		return it66121_ddc_collect(ctx, NULL, 0);

	ret = regmap_read_poll_timeout(ctx->regmap, IT66121_DDC_STATUS_REG, val,
				       val & (error | done), IT66121_EDID_SLEEP_US,
				       IT66121_EDID_TIMEOUT_US);
	if (ret)
		return ret;

	if (val & error)
		return -EAGAIN;

	return 0;
}

/**
 * it66121_ddc_command() - Issue a DDC command using transport-safe access
 * @ctx: Bridge instance.
 * @command: FIFO-clear, abort or EDID-read command.
 * @segment: EDID segment to select for a read command.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_command(struct it66121_ctx *ctx, u8 command, u8 segment)
{
	int ret;

	if (ctx->aligned_i2c)
		return it66121_ddc_write(ctx, IT66121_DDC_SEGMENT, segment, command, 0, 0);
	if (command == DDC_CMD_EDID_READ) {
		ret = regmap_write(ctx->regmap, IT66121_DDC_SEGMENT_REG, segment);
		if (ret)
			return ret;
	}
	return regmap_write(ctx->regmap, IT66121_DDC_COMMAND_REG, command);
}

/**
 * it66121_abort_ddc_ops() - Reset DDC ownership and abort pending transactions
 * @ctx: Bridge instance.
 *
 * Ordinary access preserves the native HDCP reset sequence. Aligned access
 * repeats abort according to the controller recovery sequence.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_abort_ddc_ops(struct it66121_ctx *ctx)
{
	unsigned int swreset;
	unsigned int cpdesire;
	int ret;

	if (!ctx->aligned_i2c) {
		ret = regmap_read(ctx->regmap, IT66121_SW_RST_REG, &swreset);
		if (ret)
			return ret;
		ret = regmap_read(ctx->regmap, IT66121_HDCP_REG, &cpdesire);
		if (ret)
			return ret;
		ret = regmap_write(ctx->regmap, IT66121_HDCP_REG,
				   cpdesire & (~IT66121_HDCP_CPDESIRED & 0xff));
		if (ret)
			return ret;
		ret = regmap_write(ctx->regmap, IT66121_SW_RST_REG, swreset | IT66121_SW_RST_HDCP);
		if (ret)
			return ret;
		ret = it66121_preamble_ddc(ctx);
		if (ret)
			return ret;
	}
	/* Repeat ABORT for aligned access, following the controller recovery sequence. */
	for (unsigned int i = 0; i < (ctx->aligned_i2c ? 2 : 1); i++) {
		ret = it66121_ddc_command(ctx, DDC_CMD_ABORT, 0);
		if (ret)
			return ret;
		ret = it66121_wait_ddc_ready(ctx);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * it66121_ddc_prepare() - Set host ownership and EDID slave address
 * @ctx: Bridge instance.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_prepare(struct it66121_ctx *ctx)
{
	int ret;

	if (ctx->aligned_i2c)
		return it66121_ddc_write(ctx, IT66121_DDC_CONTROL, IT66121_DDC_MASTER_HOST,
					 EDID_DDC_ADDR, 0, 0);
	ret = it66121_preamble_ddc(ctx);
	if (ret)
		return ret;
	return regmap_write(ctx->regmap, IT66121_DDC_HEADER_REG, EDID_DDC_ADDR);
}

/**
 * it66121_ddc_set_request() - Program one EDID request window
 * @ctx: Bridge instance.
 * @offset: Eight-bit byte offset within the selected segment.
 * @count: Requested FIFO bytes, including aligned transport skip.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_set_request(struct it66121_ctx *ctx, u8 offset, u8 count)
{
	int ret;

	if (ctx->aligned_i2c)
		return it66121_ddc_write(ctx, IT66121_DDC_CONTROL, IT66121_DDC_MASTER_HOST,
					 EDID_DDC_ADDR, offset, count);
	ret = regmap_write(ctx->regmap, IT66121_DDC_OFFSET_REG, offset);
	if (ret)
		return ret;
	return regmap_write(ctx->regmap, IT66121_DDC_BYTE_REG, count);
}

/**
 * it66121_ddc_read_fifo() - Drain one transport-specific EDID response
 * @ctx: Bridge instance.
 * @buf: Output buffer.
 * @count: Payload byte count to copy.
 *
 * Aligned snapshots consume FIFO data and wait for the burst before draining.
 * Ordinary access waits for DONE, then reads the non-incrementing FIFO.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_ddc_read_fifo(struct it66121_ctx *ctx, u8 *buf, size_t count)
{
	int ret;

	if (ctx->aligned_i2c) {
		/* A word snapshot consumes FIFO data; let the burst arrive first. */
		usleep_range(20000, 25000);
		return it66121_ddc_collect(ctx, buf, count);
	}
	ret = it66121_wait_ddc_ready(ctx);
	if (ret) {
		it66121_abort_ddc_ops(ctx);
		return ret;
	}
	return regmap_noinc_read(ctx->regmap, IT66121_DDC_RD_FIFO_REG, buf, count);
}

/**
 * it66121_get_edid_block() - Read an EDID block in bounded FIFO requests
 * @context: Bridge instance passed by drm_edid_read_custom().
 * @buf: Output buffer.
 * @block: Zero-based EDID block number.
 * @len: Requested block bytes, from 1 through 128.
 *
 * Aligned transport requests three preceding bytes and up to 29 payload bytes,
 * wrapping the eight-bit offset within its segment. It preserves received EDID
 * bytes rather than synthesizing headers; later segments share this algorithm.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct it66121_ctx *ctx = context;
	unsigned int offset = (block & 1) * 128;
	unsigned int segment = block >> 1;
	unsigned int skip = ctx->aligned_i2c ? IT66121_ALIGNED_FIFO_SKIP : 0;
	int ret;

	if (!buf || !len || len > 128 || segment > 255)
		return -EINVAL;
	ret = it66121_ddc_prepare(ctx);
	if (ret)
		return ret;
	if (ctx->aligned_i2c) {
		ret = it66121_abort_ddc_ops(ctx);
		if (ret)
			return ret;
	}
	while (len) {
		size_t count = min_t(size_t, len, EDID_FIFO_SIZE - skip);

		ret = it66121_ddc_command(ctx, DDC_CMD_FIFO_CLEAR, 0);
		if (ret)
			return ret;
		ret = it66121_wait_ddc_ready(ctx);
		if (ret)
			return ret;
		ret = it66121_ddc_set_request(ctx, (u8)(offset - skip), (u8)(count + skip));
		if (ret)
			return ret;
		ret = it66121_ddc_command(ctx, DDC_CMD_EDID_READ, (u8)segment);
		if (ret)
			return ret;
		ret = it66121_ddc_read_fifo(ctx, buf, count);
		if (ret)
			return ret;
		buf += count;
		offset += count;
		len -= count;
	}
	return 0;
}

/**
 * it66121_bridge_attach() - Power the transmitter and attach any downstream bridge
 * @bridge: DRM bridge.
 * @encoder: DRM encoder.
 * @flags: Must include DRM_BRIDGE_ATTACH_NO_CONNECTOR.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;

	/* Firmware-described boards may have a downstream bridge. */
	if (ctx->bridge.next_bridge) {
		ret = drm_bridge_attach(encoder, ctx->bridge.next_bridge, bridge, flags);
		if (ret)
			return ret;
	}

	if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
		ret = regmap_write_bits(ctx->regmap, IT66121_CLK_BANK_REG,
					IT66121_CLK_BANK_PWROFF_RCLK, 0);
		if (ret)
			return ret;
	}

	ret = regmap_write_bits(ctx->regmap, IT66121_INT_REG, IT66121_INT_TX_CLK_OFF, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_PWD, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG,
				IT66121_AFE_XP_PWDI | IT66121_AFE_XP_PWDPLL, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG, IT66121_AFE_IP_PWDPLL, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_RST, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG, IT66121_AFE_XP_RESETB,
				IT66121_AFE_XP_RESETB);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG, IT66121_AFE_IP_RESETB,
				IT66121_AFE_IP_RESETB);
	if (ret)
		return ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_SW_RST_REG, IT66121_SW_RST_REF,
				IT66121_SW_RST_REF);
	if (ret)
		return ret;

	/* Per programming manual, sleep here for bridge to settle */
	msleep(50);

	return 0;
}

/**
 * it66121_set_mute() - Set AV mute and repeating general-control packets
 * @ctx: Bridge instance.
 * @mute: True to mute, false to unmute.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_set_mute(struct it66121_ctx *ctx, bool mute)
{
	int ret;
	unsigned int val = 0;

	if (mute)
		val = IT66121_AV_MUTE_ON;

	ret = regmap_write_bits(ctx->regmap, IT66121_AV_MUTE_REG, IT66121_AV_MUTE_ON, val);
	if (ret)
		return ret;

	return regmap_write(ctx->regmap, IT66121_PKT_GEN_CTRL_REG,
			    IT66121_PKT_GEN_CTRL_ON | IT66121_PKT_GEN_CTRL_RPT);
}

/**
 * it66121_bridge_atomic_get_output_bus_fmts() - Advertise RGB888 parallel output
 * @bridge: DRM bridge; unused.
 * @bridge_state: Bridge atomic state; unused.
 * @crtc_state: CRTC state; unused.
 * @conn_state: Connector atomic state; unused.
 * @num_output_fmts: Set to one after successful allocation.
 *
 * Context: Process context. May sleep.
 * Return: Allocated one-format array, or NULL on allocation failure.
 */
static u32 *
it66121_bridge_atomic_get_output_bus_fmts(struct drm_bridge *bridge __always_unused,
					  struct drm_bridge_state *bridge_state __always_unused,
					  struct drm_crtc_state *crtc_state __always_unused,
					  struct drm_connector_state *conn_state __always_unused,
					  unsigned int *num_output_fmts)
{
	u32 *output_fmts;

	output_fmts = kcalloc(1, sizeof(*output_fmts), GFP_KERNEL);
	if (!output_fmts)
		return NULL;

	output_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_output_fmts = 1;

	return output_fmts;
}

/**
 * it66121_get_input_bus_fmts() - Advertise configured 12-bit DDR or 24-bit input
 * @bridge: DRM bridge.
 * @bridge_state: Bridge atomic state; unused.
 * @crtc_state: CRTC state; unused.
 * @conn_state: Connector atomic state; unused.
 * @output_fmt: Requested output format; unused.
 * @num_input_fmts: Set to zero initially and one after successful allocation.
 *
 * Context: Process context. May sleep.
 * Return: Allocated one-format array, or NULL on allocation failure.
 */
static u32 *it66121_get_input_bus_fmts(struct drm_bridge *bridge,
				       struct drm_bridge_state *bridge_state __always_unused,
				       struct drm_crtc_state *crtc_state __always_unused,
				       struct drm_connector_state *conn_state __always_unused,
				       u32 output_fmt __always_unused, unsigned int *num_input_fmts)
{
	const struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	u32 *input_fmts;

	*num_input_fmts = 0;

	input_fmts = kcalloc(1, sizeof(*input_fmts), GFP_KERNEL);
	if (!input_fmts)
		return NULL;

	if (ctx->bus_width == 12)
		/* The 12-bit DDR input uses little-endian pixel ordering. */
		input_fmts[0] = MEDIA_BUS_FMT_RGB888_2X12_LE;
	else
		input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_input_fmts = 1;

	return input_fmts;
}

/**
 * it66121_bridge_hpd_enable() - Unmask HPD and retain reset-restoration intent
 * @bridge: DRM bridge.
 *
 * Context: Process context. May sleep.
 */
static void it66121_bridge_hpd_enable(struct drm_bridge *bridge)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;
	int idx;
	struct drm_device *drm = READ_ONCE(bridge->dev);

	if (!drm || !drm_dev_enter(drm, &idx))
		return;

	mutex_lock(&ctx->lock);
	ctx->hpd_enabled = true;
	ret = regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_HPD, 0);
	mutex_unlock(&ctx->lock);
	if (ret)
		dev_err(ctx->dev, "failed to enable HPD IRQ\n");
	drm_dev_exit(idx);
}

/**
 * it66121_bridge_hpd_disable() - Mask HPD and retain reset-restoration intent
 * @bridge: DRM bridge.
 *
 * Context: Process context. May sleep.
 */
static void it66121_bridge_hpd_disable(struct drm_bridge *bridge)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;
	int idx;
	struct drm_device *drm = READ_ONCE(bridge->dev);

	if (!drm || !drm_dev_enter(drm, &idx))
		return;

	mutex_lock(&ctx->lock);
	ctx->hpd_enabled = false;
	ret = regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_HPD,
				IT66121_INT_MASK1_HPD);
	mutex_unlock(&ctx->lock);
	if (ret)
		dev_err(ctx->dev, "failed to disable HPD IRQ\n");
	drm_dev_exit(idx);
}

/**
 * it66121_restore_interrupts() - Restore IRQ wiring and HPD mask after reset
 * @ctx: Bridge instance.
 *
 * Does nothing when reset ownership is not assigned to the controller.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_restore_interrupts(struct it66121_ctx *ctx)
{
	int ret;

	if (!ctx->reset_by_controller)
		return 0;
	/* Restore settings lost when the display controller resets the bridge. */
	ret = regmap_write_bits(ctx->regmap, IT66121_INT_REG,
				IT66121_INT_ACTIVE_HIGH | IT66121_INT_OPEN_DRAIN,
				ctx->irq_output_mode);
	if (ret)
		return ret;
	return regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_ALL,
				 ctx->hpd_enabled ? IT66121_INT_MASK1_ALL & ~IT66121_INT_MASK1_HPD :
						    IT66121_INT_MASK1_ALL);
}

/**
 * it66121_bridge_disable() - Mute output and forget the active connector
 * @bridge: DRM bridge.
 * @state: Atomic transaction state; unused.
 *
 * Context: Process context. May sleep.
 */
static void it66121_bridge_disable(struct drm_bridge *bridge,
				   struct drm_atomic_state *state __always_unused)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	mutex_lock(&ctx->lock);
	it66121_set_mute(ctx, true);
	ctx->connector = NULL;

	mutex_unlock(&ctx->lock);
}

/**
 * it66121_bridge_check() - Apply the IT6610 input-edge constraints
 * @bridge: DRM bridge.
 * @bridge_state: Bridge atomic state.
 * @crtc_state: CRTC state; unused.
 * @conn_state: Connector atomic state; unused.
 *
 * Context: Process context under DRM atomic/modeset serialization.
 * Return: 0 after updating the input bus flags.
 */
static int it66121_bridge_check(struct drm_bridge *bridge, struct drm_bridge_state *bridge_state,
				struct drm_crtc_state *crtc_state __always_unused,
				struct drm_connector_state *conn_state __always_unused)
{
	const struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	if (ctx->id == ID_IT6610) {
		/* The IT6610 only supports these settings */
		bridge_state->input_bus_cfg.flags |= DRM_BUS_FLAG_DE_HIGH |
						     DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;
		bridge_state->input_bus_cfg.flags &= ~DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE;
	}

	return 0;
}

/**
 * it66121_configure_hdmi() - Select HDMI/DVI and program AVI information
 * @ctx: Bridge instance.
 * @mode: Display timing to inspect.
 *
 * Requires the active connector. Uses the shared AVI payload write sequence;
 * the register layout conflict is documented beside the register definitions.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it66121_configure_hdmi(struct it66121_ctx *ctx, const struct drm_display_mode *mode)
{
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	ssize_t packed_len;
	int ret;

	if (!ctx->connector->display_info.is_hdmi) {
		ret = regmap_write(ctx->regmap, IT66121_AVI_INFO_PKT_REG, 0);
		if (ret)
			return ret;
		return regmap_write(ctx->regmap, IT66121_HDMI_MODE_REG, IT66121_HDMI_MODE_DVI);
	}
	ret = drm_hdmi_avi_infoframe_from_display_mode(&ctx->hdmi_avi_infoframe, ctx->connector,
						       mode);
	if (ret) {
		DRM_ERROR("Failed to setup AVI infoframe: %d\n", ret);
		return ret;
	}
	packed_len = hdmi_avi_infoframe_pack(&ctx->hdmi_avi_infoframe, buf, sizeof(buf));
	if (packed_len < 0) {
		DRM_ERROR("Failed to pack infoframe: %zd\n", packed_len);
		return (int)packed_len;
	}
	ret = regmap_bulk_write(ctx->regmap, IT66121_AVIINFO_DB1_REG,
				&buf[HDMI_INFOFRAME_HEADER_SIZE], HDMI_AVI_INFOFRAME_SIZE);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, IT66121_AVIINFO_CSUM_REG, buf[3]);
	if (ret)
		return ret;
	ret = regmap_write(ctx->regmap, IT66121_AVI_INFO_PKT_REG,
			   IT66121_AVI_INFO_PKT_ON | IT66121_AVI_INFO_PKT_RPT);
	if (ret)
		return ret;
	return regmap_write(ctx->regmap, IT66121_HDMI_MODE_REG, IT66121_HDMI_MODE_HDMI);
}

/**
 * it66121_bridge_enable() - Program one mode after display-controller setup
 * @bridge: DRM bridge.
 * @state: Atomic transaction state.
 *
 * Resolves connector and adjusted CRTC mode from the committing atomic state.
 * Serializes programming, restores reset-owned IRQ settings and then unmutes.
 * Programming failures are logged.
 *
 * Context: Process context. May sleep.
 */
static void it66121_bridge_enable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	const struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	int ret;

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	crtc_state = drm_atomic_get_new_crtc_state(state, bridge->encoder->crtc);
	if (!connector || !crtc_state) {
		dev_err(ctx->dev, "Missing atomic display state\n");
		return;
	}

	mutex_lock(&ctx->lock);
	ctx->connector = connector;
	/* Atomic enable follows display-controller configuration. */
	ret = it66121_configure_hdmi(ctx, &crtc_state->adjusted_mode);
	if (ret)
		goto unlock;

	if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
		ret = regmap_write_bits(ctx->regmap, IT66121_CLK_BANK_REG,
					IT66121_CLK_BANK_PWROFF_TXCLK,
					IT66121_CLK_BANK_PWROFF_TXCLK);
		if (ret)
			goto unlock;
	}
	ret = it66121_configure_input(ctx);
	if (ret)
		goto unlock;
	ret = it66121_configure_afe(ctx, &crtc_state->adjusted_mode);
	if (ret)
		goto unlock;

	if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
		ret = regmap_write_bits(ctx->regmap, IT66121_CLK_BANK_REG,
					IT66121_CLK_BANK_PWROFF_TXCLK, 0);
		if (ret)
			goto unlock;
	}
	ret = it66121_restore_interrupts(ctx);
	if (!ret)
		ret = it66121_set_mute(ctx, false);
unlock:
	if (ret)
		dev_err(ctx->dev, "Failed to enable HDMI output: %d\n", ret);
	mutex_unlock(&ctx->lock);
}

/**
 * it66121_bridge_mode_valid() - Check clock limits for the input width
 * @bridge: DRM bridge.
 * @info: Display capability argument; unused.
 * @mode: Display timing to inspect.
 *
 * Context: Any context. Does not sleep.
 * Return: MODE_OK, MODE_CLOCK_LOW or MODE_CLOCK_HIGH.
 */
static enum drm_mode_status
it66121_bridge_mode_valid(struct drm_bridge *bridge,
			  const struct drm_display_info *info __always_unused,
			  const struct drm_display_mode *mode)
{
	const struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	unsigned long max_clock;

	max_clock = (ctx->bus_width == 12) ? 74250 : 148500;

	if (mode->clock > max_clock)
		return MODE_CLOCK_HIGH;

	if (mode->clock < 25000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

/**
 * it66121_bridge_detect() - Read HPD without acknowledging interrupts
 * @bridge: DRM bridge.
 * @connector: Requesting DRM connector.
 *
 * Checks the requesting connector device, not a potentially rebound bridge
 * device. Takes the bridge mutex while reading hardware.
 *
 * Context: Process context. May sleep.
 * Return: Known status, unknown on I2C failure, or disconnected after unplug.
 */
static enum drm_connector_status it66121_bridge_detect(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	enum drm_connector_status status;
	unsigned int value;
	int idx;
	int ret;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;
	mutex_lock(&ctx->lock);
	ret = regmap_read(ctx->regmap, IT66121_SYS_STATUS_REG, &value);
	if (ret)
		status = connector_status_unknown;
	else
		status = value & IT66121_SYS_STATUS_HPDETECT ? connector_status_connected :
							       connector_status_disconnected;
	mutex_unlock(&ctx->lock);
	drm_dev_exit(idx);
	return status;
}

/**
 * it66121_bridge_edid_read() - Read EDID for the requesting live connector
 * @bridge: DRM bridge.
 * @connector: Requesting DRM connector.
 *
 * Holds the bridge mutex over the complete DDC sequence. The requesting
 * connector determines unplug lifetime even if the bridge has been rebound.
 *
 * Context: Process context. May sleep.
 * Return: Owned DRM EDID object, or NULL on unplug/read failure.
 */
static const struct drm_edid *it66121_bridge_edid_read(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	const struct drm_edid *edid;
	int idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return NULL;
	mutex_lock(&ctx->lock);
	edid = drm_edid_read_custom(connector, it66121_get_edid_block, ctx);
	mutex_unlock(&ctx->lock);
	drm_dev_exit(idx);
	return edid;
}

/**
 * var it66121_bridge_funcs - Shared DRM bridge state, modeset, EDID and HPD callbacks
 */
static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.attach = it66121_bridge_attach,
	.atomic_get_output_bus_fmts = it66121_bridge_atomic_get_output_bus_fmts,
	.atomic_get_input_bus_fmts = it66121_get_input_bus_fmts,
	.atomic_enable = it66121_bridge_enable,
	.atomic_disable = it66121_bridge_disable,
	.atomic_check = it66121_bridge_check,
	.mode_valid = it66121_bridge_mode_valid,
	.detect = it66121_bridge_detect,
	.edid_read = it66121_bridge_edid_read,
	.hpd_enable = it66121_bridge_hpd_enable,
	.hpd_disable = it66121_bridge_hpd_disable,
};

/**
 * it66121_interrupt() - Acknowledge bridge status before reporting HPD
 * @ctx: Bridge instance.
 *
 * Takes the bridge mutex for register access and notifies DRM after dropping
 * it. Polling detection never performs this acknowledgment.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_interrupt(struct it66121_ctx *ctx)
{
	enum drm_connector_status status;
	unsigned int val;
	bool event = false;
	int ret;

	mutex_lock(&ctx->lock);
	ret = regmap_read(ctx->regmap, IT66121_SYS_STATUS_REG, &val);
	if (ret || !(val & IT66121_SYS_STATUS_ACTIVE_IRQ))
		goto unlock;
	status = val & IT66121_SYS_STATUS_HPDETECT ? connector_status_connected :
						     connector_status_disconnected;

	ret = regmap_read(ctx->regmap, IT66121_INT_STATUS1_REG, &val);
	if (ret)
		goto unlock;
	if (val & IT66121_INT_STATUS1_HPD_STATUS) {
		ret = regmap_write_bits(ctx->regmap, IT66121_INT_CLR1_REG, IT66121_INT_CLR1_HPD,
					IT66121_INT_CLR1_HPD);
		if (ret)
			goto unlock;
		event = true;
	}

	ret = regmap_write_bits(ctx->regmap, IT66121_SYS_STATUS_REG, IT66121_SYS_STATUS_CLEAR_IRQ,
				IT66121_SYS_STATUS_CLEAR_IRQ);
unlock:
	mutex_unlock(&ctx->lock);
	if (!ret && event)
		drm_bridge_hpd_notify(&ctx->bridge, status);
	return ret;
}

/**
 * it66121_irq_thread() - Run shared bridge acknowledgment
 * @irq: Linux IRQ number; unused.
 * @data: Bridge instance.
 *
 * Logs acknowledgment failure. A parent transport can independently observe
 * failed I2C transactions; IRQ_NONE would not request acknowledgment retry.
 *
 * Context: Sleepable native IRQ thread or synchronous nested dispatch.
 * Return: IRQ_HANDLED, including when acknowledgment fails.
 */
static irqreturn_t it66121_irq_thread(int irq __always_unused, void *data)
{
	struct it66121_ctx *ctx = data;

	if (it66121_interrupt(ctx)) {
		dev_err_ratelimited(ctx->dev, "Interrupt acknowledgment failed\n");
		return IRQ_HANDLED;
	}
	return IRQ_HANDLED;
}

/**
 * it66121_bind() - Attach a component provider to the supplied DRM encoder
 * @comp: I2C bridge device registered as a component.
 * @master: Component master; unused by the provider.
 * @data: The master's struct drm_encoder pointer; not a private payload.
 *
 * Looks up input zero in the parent adapter's IRQ domain, requests a nested
 * level-low threaded IRQ and attaches without a connector. The master owns
 * connector creation and must drain its IRQ dispatch before unbind. This is
 * a local component contract, not automatic attachment for arbitrary bridges.
 *
 * Context: Process context under component framework serialization.
 * Return: 0 on success or a negative error code.
 */
static int it66121_bind(struct device *comp, struct device *master __always_unused, void *data)
{
	struct drm_encoder *encoder = data;
	struct irq_domain *domain;
	struct it66121_ctx *ctx = dev_get_drvdata(comp);
	int ret;

	if (!ctx->component_binding)
		return -ENODEV;
	domain = irq_find_matching_fwnode(dev_fwnode(&ctx->adapter->dev), DOMAIN_BUS_ANY);
	if (!domain)
		return -EPROBE_DEFER;
	ctx->irq = irq_find_mapping(domain, 0);
	if (!ctx->irq)
		return -EPROBE_DEFER;
	ret = request_threaded_irq(ctx->irq, NULL, it66121_irq_thread,
				   IRQF_ONESHOT | IRQF_TRIGGER_LOW, dev_name(comp), ctx);
	if (ret)
		return ret;
	ret = drm_bridge_attach(encoder, &ctx->bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret) {
		free_irq(ctx->irq, ctx);
		ctx->irq = 0;
	}
	return ret;
}

/**
 * it66121_unbind() - Release the component IRQ after master quiescence
 * @comp: Bound bridge component.
 * @master: Component master; unused.
 * @data: Encoder bind data; unused after the master detached its chain.
 *
 * The master has stopped nested IRQ delivery and cleaned up the encoder
 * before this callback. Clears the active connector without owning its lifetime.
 *
 * Context: Process context under component framework serialization.
 */
static void it66121_unbind(struct device *comp, struct device *master __always_unused,
			   void *data __always_unused)
{
	struct it66121_ctx *ctx = dev_get_drvdata(comp);

	free_irq(ctx->irq, ctx);
	ctx->irq = 0;
	ctx->connector = NULL;
}

/**
 * var it66121_component_ops - Firmware-less encoder and IRQ attachment callbacks
 */
static const struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

/**
 * it661221_set_chstat() - Write five IEC60958 channel-status bytes
 * @ctx: Bridge instance.
 * @iec60958_chstat: Five prepared IEC60958 channel-status bytes.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it661221_set_chstat(struct it66121_ctx *ctx, const u8 iec60958_chstat[])
{
	int ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CHST_MODE_REG, iec60958_chstat[0] & 0x7C);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CHST_CAT_REG, iec60958_chstat[1]);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CHST_SRCNUM_REG, iec60958_chstat[2] & 0x0F);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CHST_CHTNUM_REG,
			   (iec60958_chstat[2] >> 4) & 0x0F);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CHST_CA_FS_REG, iec60958_chstat[3]);
	if (ret)
		return ret;

	return regmap_write(ctx->regmap, IT66121_AUD_CHST_OFS_WL_REG, iec60958_chstat[4]);
}

/**
 * it661221_set_lpcm_audio() - Program I2S sample width and channel routing
 * @ctx: Bridge instance.
 * @audio_src_num: Number of stereo I2S sources, from one through four.
 * @audio_swl: Sample word length in bits.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it661221_set_lpcm_audio(struct it66121_ctx *ctx, unsigned int audio_src_num,
				   unsigned int audio_swl)
{
	int ret;
	u8 audio_enable = 0;
	unsigned int audio_format = 0;

	switch (audio_swl) {
	case 16:
		audio_enable |= IT66121_AUD_16BIT;
		break;
	case 18:
		audio_enable |= IT66121_AUD_18BIT;
		break;
	case 20:
		audio_enable |= IT66121_AUD_20BIT;
		break;
	case 24:
	default:
		audio_enable |= IT66121_AUD_24BIT;
		break;
	}

	audio_format |= 0x40;
	switch (audio_src_num) {
	case 4:
		audio_enable |= IT66121_AUD_EN_I2S3 | IT66121_AUD_EN_I2S2 | IT66121_AUD_EN_I2S1 |
				IT66121_AUD_EN_I2S0;
		break;
	case 3:
		audio_enable |= IT66121_AUD_EN_I2S2 | IT66121_AUD_EN_I2S1 | IT66121_AUD_EN_I2S0;
		break;
	case 2:
		audio_enable |= IT66121_AUD_EN_I2S1 | IT66121_AUD_EN_I2S0;
		break;
	case 1:
	default:
		audio_format &= ~0x40;
		audio_enable |= IT66121_AUD_EN_I2S0;
		break;
	}

	audio_format |= 0x01;
	ctx->audio.ch_enable = audio_enable;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CTRL0_REG, audio_enable & 0xF0);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CTRL1_REG, audio_format);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_FIFOMAP_REG, 0xE4);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_CTRL3_REG, 0x00);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_SRCVALID_FLAT_REG, 0x00);
	if (ret)
		return ret;

	return regmap_write(ctx->regmap, IT66121_AUD_HDAUDIO_REG, 0x00);
}

/**
 * it66121_audio_get_cts() - Average a stable audio CTS counter run
 * @ctx: Bridge instance.
 *
 * The current counter-read path does not propagate register read errors.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: Mean of 32 stable readings, or the last reading on stability
 *         timeout.
 */
static unsigned int it66121_audio_get_cts(const struct it66121_ctx *ctx)
{
	unsigned int stable_count = 0;
	unsigned int sum = 0;
	unsigned int last = 0;

	for (unsigned int i = 0; i < 255; i++) {
		unsigned int cts;
		unsigned int diff;
		unsigned int val;

		msleep(30);
		regmap_read(ctx->regmap, IT66121_AUD_PKT_CTS_CNT2_REG, &val);
		cts = val << 12;
		regmap_read(ctx->regmap, IT66121_AUD_PKT_CTS_CNT1_REG, &val);
		cts |= val << 4;
		regmap_read(ctx->regmap, IT66121_AUD_PKT_CTS_CNT0_REG, &val);
		cts |= val >> 4;
		if (!cts)
			continue;
		diff = last > cts ? last - cts : cts - last;
		last = cts;
		if (diff >= 5) {
			stable_count = 0;
			sum = 0;
			continue;
		}
		sum += cts;
		if (++stable_count >= 32)
			return sum >> 5;
	}
	return last;
}

/**
 * it661221_set_ncts() - Program audio N and CTS selection
 * @ctx: Bridge instance.
 * @fs: Hardware sample-frequency selector.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it661221_set_ncts(struct it66121_ctx *ctx, u8 fs)
{
	int ret;
	unsigned int n;

	switch (fs) {
	case IT66121_AUD_FS_32K:
		n = 4096;
		break;
	case IT66121_AUD_FS_44P1K:
		n = 6272;
		break;
	case IT66121_AUD_FS_48K:
		n = 6144;
		break;
	case IT66121_AUD_FS_88P2K:
		n = 12544;
		break;
	case IT66121_AUD_FS_96K:
		n = 12288;
		break;
	case IT66121_AUD_FS_176P4K:
		n = 25088;
		break;
	case IT66121_AUD_FS_192K:
		n = 24576;
		break;
	case IT66121_AUD_FS_768K:
		n = 24576;
		break;
	default:
		n = 6144;
		break;
	}

	ret = regmap_write(ctx->regmap, IT66121_AUD_PKT_N0_REG, (u8)(n & 0xFF));
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_PKT_N1_REG, (u8)((n >> 8) & 0xFF));
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, IT66121_AUD_PKT_N2_REG, (u8)((n >> 16) & 0xF));
	if (ret)
		return ret;

	if (ctx->audio.auto_cts) {
		unsigned int cts = it66121_audio_get_cts(ctx);

		regmap_write(ctx->regmap, IT66121_AUD_PKT_CTS0_REG, (u8)(cts & 0xFF));
		regmap_write(ctx->regmap, IT66121_AUD_PKT_CTS1_REG, (u8)((cts >> 8) & 0xFF));
		regmap_write(ctx->regmap, IT66121_AUD_PKT_CTS2_REG, (u8)((cts >> 16) & 0x0F));
	}

	ret = regmap_write(ctx->regmap, 0xF8, 0xC3);
	if (ret)
		return ret;

	ret = regmap_write(ctx->regmap, 0xF8, 0xA5);
	if (ret)
		return ret;

	if (ctx->audio.auto_cts) {
		ret = regmap_write_bits(ctx->regmap, IT66121_PKT_CTS_CTRL_REG,
					IT66121_PKT_CTS_CTRL_SEL, 1);
	} else {
		ret = regmap_write_bits(ctx->regmap, IT66121_PKT_CTS_CTRL_REG,
					IT66121_PKT_CTS_CTRL_SEL, 0);
	}

	if (ret)
		return ret;

	return regmap_write(ctx->regmap, 0xF8, 0xFF);
}

/**
 * it661221_audio_output_enable() - Release or assert audio engine reset
 * @ctx: Bridge instance.
 * @enable: True to enable, false to disable.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it661221_audio_output_enable(struct it66121_ctx *ctx, bool enable)
{
	int ret;

	if (enable) {
		ret = regmap_write_bits(ctx->regmap, IT66121_SW_RST_REG,
					IT66121_SW_RST_AUD | IT66121_SW_RST_AREF, 0);
		if (ret)
			return ret;

		ret = regmap_write_bits(ctx->regmap, IT66121_AUD_CTRL0_REG,
					IT66121_AUD_EN_I2S3 | IT66121_AUD_EN_I2S2 |
						IT66121_AUD_EN_I2S1 | IT66121_AUD_EN_I2S0,
					ctx->audio.ch_enable);
	} else {
		ret = regmap_write_bits(ctx->regmap, IT66121_AUD_CTRL0_REG,
					IT66121_AUD_EN_I2S3 | IT66121_AUD_EN_I2S2 |
						IT66121_AUD_EN_I2S1 | IT66121_AUD_EN_I2S0,
					ctx->audio.ch_enable & 0xF0);
		if (ret)
			return ret;

		ret = regmap_write_bits(ctx->regmap, IT66121_SW_RST_REG,
					IT66121_SW_RST_AUD | IT66121_SW_RST_AREF,
					IT66121_SW_RST_AUD | IT66121_SW_RST_AREF);
	}

	return ret;
}

/**
 * it661221_audio_ch_enable() - Enable or disable configured audio sources
 * @ctx: Bridge instance.
 * @enable: True to enable, false to disable.
 *
 * Context: Process context. Caller holds the bridge register mutex.
 * Return: 0 on success or a negative error code.
 */
static int it661221_audio_ch_enable(struct it66121_ctx *ctx, bool enable)
{
	int ret;

	if (enable) {
		ret = regmap_write(ctx->regmap, IT66121_AUD_SRCVALID_FLAT_REG, 0);
		if (ret)
			return ret;

		ret = regmap_write(ctx->regmap, IT66121_AUD_CTRL0_REG, ctx->audio.ch_enable);
	} else {
		ret = regmap_write(ctx->regmap, IT66121_AUD_CTRL0_REG, ctx->audio.ch_enable & 0xF0);
	}

	return ret;
}

/**
 * it66121_audio_hw_params() - Program HDMI I2S audio parameters
 * @dev: Owning device.
 * @data: Callback payload; unused.
 * @daifmt: Codec serial-audio interface description.
 * @params: Channel count, sample rate and sample width.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_audio_hw_params(struct device *dev, void *data __always_unused,
				   struct hdmi_codec_daifmt *daifmt,
				   struct hdmi_codec_params *params)
{
	u8 fs;
	u8 swl;
	int ret;
	struct it66121_ctx *ctx = dev_get_drvdata(dev);
	u8 iec60958_chstat[5] = {};
	unsigned int channels = params->channels;
	unsigned int sample_rate = params->sample_rate;
	unsigned int sample_width = params->sample_width;

	if (!channels || channels > 8)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	dev_dbg(dev, "%s: %u, %u, %u, %u\n", __func__, daifmt->fmt, sample_rate, sample_width,
		channels);

	if (daifmt->fmt != HDMI_I2S) {
		dev_err(dev, "Invalid or unsupported DAI format %d\n", daifmt->fmt);
		ret = -EINVAL;
		goto out;
	}
	dev_dbg(dev, "Using HDMI I2S\n");

	ret = regmap_write(ctx->regmap, IT66121_CLK_CTRL0_REG,
			   IT66121_CLK_CTRL0_AUTO_OVER_SAMPLING | IT66121_CLK_CTRL0_EXT_MCLK_256FS |
				   IT66121_CLK_CTRL0_AUTO_IPCLK);
	if (ret)
		goto out;

	ret = regmap_write_bits(ctx->regmap, IT66121_AUD_CTRL0_REG, IT66121_AUD_CTRL0_AUD_SEL,
				0); /* Select I2S rather than SPDIF. */
	if (ret)
		goto out;

	switch (sample_rate) {
	case 44100L:
		fs = IT66121_AUD_FS_44P1K;
		break;
	case 88200L:
		fs = IT66121_AUD_FS_88P2K;
		break;
	case 176400L:
		fs = IT66121_AUD_FS_176P4K;
		break;
	case 32000L:
		fs = IT66121_AUD_FS_32K;
		break;
	case 48000L:
		fs = IT66121_AUD_FS_48K;
		break;
	case 96000L:
		fs = IT66121_AUD_FS_96K;
		break;
	case 192000L:
		fs = IT66121_AUD_FS_192K;
		break;
	case 768000L:
		fs = IT66121_AUD_FS_768K;
		break;
	default:
		fs = IT66121_AUD_FS_48K;
		break;
	}

	ctx->audio.fs = fs;
	ret = it661221_set_ncts(ctx, fs);
	if (ret) {
		dev_err(dev, "Failed to set N/CTS: %d\n", ret);
		goto out;
	}

	/* Configure the audio format before enabling channels. */
	ret = it661221_set_lpcm_audio(ctx, (channels + 1) / 2, sample_width);
	if (ret) {
		dev_err(dev, "Failed to set LPCM audio: %d\n", ret);
		goto out;
	}

	iec60958_chstat[0] = 0;
	if ((channels + 1) / 2 == 1)
		iec60958_chstat[0] |= 0x1;
	iec60958_chstat[0] &= ~(1 << 1);
	iec60958_chstat[1] = 0;
	iec60958_chstat[2] = (u8)((channels + 1) / 2);
	iec60958_chstat[2] |= (channels << 4) & 0xF0;
	iec60958_chstat[3] = fs;

	switch (sample_width) {
	case 21L:
		swl = IT66121_AUD_SWL_21BIT;
		break;
	case 24L:
		swl = IT66121_AUD_SWL_24BIT;
		break;
	case 23L:
		swl = IT66121_AUD_SWL_23BIT;
		break;
	case 22L:
		swl = IT66121_AUD_SWL_22BIT;
		break;
	case 20L:
		swl = IT66121_AUD_SWL_20BIT;
		break;
	case 17L:
		swl = IT66121_AUD_SWL_17BIT;
		break;
	case 19L:
		swl = IT66121_AUD_SWL_19BIT;
		break;
	case 18L:
		swl = IT66121_AUD_SWL_18BIT;
		break;
	case 16L:
		swl = IT66121_AUD_SWL_16BIT;
		break;
	default:
		swl = IT66121_AUD_SWL_NOT_INDICATED;
		break;
	}

	iec60958_chstat[4] = ((~(unsigned int)fs << 4) & 0xF0) | swl;
	ret = it661221_set_chstat(ctx, iec60958_chstat);
	if (ret) {
		dev_err(dev, "Failed to set channel status: %d\n", ret);
		goto out;
	}

	ret = it661221_audio_ch_enable(ctx, true);
	if (ret) {
		dev_err(dev, "Failed to enable audio channel: %d\n", ret);
		goto out;
	}

	ret = regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_AUD_OVF, 0);
	if (ret)
		goto out;

	dev_dbg(dev, "HDMI audio enabled.\n");
out:
	mutex_unlock(&ctx->lock);

	return ret;
}

/**
 * it66121_audio_startup() - Enable audio output under the bridge mutex
 * @dev: Owning device.
 * @data: Callback payload; unused.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_audio_startup(struct device *dev, void *data __always_unused)
{
	int ret;
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->lock);
	ret = it661221_audio_output_enable(ctx, true);
	if (ret)
		dev_err(dev, "Failed to enable audio output: %d\n", ret);

	mutex_unlock(&ctx->lock);

	return ret;
}

/**
 * it66121_audio_shutdown() - Disable audio output and log register errors
 * @dev: Owning device.
 * @data: Callback payload; unused.
 *
 * Context: Process context. May sleep.
 */
static void it66121_audio_shutdown(struct device *dev, void *data __always_unused)
{
	int ret;
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->lock);
	ret = it661221_audio_output_enable(ctx, false);
	if (ret)
		dev_err(dev, "Failed to disable audio output: %d\n", ret);

	mutex_unlock(&ctx->lock);
}

/**
 * it66121_audio_mute() - Set or clear source-flat audio muting
 * @dev: Owning device.
 * @data: Callback payload; unused.
 * @enable: True to mute the audio sources.
 * @direction: Stream direction, used for diagnostics.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_audio_mute(struct device *dev, void *data __always_unused, bool enable,
			      int direction)
{
	int ret;
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	dev_dbg(dev, "%s: enable=%s, direction=%d\n", __func__, enable ? "true" : "false",
		direction);

	mutex_lock(&ctx->lock);

	if (enable) {
		ret = regmap_write_bits(ctx->regmap, IT66121_AUD_SRCVALID_FLAT_REG,
					IT66121_AUD_FLAT_SRC0 | IT66121_AUD_FLAT_SRC1 |
						IT66121_AUD_FLAT_SRC2 | IT66121_AUD_FLAT_SRC3,
					IT66121_AUD_FLAT_SRC0 | IT66121_AUD_FLAT_SRC1 |
						IT66121_AUD_FLAT_SRC2 | IT66121_AUD_FLAT_SRC3);
	} else {
		ret = regmap_write_bits(ctx->regmap, IT66121_AUD_SRCVALID_FLAT_REG,
					IT66121_AUD_FLAT_SRC0 | IT66121_AUD_FLAT_SRC1 |
						IT66121_AUD_FLAT_SRC2 | IT66121_AUD_FLAT_SRC3,
					0);
	}

	mutex_unlock(&ctx->lock);

	return ret;
}

/**
 * it66121_audio_get_eld() - Copy ELD under connector locking
 * @dev: Owning device.
 * @data: Callback payload; unused.
 * @buf: Caller-owned ELD destination.
 * @len: Destination capacity.
 *
 * Returns zeros while no connector is active; otherwise copies the bounded
 * ELD length under the connector ELD mutex.
 *
 * Context: Process context. May sleep.
 * Return: 0.
 */
static int it66121_audio_get_eld(struct device *dev, void *data __always_unused, u8 *buf,
				 size_t len)
{
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->lock);
	if (!ctx->connector) {
		/* Return an empty ELD while no connector is active. */
		dev_dbg(dev, "No connector present, passing empty EDID data");
		memset(buf, 0, len);
	} else {
		mutex_lock(&ctx->connector->eld_mutex);
		memcpy(buf, ctx->connector->eld, min(sizeof(ctx->connector->eld), len));
		mutex_unlock(&ctx->connector->eld_mutex);
	}
	mutex_unlock(&ctx->lock);

	return 0;
}

/**
 * var it66121_audio_codec_ops - I2S codec operations for native firmware audio users
 */
static const struct hdmi_codec_ops it66121_audio_codec_ops = {
	.hw_params = it66121_audio_hw_params,
	.audio_startup = it66121_audio_startup,
	.audio_shutdown = it66121_audio_shutdown,
	.mute_stream = it66121_audio_mute,
	.get_eld = it66121_audio_get_eld,
};

/**
 * it66121_audio_unregister() - Release the managed HDMI codec child
 * @data: Codec platform device.
 *
 * Context: Process context. May sleep.
 */
static void it66121_audio_unregister(void *data)
{
	platform_device_unregister(data);
}

/**
 * it66121_audio_codec_init() - Register firmware-described audio support
 * @dev: Owning device.
 *
 * Without #sound-dai-cells, succeeds without creating audio support. Managed
 * unregistration ties codec lifetime to the I2C device.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_audio_codec_init(struct device *dev)
{
	struct platform_device *pdev;
	struct hdmi_codec_pdata codec_data = {
		.ops = &it66121_audio_codec_ops,
		.i2s = 1,
		.spdif = 0,
		.max_i2s_channels = 8,
		.no_capture_mute = 1,
	};

	if (!of_property_present(dev->of_node, "#sound-dai-cells")) {
		dev_info(dev, "No \"#sound-dai-cells\", no audio\n");
		return 0;
	}

	pdev = platform_device_register_data(dev, HDMI_CODEC_DRV_NAME, PLATFORM_DEVID_AUTO,
					     &codec_data, sizeof(codec_data));

	if (IS_ERR(pdev)) {
		dev_err(dev, "Failed to initialize HDMI audio codec: %ld\n", PTR_ERR(pdev));
		return PTR_ERR_OR_ZERO(pdev);
	}
	return devm_add_action_or_reset(dev, it66121_audio_unregister, pdev);
}

/**
 * var it66121_supplies - Required native-board regulator supply names
 */
static const char *const it66121_supplies[] = { "vcn33", "vcn18", "vrf12" };

/**
 * var it66xx_chip_info - Hardware IDs selecting chip-specific register programming
 */
static const struct it66121_chip_info it66xx_chip_info[] = {
	{ .id = ID_IT6610, .vid = 0xca00, .pid = 0x0611 },
	{ .id = ID_IT66121, .vid = 0x4954, .pid = 0x0612 },
	{ .id = ID_IT66122, .vid = 0x4954, .pid = 0x0622 },
};

/**
 * it66121_probe_firmware() - Resolve native graph, supplies and reset GPIO
 * @ctx: Bridge instance.
 *
 * Reads input width from graph port zero, acquires the downstream bridge,
 * enables supplies and pulses the optional reset GPIO. Used only with OF.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_probe_firmware(struct it66121_ctx *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *ep;
	int ret;

	ep = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	if (!ep)
		return -EINVAL;
	of_property_read_u32(ep, "bus-width", &ctx->bus_width);
	of_node_put(ep);
	if (ctx->bus_width != 12 && ctx->bus_width != 24)
		return -EINVAL;
	ep = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!ep)
		return -EINVAL;
	ctx->bridge.next_bridge = of_drm_find_and_get_bridge(ep);
	of_node_put(ep);
	if (!ctx->bridge.next_bridge)
		return -EPROBE_DEFER;
	ctx->bridge.of_node = dev->of_node;
	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(it66121_supplies), it66121_supplies);
	if (ret)
		return ret;
	ctx->gpio_reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpio_reset))
		return PTR_ERR_OR_ZERO(ctx->gpio_reset);
	gpiod_set_value_cansleep(ctx->gpio_reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->gpio_reset, 0);
	return 0;
}

/**
 * it66121_probe_properties() - Resolve transport constraints and attachment
 * @ctx: Allocated bridge state with device, client and adapter initialized.
 *
 * Adapter alignment defaults to 1 and accepts 1 or 4 bytes. Input clock delay
 * is a selector from 0 to 3, default 0. Controller reset ownership and the
 * active-high/open-drain IRQ output booleans default to false; the latter
 * apply only when restoring settings after a controller-owned reset.
 *
 * An OF node selects native graph, supply and GPIO setup. Without OF, explicit
 * adapter input width (12 or 24) and an adapter IRQ domain are required before
 * component registration. Missing firmware does not itself select aligned I2C
 * or reset quirks. There is no polling-only firmware-less attachment.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success, -EPROBE_DEFER for a missing domain, or a
 *         configuration error.
 */
static int it66121_probe_properties(struct it66121_ctx *ctx)
{
	const struct device *adapter = &ctx->adapter->dev;
	u32 alignment = 1;
	u32 delay = 0;
	int ret;

	if (device_property_present(adapter, "i2c-register-access-alignment")) {
		ret = device_property_read_u32(adapter, "i2c-register-access-alignment",
					       &alignment);
		if (ret)
			return ret;
		if (alignment != 1 && alignment != 4)
			return -EINVAL;
	}
	ctx->aligned_i2c = alignment == 4;
	ctx->reset_by_controller = device_property_read_bool(adapter, "display-controller-reset");
	if (device_property_read_bool(adapter, "interrupt-active-high"))
		ctx->irq_output_mode |= IT66121_INT_ACTIVE_HIGH;
	if (device_property_read_bool(adapter, "interrupt-open-drain"))
		ctx->irq_output_mode |= IT66121_INT_OPEN_DRAIN;
	if (device_property_present(adapter, "display-input-clock-delay")) {
		ret = device_property_read_u32(adapter, "display-input-clock-delay", &delay);
		if (ret)
			return ret;
		if (delay > IT66121_INPUT_MODE_PCLK_DELAY_MASK)
			return -EINVAL;
	}
	ctx->input_clock_delay = (u8)delay;
	/* Native boards retain their graph, power and IRQ configuration. */
	if (ctx->dev->of_node)
		return it66121_probe_firmware(ctx);
	ret = device_property_read_u32(adapter, "display-input-bus-width", &ctx->bus_width);
	if (ret)
		return ret;
	if (ctx->bus_width != 12 && ctx->bus_width != 24)
		return -EINVAL;
	if (!irq_find_matching_fwnode(dev_fwnode(&ctx->adapter->dev), DOMAIN_BUS_ANY))
		return -EPROBE_DEFER;
	ctx->component_binding = true;
	return 0;
}

/**
 * it66121_probe() - Identify and register the bridge
 * @client: I2C device instantiated by firmware or userspace.
 *
 * Resolves hardware constraints before register access. Native boards request
 * client->irq at probe; firmware-less components request their mapped IRQ at
 * bind. Component registration does not itself create a DRM connector.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int it66121_probe(struct i2c_client *client)
{
	struct it66121_ctx *ctx;
	unsigned int value;
	u8 bytes[4];
	const struct it66121_chip_info *chip_info = NULL;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENXIO;
	ctx = devm_drm_bridge_alloc(&client->dev, struct it66121_ctx, bridge,
				    &it66121_bridge_funcs);
	if (IS_ERR(ctx))
		return PTR_ERR_OR_ZERO(ctx);
	ctx->dev = &client->dev;
	ctx->client = client;
	ctx->adapter = client->adapter;
	ret = it66121_probe_properties(ctx);
	if (ret)
		return dev_err_probe(ctx->dev, ret, "Device configuration failed\n");
	mutex_init(&ctx->lock);
	ctx->regmap = it66121_init_regmap(ctx);
	if (IS_ERR(ctx->regmap))
		return dev_err_probe(ctx->dev, PTR_ERR_OR_ZERO(ctx->regmap),
				     "Register map initialization failed\n");
	for (unsigned int i = 0; i < ARRAY_SIZE(bytes); i++) {
		ret = regmap_read(ctx->regmap, i, &value);
		if (ret)
			return dev_err_probe(ctx->dev, ret, "ID register %u read failed\n", i);
		bytes[i] = (u8)value;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(it66xx_chip_info); i++) {
		if ((bytes[1] << 8 | bytes[0]) == it66xx_chip_info[i].vid &&
		    ((bytes[3] & IT66121_DEVICE_ID1_MASK) << 8 | bytes[2]) ==
			    it66xx_chip_info[i].pid) {
			chip_info = &it66xx_chip_info[i];
			break;
		}
	}
	if (!chip_info)
		return -ENODEV;
	ctx->id = chip_info->id;
	ctx->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	ctx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;
	i2c_set_clientdata(client, ctx);
	if (ctx->component_binding || client->irq > 0)
		ctx->bridge.ops |= DRM_BRIDGE_OP_HPD;
	if (!ctx->component_binding && client->irq > 0) {
		ret = devm_request_threaded_irq(ctx->dev, client->irq, NULL, it66121_irq_thread,
						IRQF_ONESHOT, dev_name(ctx->dev), ctx);
		if (ret)
			return ret;
	}
	ret = it66121_audio_codec_init(ctx->dev);
	if (ret)
		return ret;
	drm_bridge_add(&ctx->bridge);
	if (ctx->component_binding) {
		ret = component_add(ctx->dev, &it66121_component_ops);
		if (ret)
			drm_bridge_remove(&ctx->bridge);
	} else {
		ret = 0;
	}
	return ret;
}

/**
 * it66121_remove() - Remove component attachment before bridge registration
 * @client: I2C bridge device being removed.
 *
 * Context: Process context. May sleep.
 */
static void it66121_remove(struct i2c_client *client)
{
	struct it66121_ctx *ctx = i2c_get_clientdata(client);

	if (ctx->component_binding)
		component_del(&client->dev, &it66121_component_ops);
	drm_bridge_remove(&ctx->bridge);
}

/**
 * var it66121_dt_match - Native OF matching for supported transmitter variants
 */
static const struct of_device_id it66121_dt_match[] = { { .compatible = "ite,it6610" },
							{ .compatible = "ite,it66121" },
							{ .compatible = "ite,it66122" },
							{} };
MODULE_DEVICE_TABLE(of, it66121_dt_match);

/**
 * var it66121_ids - I2C identifiers accepted by firmware or new_device
 */
static const struct i2c_device_id it66121_ids[] = { { "it6610", 0 },
						    { "it66121", 0 },
						    { "it66122", 0 },
						    {} };
MODULE_DEVICE_TABLE(i2c, it66121_ids);
/**
 * var it66121_driver - I2C registration shared by OF and component contexts
 */
static struct i2c_driver it66121_driver = {
	.probe = it66121_probe,
	.remove = it66121_remove,
	.driver = {
		.name = "it66121",
		.of_match_table = it66121_dt_match,
	},
	.id_table = it66121_ids,
};
module_i2c_driver(it66121_driver);
MODULE_AUTHOR("Phong LE; Artem Mygaiev");
MODULE_DESCRIPTION("IT66121 HDMI bridge with FL2000 USB transport adaptations");
MODULE_LICENSE("GPL v2");
