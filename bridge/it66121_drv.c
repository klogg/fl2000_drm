// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on Linux v7.0 drivers/gpu/drm/bridge/ite-it66121.c.
 * Copyright (C) 2020 BayLibre, SAS
 * Author: Phong LE <ple@baylibre.com>
 * Copyright (C) 2018-2019, Artem Mygaiev
 * Copyright (C) 2017, Fresco Logic, Incorporated.
 *
 * FL2000 adaptations: USB component lifetime, aligned DWORD I2C/EDID,
 * fixed parallel input timing, reset restoration, and polling fallback.
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

enum chip_id {
	ID_IT6610,
	ID_IT66121,
	ID_IT66122,
};

struct it66121_chip_info {
	enum chip_id id;
	u16 vid;
	u16 pid;
};

struct it66121_ctx {
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct drm_connector *connector;
	struct device *dev;
	struct i2c_client *client;
	struct i2c_adapter *adapter;
	int irq;
	/* Aligned reads consume adjacent registers; byte writes use RMW. */
	bool aligned_i2c;
	bool reset_by_controller;
	bool component_binding;
	bool retry_irq_ack;
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
	struct mutex lock; /* Serializes bridge register sequences. */
	struct hdmi_avi_infoframe hdmi_avi_infoframe;
	enum chip_id id;
};

/* Upstream bank mapping; common registers are identical in both banks. */
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

/* FL2000 DWORD transport consumes three bytes before each EDID payload. */
#define EDID_SLEEP		  1000
#define EDID_TIMEOUT		  200000
#define EDID_FIFO_SIZE		  32
#define IT66121_ALIGNED_FIFO_SKIP 3
#define EDID_DDC_ADDR		  IT66121_DDC_HEADER_EDID
#define IT66121_DDC_CONTROL	  IT66121_MASTER_SEL_REG
#define IT66121_DDC_SEGMENT	  IT66121_DDC_SEGMENT_REG
#define IT66121_DDC_MASTER_HOST	  IT66121_MASTER_SEL_HOST
#define DDC_CMD_FIFO_CLEAR	  IT66121_DDC_COMMAND_FIFO_CLR
#define DDC_CMD_ABORT		  IT66121_DDC_COMMAND_ABORT
#define DDC_CMD_EDID_READ	  IT66121_DDC_COMMAND_EDID_READ

static inline int it66121_preamble_ddc(struct it66121_ctx *ctx)
{
	return regmap_write(ctx->regmap, IT66121_MASTER_SEL_REG, IT66121_MASTER_SEL_HOST);
}

static inline int it66121_fire_afe(struct it66121_ctx *ctx)
{
	return regmap_write(ctx->regmap, IT66121_AFE_DRV_REG, 0);
}

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
 * it66121_configure_afe() - Configure the analog front end
 * @ctx: it66121_ctx object
 * @mode: mode to configure
 *
 * RETURNS:
 * zero if success, a negative error code otherwise.
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
		/* Only LOWCLK is in the update mask; high clocks clear that bit. */
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_EC1_REG,
					IT66121_AFE_XP_EC1_LOWCLK,
					high_clock ? 0 : IT66121_AFE_XP_EC1_LOWCLK);
		if (ret)
			return ret;
	}

	/* Clear reset flags */
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
	/* FL2000's vendor recovery sequence requires ABORT twice. */
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

	/* TOFIX handle more than MEDIA_BUS_FMT_RGB888_1X24 as output format */
	output_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_output_fmts = 1;

	return output_fmts;
}

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
		/* IT66121FN Datasheet specifies Little-Endian ordering */
		input_fmts[0] = MEDIA_BUS_FMT_RGB888_2X12_LE;
	else
		/* TOFIX support more input bus formats in 24bit width */
		input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_input_fmts = 1;

	return input_fmts;
}

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

static void it66121_bridge_disable(struct drm_bridge *bridge,
				   struct drm_atomic_state *state __always_unused)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	mutex_lock(&ctx->lock);
	it66121_set_mute(ctx, true);
	ctx->connector = NULL;

	mutex_unlock(&ctx->lock);
}

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

static void
it66121_bridge_enable(struct drm_bridge *bridge,
		      struct drm_atomic_state *state) /* NOSONAR: Kernel callback ABI. */
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
	/* Program after the CRTC enable callback has reset the transport. */
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

static irqreturn_t it66121_irq_thread(int irq __always_unused, void *data)
{
	struct it66121_ctx *ctx = data;

	if (it66121_interrupt(ctx)) {
		dev_err_ratelimited(ctx->dev, "Interrupt acknowledgment failed\n");
		return ctx->retry_irq_ack ? IRQ_NONE : IRQ_HANDLED;
	}
	return IRQ_HANDLED;
}

/* Firmware-less adapters publish the bridge IRQ through their IRQ domain. */
static int it66121_bind(struct device *comp, /* NOSONAR: Kernel callback ABI. */
			struct device *master __always_unused, void *data)
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

static void it66121_unbind(struct device *comp, /* NOSONAR: Kernel callback ABI. */
			   struct device *master __always_unused, void *data __always_unused)
{
	struct it66121_ctx *ctx = dev_get_drvdata(comp);

