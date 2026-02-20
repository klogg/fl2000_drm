// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "it66121.h"

#define VENDOR_ID      0x4954
#define DEVICE_ID      0x0612
#define REVISION_MASK  0xF000
#define REVISION_SHIFT 12

#define OFFSET_BITS 8
#define VALUE_BITS  8

#define IRQ_POLL_INTRVL 100

struct it66121_priv {
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_bridge bridge;
	struct drm_connector connector;
	enum drm_connector_status conn_status;

	struct delayed_work work;
	struct workqueue_struct *work_queue;

	struct regmap_field *irq_pending;
	struct regmap_field *hpd;
	struct regmap_field *ddc_done;
	struct regmap_field *ddc_error;
	struct regmap_field *clr_irq;

	struct regmap_field *swap_pack;
	struct regmap_field *swap_ml;
	struct regmap_field *swap_yc;
	struct regmap_field *swap_rb;

	struct hdmi_avi_infoframe hdmi_avi_infoframe;

	const struct drm_edid *edid;
	bool dvi_mode;
};

/* XXX: Only one instance of IT66121 is supported!!!
 *  - need to have a way to configure several I2C buses to scan
 *  - need to have a list of objects for registration / deregistration
 */
static struct it66121_priv *ctx;
static int i2c_bus_num;
module_param(i2c_bus_num, int, 0660);

static const struct regmap_range_cfg it66121_regmap_banks[] = {
	/* Do not put common registers to any range, this will lead to skipping "bank" configuration
	 * when accessing those at addresses 0x00-0x2F
	 */
	{
		.name = "Banks",
		.range_min = IT66121_BANK_START,
		.range_max = IT66121_BANK_END,
		.selector_reg = IT66121_SYS_CONTROL,
		.selector_mask = IT66121_SYS_BANK_MASK,
		.selector_shift = 0,
		.window_start = IT66121_BANK_START,
		.window_len = IT66121_BANK_SIZE,
	},
};

static bool it66121_reg_volatile(struct device *dev, unsigned int reg)
{
	UNUSED(dev);
	return IT66121_REG_VOLATILE(reg);
}

static const struct regmap_config it66121_regmap_config = {
	.val_bits = 8, /* 8-bit register size */
	.reg_bits = 8, /* 8-bit register address space */
	.reg_stride = 1,
	.max_register = 2 * IT66121_BANK_SIZE - 1, /* 2 banks of registers */

	.cache_type = REGCACHE_RBTREE,

	.volatile_reg = it66121_reg_volatile,

	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.use_single_read = true,
	.use_single_write = true,
};

#define EDID_SLEEP   1000
#define EDID_TIMEOUT 200000

#define EDID_HDCP_ADDR 0x74
#define EDID_DDC_ADDR  0xA0

#define EDID_FIFO_SIZE 32

enum it66121_ddc_cmd {
	DDC_CMD_BURST_READ = 0x0,
	DDC_CMD_LINK_CHECK = 0x2,
	DDC_CMD_EDID_READ = 0x3,
	DDC_CMD_ASKV_WRITE = 0x4,
	DDC_CMD_AINFO_WRITE = 0x5,
	DDC_CMD_AN_WRITE = 0x6,
	DDC_CMD_FIFO_CLEAR = 0x9,
	DDC_CMD_SCL_PULSE = 0xA,
	DDC_CMD_ABORT = 0xF,
};

