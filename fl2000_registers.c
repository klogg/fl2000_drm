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

struct fl2000_reg {
	/* TODO: Use registers map here? */
	u32 value;
};

int fl2000_reg_read(struct usb_device *usb_dev, u32 *data, u16 offset)
{
	int ret;
	struct fl2000_reg *reg = dev_get_drvdata(&usb_dev->dev);

	ret = usb_control_msg(
		usb_dev,
		usb_rcvctrlpipe(usb_dev, 0),
		CONTROL_MSG_READ,
		(USB_DIR_IN | USB_TYPE_VENDOR),
		0,
		offset,
		&reg->value,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN) ret = -1;
		else ret = 0;
	}

	*data = reg->value;

	return ret;
}

int fl2000_reg_write(struct usb_device *usb_dev, u32 *data, u16 offset)
{
	int ret;
	struct fl2000_reg *reg = dev_get_drvdata(&usb_dev->dev);

	reg->value = *data;

	ret = usb_control_msg(
		usb_dev,
		usb_sndctrlpipe(usb_dev, 0),
		CONTROL_MSG_WRITE,
		(USB_DIR_OUT | USB_TYPE_VENDOR),
		0,
		offset,
		&reg->value,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN) ret = -1;
		else ret = 0;
	}
	return ret;
}

int fl2000_reg_create(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg *reg;
	u32 magic;

	reg = kzalloc(sizeof(*reg), GFP_KERNEL);
	if (IS_ERR(reg)) {
		dev_err(&usb_dev->dev, "Cannot allocate registers private" \
				"structure");
		ret = PTR_ERR(reg);
		goto error;
	}

	dev_set_drvdata(&usb_dev->dev, reg);

	/* U1 U2 magic from original driver */
	ret = fl2000_reg_read(usb_dev, &magic, FL2000_REG_0070);
	if (ret != 0) goto error;
	magic |= (1<<20);
	ret = fl2000_reg_write(usb_dev, &magic, FL2000_REG_0070);
	if (ret != 0) goto error;

	ret = fl2000_reg_read(usb_dev, &magic, FL2000_REG_0070);
	if (ret != 0) goto error;
	magic |= (1<<19);
	ret = fl2000_reg_write(usb_dev, &magic, FL2000_REG_0070);
	if (ret != 0) goto error;


	return 0;
error:
	/* Enforce cleanup in case of error */
	fl2000_reg_destroy(usb_dev);
	return ret;
}

void fl2000_reg_destroy(struct usb_device *usb_dev)
{
	struct fl2000_reg *reg = dev_get_drvdata(&usb_dev->dev);
	if (reg != NULL)
		kfree(reg);
}
