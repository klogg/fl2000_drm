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

static const struct reg_default fl2000_reg_defaults[] = {
	regmap_reg_default(FL2000_VGA_STATUS_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_PXCLK,		0x0010119C),
	regmap_reg_default(FL2000_VGA_HSYNC_REG1,		0x02800320),
	regmap_reg_default(FL2000_VGA_HSYNC_REG2,		0x00600089),
	regmap_reg_default(FL2000_VGA_VSYNC_REG1,		0x01E0020D),
	regmap_reg_default(FL2000_VGA_VSYNC_REG2,		0x0002001C),
	regmap_reg_default(FL2000_VGA_TEST_REG,			0x00000006),
	regmap_reg_default(FL2000_VGA_ISOCH_REG,		0x00850000),
	regmap_reg_default(FL2000_VGA_I2C_SC_REG,		0x80000000),
	regmap_reg_default(FL2000_VGA_I2C_RD_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_I2C_WR_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLL_REG,			0x003F6119),
	regmap_reg_default(FL2000_VGA_LBUF_REG,			0x23300001),
	regmap_reg_default(FL2000_VGA_HI_MARK,			0x00000000),
	regmap_reg_default(FL2000_VGA_LO_MARK,			0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_ACLK,		0x00000000),
	regmap_reg_default(FL2000_VGA_PXCLK_CNT_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_VCNT_REG,			0x00000000),
	regmap_reg_default(FL2000_RST_CTRL_REG,			0x00000100),
	regmap_reg_default(FL2000_BIAC_CTRL1_REG,		0x00A00120),
	regmap_reg_default(FL2000_BIAC_CTRL2_REG,		0x00000000),
	regmap_reg_default(FL2000_BIAC_STATUS_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLT_REG_PXCLK,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLT_RADDR_REG_PXCLK,	0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL2_REG_ACLK,		0x00000000),
	regmap_reg_default(FL2000_TEST_CNTL_REG1,		0xC0003C20),
	regmap_reg_default(FL2000_TEST_CNTL_REG1,		0x00000C04),
	regmap_reg_default(FL2000_TEST_CNTL_REG3,		0x00000000),
	regmap_reg_default(FL2000_TEST_STAT1,			0x00000000),
	regmap_reg_default(FL2000_TEST_STAT2,			0x00000000),
	regmap_reg_default(FL2000_TEST_STAT3,			0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_3,		0x00000488),
};

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
