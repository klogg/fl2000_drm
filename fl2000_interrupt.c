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
	struct usb_interface *interface;
	struct urb *urb;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	u8 *buf;
};

static void fl2000_intr_completion(struct urb *urb);

static void fl2000_intr_work(struct work_struct *work_item)
{
	int ret;
	struct fl2000_intr *intr = container_of(work_item,
			struct fl2000_intr, work);
	struct usb_device *usb_dev = interface_to_usbdev(intr->interface);

	fl2000_inter_check(usb_dev);

	/* Restart urb */
	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret) {
		/* TODO: WTF! Signal general failure, stop driver */
		dev_err(&usb_dev->dev, "URB submission failed (%d)", ret);
	}
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct fl2000_intr *intr = urb->context;
	struct usb_device *usb_dev = interface_to_usbdev(intr->interface);

	INIT_WORK(&intr->work, &fl2000_intr_work);

	/* Since we use a single URB for interrupt processing this means that we
	 * have already submitted URB from work and it was returned to us before
	 * work was dequeued. In this case we just re-submit URB so we will get
	 * it back hopefully with workqueue processing finished */
	if (!queue_work(intr->work_queue, &intr->work)) {
		ret = usb_submit_urb(intr->urb, GFP_KERNEL);
		if (ret) {
			/* TODO: WTF! Signal general failure, stop driver!
			 * Except in case of -EPERM, that means we are already
			 * in progress of stopping */
			dev_err(&usb_dev->dev, "URB submission failed (%d)",
					ret);
		}
	}
}

int fl2000_intr_create(struct usb_interface *interface)
{
	int ret = 0;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc = NULL;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* There's only one altsetting (#0) and one endpoint (#3) in the
	 * interrupt interface (#2) but lets try and "find" it anyway */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret)
		return ret;

	intr = devm_kzalloc(&usb_dev->dev, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt private " \
				"structure");
		return -ENOMEM;
	}

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!intr->urb) {
		dev_err(&usb_dev->dev, "Allocate interrupt URB failed");
		return -ENOMEM;
	}

	intr->buf = usb_alloc_coherent(usb_dev, INTR_BUFSIZE, GFP_KERNEL,
			&intr->urb->transfer_dma);
	if (!intr->buf) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		usb_free_urb(intr->urb);
		return -ENOMEM;
	}

	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		usb_free_coherent(usb_dev, INTR_BUFSIZE, intr->buf,
				intr->urb->transfer_dma);
		usb_free_urb(intr->urb);
		return -ENOMEM;
	}
	intr->interface = interface;

	usb_set_intfdata(interface, intr);

	usb_fill_int_urb(intr->urb, usb_dev,
			usb_rcvintpipe(usb_dev, usb_endpoint_num(desc)),
			intr->buf, INTR_BUFSIZE, fl2000_intr_completion, intr,
			desc->bInterval);

	intr->urb->transfer_flags |=
			URB_NO_TRANSFER_DMA_MAP; /* use urb->transfer_dma */

	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed");
		destroy_workqueue(intr->work_queue);
		usb_free_coherent(usb_dev, INTR_BUFSIZE, intr->buf,
				intr->urb->transfer_dma);
		usb_free_urb(intr->urb);
		return ret;
	}

	return 0;
}

void fl2000_intr_destroy(struct usb_interface *interface)
{
	struct fl2000_intr *intr = usb_get_intfdata(interface);
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	if (intr == NULL)
		return;

	usb_poison_urb(intr->urb);

	destroy_workqueue(intr->work_queue);

	usb_free_coherent(usb_dev, INTR_BUFSIZE, intr->buf,
			intr->urb->transfer_dma);

	usb_free_urb(intr->urb);
}
