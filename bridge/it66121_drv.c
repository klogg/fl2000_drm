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
#include "it66121.h"
#include <linux/ktime.h>
#include <linux/media-bus-format.h>
#include <drm/drm_atomic_state_helper.h>

enum chip_id {
	ID_IT6610,
	ID_IT66121,
	ID_IT66122,
};

struct it66121_ctx {
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct drm_connector *connector;
	struct device *dev;
	struct i2c_client *client;
	struct i2c_adapter *adapter;
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
		.range_max = 0x1ff,
		.selector_reg = IT66121_CLK_BANK_REG,
		.selector_mask = 1,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 0x100,
	},
};

static const struct regmap_config it66121_regmap_config = {
	.val_bits = 8,
	.reg_bits = 8,
	.max_register = 0x1ff,
	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),
	/* Resets are controlled by FL2000; do not cache bridge registers. */
	.cache_type = REGCACHE_NONE,
	.use_single_read = true,
	.use_single_write = true,
};

/* FL2000 DWORD transport consumes three bytes before each EDID payload. */
#define EDID_SLEEP		1000
#define EDID_TIMEOUT		200000
#define EDID_FIFO_SIZE		32
#define FL2000_EDID_FIFO_SKIP	3
#define EDID_DDC_ADDR		0xa0
#define IT66121_DDC_CONTROL	IT66121_MASTER_SEL_REG
#define IT66121_DDC_SEGMENT	IT66121_DDC_SEGMENT_REG
#define IT66121_DDC_MASTER_HOST IT66121_MASTER_SEL_HOST
#define DDC_CMD_FIFO_CLEAR	IT66121_DDC_COMMAND_FIFO_CLR
#define DDC_CMD_ABORT		IT66121_DDC_COMMAND_ABORT
#define DDC_CMD_EDID_READ	IT66121_DDC_COMMAND_EDID_READ

static inline int it66121_fire_afe(struct it66121_ctx *ctx)
{
	return regmap_write(ctx->regmap, IT66121_AFE_DRV_REG, 0);
}

static int it66121_configure_input(struct it66121_ctx *ctx)
{
	int ret;
	/* Preserve the FL2000 parallel bus's one-cycle PCLK delay. */
	u8 mode = IT66121_INPUT_MODE_RGB | BIT(0);

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
	int ret;

	ret = regmap_write(ctx->regmap, IT66121_AFE_DRV_REG, IT66121_AFE_DRV_RST);
	if (ret)
		return ret;

	if (mode->clock > IT66121_AFE_CLK_HIGH) {
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG,
					IT66121_AFE_XP_GAINBIT | IT66121_AFE_XP_ENO,
					IT66121_AFE_XP_GAINBIT);
		if (ret)
			return ret;

		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_GAINBIT | IT66121_AFE_IP_ER0,
					IT66121_AFE_IP_GAINBIT);
		if (ret)
			return ret;

		if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
			ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG, IT66121_AFE_IP_EC1,
						0);
			if (ret)
				return ret;

			ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_EC1_REG,
						IT66121_AFE_XP_EC1_LOWCLK, 0x80);
			if (ret)
				return ret;
		}
	} else {
		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_REG,
					IT66121_AFE_XP_GAINBIT | IT66121_AFE_XP_ENO,
					IT66121_AFE_XP_ENO);
		if (ret)
			return ret;

		ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG,
					IT66121_AFE_IP_GAINBIT | IT66121_AFE_IP_ER0,
					IT66121_AFE_IP_ER0);
		if (ret)
			return ret;

		if (ctx->id == ID_IT66121 || ctx->id == ID_IT66122) {
			ret = regmap_write_bits(ctx->regmap, IT66121_AFE_IP_REG, IT66121_AFE_IP_EC1,
						IT66121_AFE_IP_EC1);
			if (ret)
				return ret;

			ret = regmap_write_bits(ctx->regmap, IT66121_AFE_XP_EC1_REG,
						IT66121_AFE_XP_EC1_LOWCLK,
						IT66121_AFE_XP_EC1_LOWCLK);
			if (ret)
				return ret;
		}
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

	return ret == ARRAY_SIZE(msgs) ? 0 : ret < 0 ? ret : -EIO;
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

	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
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
		if (word[2] & BIT(7)) {
			if (!buf || received == len)
				return 0;
			if (word[2] & BIT(1))
				return -EIO;
		}
		if (!(word[2] & BIT(7)) && ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		if (!(word[2] & BIT(7)) || (word[2] & BIT(1)))
			usleep_range(EDID_SLEEP, EDID_SLEEP * 2);
	}
}

