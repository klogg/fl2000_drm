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

/* Custom class for DRM bridge autodetection */
#define I2C_CLASS_HDMI	(1<<9)

#define VENDOR_ID	0x4954
#define DEVICE_ID	0x0612
#define REVISION_MASK	0xF000

#define OFFSET_BITS	8
#define VALUE_BITS	8

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

#define ADDR_MSG	0
#define DATA_MSG	1

int it66121_detect(struct i2c_client *client, struct i2c_board_info *info)
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
		dev_err(&adapter->dev, " ...not found (0x%X-0x%X)",
				id.vendor, id.device);
		return -ENODEV;
	}

	dev_info(&client->dev, " ...found, revision %d",
			id.device & REVISION_MASK);

	strlcpy(info->type, "it66121", I2C_NAME_SIZE);
	return 0;
}

int it66121_probe(struct i2c_client *client)
{
	dev_info(&client->dev, "Probing IT66121 client");
	return 0;
}

int it66121_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "Removed IT66121 client");
	return 0;
}


#ifdef CONFIG_OF
static const struct of_device_id it66121_of_ids[] = {
	{ .compatible = "ite,it66121", },
	{ }
};
MODULE_DEVICE_TABLE(of, it66121_of_ids);
#endif /* CONFIG_OF */

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
