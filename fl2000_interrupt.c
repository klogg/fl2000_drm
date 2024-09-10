// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

struct fl2000_interrupt {
	struct usb_interface *interface;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	u8 buf;
};

static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_interrupt *intr = container_of(work, struct fl2000_intr, work);
	struct usb_interface *interface = intr->interface;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	int ret;

	/* Receive interrupt message */
	ret = usb_interrupt_msg(usb_dev, usb_rcvintpipe(usb_dev, INTERRUPT_EP), &intr->buf, sizeof(intr->buf), NULL, 0);
	if (ret) {
		dev_err(&interface->dev, "Sending interrupt message failed (%d)", ret);
		/* TODO: Signal fault to system and start shutdown of usb_dev */
		return;
	}

	ret = fl2000_check_interrupt(intr->usb_dev);
	if (ret < 0) {
		dev_err(&interface->dev, "Checking interrupt message failed (%d)", ret);
		/* TODO: Signal fault to system and start shutdown of usb_dev */
		return;
	} else if (ret > 0) {
		fl2000_drm_hotplug(usb_dev);
	}

	queue_work(intr->work_queue, &intr->work);	
}

static void fl2000_intr_release(struct device *dev, void *res)
{
	struct fl2000_interrupt *intr = res;

	UNUSED(dev);

	cancel_work_sync(&intr->work);
	destroy_workqueue(intr->work_queue);
}

/**
 * fl2000_enable_interrupt() - interrupt processing start
 * 
 * @usb_dev: USB device
 * 
 * @returns: Operation result
 */
int fl2000_enable_interrupt(struct usb_device *usb_dev)
{
	struct fl2000_interrupt *intr;

	intr = devres_find(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot find interrupt context");
		return -ENODEV;
	}

	return queue_work(intr->work_queue, &intr->work);
}

/**
 * fl2000_disable_interrupt() - interrupt processing stop
 *
 * @usb_dev: USB device
 * 
 * @returns: Operation result
 */
int fl2000_disable_interrupt(struct usb_device *usb_dev)
{
	struct fl2000_interrupt *intr;

	intr = devres_find(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot find interrupt context");
		return -ENODEV;
	}

	return cancel_work_sync(intr->work_queue, &intr->work);
}

/**
 * fl2000_interrupt_create() - interrupt processing context creation
 * 
 * @interface: USB interrupt transfers interface
 * 
 * @returns: Operation result
 */
int fl2000_interrupt_create(struct usb_interface *interface)
{
	int ret;
	struct fl2000_interrupt *intr;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	intr = devres_alloc(&fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&interface->dev, "Cannot allocate interrupt private structure");
		return -ENOMEM;
	}

	intr->interface = interface;
	
	/* This possibly involves reading I2C registers, etc. so better to schedule a work queue */
	INIT_WORK(&intr->work, &fl2000_intr_work);
	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&interface->dev, "Create interrupt workqueue failed");
		devres_free(intr);
		return -ENOMEM;
	}

	devres_add(&usb_dev->dev, intr);

	return 0;
}

/**
 * fl2000_interrupt_destroy() - interrupt processing context destruction
 * 
 * @interface: USB interrupt transfers interface
 */
void fl2000_interrupt_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
}
