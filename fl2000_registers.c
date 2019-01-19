/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_registers.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define CONTROL_MSG_LEN		4
#define CONTROL_MSG_READ	64
#define CONTROL_MSG_WRITE	65

/* Timeout in ms for USB Control Message (transport for I2C bus)  */
#define CONTROL_XFER_TIMEOUT	2000

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	int ret;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;
	u32 *data = kmalloc(sizeof(*data), GFP_KERNEL);

	BUG_ON(data == NULL);

	ret = usb_control_msg(
		usb_dev,
		usb_rcvctrlpipe(usb_dev, 0),
		CONTROL_MSG_READ,
		(USB_DIR_IN | USB_TYPE_VENDOR),
		0,
		offset,
		data,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN) ret = -1;
		else ret = 0;
	}

	*val = *data;

	kfree(data);
	return ret;
}

static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	int ret;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;
	u32 *data = kmalloc(sizeof(*data), GFP_KERNEL);

	BUG_ON(data == NULL);

	*data = val;

	ret = usb_control_msg(
		usb_dev,
		usb_sndctrlpipe(usb_dev, 0),
		CONTROL_MSG_WRITE,
		(USB_DIR_OUT | USB_TYPE_VENDOR),
		0,
		offset,
		data,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN) ret = -1;
		else ret = 0;
	}

	kfree(data);
	return ret;
}

static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = 0xFFFF,

	.cache_type = REGCACHE_RBTREE,

	.precious_reg = fl2000_reg_precious,
	.volatile_reg = fl2000_reg_volatile,

	.reg_defaults = fl2000_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(fl2000_reg_defaults),

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.reg_read = fl2000_reg_read,
	.reg_write = fl2000_reg_write,

	.use_single_rw = true,
};

int fl2000_regmap_create(struct usb_device *usb_dev)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&usb_dev->dev, NULL, usb_dev,
			&fl2000_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return 0;
}