static int it66121_wait_ddc_ready(struct it66121_ctx *priv)
{
	return it66121_ddc_collect(priv, NULL, 0);
}

static int it66121_clear_ddc_fifo(struct it66121_ctx *priv)
{
	int ret;

	ret = it66121_ddc_write(priv, IT66121_DDC_SEGMENT, 0, DDC_CMD_FIFO_CLEAR, 0, 0);
	if (ret)
		return ret;

	return it66121_wait_ddc_ready(priv);
}

static int it66121_abort_ddc_ops(struct it66121_ctx *priv)
{
	int ret;

	/* Prior to DDC abort command there was also a reset of HDCP:
	 *  1. HDCP_DESIRE clear bit CP DESIRE
	 *  2. SW_RST set bit HDCP_RST
	 * it seems wrong to keep them reset, i.e. without restoring initial state, but somehow this
	 * is how it was implemented. This sequence is removed since HDCP is not supported
	 */

	/* From original driver: According to 2009/01/15 modification by Jau-Chih.Tseng@ite.com.tw
	 * do abort DDC twice
	 */
	for (int i = 0; i < 2; i++) {
		ret = it66121_ddc_write(priv, IT66121_DDC_SEGMENT, 0, DDC_CMD_ABORT, 0, 0);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(priv);
		if (ret)
			return ret;
	}

	return 0;
}

static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct it66121_ctx *priv = context;
	unsigned int offset = (block & 1) * 128;
	unsigned int segment = block >> 1;
	int ret;

	if (!buf || !len || len > 128 || segment > 255)
		return -EINVAL;
	ret = it66121_ddc_write(priv, IT66121_DDC_CONTROL, IT66121_DDC_MASTER_HOST, EDID_DDC_ADDR,
				0, 0);
	if (ret)
		return ret;
	ret = it66121_abort_ddc_ops(priv);
	if (ret)
		return ret;

	while (len) {
		size_t count = min_t(size_t, len, EDID_FIFO_SIZE - FL2000_EDID_FIFO_SKIP);

		ret = it66121_clear_ddc_fifo(priv);
		if (ret)
			return ret;
		ret = it66121_ddc_write(priv, IT66121_DDC_CONTROL, IT66121_DDC_MASTER_HOST,
					EDID_DDC_ADDR, (u8)(offset - FL2000_EDID_FIFO_SKIP),
					count + FL2000_EDID_FIFO_SKIP);
		if (ret)
			return ret;
		/* Status and FIFO are read-only; write them as zero without a pre-read. */
		ret = it66121_ddc_write(priv, IT66121_DDC_SEGMENT, segment, DDC_CMD_EDID_READ, 0,
					0);
		if (ret)
			return ret;
		/* Let the requested burst fill the FIFO before a status snapshot can
		 * consume data whose empty flag was sampled earlier in the word.
		 */
		usleep_range(20000, 25000);
		ret = it66121_ddc_collect(priv, buf, count);
		if (ret)
			return ret;
		buf += count;
		offset += count;
		len -= count;
	}
	return 0;
}

static bool it66121_is_hpd_detect(struct it66121_ctx *ctx)
{
	unsigned int val;

	if (regmap_read(ctx->regmap, IT66121_SYS_STATUS_REG, &val))
		return false;

	return val & IT66121_SYS_STATUS_HPDETECT;
}

static int it66121_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;

	/* The USB dongle terminates at its HDMI connector. */
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

