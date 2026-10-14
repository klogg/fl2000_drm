// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
#include "fl2000.h"

#define FL2000_ALL_IFS GENMASK(FL2000_USBIF_INTERRUPT, FL2000_USBIF_AVCONTROL)
/* USB probes for different interfaces and different drivers may run concurrently. */
static DEFINE_MUTEX(fl2000_device_lock);

static const struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x1d5c, 0x2000, FL2000_USBIF_AVCONTROL) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x1d5c, 0x2000, FL2000_USBIF_STREAMING) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x1d5c, 0x2000, FL2000_USBIF_INTERRUPT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static void fl2000_destroy_interface(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	switch (interface->cur_altsetting->desc.bInterfaceNumber) {
	case FL2000_USBIF_AVCONTROL:
		fl2000_i2c_cleanup(usb_dev);
		fl2000_regmap_cleanup(usb_dev);
		break;
	case FL2000_USBIF_STREAMING:
		fl2000_streaming_destroy(interface);
		break;
	case FL2000_USBIF_INTERRUPT:
		fl2000_interrupt_destroy(interface);
		break;
	}
}

static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_device *device;
	unsigned int ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
	int ret;

	if (usb_dev->speed < USB_SPEED_HIGH)
		return -ENODEV;
	mutex_lock(&fl2000_device_lock);
	device = dev_get_drvdata(&usb_dev->dev);
	if (!device) {
		device = devm_kzalloc(&usb_dev->dev, sizeof(*device), GFP_KERNEL);
		if (!device) {
			ret = -ENOMEM;
			goto out;
		}
		dev_set_drvdata(&usb_dev->dev, device);
	}
	/* Configure the bulk endpoint before any interface may need it. */
	if (!device->active_interfaces || ifnum == FL2000_USBIF_AVCONTROL) {
		ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
		if (ret)
			goto out;
	}
	switch (ifnum) {
	case FL2000_USBIF_AVCONTROL:
		ret = fl2000_regmap_init(usb_dev);
		if (!ret) {
			ret = fl2000_reset(usb_dev);
			if (!ret)
				ret = fl2000_init_clock(usb_dev);
			if (!ret)
				ret = fl2000_usb_magic(usb_dev);
			if (!ret)
				ret = fl2000_i2c_init(usb_dev);
			if (ret)
				fl2000_regmap_cleanup(usb_dev);
		}
		break;
	case FL2000_USBIF_STREAMING:
		ret = fl2000_streaming_create(interface);
		break;
	case FL2000_USBIF_INTERRUPT:
		ret = fl2000_interrupt_create(interface);
		break;
	default:
		ret = -ENODEV;
	}
	if (ret)
		goto out;
	device->active_interfaces |= BIT(ifnum);
	usb_set_intfdata(interface, device);
	if (device->active_interfaces == FL2000_ALL_IFS) {
		ret = fl2000_drm_init(usb_dev);
		if (ret) {
			device->active_interfaces &= ~BIT(ifnum);
			usb_set_intfdata(interface, NULL);
			fl2000_destroy_interface(interface);
		} else {
			device->master_registered = true;
		}
	}
out:
	mutex_unlock(&fl2000_device_lock);
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	if (!device)
		return;
	mutex_lock(&fl2000_device_lock);
	/* Stop the complete pipeline on the first interface disconnect. */
	if (device->master_registered) {
		fl2000_drm_cleanup(usb_dev);
		device->master_registered = false;
	}
	fl2000_disable_interrupt(usb_dev);
	fl2000_streaming_disable(usb_dev);
	device->active_interfaces &= ~BIT(interface->cur_altsetting->desc.bInterfaceNumber);
	fl2000_destroy_interface(interface);
	usb_set_intfdata(interface, NULL);
	device->suspended = false;
	mutex_unlock(&fl2000_device_lock);
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	int ret = 0;

	mutex_lock(&fl2000_device_lock);
	if (device && !device->suspended) {
		ret = fl2000_drm_suspend(interface_to_usbdev(interface));
		if (!ret)
			device->suspended = true;
	}
	mutex_unlock(&fl2000_device_lock);
	return ret;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	int ret = 0;

	/* The control interface owns restoration after all USB endpoints return. */
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;
	mutex_lock(&fl2000_device_lock);
	if (device && device->suspended) {
		ret = usb_set_interface(interface_to_usbdev(interface), FL2000_USBIF_AVCONTROL, 1);
		if (!ret)
			ret = fl2000_drm_resume(interface_to_usbdev(interface));
		if (!ret)
			device->suspended = false;
	}
	mutex_unlock(&fl2000_device_lock);
	return ret;
}

static struct usb_driver fl2000_driver = {
	.name = "fl2000_usb",
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.reset_resume = fl2000_resume,
	.id_table = fl2000_id_table,
	.disable_hub_initiated_lpm = true,
};
module_usb_driver(fl2000_driver);
MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
