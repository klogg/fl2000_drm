/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_intr.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

void fl2000_inter_check(struct usb_device *usb_dev);

#define INTR_BUFSIZE	1

struct fl2000_intr {
	u8 poll_interval;
	struct urb *urb;
	struct work_struct work;
	struct workqueue_struct *work_queue;
};

static void fl2000_intr_work(struct work_struct *work)
{
	int ret;
	struct fl2000_intr *intr = container_of(work, struct fl2000_intr, work);
	struct urb *urb = intr->urb;
	struct usb_device *usb_dev;

	if (urb == NULL)
		return;

	usb_dev = urb->dev;

	switch (urb->status) {
	/* All went well */
	case 0:
		/* This possibly involves reading I2C registers, etc. so shall be
		 * scheduled as a work queue */
		fl2000_inter_check(usb_dev);
		break;

	/* URB was unlinked or device shutdown in progress, do nothing */
	case -ECONNRESET:
	case -ENOENT:
	case -ENODEV:
		return;

	/* Hardware or protocol errors - no recovery, report and do nothing */
	case -ESHUTDOWN:
	case -EPROTO:
	case -EILSEQ:
	case -ETIME:
		dev_err(&usb_dev->dev, "USB hardware unrecoverable error %d",
				urb->status);
		return;

	/* Stalled endpoint */
	case -EPIPE:
		dev_err(&usb_dev->dev, "Interrupt endpoint stalled");
		ret = usb_clear_halt(usb_dev, urb->pipe);
		if (ret != 0) {
			dev_err(&usb_dev->dev, "Cannot reset interrupt " \
					"endpoint, error %d", ret);
			return;
		}
		break;

	/* All the rest cases - just restart transfer */
	default:
		break;
	}

	/* For interrupt URBs, as part of successful URB submission
	 * urb->interval is modified to reflect the actual transfer period used,
	 * so we need to restore it */
	urb->interval = intr->poll_interval;
	urb->start_frame = -1;

	/* Restart urb */
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		/* TODO: WTF! Signal general failure, stop driver! Except in
		 * case of -EPERM, that means we already in progress of
		 * stopping */
		dev_err(&usb_dev->dev, "URB submission failed (%d)", ret);
	}
}

static void fl2000_intr_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_intr_completion(struct urb *urb)
{
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_intr *intr = devres_find(&usb_dev->dev,
			fl2000_intr_release, NULL, NULL);

	/* TODO: Process URB status */

	INIT_WORK(&intr->work, &fl2000_intr_work);
	queue_work(intr->work_queue, &intr->work);
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
int fl2000_intr_create(struct usb_interface *interface)
{
	int ret = 0;
	u8 *buf;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* There's only one altsetting (#0) and one endpoint (#3) in the
	 * interrupt interface (#2) but lets try and "find" it anyway */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot find interrupt endpoint");
		return ret;
	}

	intr = devres_alloc(&fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt private " \
				"structure");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, intr);

	intr->poll_interval = desc->bInterval;

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!intr->urb) {
		dev_err(&usb_dev->dev, "Allocate interrupt URB failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return -ENOMEM;
	}

	buf = usb_alloc_coherent(usb_dev, INTR_BUFSIZE, GFP_KERNEL,
			&intr->urb->transfer_dma);
	if (!buf) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		usb_free_urb(intr->urb);
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return -ENOMEM;
	}

	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		usb_free_coherent(usb_dev, INTR_BUFSIZE, buf,
				intr->urb->transfer_dma);
		usb_free_urb(intr->urb);
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return -ENOMEM;
	}

	dev_info(&usb_dev->dev, "Interrupt pipe number %d",
			usb_rcvintpipe(usb_dev, usb_endpoint_num(desc)));

	/* Interrupt URB configuration is static, including allocated buffer
	 * NOTE: We are setting 'transfer_dma' during coherent buffer
	 * allocation above */
	usb_fill_int_urb(intr->urb, usb_dev,
			usb_rcvintpipe(usb_dev, usb_endpoint_num(desc)),
			buf, INTR_BUFSIZE, fl2000_intr_completion, intr,
			intr->poll_interval);
	intr->urb->transfer_flags |=
			URB_NO_TRANSFER_DMA_MAP; /* use urb->transfer_dma */

	/* Start checking for interrupts */
	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed");
		destroy_workqueue(intr->work_queue);
		usb_free_coherent(usb_dev, INTR_BUFSIZE, buf,
				intr->urb->transfer_dma);
		usb_free_urb(intr->urb);
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
	}

	dev_info(&usb_dev->dev, "Interrupt interface up");

	return 0;
}

void fl2000_intr_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_intr *intr = devres_find(&usb_dev->dev,
			fl2000_intr_release, NULL, NULL);

	if (!intr)
		return;

	usb_poison_urb(intr->urb);
	cancel_work_sync(&intr->work);

	destroy_workqueue(intr->work_queue);
	usb_free_coherent(usb_dev, INTR_BUFSIZE, intr->urb->transfer_buffer,
			intr->urb->transfer_dma);
	usb_free_urb(intr->urb);
	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
}
