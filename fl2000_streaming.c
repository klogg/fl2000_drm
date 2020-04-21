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

#include "fl2000_testframe.h"

void fl2000_handle_vblank(struct drm_simple_display_pipe *pipe);

/* Streaming is implemented with a single URB for each frame. USB is configured
 * to send NULL URB automatically after each data URB */
struct fl2000_stream {
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct urb *urb, *zero_len_urb;
};

#define FL2000_DBG_FRAME_SIZE		1152000
#define FL2000_DBG_BYTES_INTERVAL	49152
#define FL2000_DBG_PACKETS		24

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work,
			struct fl2000_stream, work);
	struct urb *urb;

	if (!stream)
		return;

	urb = stream->urb;
	if (!urb)
		return;

	fl2000_handle_vblank(urb->context);

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&urb->dev->dev, "Data URB error %d", ret);
	}

	ret = usb_submit_urb(stream->zero_len_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&urb->dev->dev, "Zero length URB error %d", ret);
	}
}

static void fl2000_stream_completion(struct urb *urb)
{
}

static void fl2000_stream_zero_len_completion(struct urb *urb)
{
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	INIT_WORK(&stream->work, &fl2000_stream_work);
	queue_work(stream->work_queue, &stream->work);
}

int fl2000_stream_mode_set(struct usb_device *usb_dev)
{
	int ret;
	u8 *buf;
	dma_addr_t buf_addr;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	ret = usb_set_interface(usb_dev, 0, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface " \
				"altstting for ISO transfers");
		return ret;
	}

	/* There shall be no URB active on "mode_set" */
	if (stream->urb || stream->zero_len_urb)
		return -EBUSY;

	/* XXX: Right now we have an intermediate buffer that keeps the frame
	 * between updates to buffer. This can probably be improved with proper
	 * GEM implementation */
	buf = usb_alloc_coherent(usb_dev, FL2000_DBG_FRAME_SIZE, GFP_KERNEL,
			&buf_addr);
	if (!buf) {
		dev_err(&usb_dev->dev, "Allocate stream FB buffer failed");
		return -ENOMEM;
	}

	stream->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->urb) {
		dev_err(&usb_dev->dev, "Allocate data URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			buf, FL2000_DBG_FRAME_SIZE,
			fl2000_stream_completion, NULL);
	stream->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* Zero-length URB must be sent after FB data to indicate EOF */
	stream->zero_len_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->zero_len_urb) {
		dev_err(&usb_dev->dev, "Allocate zero length URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->zero_len_urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_zero_len_completion, NULL);

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

	memcpy(stream->urb->transfer_buffer, packet_bytes, ARRAY_SIZE(packet_bytes));

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

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
	}

	ret = usb_submit_urb(stream->zero_len_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "Zero length URB error %d", ret);
	}

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
