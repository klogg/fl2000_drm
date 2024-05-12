// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define INTR_BUFSIZE 1

struct fl2000_intr {
	struct usb_device *usb_dev;
	struct drm_device *drm;
	u8 *buf;
	int pipe;
	struct work_struct work;
	struct workqueue_struct *work_queue;
};

static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_intr *intr = container_of(work, struct fl2000_intr, work);
	struct usb_device *usb_dev = intr->usb_dev;

	while (1) {
		int ret;

		/* Receive interrupt message */
		ret = usb_interrupt_msg(usb_dev, intr->pipe, intr->buf, INTR_BUFSIZE, NULL, 0);
		if (ret) {
			dev_err(&usb_dev->dev, "Interrupt message failed (%d)", ret);
			/* TODO: Signal fault to system and start shutdown of usb_dev */
			return;
		}

		ret = fl2000_check_interrupt(intr->usb_dev);
		if (ret)
			drm_kms_helper_hotplug_event(intr->drm);
	}
}

static void fl2000_intr_release(struct device *dev, void *res)
{
	struct fl2000_intr *intr = res;

	UNUSED(dev);

	cancel_work_sync(&intr->work);
	destroy_workqueue(intr->work_queue);

	kfree(intr->buf);
}

/**
 * fl2000_intr_create() - interrupt processing context creation
 * @interface:	USB interrupt transfers interface
 *
 * This function is called only on Interrupt interface probe
 *
 * Function initiates USB Interrupt transfers
 *
 * Return: Operation result
 */
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct drm_device *drm)
{
	int ret;
	unsigned int ep;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc;
	struct usb_interface *interface = usb_ifnum_to_if(usb_dev, FL2000_USBIF_INTERRUPT);

	/* There's only one altsetting (#0) and one endpoint (#3) in the interrupt interface (#2)
	 * but lets try and "find" it anyway
	 */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot find interrupt endpoint");
		return ERR_PTR(ret);
	}
	ep = usb_endpoint_num(desc);

	intr = devres_alloc(&fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt private structure");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, intr);

	intr->usb_dev = usb_dev;
	intr->pipe = usb_rcvintpipe(usb_dev, ep);
	intr->drm = drm;
	intr->buf = kmalloc(INTR_BUFSIZE, GFP_KERNEL);
	if (!intr->buf) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	/* This possibly involves reading I2C registers, etc. so better to schedule a work queue */
	INIT_WORK(&intr->work, &fl2000_intr_work);
	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}
	queue_work(intr->work_queue, &intr->work);

	return intr;
}

void fl2000_intr_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
}
