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

void fl2000_display_vblank(struct usb_device *usb_dev);

/* Streaming is implemented with a single URB for each frame. USB is configured
 * to send NULL URB automatically after each data URB */

#define FL2000_FB_NUM	2

struct fl2000_fb {
     struct list_head list;
     void *buf;
     dma_addr_t buf_addr;
};

struct fl2000_stream {
	struct urb *urb, *zero_len_urb;
	struct usb_device *usb_dev;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct list_head fb_list;
};

static inline int fl2000_fb_list_alloc(struct fl2000_stream *stream)
{
	int i;

	INIT_LIST_HEAD(&stream->fb_list);

	for (i = 0; i < FL2000_FB_NUM; i++) {
		/* TODO: Check malloc failure */
		struct fl2000_fb *fb = kzalloc(sizeof(struct fl2000_fb),
				GFP_KERNEL);

		INIT_LIST_HEAD(&fb->list);
		list_add(&fb->list, &stream->fb_list);
	}

	return 0;
}

static inline void fl2000_fb_list_free(struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor, *temp;

	list_for_each_entry_safe(cursor, temp, &stream->fb_list, list) {
		list_del(&cursor->list);
		kfree(cursor);
	}
}

static inline int fl2000_fb_get_buffers(struct usb_device *usb_dev,
		struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor;
	ssize_t size = stream->urb->transfer_buffer_length;
	list_for_each_entry(cursor, &stream->fb_list, list) {
		/* TODO: Signal error */
		if (cursor->buf)
			continue;
		/* TODO: Check malloc failure */
		cursor->buf = usb_alloc_coherent(usb_dev, size, GFP_KERNEL,
				&cursor->buf_addr);
	}

	return 0;
}

static inline void fl2000_fb_put_buffers(struct usb_device *usb_dev,
		struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor;
	ssize_t size = stream->urb->transfer_buffer_length;
	list_for_each_entry(cursor, &stream->fb_list, list) {
		if (!cursor->buf)
			continue;
		usb_free_coherent(usb_dev, size, cursor->buf,
				cursor->buf_addr);
	}
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_work(struct work_struct *work)
{
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream,
			work);

	fl2000_display_vblank(stream->usb_dev);
}

static void fl2000_stream_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	switch (urb->status) {
	/* All went well */
	case 0:
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

	ret = usb_submit_urb(stream->zero_len_urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Zero length URB error %d", ret);
	}
}

static void fl2000_stream_zero_len_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct fl2000_fb *cursor;

	switch (urb->status) {
	/* All went well */
	case 0:
		/* Process vblank */
		INIT_WORK(&stream->work, &fl2000_stream_work);
		queue_work(stream->work_queue, &stream->work);
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

	cursor = list_first_entry(&stream->fb_list, struct fl2000_fb, list);
	stream->urb->transfer_buffer = cursor->buf;
	list_rotate_left(&stream->fb_list);

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
	}
}

void fl2000_framebuffer_decompress(struct usb_device *usb_dev,
		struct drm_framebuffer *fb, u32 *src)
{
	struct fl2000_fb *cursor;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	unsigned int x, y;
	u8 *dst;

	cursor = list_first_entry(&stream->fb_list, struct fl2000_fb, list);
	dst = cursor->buf;

	switch (fb->format->format) {
	case DRM_FORMAT_RGB888:
		for (y = 0; y < fb->height; y++) {
			memcpy(dst, src, fb->width * 3);
			src += fb->pitches[0] / 4;
			dst += fb->width * 3;
		}
		break;
	case DRM_FORMAT_XRGB8888:
		for (y = 0; y < fb->height; y++) {
			for (x = 0; x < fb->width; x++) {
				*dst++ = (src[x] & 0x000000FF) >>  0;
				*dst++ = (src[x] & 0x0000FF00) >>  8;
				*dst++ = (src[x] & 0x00FF0000) >> 16;
			}
			src += fb->pitches[0] / 4;
		}
		break;
	default:
		/* Unknown format, do nothing */
		break;
	}
}

int fl2000_stream_mode_set(struct usb_device *usb_dev, ssize_t size)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	/* If there are buffers with same size - keep them */
	if (stream->urb->transfer_buffer_length == size)
		return 0;

	stream->urb->transfer_buffer_length = size;

	/* Destroy wrong size buffers if they exist */
	if (stream->urb->transfer_buffer_length != 0)
		fl2000_fb_put_buffers(usb_dev, stream);

	/* Allocate new buffers */
	fl2000_fb_get_buffers(usb_dev, stream);

	return 0;
}

int fl2000_stream_enable(struct usb_device *usb_dev)
{
	int ret = 0;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct fl2000_fb *cursor;

	if (!stream)
		return -ENODEV;

	cursor = list_first_entry(&stream->fb_list, struct fl2000_fb, list);
	stream->urb->transfer_buffer = cursor->buf;
	list_rotate_left(&stream->fb_list);

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret) {
		/* TODO: handle USB errors in ret */
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
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
	usb_kill_urb(stream->zero_len_urb);
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

	ret = usb_set_interface(usb_dev, 0, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface " \
				"altstting for bulk transfers");
		return ret;
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream),
			GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, stream);

	stream->work_queue = create_workqueue("fl2000_streaming");
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Create streaming workqueue failed");
		return -ENOMEM;
	}

	stream->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->urb) {
		dev_err(&usb_dev->dev, "Allocate data URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_completion, NULL);

	stream->zero_len_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->zero_len_urb) {
		dev_err(&usb_dev->dev, "Allocate zero length URB failed");
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->zero_len_urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_zero_len_completion, NULL);

	stream->usb_dev = usb_dev;

	fl2000_fb_list_alloc(stream);

	dev_info(&usb_dev->dev, "Streaming interface up");

	return ret;
}

void fl2000_stream_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	struct urb *urb;

	if (stream) {
		urb = stream->urb;

		fl2000_fb_put_buffers(usb_dev, stream);

		usb_free_urb(stream->urb);
		usb_free_urb(stream->zero_len_urb);

		destroy_workqueue(stream->work_queue);
	}

	fl2000_fb_list_free(stream);

	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
