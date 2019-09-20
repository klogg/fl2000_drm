/* SPDX-License-Identifier: GPL-2.0 */
/*
 * it66121_drv.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "it66121.h"

#define VENDOR_ID	0x4954
#define DEVICE_ID	0x0612
#define REVISION_MASK	0xF000
#define REVISION_SHIFT	12

#define OFFSET_BITS	8
#define VALUE_BITS	8

#define IRQ_POLL_INTRVL	100

enum it66121_intr_state {
	RUN = (1U),
	STOP = (0U),
};

struct it66121_priv {
	struct regmap *regmap;
	struct drm_display_mode curr_mode;
	struct drm_bridge bridge;
	struct drm_connector connector;
	enum drm_connector_status status;

	struct delayed_work work;
	struct workqueue_struct *work_queue;
	atomic_t state;

	struct regmap_field *irq_pending;
	struct regmap_field *hpd;
	struct regmap_field *ddc_done;
	struct regmap_field *ddc_error;
	struct regmap_field *clr_irq;

	struct hdmi_avi_infoframe hdmi_avi_infoframe;
	u8 *hdmi_avi_infoframe_raw;
};

static int it66121_remove(struct i2c_client *client);

/* According to datasheet IT66121 addresses are 0x98 or 0x9A including cmd */
static const unsigned short it66121_addr[] = {(0x98 >> 1), /*(0x9A >> 1),*/
	I2C_CLIENT_END};

static const struct regmap_range_cfg it66121_regmap_banks[] = {
	/* Do not put common registers to any range, this will lead to skipping
	 * "bank" configuration when accessing those at addresses 0x00-0x2F */
	{
		.name = "Banks",
		.range_min = IT66121_BANK_START,
		.range_max = IT66121_BANK_END,
		.selector_reg = IT66121_SYS_CONTROL,
		.selector_mask = 3,
		.selector_shift = 0,
		.window_start = IT66121_BANK_START,
		.window_len = IT66121_BANK_SIZE,
	},
};

static const struct regmap_config it66121_regmap_config = {
	.val_bits = 8, /* 8-bit register size */
	.reg_bits = 8, /* 8-bit register address space */
	.reg_stride = 1,
	.max_register = 2 * IT66121_BANK_SIZE - 1, /* 2 banks of registers */

	.cache_type = REGCACHE_NONE,

	//.precious_reg = it66121_precious_reg,
	//.volatile_reg = it66121_volatile_reg,

	//.reg_defaults = it66121_reg_defaults,
	//.num_reg_defaults = ARRAY_SIZE(it66121_reg_defaults),

	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.use_single_read = true,
	.use_single_write = true,
};

#define EDID_SLEEP	1000
#define EDID_TIMEOUT	200000

#define EDID_HDCP_ADDR	0x74
#define EDID_DDC_ADDR	0xA0

#define EDID_FIFO_SIZE	32

enum {
	DDC_CMD_BURST_READ = 0x0,
	DDC_CMD_LINK_CHECK = 0x2,
	DDC_CMD_EDID_READ = 0x3,
	DDC_CMD_ASKV_WRITE = 0x4,
	DDC_CMD_AINFO_WRITE = 0x5,
	DDC_CMD_AN_WRITE = 0x6,
	DDC_CMD_FIFO_CLEAR = 0x9,
	DDC_CMD_SCL_PULSE = 0xA,
	DDC_CMD_ABORT = 0xF,
} ddc_cmd;

static inline int it66121_wait_ddc_ready(struct it66121_priv *priv)
{
	int ret, val;

	ret = regmap_field_read_poll_timeout(priv->ddc_done, val, true,
			EDID_SLEEP, EDID_TIMEOUT);
	if (ret != 0)
		return ret;

	ret = regmap_field_read(priv->ddc_error, &val);
	if (ret != 0)
		return ret;
	if (val != 0)
		return -EIO;

	return 0;
}

static int it66121_clear_ddc_fifo(struct it66121_priv *priv)
{
	int ret;
	unsigned int ddc_control_val;

	struct device *dev = priv->bridge.dev->dev;
	dev_info(dev, "Clear DDC FIFO");

	ret = regmap_read(priv->regmap, IT66121_DDC_CONTROL, &ddc_control_val);
	if (ret != 0)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
			IT66121_DDC_MASTER_DDC |
			IT66121_DDC_MASTER_HOST);
	if (ret != 0)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
			DDC_CMD_FIFO_CLEAR);
	if (ret != 0)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL, ddc_control_val);
	if (ret != 0)
		return ret;

	return 0;
}