/* XXX: mode, adjusted_mode can be used here for transformation configuration */
static int it66121_configure_input(struct it66121_priv *priv)
{
	int ret;

	/* XXX: This configures bridge IC connection to encoder IC. In our case this is the
	 * interface between IT66121 and FL2000. Of course it is static so shall be read from
	 * device's EEPROM (not sure if there is EEPROM on our HW), or at least to be set by
	 * driver's parameters. For now, let's keep it simple - hardcode it:
	 *   - input mode RGB
	 *   - IO latch clock = TXCLK
	 *   - CCIR656 disabled
	 *   - embedded sync disabled
	 *   - DDR mode disabled
	 *   - 1 cycle input PCLK delay
	 * NOTE: some flexible encoders may support non-static mode on the bus, for those we may
	 * need to also use modeset parameters
	 */
	ret = regmap_write(priv->regmap, IT66121_INPUT_MODE,
			   IT66121_INPUT_MODE_RGB | IT66121_INPUT_PCLKDELAY1);
	if (ret)
		return ret;

	/* XXX: Also we can change some parameters of IT66121_INPUT_IO_CONTROL related to TX FIFO
	 * resetting, 10/12bit YCbCr422 sequential IO mode
	 */

	/* XXX: This configures transformation needed in order to properly convert input signal to
	 * output. For now we hardcode "bypassing". In case when conversion is needed we have to
	 * also to set Color Space Conversion Matrix and RGB or YUV blank levels
	 */
	ret = regmap_write(priv->regmap, IT66121_INPUT_COLOR_CONV, IT66121_INPUT_NO_CONV);
	if (ret)
		return ret;

	return 0;
}

static int it66121_configure_afe(struct it66121_priv *priv, int clock_khz)
{
	int ret;

	ret = regmap_write(priv->regmap, IT66121_AFE_DRV_CONTROL, IT66121_AFE_RST);
	if (ret)
		return ret;

	/* TODO: Rewrite with proper bit names */
	if (clock_khz > 80000) {
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, 0x90, 0x80);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, 0x89, 0x80);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_3, 0x10, 0x80);
		if (ret)
			return ret;
	} else {
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, 0x90, 0x10);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, 0x89, 0x09);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_3, 0x10, 0x10);
		if (ret)
			return ret;
	}

	/* Fire AFE */
	ret = regmap_write(priv->regmap, IT66121_AFE_DRV_CONTROL, 0);
	if (ret)
		return ret;

	return 0;
}

static int it66121_wait_ddc_ready(struct it66121_priv *priv)
{
	int ret;
	unsigned int val;

	ret = regmap_field_read_poll_timeout(priv->ddc_done, val, true, EDID_SLEEP, EDID_TIMEOUT);
	if (ret)
		return ret;

	ret = regmap_field_read(priv->ddc_error, &val);
	if (ret)
		return ret;
	if (val)
		return -EIO;

	return 0;
}

static int it66121_clear_ddc_fifo(struct it66121_priv *priv)
{
	int ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_FIFO_CLEAR);
	if (ret)
		return ret;

	return 0;
}

static int it66121_abort_ddc_ops(struct it66121_priv *priv)
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
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_ABORT);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void it66121_is_hpd_detect(struct it66121_priv *priv)
{
	int ret;
	unsigned int val;
	struct device *dev = priv->bridge.dev->dev;

	ret = regmap_field_read(priv->hpd, &val);
	if (ret) {
		dev_err(dev, "Cannot get monitor status (%d)", ret);
		priv->conn_status = connector_status_unknown;
	} else {
		priv->conn_status = val ? connector_status_connected :
					  connector_status_disconnected;
	}
}

