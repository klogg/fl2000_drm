/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_intr.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

enum fl2000_intr_state {
	RUN = (1U),
	STOP = (0U),
};

#define INTR_BUFSIZE	4

struct fl2000_intr {
	struct usb_interface *interface;
	unsigned int pipe;
	int interval;
	struct urb *urb;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	u8 *buf;
	atomic_t state;
};

#if defined(CONFIG_DEBUG_FS)

/* TODO: This shall not be static, and TBH the whole design of checking
 * in debugfs here is completely wrong. This part shall be rewritten considering
 * interrupts happening all the time and statuses read and stored somewhere */
static u32 intr_status;

static void fl2000_debugfs_intr_status(u32 status)
{
	intr_status = status;
}

static int fl2000_debugfs_intr_init(void)
{
	struct dentry *root_dir;
	struct dentry *interrupt_file;

	root_dir = debugfs_create_dir("fl2000_interrupt", NULL);

	interrupt_file = debugfs_create_x32("intr_status", fl2000_debug_umode,
			root_dir, &intr_status);

	return 0;
}
#else /* CONFIG_DEBUG_FS */
#define fl2000_debugfs_intr_init()
#define fl2000_debugfs_intr_status(status)
#endif /* CONFIG_DEBUG_FS */


static void fl2000_intr_completion(struct urb *urb);

void fl2000_inter_check(struct usb_device *usb_dev, u32 status);

static inline int fl2000_intr_submit_urb(struct fl2000_intr *intr)
{
	struct usb_device *usb_dev = interface_to_usbdev(intr->interface);

	/* NOTE: always submit data, never set/process it, why? */
	usb_fill_int_urb(
		intr->urb,
		usb_dev,
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
	struct usb_device *usb_dev = interface_to_usbdev(intr->interface);
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 status;

	if (atomic_read(&intr->state) != RUN)
		return;

	/* Process interrupt */
	if (regmap) {
		ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status);
		if (ret != 0) {
			dev_err(&usb_dev->dev, "Cannot read interrupt " \
					"register (%d)", ret);
		} else {
			dev_info(&usb_dev->dev, "FL2000 interrupt 0x%X",
					status);

			fl2000_debugfs_intr_status(status);

			/* TODO: This shall be called only for relevant
			 * interrupts, others shall be processed differently */
			fl2000_inter_check(usb_dev, status);
		}
	}

	/* Restart urb */
	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		/* TODO: WTF! Signal general failure, stop driver */
		dev_err(&intr->interface->dev, "URB submission failed (%d)",
				ret);
	}
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct fl2000_intr *intr = urb->context;

	if (intr == NULL || atomic_read(&intr->state) != RUN)
		return;

	INIT_WORK(&intr->work, &fl2000_intr_work);

	/* Since we use a single URB for interrupt processing this means that we
	 * have already submitted URB from work and it was returned to us before
	 * work was dequeued. In this case we just re-submit URB so we will get
	 * it back hopefully with workqueue processing finished */
	if (!queue_work(intr->work_queue, &intr->work)) {
		ret = fl2000_intr_submit_urb(intr);
		if (ret != 0) {
			/* TODO: WTF! Signal general failure, stop driver */
			dev_err(&intr->interface->dev, "URB submission " \
					"failed (%d)", ret);
		}
	}
}

void fl2000_intr_destroy(struct usb_interface *interface);

int fl2000_intr_create(struct usb_interface *interface)
{
	int i, ret = 0;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc = NULL;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	/* There's only one altsetting (#0) and one endpoint (#3) in the
	 * interrupt interface (#2) but lets try and "find" it anyway */
	for (i = 0; i < interface->num_altsetting; i++) {
		if (!usb_find_int_in_endpoint(&interface->altsetting[i],
				&desc)) {
			dev_info(&interface->dev, "Found interrupt endpoint " \
					"%d in altsetting %d",
					usb_endpoint_num(desc),
					desc->bEndpointAddress);
			break;
		}
	}
	if (desc == NULL) {
		dev_err(&interface->dev, "Cannot find altsetting containing " \
				"interrupt endpoint");
		return -ENXIO;
	}

	intr = devm_kzalloc(&interface->dev, sizeof(*intr), GFP_KERNEL);
	if (IS_ERR_OR_NULL(intr)) {
		dev_err(&interface->dev, "Cannot allocate interrupt private " \
				"structure");
		return PTR_ERR(intr);
	}

	intr->buf = devm_kzalloc(&interface->dev, INTR_BUFSIZE, GFP_DMA);
	if (IS_ERR_OR_NULL(intr->buf)) {
		dev_err(&interface->dev, "Cannot allocate interrupt data");
		return PTR_ERR(intr->buf);
	}

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (IS_ERR_OR_NULL(intr->urb)) {
		dev_err(&interface->dev, "Allocate interrupt URB failed");
		return PTR_ERR(intr->urb);
	}

	/* Mark interrupt as operation as soon as URB is allocated */
	atomic_set(&intr->state, RUN);

	intr->work_queue = create_workqueue("work_queue");
	if (IS_ERR_OR_NULL(intr->work_queue)) {
		dev_err(&interface->dev, "Create interrupt workqueue failed");
		atomic_set(&intr->state, STOP);
		usb_free_urb(intr->urb);
		return PTR_ERR(intr->work_queue);
	}
	intr->interface = interface;
	intr->pipe = usb_rcvintpipe(usb_dev, usb_endpoint_num(desc));
	intr->interval = desc->bInterval;

	usb_set_intfdata(interface, intr);

	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		dev_err(&intr->interface->dev, "URB submission failed");
		atomic_set(&intr->state, STOP);
		destroy_workqueue(intr->work_queue);
		usb_free_urb(intr->urb);
		return ret;
	}

	fl2000_debugfs_intr_init();

	return 0;
}

void fl2000_intr_destroy(struct usb_interface *interface)
{
	struct fl2000_intr *intr = usb_get_intfdata(interface);

	if (intr == NULL || atomic_read(&intr->state) != RUN)
		return;

	atomic_set(&intr->state, STOP);
	usb_kill_urb(intr->urb);
	drain_workqueue(intr->work_queue);
	destroy_workqueue(intr->work_queue);
	usb_free_urb(intr->urb);
}