static int it66121_abort_ddc_ops(struct it66121_priv *priv)
{
	int i, ret;
	unsigned int ddc_control_val;

	struct device *dev = priv->bridge.dev->dev;
	dev_info(dev, "Abort DDC Operations");

	/* XXX: Prior to DDC abort command there was also a reset of HDCP:
	 *  1. HDCP_DESIRE clear bit CP DESIRE
	 *  2. SW_RST set bit HDCP_RST
	 * it seems wrong to keep them reset, i.e. without restoring initial
	 * state, but somehow this is how it was implemented. This sequence is
	 * removed since HDCP is not supported */

	ret = regmap_read(priv->regmap, IT66121_DDC_CONTROL, &ddc_control_val);
	if (ret != 0)
		return ret;

	/* According to 2009/01/15 modification by Jau-Chih.Tseng@ite.com.tw
	 * do abort DDC twice */
	for (i = 0; i < 2; i++) {
		ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
				IT66121_DDC_MASTER_DDC |
				IT66121_DDC_MASTER_HOST);
		if (ret != 0)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_ABORT);
		if (ret != 0)
			return ret;

		ret = it66121_wait_ddc_ready(priv);
		if (ret != 0)
			return ret;
	}

	ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL, ddc_control_val);
	if (ret != 0)
		return ret;

	return 0;
}

static void it66121_intr_work(struct work_struct *work_item)
{
	int ret;
	unsigned int val;
	struct delayed_work *dwork = container_of(work_item,
			struct delayed_work, work);
	struct it66121_priv *priv = container_of(dwork, struct it66121_priv,
			work);
	struct device *dev = priv->bridge.dev->dev;

	/* TODO: use mutex */

	ret = regmap_field_read(priv->irq_pending, &val);
	if (ret < 0) {
		dev_err(dev, "Cannot read interrupt status (%d)", ret);
	}
	/* XXX: There are at least 5 registers that can source interrupt:
	 *  - 0x06 (IT66121_INT_STATUS_1)
	 *  - 0x07 (IT66121_INT_STATUS_2)
	 *  - 0x08 (IT66121_INT_STATUS_3)
	 *  - 0xEE (IT66121_EXT_INT_STATUS_1)
	 *  - 0xF0 (IT66121_EXT_INT_STATUS_2)
	 * For now we process only DDC events of the IT66121_INT_STATUS_1 which
	 * implies proper masks configuration */
	else if (val == true) {
		it666121_int_status_1_reg status_1;

		ret = regmap_read(priv->regmap, IT66121_INT_STATUS_1,
				&status_1.val);
		if (ret < 0) {
			dev_err(dev, "Cannot read IT66121_INT_STATUS_1 (%d)",
					ret);
		} else {
			if (status_1.ddc_fifo_err)
				it66121_clear_ddc_fifo(priv);
			if (status_1.ddc_bus_hang)
				it66121_abort_ddc_ops(priv);
		}

		regmap_field_write(priv->clr_irq, 1);
	}

	if (atomic_read(&priv->state) != RUN)
		return;

	INIT_DELAYED_WORK(&priv->work, &it66121_intr_work);

	queue_delayed_work(priv->work_queue, &priv->work,
			msecs_to_jiffies(IRQ_POLL_INTRVL));
}