/* TODO: Add protection for I2C register / EDID / SPI access, e.g. mutex*/
static void it66121_intr_work(struct work_struct *work_item)
{
	int ret;
	unsigned int val;
	struct delayed_work *dwork = container_of(work_item, struct delayed_work, work);
	struct it66121_priv *priv = container_of(dwork, struct it66121_priv, work);
	struct device *dev = priv->bridge.dev->dev;
	bool event = false;

	ret = regmap_field_read(priv->irq_pending, &val);
	if (ret) {
		/* TODO: Process error? */
		dev_err(dev, "Cannot read interrupt status (%d)", ret);
		return;
	}

	/* XXX: There are at least 5 registers that can source interrupt:
	 *  - 0x06 (IT66121_INT_STATUS_1)
	 *  - 0x07 (IT66121_INT_STATUS_2)
	 *  - 0x08 (IT66121_INT_STATUS_3)
	 *  - 0xEE (IT66121_EXT_INT_STATUS_1)
	 *  - 0xF0 (IT66121_EXT_INT_STATUS_2)
	 * For now we process only DDC events of the IT66121_INT_STATUS_1 which implies proper masks
	 * configuration
	 */
	if (val) {
		union it666121_int_status_1_reg status_1;

		ret = regmap_read(priv->regmap, IT66121_INT_STATUS_1, &status_1.val);
		if (ret) {
			/* TODO: Process error? */
			dev_err(dev, "Cannot read IT66121_INT_STATUS_1 (%d)", ret);
			return;
		}

		regmap_field_write(priv->clr_irq, 1);

		/* XXX: lock */

		if (status_1.ddc_fifo_err)
			it66121_clear_ddc_fifo(priv);
		if (status_1.ddc_bus_hang || status_1.ddc_noack)
			it66121_abort_ddc_ops(priv);
		if (status_1.hpd_plug) {
			it66121_is_hpd_detect(priv);
			event = true;
			if (priv->conn_status == connector_status_disconnected) {
				drm_edid_free(priv->edid);
				priv->edid = NULL;
			}
		}

		/* XXX: unlock */
	}

	if (event)
		drm_helper_hpd_irq_event(priv->bridge.dev);

	queue_delayed_work(priv->work_queue, &priv->work, msecs_to_jiffies(IRQ_POLL_INTRVL));
}

static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	int ret;
	size_t remain = len;
	unsigned int val;
	unsigned int segment = block >> 1;
	unsigned int offset = block & 1 ? 128 : 0;
	static const u8 header[EDID_LOSS_LEN] = { 0x00, 0xFF, 0xFF };
	struct it66121_priv *priv = context;

	/* Abort DDC */
	ret = it66121_abort_ddc_ops(priv);
	if (ret)
		return ret;

	/* Statically fill first 3 bytes (due to EDID reading HW bug) */
	while ((offset < EDID_LOSS_LEN) && (remain > 0)) {
		*(buf++) = header[offset];
		remain--;
		offset++;
	}

	while (remain > 0) {
		/* Add bytes that will be lost during EDID read */
		size_t size = remain + EDID_LOSS_LEN;

		/* ... and check size fits FIFO */
		size = size > EDID_FIFO_SIZE ? EDID_FIFO_SIZE : size;

		/* Clear DDC FIFO */
		ret = it66121_clear_ddc_fifo(priv);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_ADDRESS, EDID_DDC_ADDR);
		if (ret)
			return ret;

		/* Account 3 bytes that will be lost */
		ret = regmap_write(priv->regmap, IT66121_DDC_OFFSET, offset - EDID_LOSS_LEN);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_SIZE, (unsigned int)size);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_SEGMENT, segment);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_EDID_READ);
		if (ret)
			return ret;

		/* Deduct lost bytes when reading from FIFO */
		size -= EDID_LOSS_LEN;

		for (int i = 0; i < size; i++) {
			ret = regmap_read(priv->regmap, IT66121_DDC_RD_FIFO, &val);
			if (ret)
				return ret;

			*(buf++) = val & 0xFF;
		}

		remain -= size;
		offset += size;
	}

	return ret;
}

static int it66121_connector_get_modes(struct drm_connector *connector)
{
	struct it66121_priv *priv = container_of(connector, struct it66121_priv, connector);
	const struct drm_edid *drm_edid = priv->edid;

	if (!drm_edid) {
		drm_edid = drm_edid_read_custom(connector, it66121_get_edid_block, priv);
		if (!drm_edid)
			return 0;

		drm_edid_connector_update(connector, drm_edid);

		priv->dvi_mode = !drm_detect_hdmi_monitor(drm_edid_raw(drm_edid));
		priv->edid = drm_edid;
	}

	return drm_edid_connector_add_modes(connector);
}

