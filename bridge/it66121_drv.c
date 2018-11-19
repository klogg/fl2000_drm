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
#include <linux/regmap.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>

#define VENDOR_ID	0x4954
#define DEVICE_ID	0x0612
#define REVISION_MASK	0xF000
#define REVISION_SHIFT	12

#define OFFSET_BITS	8
#define VALUE_BITS	8

static char it66121_name[] = "it66121";

/* Custom code for DRM bridge autodetection since there is no DT support */
#define I2C_CLASS_HDMI	(1<<9)
#define CONNECTION_SIZE	64
static inline int drm_i2c_bridge_connection_id(char *connection_id,
		struct i2c_adapter *adapter)
{
	return snprintf(connection_id, CONNECTION_SIZE, "%s-bridge",
			adapter->name);
}

struct it66121_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_display_mode curr_mode;
	struct drm_bridge bridge;
	struct drm_connector connector;
	enum drm_connector_status status;
	char connection_id[CONNECTION_SIZE];
	struct device_connection connection;
};

static int it66121_remove(struct i2c_client *client);

/* According to datasheet IT66121 addresses are 0x98 or 0x9A, but this is
 * including lsb which is responsible for r/w command - that's why shift */
static const unsigned short it66121_addr[] = {(0x98 >> 1), (0x9A >> 1),
		I2C_CLIENT_END};

#if 0
static const struct regmap_config it66121_regmap_config = {
	.reg_bits = OFFSET_BITS,
	.val_bits = VALUE_BITS,

	.max_register = 0xFF,
	.cache_type = REGCACHE_RBTREE,
	.reg_defaults_raw = adv7511_register_defaults,
	.num_reg_defaults_raw = ARRAY_SIZE(adv7511_register_defaults),

	.volatile_reg = adv7511_register_volatile,
};
#endif

static int it66121_connector_get_modes(struct drm_connector *connector)
{
	return 0;
}

static struct drm_connector_helper_funcs it66121_connector_helper_funcs = {
	.get_modes = it66121_connector_get_modes,
};

static enum drm_connector_status it66121_connector_detect(
		struct drm_connector *connector, bool force)
{
	return connector_status_disconnected;
}

static const struct drm_connector_funcs it66121_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = it66121_connector_detect,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
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
}

static void it66121_bridge_enable(struct drm_bridge *bridge)
{
}

static void it66121_bridge_disable(struct drm_bridge *bridge)
{
}

static void it66121_bridge_mode_set(struct drm_bridge *bridge,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
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
	if (IS_ERR(priv)) {
		dev_err(&client->dev, "Cannot allocate IT66121 client private" \
				"structure");
		ret = PTR_ERR(priv);
		goto error;
	}

	priv->client = client;
	priv->regmap = NULL;
	priv->bridge.funcs = &it66121_bridge_funcs;

	drm_bridge_add(&priv->bridge);

	i2c_set_clientdata(client, priv);

	/* Set up connection between I2C endpoints of encoder and bridge
	 * NOTE: only one DRM bridge on DPI, so only one device on I2C bus */
	priv->connection.endpoint[0] = dev_name(&client->adapter->dev);
	priv->connection.endpoint[1] = dev_name(&client->dev);
	priv->connection.id = priv->connection_id;

	/* Calculate connection ID for I2C DRM encoder */
	drm_i2c_bridge_connection_id(priv->connection_id, client->adapter);

	device_connection_add(&priv->connection);

	return 0;

error:
	it66121_remove(client);
	return ret;
}

static int it66121_remove(struct i2c_client *client)
{
	struct it66121_priv *priv = i2c_get_clientdata(client);

	if (priv == NULL)
		return 0;

	device_connection_remove(&priv->connection);

	drm_bridge_remove(&priv->bridge);

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

	/* TODO: change to register map */
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

	strlcpy(info->type, it66121_name, I2C_NAME_SIZE);
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
		.name = it66121_name,
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
