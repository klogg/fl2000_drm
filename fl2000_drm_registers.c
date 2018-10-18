/*
 * fl2000_drm_registers.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000_drm.h"

#define CONTROL_MSG_LEN		4
#define CONTROL_MSG_READ	64
#define CONTROL_MSG_WRITE	65

/* Timeout in ms for USB Control Message (transport for I2C bus)  */
#define CONTROL_XFER_TIMEOUT	2000

int fl2000_reg_read(struct usb_device *usb_dev, u32 *data, u16 offset)
{
	return usb_control_msg(
		usb_dev,
		usb_rcvctrlpipe(usb_dev, 0),
		CONTROL_MSG_READ,
		(USB_DIR_IN | USB_TYPE_VENDOR),
		0,
		offset,
		data,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
}

int fl2000_reg_write(struct usb_device *usb_dev, u32 *data, u16 offset)
{
	return usb_control_msg(
		usb_dev,
		usb_sndctrlpipe(usb_dev, 0),
		CONTROL_MSG_WRITE,
		(USB_DIR_OUT | USB_TYPE_VENDOR),
		0,
		offset,
		data,
		CONTROL_MSG_LEN,
		CONTROL_XFER_TIMEOUT);
}