static enum drm_mode_status it66121_connector_mode_valid(struct drm_connector *connector,
							 struct drm_display_mode *mode)
{
	/* TODO: validate mode */
	UNUSED(connector);
	UNUSED(mode);

	return MODE_OK;
}

static struct drm_connector_helper_funcs it66121_connector_helper_funcs = {
	.get_modes = it66121_connector_get_modes,
	.mode_valid = it66121_connector_mode_valid,
};

static enum drm_connector_status it66121_connector_detect(struct drm_connector *connector,
							  bool force)
{
	struct it66121_priv *priv = container_of(connector, struct it66121_priv, connector);

	if (force || priv->conn_status == connector_status_unknown)
		it66121_is_hpd_detect(priv);

	return priv->conn_status;
}

static const struct drm_connector_funcs it66121_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = it66121_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int it66121_bind(struct device *comp, struct device *master, void *master_data)
{
	int ret;
	struct drm_bridge *bridge = dev_get_drvdata(comp);
	struct drm_simple_display_pipe *pipe = master_data;
	struct i2c_adapter *adapter = i2c_verify_adapter(master);

	if (!adapter)
		return -ENODEV;

	dev_info(comp, "Binding IT66121 component");

	/* XXX: check adapter, check bridge */

	ret = drm_simple_display_pipe_attach_bridge(pipe, bridge);
	if (ret)
		dev_err(comp, "Cannot attach IT66121 bridge (%d)", ret);

	return ret;
}

static void it66121_unbind(struct device *comp, struct device *master, void *master_data)
{
	/* TODO: drm_bridge_detach()? */
	UNUSED(master);
	UNUSED(master_data);

	dev_info(comp, "Unbinding IT66121 component");
}

static const struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

/* TODO: rewrite register access properly, add error processing */
static int it66121_bridge_attach(struct drm_bridge *bridge, enum drm_bridge_attach_flags flags)
{
	int ret;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR) {
		DRM_ERROR("Need connector for mode configuration");
		return -ENODEV;
	}

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found");
		return -ENODEV;
	}

	/* Reset according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
			  IT66121_SW_REF_RST_HDMITX);
	msleep(50);

	/* Power up GRCLK & power down IACLK, TxCLK, CRCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (0xf << 3), (7 << 3));

	/* Continue according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_INT_CONTROL, (1 << 0), (0 << 0));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1 << 5), (0 << 5));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1 << 2) | (1 << 6),
			  (0 << 2) | (0 << 6));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1 << 6), (0 << 6));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1 << 4), (0 << 4));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1 << 3), (1 << 3));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1 << 2), (1 << 2));

	/* Extra steps for AFE from original driver */
	/* whole register is defined as XP_TEST, values are undisclosed */
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_TEST, 0xFF, 0x70);
	/* lower 5 bits are undisclosed in manual */
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_HS, 0xFF, 0x1F);
	/* DRV_ISW[5:3] value '011' is a default output current level swing, with change to '111' we
	 * set output current level swing to maximum
	 */
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_2, 0xFF, 0x38);

	/* power up IACLK, TxCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (3 << 4), (0 << 4));

	/* Reset according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
			  IT66121_SW_REF_RST_HDMITX);
	msleep(50);

	/* We do not support HDCP so its ok to statically set host controls */
	regmap_write(priv->regmap, IT66121_DDC_CONTROL,
		     IT66121_DDC_MASTER_DDC | IT66121_DDC_MASTER_HOST);

	ret = drm_connector_init(bridge->dev, &priv->connector, &it66121_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		DRM_ERROR("Cannot initialize bridge connector");
		return ret;
	}

	drm_connector_helper_add(&priv->connector, &it66121_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&priv->connector, bridge->encoder);
	if (ret) {
		DRM_ERROR("Cannot attach bridge");
		return ret;
	}

	drm_connector_register(&priv->connector);

	/* Start interrupts */
	regmap_write_bits(priv->regmap, IT66121_INT_MASK_1, IT66121_MASK_DDC, 0);
	INIT_DELAYED_WORK(&priv->work, &it66121_intr_work);
	queue_delayed_work(priv->work_queue, &priv->work, msecs_to_jiffies(IRQ_POLL_INTRVL));

	dev_info(bridge->dev->dev, "Bridge attached");

	return 0;
}

