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

	if (atomic_read(&intr->state) != RUN) return;

	/* Process interrupt */
	if (regmap) {
		ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status);
		if (ret != 0) {
			dev_err(&usb_dev->dev, "Cannot read interrupt" \
					"register (%d)", ret);
		} else {
			fl2000_debugfs_intr_status(status);
		}
	}

	/* Restart urb */
	ret = fl2000_intr_submit_urb(intr);
	if (ret != 0) {
		atomic_set(&intr->state, STOP);
		dev_err(&intr->interface->dev, "URB submission failed");
	}
}

static void fl2000_intr_completion(struct urb *urb)
{
	struct fl2000_intr *intr = urb->context;
	struct usb_device *usb_dev = interface_to_usbdev(intr->interface);

	dev_info(&usb_dev->dev, " Interrupt!!!");

	if (intr == NULL) return;

	if (atomic_read(&intr->state) != RUN) return;

	INIT_WORK(&intr->work, &fl2000_intr_work);

	queue_work(intr->work_queue, &intr->work);
}

void fl2000_intr_destroy(struct usb_interface *interface);

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
	if (IS_ERR_OR_NULL(intr)) {
		dev_err(&interface->dev, "Cannot allocate interrupt private" \
				"structure");
		ret = PTR_ERR(intr);
		goto error;
	}

	intr->buf = kzalloc(INTR_BUFSIZE, GFP_DMA);
	if (IS_ERR_OR_NULL(intr->buf)) {
		dev_err(&interface->dev, "Cannot allocate interrupt data");
		ret = PTR_ERR(intr->buf);
		goto error;
	}

	intr->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (IS_ERR_OR_NULL(intr->urb)) {
		dev_err(&interface->dev, "Allocate interrupt URB failed");
		ret = PTR_ERR(intr->urb);
		goto error;
	}

	intr->work_queue = create_workqueue("work_queue");
	if (IS_ERR_OR_NULL(intr->work_queue)) {
		dev_err(&interface->dev, "Create interrupt workqueue failed");
		ret = PTR_ERR(intr->work_queue);
		goto error;
	}
	intr->interface = interface;
	intr->pipe = usb_rcvintpipe(usb_dev, usb_endpoint_num(desc));
	intr->interval = desc->bInterval;
	atomic_set(&intr->state, RUN);

	usb_set_intfdata(interface, intr);

	fl2000_intr_work(&intr->work);

	fl2000_debugfs_intr_init();

	return 0;
error:
	/* Enforce cleanup in case of error */
	fl2000_intr_destroy(interface);
	return ret;
}

void fl2000_intr_destroy(struct usb_interface *interface)
{
	struct fl2000_intr *intr = usb_get_intfdata(interface);

	if (IS_ERR_OR_NULL(intr))
		return;

	atomic_set(&intr->state, STOP);

	if (!IS_ERR_OR_NULL(intr->urb))
		usb_kill_urb(intr->urb);

	if (!IS_ERR_OR_NULL(intr->work_queue))
		drain_workqueue(intr->work_queue);

	if (!IS_ERR_OR_NULL(intr->work_queue))
		destroy_workqueue(intr->work_queue);

	if (!IS_ERR_OR_NULL(intr->urb))
		usb_free_urb(intr->urb);

	usb_set_intfdata(interface, NULL);

	if (!IS_ERR_OR_NULL(intr->buf))
		kfree(intr->buf);

	kfree(intr);
}