	free_irq(ctx->irq, ctx);
	ctx->irq = 0;
	ctx->connector = NULL;
}

static const struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

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

static int
it66121_audio_hw_params(struct device *dev, /* NOSONAR: Kernel callback ABI. */
			void *data __always_unused,
			struct hdmi_codec_daifmt *daifmt, /* NOSONAR: Kernel callback ABI. */
			struct hdmi_codec_params *params) /* NOSONAR: Kernel callback ABI. */
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

	// Set audio clock recovery (N/CTS)
	ret = regmap_write(ctx->regmap, IT66121_CLK_CTRL0_REG,
			   IT66121_CLK_CTRL0_AUTO_OVER_SAMPLING | IT66121_CLK_CTRL0_EXT_MCLK_256FS |
				   IT66121_CLK_CTRL0_AUTO_IPCLK);
	if (ret)
		goto out;

	ret = regmap_write_bits(ctx->regmap, IT66121_AUD_CTRL0_REG, IT66121_AUD_CTRL0_AUD_SEL,
				0); // remove spdif selection
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

	// Set audio format register (except audio channel enable)
	ret = it661221_set_lpcm_audio(ctx, (channels + 1) / 2, sample_width);
	if (ret) {
		dev_err(dev, "Failed to set LPCM audio: %d\n", ret);
		goto out;
	}

	// Set audio channel status
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

	// Enable audio channel enable while input clock stable (if SPDIF).
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

static int it66121_audio_startup(struct device *dev, /* NOSONAR: Kernel callback ABI. */
				 void *data __always_unused)
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

static void it66121_audio_shutdown(struct device *dev, /* NOSONAR: Kernel callback ABI. */
				   void *data __always_unused)
{
	int ret;
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->lock);
	ret = it661221_audio_output_enable(ctx, false);
	if (ret)
		dev_err(dev, "Failed to disable audio output: %d\n", ret);

	mutex_unlock(&ctx->lock);
}

static int it66121_audio_mute(struct device *dev, /* NOSONAR: Kernel callback ABI. */
			      void *data __always_unused, bool enable, int direction)
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

static int it66121_audio_get_eld(struct device *dev, /* NOSONAR: Kernel callback ABI. */
				 void *data __always_unused, u8 *buf, size_t len)
{
	struct it66121_ctx *ctx = dev_get_drvdata(dev);

	mutex_lock(&ctx->lock);
	if (!ctx->connector) {
		/* Pass en empty ELD if connector not available */
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

static const struct hdmi_codec_ops it66121_audio_codec_ops = {
	.hw_params = it66121_audio_hw_params,
	.audio_startup = it66121_audio_startup,
	.audio_shutdown = it66121_audio_shutdown,
	.mute_stream = it66121_audio_mute,
	.get_eld = it66121_audio_get_eld,
};

static void it66121_audio_unregister(void *data)
{
	platform_device_unregister(data);
}

static int it66121_audio_codec_init(struct device *dev)
{
	struct platform_device *pdev;
	struct hdmi_codec_pdata codec_data = {
		.ops = &it66121_audio_codec_ops,
		.i2s = 1, /* Only i2s support for now */
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

static const char *const it66121_supplies[] = { "vcn33", "vcn18", "vrf12" };

static const struct it66121_chip_info it66xx_chip_info[] = {
	{ .id = ID_IT6610, .vid = 0xca00, .pid = 0x0611 },
	{ .id = ID_IT66121, .vid = 0x4954, .pid = 0x0612 },
	{ .id = ID_IT66122, .vid = 0x4954, .pid = 0x0622 },
};

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

/* Adapter properties describe transport limits and controller-owned wiring.
 * They are independent of adapter identity and are not inferred from a lack
 * of firmware. Existing firmware graph users retain their configuration.
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
	ctx->component_binding = device_property_read_bool(adapter, "drm-component-attachment");
	ctx->retry_irq_ack = device_property_read_bool(adapter, "interrupt-ack-retry");
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
	if (!ctx->component_binding)
		return it66121_probe_firmware(ctx);
	ret = device_property_read_u32(adapter, "display-input-bus-width", &ctx->bus_width);
	if (ret)
		return ret;
	if (ctx->bus_width != 12 && ctx->bus_width != 24)
		return -EINVAL;
	return 0;
}

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

static void it66121_remove(struct i2c_client *client)
{
	struct it66121_ctx *ctx = i2c_get_clientdata(client);

	if (ctx->component_binding)
		component_del(&client->dev, &it66121_component_ops);
	drm_bridge_remove(&ctx->bridge);
}

static const struct of_device_id it66121_dt_match[] = { { .compatible = "ite,it6610" },
							{ .compatible = "ite,it66121" },
							{ .compatible = "ite,it66122" },
							{} };
MODULE_DEVICE_TABLE(of, it66121_dt_match);

static const struct i2c_device_id it66121_ids[] = { { "it6610", 0 },
						    { "it66121", 0 },
						    { "it66122", 0 },
						    {} };
MODULE_DEVICE_TABLE(i2c, it66121_ids);
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