static void it66121_bridge_detach(struct drm_bridge *bridge)
{
	/* TODO: Detach encoder */
	dev_info(bridge->dev->dev, "it66121_bridge_detach");
}

static void it66121_bridge_enable(struct drm_bridge *bridge)
{
	int ret;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	dev_info(bridge->dev->dev, "it66121_bridge_enable");

	/* Unmute AV */
	ret = regmap_write(priv->regmap, IT66121_HDMI_AV_MUTE, 0);
	if (ret)
		return;
}

static void it66121_bridge_disable(struct drm_bridge *bridge)
{
	int ret;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	dev_info(bridge->dev->dev, "it66121_bridge_disable");

	/* Mute AV */
	ret = regmap_write_bits(priv->regmap, IT66121_HDMI_AV_MUTE, IT66121_HDMI_AV_MUTE_ON,
				IT66121_HDMI_AV_MUTE_ON);
	if (ret)
		return;
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge, const struct drm_display_mode *mode,
				    const struct drm_display_mode *adjusted_mode)

{
	int ret;
	ssize_t frame_size;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	const u16 aviinfo_reg[HDMI_AVI_INFOFRAME_SIZE] = {
		IT66121_HDMI_AVIINFO_DB1,  IT66121_HDMI_AVIINFO_DB2,  IT66121_HDMI_AVIINFO_DB3,
		IT66121_HDMI_AVIINFO_DB4,  IT66121_HDMI_AVIINFO_DB5,  IT66121_HDMI_AVIINFO_DB6,
		IT66121_HDMI_AVIINFO_DB7,  IT66121_HDMI_AVIINFO_DB8,  IT66121_HDMI_AVIINFO_DB9,
		IT66121_HDMI_AVIINFO_DB10, IT66121_HDMI_AVIINFO_DB11, IT66121_HDMI_AVIINFO_DB12,
		IT66121_HDMI_AVIINFO_DB13
	};

	UNUSED(adjusted_mode);

	dev_info(bridge->dev->dev, "Setting AVI infoframe for mode: " DRM_MODE_FMT,
		 DRM_MODE_ARG(mode));

	ret = drm_hdmi_avi_infoframe_from_display_mode(&priv->hdmi_avi_infoframe, &priv->connector,
						       mode);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot create AVI infoframe (%d)", ret);
		return;
	}

	/* TODO: Set up color information here */

	frame_size = hdmi_avi_infoframe_pack(&priv->hdmi_avi_infoframe, buf, sizeof(buf));
	if (frame_size < 0) {
		dev_err(bridge->dev->dev, "Cannot pack AVI infoframe (%ld)", frame_size);
		return;
	}

	/* Set TX mode */
	ret = regmap_write(priv->regmap, IT66121_HDMI_MODE,
			   priv->dvi_mode ? IT66121_HDMI_MODE_DVI : IT66121_HDMI_MODE_HDMI);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot set TX mode (%d)", ret);
		return;
	}

	/* Enable HDMI packets, repeat for every data frame as recommended */
	ret = regmap_write(priv->regmap, IT66121_HDMI_GEN_CTRL_PKT,
			   IT66121_HDMI_GEN_CTRL_PKT_ON | IT66121_HDMI_GEN_CTRL_PKT_RPT);
	if (ret) {
		DRM_ERROR("Cannot enable HDMI packets");
		return;
	}

	/* Mute AV */
	ret = regmap_write(priv->regmap, IT66121_HDMI_AV_MUTE,
			   IT66121_HDMI_AV_MUTE_ON | IT66121_HDMI_AV_MUTE_BLUE);
	if (ret) {
		DRM_ERROR("Cannot mute AV");
		return;
	}

	/* Write new AVI infoframe packet */
	for (int i = 0; i < HDMI_AVI_INFOFRAME_SIZE; i++) {
		ret = regmap_write(priv->regmap, aviinfo_reg[i],
				   buf[i + HDMI_INFOFRAME_HEADER_SIZE]);
		if (ret) {
			dev_err(bridge->dev->dev, "Cannot write AVI infoframe byte %d (%d)", i,
				ret);
			return;
		}
	}
	ret = regmap_write(priv->regmap, IT66121_HDMI_AVIINFO_CSUM, buf[3]);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot write AVI infoframe checksum (%d)", ret);
		return;
	}

	/* Enable AVI infoframe */
	ret = regmap_write(priv->regmap, IT66121_HDMI_AVI_INFO_PKT,
			   IT66121_HDMI_AVI_INFO_PKT_ON | IT66121_HDMI_AVI_INFO_RPT);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot enable AVI infoframe (%d)", ret);
		return;
	}

	/* Set reset flags */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST);
	if (ret)
		return;

	/* Disable TXCLK prior to configuration */
	ret = regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF,
				IT66121_SYS_TXCLK_OFF);
	if (ret)
		return;

	/* Configure connection, conversions, etc. */
	ret = it66121_configure_input(priv);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot configure input bus (%d)", ret);
		return;
	}

	/* Configure AFE */
	ret = it66121_configure_afe(priv, mode->clock);
	if (ret) {
		dev_err(bridge->dev->dev, "Cannot configure AFE (%d)", ret);
		return;
	}

	/* Clear reset flags */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST, 0);
	if (ret)
		return;

	/* Enable TXCLK */
	ret = regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF, 0);
	if (ret)
		return;
}