static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block,
		size_t len)
{
	int i, ret, remain = len, offset = 0;
	unsigned int rd_fifo_val;
	static u8 header[EDID_LOSS_LEN] = {0x00, 0xFF, 0xFF};

	struct it66121_priv *priv = context;
	struct device *dev = priv->bridge.dev->dev;

	dev_info(dev, "Reading EDID block %d (%zd bytes)", block, len);

	/* Statically fill first 3 bytes (due to EDID reading HW bug) */
	for (i = 0; (i < EDID_LOSS_LEN) && (remain > 0); i++) {
		*(buf++) = header[i];
		remain--;
		offset++;
	}

	while (remain > 0) {
		/* Add bytes that will be lost during EDID read */
		int size = ((remain + EDID_LOSS_LEN) > EDID_FIFO_SIZE) ?
				EDID_FIFO_SIZE : (remain + EDID_LOSS_LEN);

		/* Switch port to PC */
		ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
				IT66121_DDC_MASTER_DDC |
				IT66121_DDC_MASTER_HOST);
		if (ret != 0)
			return ret;

		/* Clear DDC FIFO */
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_FIFO_CLEAR);
		if (ret != 0)
			return ret;

		/* Power up CRCLK */
		regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (1<<3), (1<<3));
		if (ret != 0)
			return ret;

		/* Do abort DDC twice - HW defect */
		for (i = 0; i < 2; i++) {
			ret = regmap_write_bits(priv->regmap, IT66121_DDC_CONTROL, (3<<0), (3<<0));
			if (ret != 0)
				return ret;

			ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
					DDC_CMD_ABORT);
			if (ret != 0)
				return ret;
		}

		/* Clear DDC FIFO */
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_FIFO_CLEAR);
		if (ret != 0)
			return ret;

		/* Start reading */
		ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
				IT66121_DDC_MASTER_DDC |
				IT66121_DDC_MASTER_HOST);
		if (ret != 0)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_ADDRESS,
				EDID_DDC_ADDR);
		if (ret != 0)
			return ret;

		/* Account 3 bytes that will be lost */
		ret = regmap_write(priv->regmap, IT66121_DDC_OFFSET,
				offset - EDID_LOSS_LEN);
		if (ret != 0)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_SIZE, size);
		if (ret != 0)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_SEGMENT, block);
		if (ret != 0)
			return ret;
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_EDID_READ);
		if (ret != 0)
			return ret;

		/* Deduct lost bytes when reading from FIFO */
		size -= EDID_LOSS_LEN;

		for (i = 0; i < size; i++) {
			ret = regmap_read(priv->regmap, IT66121_DDC_RD_FIFO,
					&rd_fifo_val);
			if (ret != 0)
				return ret;

			*(buf++) = rd_fifo_val & 0xFF;
		}

		remain -= size;
		offset += size;
	}

	return 0;
}

static int it66121_connector_get_modes(struct drm_connector *connector)
{
	int ret;
	struct edid *edid;
	struct it66121_priv *priv = container_of(connector, struct it66121_priv,
			connector);

	edid = drm_do_get_edid(connector, it66121_get_edid_block, priv);
	if (edid == NULL)
		return -ENOMEM;

	ret = drm_add_edid_modes(connector, edid);
	if (ret != 0)
		return ret;

	ret = drm_connector_update_edid_property(connector, edid);
	if (ret != 0)
		return ret;

	return 0;
}

static int it66121_connector_detect_ctx(struct drm_connector *connector,
		struct drm_modeset_acquire_ctx *ctx, bool force)
{
	int ret, val;
	struct it66121_priv *priv = container_of(connector, struct it66121_priv,
			connector);

	ret = regmap_field_read(priv->hpd, &val);
	return val == 0 ? connector_status_disconnected :
			connector_status_connected;
}

static struct drm_connector_helper_funcs it66121_connector_helper_funcs = {
	.get_modes = it66121_connector_get_modes,
	.detect_ctx = it66121_connector_detect_ctx,
};

static enum drm_connector_status it66121_connector_detect(
		struct drm_connector *connector, bool force)
{
	return it66121_connector_detect_ctx(connector, NULL, force);
}

static const struct drm_connector_funcs it66121_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = it66121_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};


static int it66121_bind(struct device *comp, struct device *master,
	    void *master_data)
{
	int ret;
	struct drm_bridge *bridge = dev_get_drvdata(comp);
	struct drm_simple_display_pipe *pipe = master_data;

	dev_info(comp, "Binding IT66121 component");
	/* TODO: Do some checks? */

	ret = drm_simple_display_pipe_attach_bridge(pipe, bridge);
	if (ret != 0)
		dev_err(comp, "Cannot attach IT66121 bridge");

	return ret;
}

static void it66121_unbind(struct device *comp, struct device *master,
		void *master_data)
{
	dev_info(comp, "Unbinding IT66121 component");
	/* TODO: Detach? */
}

static struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

