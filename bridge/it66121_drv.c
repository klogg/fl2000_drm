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

	struct work_struct work;
	struct workqueue_struct *work_queue;
	atomic_t state;

	struct regmap_field *sys_status_int;
	struct regmap_field *sys_status_hpd;
	struct regmap_field *ddc_status_done;
	struct regmap_field *ddc_status_error;
};

static int it66121_remove(struct i2c_client *client);

/* According to datasheet IT66121 addresses are 0x98 or 0x9A including cmd */
static const unsigned short it66121_addr[] = {(0x98 >> 1), (0x9A >> 1),
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
	.max_register = 0xFF,

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
	int res, val;

	res = regmap_field_read_poll_timeout(priv->ddc_status_done, val, true,
			EDID_SLEEP, EDID_TIMEOUT);
	if (res != 0)
		return res;

	res = regmap_field_read(priv->ddc_status_error, &val);
	if (res != 0)
		return res;
	if (val != 0)
		return -EIO;

	return 0;
}

static int it66121_clear_ddc_fifo(struct it66121_priv *priv)
{
	int res;
	unsigned int val;

	res = regmap_read(priv->regmap, IT66121_DDC_CONTROL, &val);
	if (res != 0)
		return res;

	res = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
			IT66121_DDC_MASTER_DDC |
			IT66121_DDC_MASTER_HOST);
	if (res != 0)
		return res;

	res = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
			DDC_CMD_FIFO_CLEAR);
	if (res != 0)
		return res;

	res = regmap_write(priv->regmap, IT66121_DDC_CONTROL, val);
	if (res != 0)
		return res;

	return 0;
}

static int it66121_abort_ddc_ops(struct it66121_priv *priv)
{
	int i, res;
	unsigned int val;

	/* XXX: Prior to DDC abort command there was also a reset of HDCP:
	 *  1. HDCP_DESIRE clear bit CP DESIRE
	 *  2. SW_RST set bit HDCP_RST
	 * it seems wrong to keep them reset, i.e. without restoring initial
	 * state, but somehow this is how it was implemented. This sequence is
	 * removed since HDCP is not supported */

	res = regmap_read(priv->regmap, IT66121_DDC_CONTROL, &val);
	if (res != 0)
		return res;

	res = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
			IT66121_DDC_MASTER_DDC |
			IT66121_DDC_MASTER_HOST);
	if (res != 0)
		return res;

	/* According to 2009/01/15 modification by Jau-Chih.Tseng@ite.com.tw
	 * do abort DDC twice */
	for (i = 0; i < 2; i++) {
		res = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_ABORT);
		if (res != 0)
			return res;

		res = it66121_wait_ddc_ready(priv);
		if (res != 0)
			return res;
	}

	res = regmap_write(priv->regmap, IT66121_DDC_CONTROL, val);
	if (res != 0)
		return res;

	return 0;
}

static void it66121_intr_work(struct work_struct *work_item)
{
	int res;
	unsigned int val;
	struct it66121_priv *priv = container_of(work_item, struct it66121_priv,
			work);

	/* TODO: use mutex */

	res = regmap_field_read(priv->sys_status_int, &val);
	if (res < 0)
		/* TODO: Signal error here somehow */
		return;

	/* XXX: There are at least 5 registers that can source interrupt:
	 *  - 0x06 (IT66121_INT_STATUS_1)
	 *  - 0x07 (IT66121_INT_STATUS_2)
	 *  - 0x08 (IT66121_INT_STATUS_3)
	 *  - 0xEE (IT66121_EXT_INT_STATUS_1)
	 *  - 0xF0 (IT66121_EXT_INT_STATUS_2)
	 * For now we process only DDC events of the IT66121_INT_STATUS_1 which
	 * implies proper masks configuration */
	if (val == true) {
		it666121_int_status_1_reg status_1;
		res = regmap_read(priv->regmap, IT66121_INT_STATUS_1,
				&status_1.val);
		if (res < 0)
			/* TODO: Signal error here somehow */
			return;
		if (status_1.ddc_fifo_err)
			it66121_clear_ddc_fifo(priv);
		if (status_1.ddc_bus_hang)
			it66121_abort_ddc_ops(priv);
	}

	if (atomic_read(&priv->state) != RUN)
		return;

	INIT_WORK(&priv->work, &it66121_intr_work);

	queue_work(priv->work_queue, &priv->work);
}

static int it66121_get_edid_block(void *data, u8 *buf, unsigned int block,
		size_t len)
{
	int res, val, offset = 0, remain = len;

	struct it66121_priv *priv = data;

	/* XXX: Probably we may need to
	 *  - abort running DDC operation
	 *  - clear DDC FIFO */

	do {
		int i;
		int size = (remain > EDID_FIFO_SIZE) ? EDID_FIFO_SIZE : remain;

		/* Set PC master */
		res = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
				IT66121_DDC_MASTER_DDC |
				IT66121_DDC_MASTER_HOST);
		if (res != 0)
			return res;

		/* Clear DDC FIFO */
		res = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_FIFO_CLEAR);
		if (res != 0)
			return res;

		res = it66121_wait_ddc_ready(priv);
		if (res != 0)
			return res;

		res = regmap_write(priv->regmap, IT66121_DDC_ADDRESS,
				EDID_DDC_ADDR);
		if (res != 0)
			return res;
		res = regmap_write(priv->regmap, IT66121_DDC_OFFSET, offset);
		if (res != 0)
			return res;
		res = regmap_write(priv->regmap, IT66121_DDC_SIZE, size);
		if (res != 0)
			return res;
		res = regmap_write(priv->regmap, IT66121_DDC_SEGMENT, block);
		if (res != 0)
			return res;
		res = regmap_write(priv->regmap, IT66121_DDC_COMMAND,
				DDC_CMD_EDID_READ);
		if (res != 0)
			return res;

		res = it66121_wait_ddc_ready(priv);
		if (res != 0)
			return res;

		for (i = 0; i < size; i++) {
			res = regmap_read(priv->regmap, IT66121_DDC_RD_FIFO,
					&val);
			if (res != 0)
				return res;

			*(buf++) = val & 0xFF;
		}

		remain -= size;
		offset += size;
	} while (remain > 0);

	return 0;
}