static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.attach = it66121_bridge_attach,
	.detach = it66121_bridge_detach,
	.enable = it66121_bridge_enable,
	.disable = it66121_bridge_disable,
	.mode_set = it66121_bridge_mode_set,
};

static int it66121_regs_init(struct it66121_priv *priv, struct i2c_client *client)
{
	priv->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(priv->regmap))
		return (int)PTR_ERR(priv->regmap);

	priv->irq_pending =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_irq_pending);
	priv->hpd = devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_hpd);
	priv->clr_irq =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_clr_irq);
	priv->ddc_done =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_DDC_STATUS_ddc_done);
	priv->ddc_error =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_DDC_STATUS_ddc_error);

	priv->swap_pack =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_HDMI_DATA_SWAP_pack);
	priv->swap_ml =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_HDMI_DATA_SWAP_ml);
	priv->swap_yc =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_HDMI_DATA_SWAP_yc);
	priv->swap_rb =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_HDMI_DATA_SWAP_rb);

	/* Dont really care which one failed */
	if (IS_ERR(priv->irq_pending) || IS_ERR(priv->hpd) || IS_ERR(priv->clr_irq) ||
	    IS_ERR(priv->ddc_done) || IS_ERR(priv->ddc_error)) {
		return -1;
	}

	return 0;
}

