// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME "fl2000_usb"

#define FL2000_USB_VENDOR  0x1D5C
#define FL2000_USB_PRODUCT 0x2000
#define FL2000_USB_INTERFACE(ifnum, api_addr)                                              \
	{                                                                                  \
		USB_DEVICE_INTERFACE_NUMBER(FL2000_USB_VENDOR, FL2000_USB_PRODUCT, ifnum), \
			.driver_info = (kernel_ulong_t)(api_addr)                          \
	}

struct fl2000_if_api {
	int (*create)(struct usb_interface *interface);
	void (*destroy)(struct usb_interface *interface);
};

static int fl2000_avcontrol_create(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	int ret;

	/* This seem to be needed to workaround buggy implementation of EPs */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&interface->dev, "Cannot set streaming interface for bulk transfers (%d)",
			ret);
		return ret;
	}

	ret = fl2000_regmap_init(usb_dev);
	if (ret) {
		dev_err(&interface->dev, "Cannot initialize regmap (%d)", ret);
		return ret;
	}

	ret = fl2000_i2c_init(usb_dev);
	if (ret) {
		dev_err(&interface->dev, "Cannot initialize I2C (%d)", ret);
		return ret;
	}

	ret = fl2000_drm_init(usb_dev);
	if (ret) {
		dev_err(&interface->dev, "Cannot initialize DRM (%d)", ret);
		return ret;
	}

	return 0;
}

static void fl2000_avcontrol_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	fl2000_drm_cleanup(usb_dev);
	fl2000_i2c_cleanup(usb_dev);
	fl2000_regmap_cleanup(usb_dev);
}

/* I2C interface, registers, master component */
static const struct fl2000_if_api fl2000_avcontrol = {
	.create = fl2000_avcontrol_create,
	.destroy = fl2000_avcontrol_destroy,
};

/* DRM device, FB device, screen rendering (bulk/iso transfers) */
static const struct fl2000_if_api fl2000_streaming = {
	.create = fl2000_streaming_create,
	.destroy = fl2000_streaming_destroy,
};

/* Interrupt polling (int transfers) */
static const struct fl2000_if_api fl2000_interrupt = {
	.create = fl2000_interrupt_create,
	.destroy = fl2000_interrupt_destroy,
};

static const struct usb_device_id fl2000_id_table[] = {
	FL2000_USB_INTERFACE(FL2000_USBIF_AVCONTROL, &fl2000_avcontrol),
	FL2000_USB_INTERFACE(FL2000_USBIF_STREAMING, &fl2000_streaming),
	FL2000_USB_INTERFACE(FL2000_USBIF_INTERRUPT, &fl2000_interrupt),
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	const struct fl2000_if_api *api = (const struct fl2000_if_api *)usb_dev_id->driver_info;

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "USB 1.1 is not supported!");
		return -ENODEV;
	}

	if (api && api->create)
		return api->create(interface);

	return 0;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	const struct usb_device_id *id;
	const struct fl2000_if_api *api;

	id = usb_match_id(interface, fl2000_id_table);
	if (!id) {
		dev_err(&interface->dev, "Cannot find matching USB ID");
		return;
	}

	api = (const struct fl2000_if_api *)id->driver_info;
	if (api && api->destroy)
		api->destroy(interface);
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	UNUSED(message);

	dev_dbg(&interface->dev, "suspend");

	/* TODO: suspend
	 * e.g. drm_mode_config_helper_suspend()
	 */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	dev_dbg(&interface->dev, "resume");

	/* TODO: resume
	 * e.g. drm_mode_config_helper_resume()
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
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