static u32 *it66121_bridge_atomic_get_output_bus_fmts(struct drm_bridge *bridge,
						      struct drm_bridge_state *bridge_state,
						      struct drm_crtc_state *crtc_state,
						      struct drm_connector_state *conn_state,
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

static u32 *it66121_bridge_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
						     struct drm_bridge_state *bridge_state,
						     struct drm_crtc_state *crtc_state,
						     struct drm_connector_state *conn_state,
						     u32 output_fmt, unsigned int *num_input_fmts)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
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

	ret = regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_HPD, 0);
	if (ret)
		dev_err(ctx->dev, "failed to enable HPD IRQ\n");
}

static void it66121_bridge_hpd_disable(struct drm_bridge *bridge)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;

	ret = regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, IT66121_INT_MASK1_HPD,
				IT66121_INT_MASK1_HPD);
	if (ret)
		dev_err(ctx->dev, "failed to disable HPD IRQ\n");
}

static int it66121_restore_interrupts(struct it66121_ctx *ctx)
{
	/* Use the active-low push-pull output verified on the FL2000 adapter. */
	int ret = regmap_write_bits(ctx->regmap, IT66121_INT_REG,
				    IT66121_INT_ACTIVE_HIGH | IT66121_INT_OPEN_DRAIN, 0);

	if (ret)
		return ret;
	return regmap_write_bits(ctx->regmap, IT66121_INT_MASK1_REG, 0xff, 0xfe);
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge, const struct drm_display_mode *mode,
				    const struct drm_display_mode *adjusted_mode);

static void it66121_bridge_enable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	struct drm_crtc_state *crtc_state;

	mutex_lock(&ctx->lock);
	ctx->connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	mutex_unlock(&ctx->lock);
	/* The encoder's enable callback resets FL2000 and its attached bridge. */
	crtc_state = drm_atomic_get_new_crtc_state(state, bridge->encoder->crtc);
	if (crtc_state)
		it66121_bridge_mode_set(bridge, &crtc_state->mode, &crtc_state->adjusted_mode);
	mutex_lock(&ctx->lock);
	if (it66121_restore_interrupts(ctx) || it66121_set_mute(ctx, false))
		dev_err(ctx->dev, "Failed to enable HDMI output\n");
	mutex_unlock(&ctx->lock);
}

static void it66121_bridge_disable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	mutex_lock(&ctx->lock);
	it66121_set_mute(ctx, true);

	mutex_unlock(&ctx->lock);
}

