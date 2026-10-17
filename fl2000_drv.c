// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
#include "fl2000.h"
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

static struct usb_driver fl2000_driver;

static const struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x1d5c, 0x2000, FL2000_USBIF_AVCONTROL) },
	{}
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
	default:
		break;
	}
}

/* The caller serializes control-interface setup with the device mutex. */
static int fl2000_control_init(struct usb_device *usb_dev)
{
	int ret;

	ret = fl2000_regmap_init(usb_dev);
	if (ret)
		return ret;
	ret = fl2000_reset(usb_dev);
	if (!ret)
		ret = fl2000_init_clock(usb_dev);
	if (!ret)
		ret = fl2000_usb_magic(usb_dev);
	if (!ret)
		ret = fl2000_i2c_init(usb_dev);
	if (ret)
		fl2000_regmap_cleanup(usb_dev);
	return ret;
}

/* Clear every callback's data before synchronous sibling disconnects. */
static void fl2000_release_interfaces(struct fl2000_device *device,
				      const struct usb_interface *origin)
{
	struct usb_interface *interfaces[FL2000_USBIF_INTERRUPT + 1];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(interfaces); i++) {
		interfaces[i] = device->interfaces[i];
		device->interfaces[i] = NULL;
		if (interfaces[i])
			usb_set_intfdata(interfaces[i], NULL);
	}
	for (i = ARRAY_SIZE(interfaces); i-- > 0;)
		if (interfaces[i] && interfaces[i] != origin)
			usb_driver_release_interface(&fl2000_driver, interfaces[i]);
}

static void fl2000_cleanup(struct usb_device *usb_dev, struct fl2000_device *device)
{
	if (device->master_registered) {
		fl2000_drm_cleanup(usb_dev);
		device->master_registered = false;
	}
	for (unsigned int i = ARRAY_SIZE(device->interfaces); i-- > 0;)
		if (device->active_interfaces & BIT(i))
			fl2000_destroy_interface(device->interfaces[i]);
	device->active_interfaces = 0;
	device->suspended = false;
}

static int fl2000_probe(struct usb_interface *interface,
			const struct usb_device_id *id __always_unused)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_device *device;
	struct usb_interface *sibling;
	int ret;

	if (usb_dev->speed < USB_SPEED_HIGH ||
	    interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return -ENODEV;
	/* USB core holds the parent device lock while probing an interface. */
	device = dev_get_drvdata(&usb_dev->dev);
	if (!device) {
		device = devm_kzalloc(&usb_dev->dev, sizeof(*device), GFP_KERNEL);
		if (!device) {
			ret = -ENOMEM;
			return ret;
		}
		mutex_init(&device->lock);
		atomic_set(&device->i2c_errors, 0);
		dev_set_drvdata(&usb_dev->dev, device);
	}
	mutex_lock(&device->lock);
	device->interfaces[FL2000_USBIF_AVCONTROL] = interface;
	usb_set_intfdata(interface, device);
	/* Own the complete device before changing endpoints or hardware state. */
	for (unsigned int i = FL2000_USBIF_STREAMING; i <= FL2000_USBIF_INTERRUPT; i++) {
		sibling = usb_ifnum_to_if(usb_dev, i);
		ret = usb_driver_claim_interface(&fl2000_driver, sibling, device);
		if (ret)
			goto release;
		device->interfaces[i] = sibling;
	}
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret)
		goto release;
	ret = fl2000_control_init(usb_dev);
	if (ret)
		goto release;
	device->active_interfaces |= BIT(FL2000_USBIF_AVCONTROL);
	ret = fl2000_streaming_create(device->interfaces[FL2000_USBIF_STREAMING]);
	if (ret)
		goto cleanup;
	device->active_interfaces |= BIT(FL2000_USBIF_STREAMING);
	ret = fl2000_interrupt_create(device->interfaces[FL2000_USBIF_INTERRUPT]);
	if (ret)
		goto cleanup;
	device->active_interfaces |= BIT(FL2000_USBIF_INTERRUPT);
	ret = fl2000_drm_init(usb_dev);
	if (ret)
		goto cleanup;
	device->master_registered = true;
	goto out;

cleanup:
	fl2000_cleanup(usb_dev, device);
release:
	fl2000_release_interfaces(device, interface);
out:
	mutex_unlock(&device->lock);
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	if (!device)
		return;
	mutex_lock(&device->lock);
	fl2000_cleanup(usb_dev, device);
	fl2000_release_interfaces(device, interface);
	mutex_unlock(&device->lock);
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message __always_unused)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	int ret = 0;

	if (!device)
		return 0;
	mutex_lock(&device->lock);
	if (!device->suspended) {
		ret = fl2000_drm_suspend(interface_to_usbdev(interface));
		if (!ret)
			device->suspended = true;
	}
	mutex_unlock(&device->lock);
	return ret;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct fl2000_device *device = usb_get_intfdata(interface);
	int ret = 0;

	/* The control interface owns restoration after all USB endpoints return. */
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;
	if (!device)
		return 0;
	mutex_lock(&device->lock);
	if (device->suspended) {
		ret = usb_set_interface(interface_to_usbdev(interface), FL2000_USBIF_AVCONTROL, 1);
		if (!ret)
			ret = fl2000_drm_resume(interface_to_usbdev(interface));
		if (!ret)
			device->suspended = false;
	}
	mutex_unlock(&device->lock);
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
