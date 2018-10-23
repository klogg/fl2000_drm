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

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_ID_FRESCO_LOGIC, \
		USB_PRODUCT_ID_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static int fl2000_probe(struct usb_interface *interface,
		const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* TODO:
	 * - register DRM device (NOTE: resolution etc is yet unknown)
	 * - allocate control structure for USB
	 * - allocate streaming structures
	 * - 2 gpio pins
	 * NOTE: HDMI interface shall sit on top of I2C (I guess) */

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		dev_info(&usb_dev->dev, "Probing AVControl interface (%u)",
				iface_num);

		/* This is rather useless, AVControl is not properly implemented
		 * on FL2000 chip - that is why all the "magic" needed */
		ret = fl2000_i2c_connect(usb_dev);
		if (ret != 0) goto error;
		break;

	case FL2000_USBIF_STREAMING:
		dev_info(&usb_dev->dev, "Probing Streaming interface (%u)",
				iface_num);

		break;

	case FL2000_USBIF_INTERRUPT:
		dev_info(&usb_dev->dev, "Probing Interrupt interface (%u)",
				iface_num);

		ret = fl2000_intr_create(interface);
		if (ret != 0) goto error;
		break;

	case FL2000_USBIF_MASSSTORAGE:
		/* This is defined but not getting probed - do nothing */
		dev_warn(&interface->dev, "Unexpectedly probed Mass Storage");
		break;

	default:
		/* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?",
				interface->cur_altsetting->desc.iInterface);
		break;
	}

error:
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct fl2000_drm_intfdata *intfdata;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	struct usb_device *usb_dev = interface_to_usbdev(interface);
	dev_info(&usb_dev->dev, "Disconnecting interface: %u", iface_num);

	intfdata = usb_get_intfdata(interface);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		fl2000_i2c_disconnect(usb_dev);
		break;

	case FL2000_USBIF_STREAMING:
		break;

	case FL2000_USBIF_INTERRUPT:
		fl2000_intr_destroy(interface);
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
	.name 		= "fl2000_drm",
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
MODULE_DESCRIPTION("FL2000 USB HDMI video driver");
MODULE_LICENSE("GPL");