static int it66121_bridge_attach(struct drm_bridge *bridge)
{
	int ret;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv,
			bridge);

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found");
		return -ENODEV;
	}

	/* Reset according to IT66121 manual */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RESET, (1<<5), (1<<5));
	msleep(50);

	/* Power up GRCLK & power down IACLK, TxCLK, CRCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (0xf<<3), (7<<3));

	/* Continue according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_INT_CONTROL, (1<<0), (0<<0));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<5), (0<<5));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<2)|(1<<6), (0<<2)|(0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<6), (0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<4), (0<<4));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<3), (1<<3));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<2), (1<<2));

	/* Extra steps for AFE from original driver */
	/* whole register is defined as XP_TEST, values are undisclosed */
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_TEST, 0xFF, 0x70);
	/* lower 5 bits are undisclosed in manual */
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_HS, 0xFF, 0x1F);
	/* DRV_ISW[5:3] value '011' is a default output current level swing,
	 * with change to '111' we set output current level swing to maximum */
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_2, 0xFF, 0x38);

	/* power up IACLK, TxCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (3<<4), (0<<4));

	/* Reset according to IT66121 manual */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RESET, (1<<5), (1<<5));
	msleep(50);

	ret = drm_connector_init(bridge->dev, &priv->connector,
					 &it66121_connector_funcs,
					 DRM_MODE_CONNECTOR_HDMIA);
	if (ret != 0) {
		DRM_ERROR("Cannot initialize bridge connector");
		return ret;
	}

	drm_connector_helper_add(&priv->connector,
			&it66121_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&priv->connector, bridge->encoder);
	if (ret != 0) {
		DRM_ERROR("Cannot attach bridge");
		return ret;
	}

	/* Start interrupts */
	regmap_write_bits(priv->regmap, IT66121_INT_MASK_1,
			IT66121_MASK_DDC_NOACK | IT66121_MASK_DDC_FIFOERR | IT66121_MASK_DDC_BUSHANG,
			~(IT66121_MASK_DDC_NOACK | IT66121_MASK_DDC_FIFOERR | IT66121_MASK_DDC_BUSHANG));
	atomic_set(&priv->state, RUN);
	INIT_DELAYED_WORK(&priv->work, &it66121_intr_work);
	queue_delayed_work(priv->work_queue, &priv->work,
			msecs_to_jiffies(IRQ_POLL_INTRVL));

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
	/* TODO: Enable HW */
	dev_info(bridge->dev->dev, "it66121_bridge_enable");
}

static void it66121_bridge_disable(struct drm_bridge *bridge)
{
	/* TODO: Disable HW */
	dev_info(bridge->dev->dev, "it66121_bridge_disable");
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)

{
	int ret;
	ssize_t frame_size;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv,
			bridge);

	dev_info(bridge->dev->dev, "Setting AVI infoframe for mode: " \
			DRM_MODE_FMT, DRM_MODE_ARG(adjusted_mode));

	hdmi_avi_infoframe_init(&priv->hdmi_avi_infoframe);

	ret = drm_hdmi_avi_infoframe_from_display_mode(&priv->hdmi_avi_infoframe,
			adjusted_mode, false);
	if (ret != 0) {
		dev_err(bridge->dev->dev, "Cannot create AVI infoframe");
		return;
	}

	frame_size = hdmi_avi_infoframe_pack(&priv->hdmi_avi_infoframe,
			priv->hdmi_avi_infoframe_raw, HDMI_INFOFRAME_SIZE(AVI));
	if (frame_size < 0) {
		dev_err(bridge->dev->dev, "Cannot pack AVI infoframe (%ld)",
				frame_size);
		return;
	}

	/* TODO: send raw avi info frame to it66121 */
}

static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.attach = it66121_bridge_attach,
	.detach = it66121_bridge_detach,
	.enable = it66121_bridge_enable,
	.disable = it66121_bridge_disable,
	.mode_set = it66121_bridge_mode_set,
};