static int it66121_i2c_probe(struct i2c_adapter *adapter, unsigned short address)
{
	int ret;
	u8 id_regs[] = { IT66121_VENDOR_ID_1, IT66121_VENDOR_ID_2, IT66121_DEVICE_ID_1,
			 IT66121_DEVICE_ID_2 };
	union {
		struct {
			u16 vendor;
			u16 device;
		};
		u8 b[ARRAY_SIZE(id_regs)];
	} id;

	dev_dbg(&adapter->dev, "Detecting IT66121 at address 0x%X on %s", address, adapter->name);

	/* We rely on full I2C protocol + 1 byte SMBUS read for detection */
	ret = i2c_check_functionality(adapter, I2C_FUNC_I2C | I2C_FUNC_SMBUS_READ_BYTE);
	if (!ret) {
		dev_dbg(&adapter->dev, "Adapter does not support I2C functions properly");
		return -ENODEV;
	}

	for (int i = 0; i < ARRAY_SIZE(id_regs); i++) {
		struct i2c_msg msgs[] = {
			{ .addr = address, .flags = 0, .len = 1, .buf = &id_regs[i] },
			{ .addr = address, .flags = I2C_M_RD, .len = 1, .buf = &id.b[i] }
		};

		ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
		if (ret < 0) {
			dev_err(&adapter->dev, "I2C transfer failed (%d)", ret);
			return -ENODEV;
		}
	}

	if (id.vendor != VENDOR_ID || (id.device & ~REVISION_MASK) != DEVICE_ID) {
		dev_dbg(&adapter->dev, "IT66121 not found (0x%X-0x%X)", id.vendor, id.device);
		return -ENODEV;
	}

	dev_info(&adapter->dev, "IT66121 found, revision %d",
		 (id.device & REVISION_MASK) >> REVISION_SHIFT);

	return 0;
}

static struct i2c_client *it66121_i2c_init(void)
{
	struct i2c_client *client;
	struct i2c_board_info board_info = { I2C_BOARD_INFO("it66121", 0) };
	struct i2c_adapter *adapter;

	/* According to datasheet IT66121 addresses are 0x98 or 0x9A including cmd */
	const unsigned short it66121_addr[] = { (0x98 >> 1), (0x9A >> 1), I2C_CLIENT_END };

	adapter = i2c_get_adapter(i2c_bus_num);
	if (!adapter)
		return ERR_PTR(-ENODEV);

	client = i2c_new_scanned_device(adapter, &board_info, it66121_addr, it66121_i2c_probe);

	i2c_put_adapter(adapter);

	return client;
}

static void __exit it66121_remove(void)
{
	struct it66121_priv *priv = ctx;

	cancel_delayed_work_sync(&priv->work);

	destroy_workqueue(priv->work_queue);

	component_del(&priv->client->dev, &it66121_component_ops);

	drm_edid_free(priv->edid);

	drm_bridge_remove(&priv->bridge);

	i2c_unregister_device(priv->client);

	kfree(priv);
}

static int __init it66121_probe(void)
{
	int ret;
	struct it66121_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = it66121_i2c_init();
	if (IS_ERR(priv->client)) {
		ret = (int)PTR_ERR(priv->client);
		pr_err("Cannot find IT66121 I2C client");
		kfree(priv);
		return ret;
	}

	it66121_regs_init(priv, priv->client);

	priv->conn_status = connector_status_unknown;
	priv->bridge.funcs = &it66121_bridge_funcs;

	drm_bridge_add(&priv->bridge);

	/* XXX: Store private context properly*/
	ctx = priv;

	/* Setup work queue for interrupt processing work */
	priv->work_queue = create_workqueue("work_queue");
	if (!priv->work_queue) {
		pr_err("Create interrupt workqueue failed");
		drm_bridge_remove(&priv->bridge);
		kfree(priv);
		return -ENOMEM;
	}

	/* Store bridge pointer at I2C client device for further binding */
	dev_set_drvdata(&priv->client->dev, &priv->bridge);

	/* Bind component of I2C client to master I2C adapter */
	ret = component_add(&priv->client->dev, &it66121_component_ops);
	if (ret) {
		pr_err("Cannot register IT66121 component");
		destroy_workqueue(priv->work_queue);
		drm_bridge_remove(&priv->bridge);
		kfree(priv);
		return ret;
	}

	return 0;
}

module_init(it66121_probe);
module_exit(it66121_remove);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("IT66121 HDMI transmitter driver");
MODULE_LICENSE("GPL v2");
