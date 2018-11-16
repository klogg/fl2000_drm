/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_module.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME			"fl2000_usb"

#define USB_CLASS_AV 			0x10
#define USB_SUBCLASS_AV_CONTROL		0x01
#define USB_SUBCLASS_AV_VIDEO		0x02
#define USB_SUBCLASS_AV_AUDIO		0x03

#define USB_VENDOR_ID_FRESCO_LOGIC	0x1D5C
#define USB_PRODUCT_ID_FL2000		0x2000

#define FL2000_USBIF_AVCONTROL		0
#define FL2000_USBIF_STREAMING		1
#define FL2000_USBIF_INTERRUPT		2

static unsigned long fl2000_init_state = 0;
#define REGS_DONE			(1ul<<0)
#define DRM_DONE			(1ul<<1)
#define I2C_DONE			(1ul<<2)
#define INTR_DONE			(1ul<<3)

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_ID_FRESCO_LOGIC, \
		USB_PRODUCT_ID_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

#if 0

u32 magic;
fl2000_bus_control_reg control;

/* Application reset & self cleanup */
ret = fl2000_reg_read(usb_dev, &magic, FL2000_REG_8048);
if (ret != 0) goto error;
magic |= (1<<15);
ret = fl2000_reg_write(usb_dev, &magic, FL2000_REG_8048);
if (ret != 0) goto error;

/* Turn off HW reset */
ret = fl2000_reg_read(usb_dev, &magic, FL2000_REG_8088);
if (ret != 0) goto error;
magic |= (1<<10);
ret = fl2000_reg_write(usb_dev, &magic, FL2000_REG_8088);
if (ret != 0) goto error;

/* TODO: sort out this U1/U2 magic from original driver. Could it be
 * some sort of GPIO management? */
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

/* Enable I2C connection */
ret = fl2000_reg_read(usb_dev, &control.w, FL2000_REG_BUS_CTRL);
if (ret != 0) goto error;
control.flags |= FL2000_CONECTION_ENABLE;
ret = fl2000_reg_write(usb_dev, &control.w, FL2000_REG_BUS_CTRL);
if (ret != 0) goto error;
msleep(I2C_ENABLE_TIMEOUT);

/* Enable monitor detection (not sure if it is needed) */
ret = fl2000_reg_read(usb_dev, &control.w, FL2000_REG_BUS_CTRL);
if (ret != 0) goto error;
control.flags |= FL2000_DETECT_MONITOR;
ret = fl2000_reg_write(usb_dev, &control.w, FL2000_REG_BUS_CTRL);
if (ret != 0) goto error;


#endif

static int fl2000_probe(struct usb_interface *interface,
		const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* Enable register access before first interface set */
	if (fl2000_init_state == 0) {
		ret = fl2000_reg_create(usb_dev);
		if (ret != 0) goto error;
		fl2000_init_state |= REGS_DONE;
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		dev_info(&usb_dev->dev, "Probing AVControl interface (%u)",
				iface_num);

		/* This is rather useless, AVControl is not properly implemented
		 * on FL2000 chip - that is why all the "magic" needed */

		/* TODO: Move I2C initialization to DRM device */
		ret = fl2000_i2c_create(interface);
		if (ret != 0) goto error;
		fl2000_init_state |= I2C_DONE;
		break;

	case FL2000_USBIF_STREAMING:
		dev_info(&usb_dev->dev, "Probing Streaming interface (%u)",
				iface_num);
		ret = fl2000_drm_create(interface);
		if (ret != 0) goto error;
		fl2000_init_state |= DRM_DONE;
		break;

	case FL2000_USBIF_INTERRUPT:
		dev_info(&usb_dev->dev, "Probing Interrupt interface (%u)",
				iface_num);

		ret = fl2000_intr_create(interface);
		if (ret != 0) goto error;
		fl2000_init_state |= INTR_DONE;
break;

	default:
		/* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?",
				interface->cur_altsetting->desc.iInterface);
		ret = -ENODEV;
		break;
	}

error:
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_info(&usb_dev->dev, "Disconnecting interface: %u", iface_num);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		fl2000_i2c_destroy(interface);
		fl2000_init_state &= ~I2C_DONE;
		break;

	case FL2000_USBIF_STREAMING:
		fl2000_drm_destroy(interface);
		fl2000_init_state &= ~DRM_DONE;
		break;

	case FL2000_USBIF_INTERRUPT:
		fl2000_intr_destroy(interface);
		fl2000_init_state &= ~INTR_DONE;
		break;

	default:
		/* Device does not have any other interfaces */
		break;
	}

	/* Disable register access after last interface cleared */
	if (fl2000_init_state == REGS_DONE){
		fl2000_reg_destroy(usb_dev);
		fl2000_init_state &= ~REGS_DONE;
	}

}

static int fl2000_suspend(struct usb_interface *interface,
			   pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "resume");

	/* TODO: suspend */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "suspend");

	/* TODO: resume */

	return 0;
}

static struct usb_driver fl2000_driver = {
	.name 		= USB_DRIVER_NAME,
	.probe 		= fl2000_probe,
	.disconnect 	= fl2000_disconnect,
	.suspend	= fl2000_suspend,
	.resume		= fl2000_resume,
	.id_table 	= fl2000_id_table,
};

module_usb_driver(fl2000_driver); /* @suppress("Unused static function")
			@suppress("Unused variable declaration in file scope")
			@suppress("Unused function declaration") */

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
