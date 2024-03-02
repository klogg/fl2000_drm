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
	u8 poll_interval;
	struct urb *urb;
	u8 *buf;
	dma_addr_t transfer_dma;
	struct work_struct work;
	struct workqueue_struct *work_queue;
};

static void fl2000_intr_work(struct work_struct *work)
{
	int event;
	struct fl2000_intr *intr = container_of(work, struct fl2000_intr, work);

	event = fl2000_check_interrupt(intr->usb_dev);
	if (event)
		drm_kms_helper_hotplug_event(intr->drm);
}

static void fl2000_intr_release(struct device *dev, void *res)
{
	struct fl2000_intr *intr = res;
	struct usb_device *usb_dev = to_usb_device(dev);

	usb_poison_urb(intr->urb);
	cancel_work_sync(&intr->work);
	destroy_workqueue(intr->work_queue);
	usb_free_coherent(usb_dev, INTR_BUFSIZE, intr->buf, intr->transfer_dma);
	usb_free_urb(intr->urb);
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_intr *intr = urb->context;

	ret = fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	if (ret) {
		dev_err(&usb_dev->dev, "Stopping interrupts");
		return;
	}

	/* This possibly involves reading I2C registers, etc. so better to schedule a work queue */
	queue_work(intr->work_queue, &intr->work);

	/* For interrupt URBs, as part of successful URB submission urb->interval is modified to
	 * reflect the actual transfer period used, so we need to restore it
	 */
	urb->interval = intr->poll_interval;
	urb->start_frame = -1;

	/* Restart urb */
	ret = fl2000_submit_urb(urb);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed (%d)", ret);
		/* TODO: Signal fault to system and start shutdown of usb_dev */
	}
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

	intr = devres_alloc(&fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt private structure");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, intr);

	intr->poll_interval = desc->bInterval;
	intr->usb_dev = usb_dev;
	intr->drm = drm;
	INIT_WORK(&intr->work, &fl2000_intr_work);

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!intr->urb) {
		dev_err(&usb_dev->dev, "Allocate interrupt URB failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	intr->buf = usb_alloc_coherent(usb_dev, INTR_BUFSIZE, GFP_KERNEL, &intr->transfer_dma);
	if (!intr->buf) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	/* Interrupt URB configuration is static, including allocated buffer */
	usb_fill_int_urb(intr->urb, usb_dev, usb_rcvintpipe(usb_dev, 3),
			 intr->buf, INTR_BUFSIZE, fl2000_intr_completion, intr, intr->poll_interval);
	intr->urb->transfer_dma = intr->transfer_dma;
	intr->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP; /* use urb->transfer_dma */

	/* Start checking for interrupts */
	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(ret);
	}

	return intr;
}

void fl2000_intr_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
}
