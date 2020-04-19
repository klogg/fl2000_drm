/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_streaming.c
 *
 * Original driver uses default altsetting (#0) of streaming interface, which
 * allows bursts of bulk transfers of 15x1024 bytes on output. But the HW
 * actually works incorrectly here: it uses same endpoint #1 across interfaces
 * 1 and 2, which is not allowed by USB specification: endpoint addresses can be
 * shared only between alternate settings, not interfaces. In order to
 * workaround this we use isochronous transfers instead of bulk. There is a
 * possibility that we still can use bulk transfers with interface 0, but this
 * is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

void fl2000_handle_vblank(struct drm_simple_display_pipe *pipe);

/* Streaming is implemented with a single URB for each frame. USB is configured
 * to send NULL URB automatically after each data URB */
struct fl2000_stream {
	struct work_struct work, fbk_work;
	struct workqueue_struct *work_queue;
	struct urb *urb, *fbk_urb;
	struct hrtimer timer;
};

#define FL2000_DBG_TIMEOUT   (15151515UL) // 66FPS
enum hrtimer_restart fl2000_stream_timer(struct hrtimer *timer)
{
	int ret;
	struct fl2000_stream *stream = container_of(timer,
			struct fl2000_stream, timer);
	struct usb_device *usb_dev;
	struct urb *urb;

	hrtimer_forward_now(timer, ktime_set(0, FL2000_DBG_TIMEOUT));

	urb = stream->urb;

	if (!urb)
		return HRTIMER_NORESTART;

	usb_dev = urb->dev;

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		dev_err(&urb->dev->dev, "URB error %d", ret);
	}

	return HRTIMER_RESTART;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_work(struct work_struct *work_item)
{
	int ret;
	struct fl2000_stream *stream = container_of(work_item,
			struct fl2000_stream, work);
	struct usb_device *usb_dev;
	struct urb *urb;

	if (!stream)
		return;

	/* Get feedback */
	urb = stream->fbk_urb;

	if (!urb)
		return;

	usb_dev = urb->dev;

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		dev_err(&urb->dev->dev, "URB error %d", ret);
	}
}

static void fl2000_stream_completion(struct urb *urb)
{
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	fl2000_handle_vblank(urb->context);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	queue_work(stream->work_queue, &stream->work);
}


static void fl2000_stream_feedback_work(struct work_struct *work_item)
{
	/* TODO: based on feedback adjust somehow next frame? */
}

static void fl2000_stream_feedback_completion(struct urb *urb)
{
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	INIT_WORK(&stream->fbk_work, &fl2000_stream_feedback_work);
	queue_work(stream->work_queue, &stream->fbk_work);
}

#define FL2000_DBG_FRAME_SIZE		1152000
#define FL2000_DBG_BYTES_INTERVAL	49152
#define FL2000_DBG_PACKETS		24

int fl2000_stream_mode_set(struct usb_device *usb_dev)
{
	int ret, i, j;
	u8 *buf;
	dma_addr_t transfer_dma;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct urb *urb;

	if (!stream)
		return -ENODEV;

	/* TODO: Probably it would be better to use different altsettings for
	 * different display configurations. For now lets take altsetting with
	 * highest possible throughput */
	ret = usb_set_interface(usb_dev, 1, 6);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface " \
				"altstting for ISO transfers");
		return ret;
	}

	/* There shall be no URB active on "mode_set" */
	if (stream->urb)
		return -EBUSY;

	buf = usb_alloc_coherent(usb_dev, FL2000_DBG_FRAME_SIZE,
			GFP_KERNEL, &transfer_dma);
	if (!buf) {
		dev_err(&usb_dev->dev, "Allocate stream FB buffer failed");
		return -ENOMEM;
	}

	urb = usb_alloc_urb(FL2000_DBG_PACKETS+1, GFP_KERNEL);
	if (!urb) {
		dev_err(&usb_dev->dev, "Allocate stream FB URB failed");
		return -ENOMEM;
	}
	urb->number_of_packets = FL2000_DBG_PACKETS+1;

	usb_fill_int_urb(urb, usb_dev,
			usb_sndisocpipe(usb_dev, 2),
			buf, FL2000_DBG_FRAME_SIZE,
			fl2000_stream_completion, NULL,
			1);

	for (i = 0; i < FL2000_DBG_PACKETS; i++) {
		int off, rem;
		off = i * FL2000_DBG_BYTES_INTERVAL;
		rem = FL2000_DBG_FRAME_SIZE - off;
		rem = rem < FL2000_DBG_BYTES_INTERVAL ? rem : FL2000_DBG_BYTES_INTERVAL;

		urb->iso_frame_desc[i].length = rem;
		urb->iso_frame_desc[i].offset = off;

		/* R - G - B pattern */
		for (j = off; j < off + rem; j+=3) {
			buf[j] = 0xFF;
			buf[j+1] = 0;
			buf[j+2] = 0;
		}
	}
	urb->iso_frame_desc[FL2000_DBG_PACKETS].length = 0;
	urb->iso_frame_desc[FL2000_DBG_PACKETS].offset = 0;

	urb->transfer_dma = transfer_dma;
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	stream->urb = urb;

	/* Feedback EP */

	buf = usb_alloc_coherent(usb_dev, 8, GFP_KERNEL, &transfer_dma);
	if (!buf) {
		dev_err(&usb_dev->dev, "Allocate stream feedback buffer failed");
		return -ENOMEM;
	}

	urb = usb_alloc_urb(1, GFP_KERNEL);
	if (!urb) {
		dev_err(&usb_dev->dev, "Allocate stream feedback URB failed");
		return -ENOMEM;
	}
	urb->number_of_packets = 1;

	usb_fill_int_urb(urb, usb_dev,
			usb_rcvisocpipe(usb_dev, 2),
			buf, 8,
			fl2000_stream_feedback_completion, NULL,
			7);

	urb->iso_frame_desc[0].length = 8;
	urb->iso_frame_desc[0].offset = 0;

	urb->transfer_dma = transfer_dma;
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	stream->fbk_urb = urb;

	/* Timer for packet sending */
	hrtimer_init(&stream->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	stream->timer.function = fl2000_stream_timer;

	return 0;
}


int fl2000_stream_update(struct usb_device *usb_dev, dma_addr_t addr,
		size_t fb_size, struct drm_simple_display_pipe *pipe)
{
	int ret = 0;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	/* XXX: Maybe we need to check timings or lbuf? */

	stream->urb->context = pipe;

	return ret;
}

int fl2000_stream_enable(struct usb_device *usb_dev)
{
	int ret = 0;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	hrtimer_start(&stream->timer, ktime_set(0, FL2000_DBG_TIMEOUT), HRTIMER_MODE_REL);

	return ret;
}

void fl2000_stream_disable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	usb_kill_urb(stream->urb);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because
 * we do not know the stream requirements yet.
 *
 * Return: Operation result
 */
int fl2000_stream_create(struct usb_interface *interface)
{
	int ret = 0;
	struct fl2000_stream *stream;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream),
			GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, stream);

	/* No URB allocated so far */
	stream->urb = NULL;


	stream->work_queue = create_workqueue("fl2000_stream");

	dev_info(&usb_dev->dev, "Streaming interface up");

	return ret;
}

void fl2000_stream_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	usb_kill_urb(stream->urb);
	usb_free_urb(stream->urb);

	destroy_workqueue(stream->work_queue);

	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
