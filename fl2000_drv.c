/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drv.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME			"fl2000_usb"

#define USB_CLASS_AV 			0x10
#define USB_SUBCLASS_AV_CONTROL		0x01
#define USB_SUBCLASS_AV_VIDEO		0x02
#define USB_SUBCLASS_AV_AUDIO		0x03

#define USB_VENDOR_ID_FRESCO_LOGIC	0x1D5C
#define USB_PRODUCT_ID_FL2000		0x2000

enum {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
	FL2000_USBIFS = 3
};

/* Interrupt polling task */
int fl2000_intr_create(struct usb_interface *interface);
void fl2000_intr_destroy(struct usb_interface *interface);

/* Stream transfer task */
int fl2000_stream_create(struct usb_interface *interface);
void fl2000_stream_destroy(struct usb_interface *interface);

/* I2C adapter interface creation */
int fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct usb_device *usb_dev);

/* Register map creation */
int fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct usb_device *usb_dev);

/* DRM device creation */
int fl2000_drm_init(struct usb_device *usb_dev);
void fl2000_drm_cleanup(struct usb_device *usb_dev);

/* Ordered list of "init"/"cleanup functions for sub-devices. Shall be processed
 * forward order on init and backward on cleanup*/
static const struct {
	const char *name;
	int (*init_fn)(struct usb_device *usb_dev);
	void (*cleanup_fn)(struct usb_device *usb_dev);
} fl2000_devices[] = {
	{"registers map", fl2000_regmap_init, fl2000_regmap_cleanup},
	{"I2C adapter", fl2000_i2c_init, fl2000_i2c_cleanup},
	{"DRM device", fl2000_drm_init, fl2000_drm_cleanup}
};

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_ID_FRESCO_LOGIC, \
		USB_PRODUCT_ID_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

struct fl2000_drv {
	int usbifs_up;
};

static void fl2000_drv_release(struct device *dev, void *res)
{
	/* Noop */
}

static int fl2000_probe(struct usb_interface *interface,
		const struct usb_device_id *usb_dev_id)
{
	int i, ret;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_drv *drv = devres_find(&usb_dev->dev,
				fl2000_drv_release, NULL, NULL);

	/* Create local data structure if it does not exist yet */
	if (!drv) {
		drv = devres_alloc(fl2000_drv_release, sizeof(*drv),
				GFP_KERNEL);
		if (!drv) {
			dev_err(&usb_dev->dev, "USB data allocation failed");
			return -ENOMEM;
		}
		devres_add(&usb_dev->dev, drv);
		drv->usbifs_up = 0;
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		ret = 0; /* Do nothing */
		break;

	case FL2000_USBIF_STREAMING:
		ret = fl2000_stream_create(interface);
		break;

	case FL2000_USBIF_INTERRUPT:
		ret = fl2000_intr_create(interface);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		ret = -ENODEV;
		break;
	}
	if (ret != 0)
		return ret;

	/* Start everything when all interfaces are up */
	if (++drv->usbifs_up == FL2000_USBIFS) {
		for (i = 0; i < ARRAY_SIZE(fl2000_devices); i++) {
			ret = (*fl2000_devices[i].init_fn)(usb_dev);
			if (ret) {
				/* TODO: Cleanup what was init before error */
				dev_err(&usb_dev->dev, "Cannot create %s (%d)",
						fl2000_devices[i].name, ret);
			}
		}
	}

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	int i;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_drv *drv = devres_find(&usb_dev->dev,
				fl2000_drv_release, NULL, NULL);

	/* De-initialize everything while all interfaces are still up */
	if (drv) {
		for (i = ARRAY_SIZE(fl2000_devices) - 1; i >= 0; i--)
			(*fl2000_devices[i].cleanup_fn)(usb_dev);

		devres_release(&usb_dev->dev, fl2000_drv_release, NULL, NULL);
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		/* Do nothing */
		break;

	case FL2000_USBIF_STREAMING:
		fl2000_stream_destroy(interface);
		break;

	case FL2000_USBIF_INTERRUPT:
		fl2000_intr_destroy(interface);
		break;

	default:
		/* Device does not have any other interfaces */
		break;
	}
}

static int fl2000_suspend(struct usb_interface *interface,
			   pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "resume");

	/* TODO: suspend
	 * drm_mode_config_helper_suspend() */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_dbg(&usb_dev->dev, "suspend");

	/* TODO: resume
	 * drm_mode_config_helper_resume() */

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