static int it66121_bridge_check(struct drm_bridge *bridge, struct drm_bridge_state *bridge_state,
				struct drm_crtc_state *crtc_state,
				struct drm_connector_state *conn_state)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);

	if (ctx->id == ID_IT6610) {
		/* The IT6610 only supports these settings */
		bridge_state->input_bus_cfg.flags |= DRM_BUS_FLAG_DE_HIGH |
						     DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;
		bridge_state->input_bus_cfg.flags &= ~DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE;
	}

	return 0;
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge, const struct drm_display_mode *mode,
				    const struct drm_display_mode *adjusted_mode)
{
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	int ret;

	mutex_lock(&ctx->lock);
	/* FL2000 owns resets; restore transport-facing masks after modesets. */
	if (!ctx->connector)
		goto unlock;
	if (!ctx->connector->display_info.is_hdmi) {
		if (regmap_write(ctx->regmap, IT66121_AVI_INFO_PKT_REG, 0) ||
		    regmap_write(ctx->regmap, IT66121_HDMI_MODE_REG, 0))
			goto unlock;
		goto configure;
	}

	ret = drm_hdmi_avi_infoframe_from_display_mode(&ctx->hdmi_avi_infoframe, ctx->connector,
						       adjusted_mode);
	if (ret) {
		DRM_ERROR("Failed to setup AVI infoframe: %d\n", ret);
		goto unlock;
	}

	ret = hdmi_avi_infoframe_pack(&ctx->hdmi_avi_infoframe, buf, sizeof(buf));
	if (ret < 0) {
		DRM_ERROR("Failed to pack infoframe: %d\n", ret);
		goto unlock;
	}

	/* Write new AVI infoframe packet */
	ret = regmap_bulk_write(ctx->regmap, IT66121_AVIINFO_DB1_REG,
				&buf[HDMI_INFOFRAME_HEADER_SIZE], HDMI_AVI_INFOFRAME_SIZE);
	if (ret)
		goto unlock;

	if (regmap_write(ctx->regmap, IT66121_AVIINFO_CSUM_REG, buf[3]))
		goto unlock;

	/* Enable AVI infoframe */
	if (regmap_write(ctx->regmap, IT66121_AVI_INFO_PKT_REG,
			 IT66121_AVI_INFO_PKT_ON | IT66121_AVI_INFO_PKT_RPT))
		goto unlock;

	/* Set TX mode to HDMI */
	if (regmap_write(ctx->regmap, IT66121_HDMI_MODE_REG, IT66121_HDMI_MODE_HDMI))
		goto unlock;

configure:
	if ((ctx->id == ID_IT66121 || ctx->id == ID_IT66122) &&
	    regmap_write_bits(ctx->regmap, IT66121_CLK_BANK_REG, IT66121_CLK_BANK_PWROFF_TXCLK,
			      IT66121_CLK_BANK_PWROFF_TXCLK)) {
		goto unlock;
	}

	if (it66121_configure_input(ctx))
		goto unlock;

	if (it66121_configure_afe(ctx, adjusted_mode))
		goto unlock;

	if ((ctx->id == ID_IT66121 || ctx->id == ID_IT66122) &&
	    regmap_write_bits(ctx->regmap, IT66121_CLK_BANK_REG, IT66121_CLK_BANK_PWROFF_TXCLK,
			      0)) {
		goto unlock;
	}

	if (it66121_restore_interrupts(ctx))
		dev_err(ctx->dev, "Failed to restore HPD interrupts\n");
unlock:
	mutex_unlock(&ctx->lock);
}

static enum drm_mode_status it66121_bridge_mode_valid(struct drm_bridge *bridge,
						      const struct drm_display_info *info,
						      const struct drm_display_mode *mode)
{
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	unsigned long max_clock;

	max_clock = (ctx->bus_width == 12) ? 74250 : 148500;

	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return MODE_BAD;

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
	int idx, ret;

	if (!drm_dev_enter(bridge->dev, &idx))
		return connector_status_disconnected;
	mutex_lock(&ctx->lock);
	ret = regmap_read(ctx->regmap, IT66121_SYS_STATUS_REG, &value);
	status = ret				       ? connector_status_unknown :
		 (value & IT66121_SYS_STATUS_HPDETECT) ? connector_status_connected :
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

	if (!drm_dev_enter(bridge->dev, &idx))
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
	.atomic_get_input_bus_fmts = it66121_bridge_atomic_get_input_bus_fmts,
	.atomic_enable = it66121_bridge_enable,
	.atomic_disable = it66121_bridge_disable,
	.atomic_check = it66121_bridge_check,
	.mode_set = it66121_bridge_mode_set,
	.mode_valid = it66121_bridge_mode_valid,
	.detect = it66121_bridge_detect,
	.edid_read = it66121_bridge_edid_read,
	.hpd_enable = it66121_bridge_hpd_enable,
	.hpd_disable = it66121_bridge_hpd_disable,
};