static int it66121_connector_get_modes(struct drm_connector *connector)
{
	int res;
	struct edid *edid;
	struct it66121_priv *priv = container_of(connector, struct it66121_priv,
			connector);

	edid = drm_do_get_edid(connector, it66121_get_edid_block, priv);
	if (edid == NULL)
		return -ENOMEM;

	res = drm_add_edid_modes(connector, edid);
	if (res != 0)
		return res;

	res = drm_connector_update_edid_property(connector, edid);
	if (res != 0)
		return res;

	return 0;
}

static int it66121_connector_detect_ctx(struct drm_connector *connector,
		struct drm_modeset_acquire_ctx *ctx, bool force)
{
	int res, val;
	struct it66121_priv *priv = container_of(connector, struct it66121_priv,
			connector);

	res = regmap_field_read(priv->sys_status_hpd, &val);
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

	ret = drm_connector_init(bridge->dev, &priv->connector,
					 &it66121_connector_funcs,
					 DRM_MODE_CONNECTOR_HDMIA);
	if (ret != 0) return ret;

	drm_connector_helper_add(&priv->connector,
			&it66121_connector_helper_funcs);

	drm_connector_attach_encoder(&priv->connector, bridge->encoder);

	return 0;
}

static void it66121_bridge_detach(struct drm_bridge *bridge)
{
	/* TODO: Detach encoder */
}

static void it66121_bridge_enable(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv,
			bridge);

	/* Reset according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SW_RESET, (1<<5), (1<<5));

	/* Power up according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (1<<6), (0<<6));
	regmap_write_bits(priv->regmap, IT66121_INT_CONTROL, (1<<0), (0<<0));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<5), (0<<5));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<2)|(1<<6), (0<<2)|(0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<6), (0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<4), (0<<4));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<3), (1<<3));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<2), (1<<2));

	/* Extra steps for AFE from original driver */
	/* whole register is defined as XP_TEST, values are undisclosed */
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_TEST, (1<<2), (1<<2));
	/* lower 5 bits are undisclosed in manual */
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_HS, 0x1F, 0x1F);
	/* DRV_ISW[5:3] value '011' is a default output current level swing,
	 * with change to '111' we set output current level swing to maximum */
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_2, (1<<5), (1<<5));
}

static void it66121_bridge_disable(struct drm_bridge *bridge)
{
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)

{
	/*
	 * hdmi_avi_infoframe_init()
	 * drm_hdmi_avi_infoframe_from_display_mode()
	 * */

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
		dev_err(&client->dev, "Cannot allocate IT66121 client private" \
				"structure");
		ret = PTR_ERR(priv);
		goto error;
	}

	priv->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv);
		goto error;
	}

	priv->bridge.funcs = &it66121_bridge_funcs;

	priv->sys_status_int = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_SYS_STATUS_int);
	if (IS_ERR(priv->sys_status_int)) {
		ret = PTR_ERR(priv->sys_status_int);
		goto error;
	}

	priv->sys_status_hpd = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_SYS_STATUS_hpd);
	if (IS_ERR(priv->sys_status_hpd)) {
		ret = PTR_ERR(priv->sys_status_hpd);
		goto error;
	}

	priv->ddc_status_done = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_DDC_STATUS_done);
	if (IS_ERR(priv->ddc_status_done)) {
		ret = PTR_ERR(priv->ddc_status_done);
		goto error;
	}

	priv->ddc_status_error = devm_regmap_field_alloc(&client->dev,
			priv->regmap, IT66121_DDC_STATUS_error);
	if (IS_ERR(priv->ddc_status_error)) {
		ret = PTR_ERR(priv->ddc_status_error);
		goto error;
	}

	drm_bridge_add(&priv->bridge);

	/* Important and somewhat unsafe - bridge pointer is in device structure
	 * Ideally, after detecting connection encoder would need to find bridge
	 * using connection's peer device name, but this is not supported yet */
	i2c_set_clientdata(client, &priv->bridge);

	ret = component_add(&client->dev, &it66121_component_ops);
	if (ret != 0) {
		dev_err(&client->dev, "Cannot register IT66121 component");
		goto error;
	}

	/* Setup work queue for interrupt processing work */
	priv->work_queue = create_workqueue("work_queue");
	if (IS_ERR_OR_NULL(priv->work_queue)) {
		dev_err(&client->dev, "Create interrupt workqueue failed");
		ret = PTR_ERR(priv->work_queue);
		goto error;
	}

	it66121_intr_work(&priv->work);

	return 0;

error:
	it66121_remove(client);
	return ret;
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

	/*TODO: Cancel pending work and destroy workqueue */

	component_del(&client->dev, &it66121_component_ops);

	drm_bridge_remove(bridge);

	i2c_set_clientdata(client, NULL);
	kfree(priv);

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

	/* TODO: i2c_check_functionality()? */

	/* No regmap here yet: we will allocate it if detection succeeds */
	for (i = 0; i < 4; i++) {
		ret = i2c_smbus_read_byte_data(client, i);
		if (ret < 0) {
			dev_dbg(&adapter->dev, "I2C transfer failed (%d)", ret);
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