static int it66121_probe(struct i2c_client *client)
{
	int ret;
	struct it66121_priv *priv;

	dev_info(&client->dev, "Probing IT66121 client");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (IS_ERR_OR_NULL(priv)) {
		dev_err(&client->dev, "Cannot allocate IT66121 client " \
				"private structure");
		return PTR_ERR(priv);
	}

	priv->hdmi_avi_infoframe_raw = devm_kzalloc(&client->dev,
			HDMI_INFOFRAME_SIZE(AVI), GFP_KERNEL);
	if (IS_ERR_OR_NULL(priv)) {
		dev_err(&client->dev, "Cannot allocate IT66121 AVI " \
				"infoframe buffer");
		return PTR_ERR(priv);
	}

	priv->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(priv->regmap)) {
		return PTR_ERR(priv);
	}

	priv->bridge.funcs = &it66121_bridge_funcs;

	priv->irq_pending = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_SYS_STATUS_irq_pending);
	priv->hpd = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_SYS_STATUS_hpd);
	priv->clr_irq = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_SYS_STATUS_clr_irq);
	priv->ddc_done = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_DDC_STATUS_ddc_done);
	priv->ddc_error = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_DDC_STATUS_ddc_error);

	/* Dont really care which one failed */
	if (IS_ERR(priv->irq_pending) ||
			IS_ERR(priv->hpd) ||
			IS_ERR(priv->clr_irq) ||
			IS_ERR(priv->ddc_done) ||
			IS_ERR(priv->ddc_error)) {
		return -1;
	}

	drm_bridge_add(&priv->bridge);

	/* Setup work queue for interrupt processing work */
	priv->work_queue = create_workqueue("work_queue");
	if (IS_ERR_OR_NULL(priv->work_queue)) {
		dev_err(&client->dev, "Create interrupt workqueue failed");
		drm_bridge_remove(&priv->bridge);
		return PTR_ERR(priv->work_queue);
	}

	/* Important and somewhat unsafe - bridge pointer is in device structure
	 * Ideally, after detecting connection encoder would need to find bridge
	 * using connection's peer device name, but this is not supported yet */
	i2c_set_clientdata(client, &priv->bridge);

	ret = component_add(&client->dev, &it66121_component_ops);
	if (ret != 0) {
		dev_err(&client->dev, "Cannot register IT66121 component");
		destroy_workqueue(priv->work_queue);
		drm_bridge_remove(&priv->bridge);
		return ret;
	}

	return 0;
}

static int it66121_remove(struct i2c_client *client)
{
	struct drm_bridge *bridge = i2c_get_clientdata(client);
	struct it66121_priv *priv;

	if (IS_ERR_OR_NULL(bridge))
		return 0;

	priv = container_of(bridge, struct it66121_priv, bridge);

	if (IS_ERR_OR_NULL(priv))
		return 0;

	atomic_set(&priv->state, STOP);
	drain_workqueue(priv->work_queue);
	destroy_workqueue(priv->work_queue);

	component_del(&client->dev, &it66121_component_ops);

	drm_bridge_remove(bridge);

	i2c_set_clientdata(client, NULL);

	return 0;
}

static int it66121_detect(struct i2c_client *client,
		struct i2c_board_info *info)
{
	int i, ret, address = client->addr;
	struct i2c_adapter *adapter = client->adapter;
	union {
		struct {
			u16 vendor;
			u16 device;
		};
		u8 b[4];
	} id;
	dev_info(&adapter->dev, "Detecting IT66121 at address 0x%X on %s",
			address, adapter->name);

	/* We rely on full I2C protocol + 1 byte SMBUS read for detection */
	ret = i2c_check_functionality(adapter, I2C_FUNC_I2C |
			I2C_FUNC_SMBUS_READ_BYTE);
	if (!ret) {
		dev_info(&adapter->dev, "Adapter does not support I2C " \
				"functions properly");
		return -ENODEV;
	}

	/* No regmap here yet: we will allocate it if detection succeeds */
	for (i = 0; i < 4; i++) {
		ret = i2c_smbus_read_byte_data(client, i);
		if (ret < 0) {
			dev_err(&adapter->dev, "I2C transfer failed (%d)", ret);
			return -ENODEV;
		}
		id.b[i] = ret;
	}

	if ((id.vendor != VENDOR_ID) ||
			((id.device & ~REVISION_MASK) != DEVICE_ID)) {
		dev_dbg(&adapter->dev, "IT66121 not found (0x%X-0x%X)",
				id.vendor, id.device);
		return -ENODEV;
	}

	dev_info(&adapter->dev, "IT66121 found, revision %d",
			(id.device & REVISION_MASK) >> REVISION_SHIFT);

	strlcpy(info->type, "it66121", I2C_NAME_SIZE);
	return 0;
}

static const struct i2c_device_id it66121_i2c_ids[] = {
	{ "it66121", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, it66121_i2c_ids);

static struct i2c_driver it66121_driver = {
	.class = I2C_CLASS_HDMI,
	.driver = {
		.name = "it66121",
		.of_match_table = of_match_ptr(it66121_of_ids),
	},
	.id_table = it66121_i2c_ids,
	.probe_new = it66121_probe,
	.remove = it66121_remove,
	.detect = it66121_detect,
	.address_list = it66121_addr,
};

module_i2c_driver(it66121_driver); /* @suppress("Unused static function")
			@suppress("Unused variable declaration in file scope")
			@suppress("Unused function declaration") */

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("IT66121 HDMI transmitter driver");
MODULE_LICENSE("GPL v2");
