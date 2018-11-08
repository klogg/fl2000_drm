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

#define OFFSET_BITS	8
#define VALUE_BITS	8

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
	.driver = {
		.name = "it66121",
		.of_match_table = of_match_ptr(it66121_of_ids),
	},
	.id_table = it66121_i2c_ids,
	.probe = it66121_probe,
	.remove = it66121_remove,
};

module_i2c_driver(it66121_driver); /* @suppress("Unused static function")
			@suppress("Unused variable declaration in file scope")
			@suppress("Unused function declaration") */

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("IT66121 HDMI transmitter driver");
MODULE_LICENSE("GPL v2");
