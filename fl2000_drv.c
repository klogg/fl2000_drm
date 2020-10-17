// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME "fl2000_usb"

#define USB_CLASS_AV		0x10
#define USB_SUBCLASS_AV_CONTROL 0x01
#define USB_SUBCLASS_AV_VIDEO	0x02
#define USB_SUBCLASS_AV_AUDIO	0x03

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

/* Ordered list of "init"/"cleanup functions for sub-devices. Shall be processed forward order on
 * init and backward on cleanup
 */
static const struct {
	const char *name;
	int (*init_fn)(struct usb_device *usb_dev);
	void (*cleanup_fn)(struct usb_device *usb_dev);
} fl2000_devices[] = { { "registers map", fl2000_regmap_init, fl2000_regmap_cleanup },
		       { "I2C adapter", fl2000_i2c_init, fl2000_i2c_cleanup },
		       { "DRM device", fl2000_drm_init, fl2000_drm_cleanup } };

static void fl2000_destroy_devices(struct usb_device *usb_dev)
{
	int i;

	for (i = ARRAY_SIZE(fl2000_devices); i > 0; i--)
		(*fl2000_devices[i - 1].cleanup_fn)(usb_dev);
}

static int fl2000_create_devices(struct usb_device *usb_dev)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(fl2000_devices); i++) {
		ret = (*fl2000_devices[i].init_fn)(usb_dev);
		if (ret) {
			fl2000_destroy_devices(usb_dev);
			break;
		}
	}

	return ret;
}

static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	int ret;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&interface->dev, "USB 1.1 is not supported!");
		return -ENODEV;
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		/* Do nothing */;
		ret = 0;
		break;

	case FL2000_USBIF_STREAMING:
		ret = fl2000_stream_create(interface);
		if (ret)
			break;
		ret = fl2000_create_devices(usb_dev);
		break;

	case FL2000_USBIF_INTERRUPT:
		ret = fl2000_intr_create(interface);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		ret = -ENODEV;
		break;
	}

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		/* Do nothing */;
		break;

	case FL2000_USBIF_STREAMING:
		fl2000_destroy_devices(usb_dev);
		fl2000_stream_destroy(interface);
		break;

	case FL2000_USBIF_INTERRUPT:
		fl2000_intr_destroy(interface);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		break;
	}
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	dev_dbg(&usb_dev->dev, "resume");

	/* TODO: suspend
	 * drm_mode_config_helper_suspend()
	 */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	dev_dbg(&usb_dev->dev, "suspend");

	/* TODO: resume
	 * drm_mode_config_helper_resume()
	 */

	return 0;
}

static struct usb_driver fl2000_driver = {
	.name = USB_DRIVER_NAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.id_table = fl2000_id_table,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
