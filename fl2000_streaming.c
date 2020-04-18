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
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct urb *urb;
};

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_work(struct work_struct *work_item)
{
	int i, ret;
	struct fl2000_stream *stream = container_of(work_item,
			struct fl2000_stream, work);
	struct urb *urb;
	struct usb_device *usb_dev;

	static int counter = 3;

	if (!stream)
		return;

	urb = stream->urb;
	usb_dev = urb->dev;

	if (counter) {
		counter--;
		dev_info(&usb_dev->dev, "URB: transmitted length %d, frame %d",
				urb->actual_length, urb->start_frame);
		for (i = 0; i < urb->number_of_packets; i++) {
			dev_info(&usb_dev->dev, "Descriptor %d: transmitted length %d, status %d",
					i,
					urb->iso_frame_desc[i].actual_length,
					urb->iso_frame_desc[i].status);
		}
	}

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		dev_err(&urb->dev->dev, "URB error %d", ret);
		fl2000_handle_vblank(urb->context);
	}

	fl2000_handle_vblank(urb->context);

	if (!urb)
		return;

}

static void fl2000_stream_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	INIT_WORK(&stream->work, &fl2000_stream_work);
	queue_work(stream->work_queue, &stream->work);

}

#define FL2000_DBG_FRAME_SIZE		1152000
#define FL2000_DBG_BYTES_INTERVAL	49152
#define FL2000_DBG_PACKETS		24

int fl2000_stream_mode_set(struct usb_device *usb_dev)
{
	int pipe, ret, i, j;
	u8 *buf;
	dma_addr_t transfer_dma;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

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
	pipe = usb_sndisocpipe(usb_dev, 2);

	/* There shall be no URB active on "mode_set" */
	if (stream->urb)
		return -EBUSY;

	buf = usb_alloc_coherent(usb_dev, FL2000_DBG_FRAME_SIZE,
			GFP_KERNEL, &transfer_dma);
	if (!buf) {
		dev_err(&usb_dev->dev, "Allocate stream FB buffer failed");
		return -ENOMEM;
	}

	stream->urb = usb_alloc_urb(FL2000_DBG_PACKETS, GFP_KERNEL);
	if (!stream->urb) {
		dev_err(&usb_dev->dev, "Allocate stream FB URB failed");
		return -ENOMEM;
	}
	stream->urb->number_of_packets = FL2000_DBG_PACKETS;

	for (i = 0; i < FL2000_DBG_PACKETS; i++) {
		int off, rem;
		off = i * FL2000_DBG_BYTES_INTERVAL;
		rem = FL2000_DBG_FRAME_SIZE - off;
		rem = rem < FL2000_DBG_BYTES_INTERVAL ? rem : FL2000_DBG_BYTES_INTERVAL;

		stream->urb->iso_frame_desc[i].length = rem;
		stream->urb->iso_frame_desc[i].offset = off;

		for (j = off; j < off + rem; j++)
			buf[j] = i;
	}

	usb_fill_int_urb(stream->urb, usb_dev,
			pipe,
			buf, FL2000_DBG_FRAME_SIZE,
			fl2000_stream_completion, stream,
			1);
	stream->urb->transfer_dma = transfer_dma;
	stream->urb->transfer_flags |= URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;

	return 0;
}


int fl2000_stream_update(struct usb_device *usb_dev, dma_addr_t addr,
		size_t fb_size, struct drm_simple_display_pipe *pipe)
{
	int ret;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	/* XXX: Maybe we need to check timings or lbuf? */

	stream->urb->context = pipe;

	return 0;
}

int fl2000_stream_enable(struct usb_device *usb_dev)
{
	int ret = 0;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	{
		struct usb_host_endpoint *ep;
		ep = usb_pipe_endpoint(usb_dev, stream->urb->pipe);
		dev_info(&usb_dev->dev, "address %d, type %d, dir %d, interval %d, bytes per interval %d, speed %d",
				ep->desc.bEndpointAddress,
				usb_endpoint_type(&ep->desc),
				usb_endpoint_dir_out(&ep->desc),
				stream->urb->interval,
				ep->ss_ep_comp.wBytesPerInterval,
				usb_dev->speed);
	}

	ret = usb_submit_urb(stream->urb, GFP_ATOMIC);
	if (ret) {
		dev_err(&usb_dev->dev, "URB error %d", ret);
		fl2000_handle_vblank(stream->urb->context);
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
	int ret;
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

	return 0;
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
