/* SPDX-License-Identifier: GPL-2.0 */
/*
 * it66121_drv.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include <linux/device.h>
#include <linux/module.h>

#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/regmap.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_edid.h>

#include "it66121.h"

#define VENDOR_ID	0x4954
#define DEVICE_ID	0x0612
#define REVISION_MASK	0xF000
#define REVISION_SHIFT	12

#define OFFSET_BITS	8
#define VALUE_BITS	8

/* Custom code for DRM bridge autodetection since there is no DT support */
#define I2C_CLASS_HDMI	(1<<9)

struct it66121_priv {
	struct regmap *regmap;
	struct drm_display_mode curr_mode;
	struct drm_bridge bridge;
	struct drm_connector connector;
	enum drm_connector_status status;
};

static int it66121_remove(struct i2c_client *client);

/* According to datasheet IT66121 addresses are 0x98 or 0x9A, but this is
 * including lsb which is responsible for r/w command - that's why shift */
static const unsigned short it66121_addr[] = {(0x98 >> 1), I2C_CLIENT_END};

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

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.use_single_rw = true,
};

static int it66121_connector_get_modes(struct drm_connector *connector)
{
	/* 1. drm_do_get_edid
	 * 2. drm_add_edid_modes
	 * 3. drm_mode_connector_update_edid_property */
	return 0;
}

static int it66121_connector_detect_ctx(struct drm_connector *connector,
		struct drm_modeset_acquire_ctx *ctx, bool force)
{
	/* TODO: check connected monitor */
	return connector_status_disconnected;
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

	drm_mode_connector_attach_encoder(&priv->connector, bridge->encoder);

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
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL_2, (1<<6), (0<<6));
	regmap_write_bits(priv->regmap, IT66121_INT_CONTROL, (1<<0), (0<<0));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<5), (0<<5));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<2)|(1<<6), (0<<2)|(0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<6), (0<<6));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1<<4), (0<<4));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1<<3), (1<<3));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1<<2), (1<<2));

	/* Extra steps for AFE from original driver */
	/* IT66121_AFE_XP_TEST: 0x30 --> 0x70
	 * whole register is defined as XP_TEST, values are undisclosed */

	/* IT66121_AFE_DRV_HS: 0x00 --> 0x1F
	 * lower 5 bits are undisclosed in manual */

	/* IT66121_AFE_IP_CONTROL_2: 0x18 --> 0x38
	 * DRV_ISW[5:3] value '011' is a default output current level swing,
	 * with change to '111' we set output current level swing to maximum */
}

static void it66121_bridge_disable(struct drm_bridge *bridge)
{
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge,
		struct drm_display_mode *mode,
		struct drm_display_mode 	*adjusted_mode)

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

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
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
			dev_err(&adapter->dev, "I2C transfer failed (%d)", ret);
			return -ENODEV;
		}
		id.b[i] = ret;
	}

	if ((id.vendor != VENDOR_ID) ||
			((id.device & ~REVISION_MASK) != DEVICE_ID)) {
		dev_err(&adapter->dev, "IT66121 not found (0x%X-0x%X)",
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
