/*
 * fl2000_drm_module.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000_drm.h"

#define USB_CLASS_AV 			0x10
#define USB_SUBCLASS_AV_CONTROL		0x01
#define USB_SUBCLASS_AV_VIDEO		0x02
#define USB_SUBCLASS_AV_AUDIO		0x03

#define USB_VENDOR_ID_FRESCO_LOGIC	0x1D5C
#define USB_PRODUCT_ID_FL2000		0x2000

#define FL2000_USBIF_AVCONTROL		0
#define FL2000_USBIF_STREAMING		1
#define FL2000_USBIF_INTERRUPT		2
#define FL2000_USBIF_MASSSTORAGE	3

struct fl2000_drm_intfdata {
	struct device *dev;
};

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_ID_FRESCO_LOGIC, \
		USB_PRODUCT_ID_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static int fl2000_device_probe(struct usb_interface *interface,
		const struct usb_device_id *usb_dev_id)
{
	struct usb_device		*usb_dev;
	struct fl2000_drm_intfdata	*intfdata;
	int ret = 0;

	usb_dev = interface_to_usbdev(interface);

	intfdata = kzalloc(sizeof(struct fl2000_drm_intfdata), GFP_KERNEL);
	if (!intfdata) {
		ret = -ENOMEM;
		goto exit;
	}

	usb_set_intfdata(interface, intfdata);

	switch (interface->cur_altsetting->desc.bInterfaceNumber) {
	case FL2000_USBIF_AVCONTROL:
		/* TODO:
		 * 1. register DRM device (NOTE: resolution etc is yet unknown)
		 * 2. allocate control structure for USB */
		break;

	case FL2000_USBIF_STREAMING:
		/* TODO:
		 * 1. allocate streaming structures
		 * 2. attach interface to device
		 * 3. create i2c interface + 2 gpio pins */
		usb_set_interface(usb_dev, interface->cur_altsetting->desc.bInterfaceNumber, 1);
		break;

	case FL2000_USBIF_INTERRUPT:
		/* TODO:
		 * 1. create/allocate pipes for interrupt
		 * 2. attach interface to device
		 * 3. probe HDMI chips */
		break;

	case FL2000_USBIF_MASSSTORAGE:
		/* What TODO?
		 */
		break;

	default:
		/* Device does not have any other interfaces */
		break;
	}

exit:
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct usb_device		*usb_dev;
	struct fl2000_drm_intfdata 	*intfdata;

	usb_dev = interface_to_usbdev(interface);

	intfdata = usb_get_intfdata(interface);

	switch (interface->cur_altsetting->desc.bInterfaceNumber) {
	case FL2000_USBIF_AVCONTROL:
		break;

	case FL2000_USBIF_STREAMING:
		usb_set_interface(usb_dev, interface->cur_altsetting->desc.bInterfaceNumber, 0);
		break;

	case FL2000_USBIF_INTERRUPT:
		break;

	case FL2000_USBIF_MASSSTORAGE:
		break;

	default:
		/* Device does not have any other interfaces */
		break;
	}

	usb_set_intfdata(interface, NULL);

	kfree(intfdata);
}

static int fl2000_suspend(struct usb_interface *interface,
			   pm_message_t message)
{
	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	/* modeset restore */
	return 0;
}

static struct usb_driver fl2000_driver = {
	.name 		= "fl2000_drm",
	.probe 		= fl2000_device_probe,
	.disconnect 	= fl2000_disconnect,
	.suspend	= fl2000_suspend,
	.resume		= fl2000_resume,
	.id_table 	= fl2000_id_table,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB HDMI video driver");
MODULE_LICENSE("GPL");
