/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_intr.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define RUN	(1U)
#define STOP	(0U)

#define INTR_BUFSIZE	4

struct fl2000_intr {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
	unsigned int pipe;
	int interval;
	struct urb *urb;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	u8 *buf;
	u32 status;
	atomic_t state;
};

static void fl2000_intr_completion(struct urb *urb);

static inline int fl2000_intr_submit_urb(struct fl2000_intr *intr)
{
	/* NOTE: always submit data, never set/process it, why? */
	usb_fill_int_urb(
		intr->urb,
		intr->usb_dev,
		intr->pipe,
		intr->buf,
		INTR_BUFSIZE,
		fl2000_intr_completion,
		intr,
		intr->interval);

	return usb_submit_urb(intr->urb, GFP_KERNEL);
}

static void fl2000_intr_work(struct work_struct *work_item)
{
	int ret;
	struct fl2000_intr *intr = container_of(work_item,
			struct fl2000_intr, work);

	if (intr == NULL) return;

	/* Read interrupt status register */
	ret = fl2000_reg_read(intr->usb_dev, &intr->status, FL2000_REG_INT_STATUS);
	if (ret != 0) {
		dev_err(&intr->interface->dev, "Cannot read interrupt" \
				"register (%d)", ret);
	}

	/* TODO: Process interrupt:
	 * - maybe signal to HDMI subsystem so it will handle I2C registers?
	 * - or check status I2C registers ourselves, and the? */

	if (atomic_read(&intr->state) != RUN) return;

	/* Restart urb */
	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		atomic_set(&intr->state, STOP);
		dev_err(&intr->interface->dev, "URB submission failed failed");
	}
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct fl2000_intr *intr = urb->context;

	if (intr == NULL) return;

	INIT_WORK(&intr->work, &fl2000_intr_work);

	if (queue_work(intr->work_queue, &intr->work))
		/* Exit on success */
		return;

	dev_err(&intr->interface->dev, "Cannot queue URB processing work");

	if (atomic_read(&intr->state) != RUN) return;

	/* Restart urb in case of work queuing failure */
	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		atomic_set(&intr->state, STOP);
		dev_err(&intr->interface->dev, "URB submission failed failed");
	}
}

int fl2000_intr_create(struct usb_interface *interface)
{
	int i, ret = 0;
	struct fl2000_intr *intr;
	struct usb_host_interface *host_interface = NULL;
	struct usb_endpoint_descriptor *desc = NULL;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* There's only one altsetting (#0) and one endpoint (#3) in the
	 * interrupt interface (#2) but lets try and "find" it anyway */
	for (i = 0; i < interface->num_altsetting; i++) {
		host_interface = &interface->altsetting[i];
		if (!usb_find_int_in_endpoint(host_interface, &desc))
			break;
	}

	if (desc == NULL) {
		dev_err(&interface->dev, "Cannot find altsetting with " \
				"interrupt endpoint");
		ret = -ENXIO;
		goto error;
	}

	dev_info(&interface->dev, "Setting interrupt interface %d: " \
			"altsetting %d, endpoint %d",
			host_interface->desc.bInterfaceNumber,
			host_interface->desc.bAlternateSetting,
			usb_endpoint_num(desc));

	ret = usb_set_interface(usb_dev,
			host_interface->desc.bInterfaceNumber,
			host_interface->desc.bAlternateSetting);
	if (ret != 0) {
		dev_err(&interface->dev,"Cannot set interrupt endpoint " \
				"altsetting");
		goto error;
	}

	intr = kzalloc(sizeof(*intr), GFP_KERNEL);
	if (IS_ERR(intr)) {
		dev_err(&interface->dev, "Cannot allocate interrupt private" \
				"structure");
		ret = PTR_ERR(intr);
		goto error;
	}

	intr->buf = kzalloc(INTR_BUFSIZE, GFP_DMA);
	if (IS_ERR(intr->buf)) {
		dev_err(&interface->dev, "Cannot allocate interrupt data");
		ret = PTR_ERR(intr->buf);
		goto error;
	}

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (IS_ERR(intr->urb)) {
		dev_err(&interface->dev, "Allocate interrupt URB failed");
		ret = PTR_ERR(intr->urb);
		goto error;
	}

	intr->work_queue = create_workqueue("work_queue");
	if (IS_ERR(intr->work_queue)) {
		dev_err(&interface->dev, "Create interrupt workqueue failed");
		ret = PTR_ERR(intr->work_queue);
		goto error;
	}
	intr->usb_dev = usb_dev;
	intr->interface = interface;
	intr->pipe = usb_rcvintpipe(usb_dev, usb_endpoint_num(desc));
	intr->interval = desc->bInterval;
	atomic_set(&intr->state, RUN);

	usb_set_intfdata(interface, intr);

	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		dev_err(&interface->dev, "URB submission failed failed");
		goto error;
	}

	return 0;
error:
	/* Enforce cleanup in case of error */
	fl2000_intr_destroy(interface);
	return ret;
}

void fl2000_intr_destroy(struct usb_interface *interface)
{
	struct fl2000_intr *intr = usb_get_intfdata(interface);

	if (intr == NULL)
		return;

	atomic_set(&intr->state, STOP);

	if (intr->urb != NULL)
		usb_kill_urb(intr->urb);

	if (intr->work_queue != NULL)
		drain_workqueue(intr->work_queue);

	if (intr->work_queue != NULL)
		destroy_workqueue(intr->work_queue);

	if (intr->urb != NULL)
		usb_free_urb(intr->urb);

	usb_set_intfdata(interface, NULL);

	if (intr->buf != NULL)
		kfree(intr->buf);

	kfree(intr);
}