static void it66121_bridge_interrupt(struct drm_bridge *bridge)
{
	int ret;
	unsigned int val;
	struct it66121_ctx *ctx = container_of(bridge, struct it66121_ctx, bridge);
	struct device *dev = ctx->dev;
	enum drm_connector_status status;
	bool event = false;

	mutex_lock(&ctx->lock);

	ret = regmap_read(ctx->regmap, IT66121_SYS_STATUS_REG, &val);
	if (ret)
		goto unlock;

	if (!(val & IT66121_SYS_STATUS_ACTIVE_IRQ))
		goto unlock;

	ret = regmap_read(ctx->regmap, IT66121_INT_STATUS1_REG, &val);
	if (ret) {
		dev_err(dev, "Cannot read STATUS1_REG %d\n", ret);
	} else if (val & IT66121_INT_STATUS1_HPD_STATUS) {
		regmap_write_bits(ctx->regmap, IT66121_INT_CLR1_REG, IT66121_INT_CLR1_HPD,
				  IT66121_INT_CLR1_HPD);

		status = it66121_is_hpd_detect(ctx) ? connector_status_connected :
						      connector_status_disconnected;

		event = true;
	}

	regmap_write_bits(ctx->regmap, IT66121_SYS_STATUS_REG, IT66121_SYS_STATUS_CLEAR_IRQ,
			  IT66121_SYS_STATUS_CLEAR_IRQ);

unlock:
	mutex_unlock(&ctx->lock);

	if (event)
		drm_bridge_hpd_notify(&ctx->bridge, status);
}

/* USB component glue: the bridge has no Device Tree graph or GPIO IRQ. */
static int it66121_bind(struct device *comp, struct device *master, void *data)
{
	struct fl2000_bridge *binding = data;
	struct it66121_ctx *ctx = dev_get_drvdata(comp);

	binding->bridge = &ctx->bridge;
	binding->connector = &ctx->connector;
	binding->interrupt = it66121_bridge_interrupt;
	return 0;
}

static void it66121_unbind(struct device *comp, struct device *master, void *data)
{
	struct fl2000_bridge *binding = data;

	binding->interrupt = NULL;
	binding->connector = NULL;
	binding->bridge = NULL;
}

static const struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

static int it66121_probe(struct i2c_client *client)
{
	struct it66121_ctx *ctx;
	unsigned int value;
	u8 bytes[4];
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENXIO;
	ctx = devm_drm_bridge_alloc(&client->dev, struct it66121_ctx, bridge,
				    &it66121_bridge_funcs);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dev = &client->dev;
	ctx->client = client;
	ctx->adapter = client->adapter;
	ctx->bus_width = 24;
	ctx->id = ID_IT66121;
	mutex_init(&ctx->lock);
	ctx->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(ctx->regmap))
		return dev_err_probe(ctx->dev, PTR_ERR(ctx->regmap),
				     "Register map initialization failed\n");
	for (unsigned int i = 0; i < ARRAY_SIZE(bytes); i++) {
		ret = regmap_read(ctx->regmap, i, &value);
		if (ret)
			return dev_err_probe(ctx->dev, ret, "ID register %u read failed\n", i);
		bytes[i] = value;
	}
	if (bytes[0] != 0x54 || bytes[1] != 0x49 || bytes[2] != 0x12 || (bytes[3] & 0x0f) != 0x06)
		return -ENODEV;
	ctx->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	/* The USB worker acknowledges HPD and updates the DRM connector. */
	ctx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;
	drm_bridge_add(&ctx->bridge);
	i2c_set_clientdata(client, ctx);
	ret = component_add(ctx->dev, &it66121_component_ops);
	if (ret)
		drm_bridge_remove(&ctx->bridge);
	return ret;
}

static void it66121_remove(struct i2c_client *client)
{
	struct it66121_ctx *ctx = i2c_get_clientdata(client);

	component_del(&client->dev, &it66121_component_ops);
	drm_bridge_remove(&ctx->bridge);
}

static const struct i2c_device_id it66121_ids[] = { { "it66121", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, it66121_ids);
static struct i2c_driver it66121_driver = {
	.probe = it66121_probe,
	.remove = it66121_remove,
	.driver = {
		.name = "it66121",
	},
	.id_table = it66121_ids,
};
module_i2c_driver(it66121_driver);
MODULE_AUTHOR("Phong LE; Artem Mygaiev");
MODULE_DESCRIPTION("IT66121 HDMI bridge with FL2000 USB transport adaptations");
MODULE_LICENSE("GPL v2");
