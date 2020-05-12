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
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define FL2000_FB_NUM			2

void fl2000_display_vblank(struct usb_device *usb_dev);

struct fl2000_fb {
     struct list_head list;
     void *buf;
     dma_addr_t buf_addr;
};

struct fl2000_stream {
	struct urb *urb, *zero_len_urb;
	struct usb_device *usb_dev;
	struct list_head fb_list;
	int bytes_pix;
};

static int fl2000_fb_list_alloc(struct fl2000_stream *stream)
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

static void fl2000_fb_list_free(struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor, *temp;

	list_for_each_entry_safe(cursor, temp, &stream->fb_list, list) {
		list_del(&cursor->list);
		kfree(cursor);
	}
}

static int fl2000_fb_get_buffers(struct usb_device *usb_dev,
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

static void fl2000_fb_put_buffers(struct usb_device *usb_dev,
		struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor;
	ssize_t size = stream->urb->transfer_buffer_length;
	list_for_each_entry(cursor, &stream->fb_list, list) {
		if (!cursor->buf)
			continue;
		usb_free_coherent(usb_dev, size, cursor->buf,
				cursor->buf_addr);
		cursor->buf = NULL;
	}
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_stream_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	ret = fl2000_urb_status(usb_dev, urb);
	if (ret) {
		dev_err(&usb_dev->dev, "Stopping streaming");
		return;
	}

	ret = usb_submit_urb(stream->zero_len_urb, GFP_KERNEL);
	if (ret && ret != -EPERM) {
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

	ret = fl2000_urb_status(usb_dev, urb);
	if (ret) {
		dev_err(&usb_dev->dev, "Stopping streaming");
		return;
	}

	/* Process vblank */
	fl2000_display_vblank(stream->usb_dev);

	cursor = list_first_entry(&stream->fb_list, struct fl2000_fb, list);
	stream->urb->transfer_buffer = cursor->buf;

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret && ret != -EPERM) {
		dev_err(&usb_dev->dev, "Data URB error %d", ret);
	}
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf,
		u32 pixels)
{
	unsigned int x, xx = 0;

	for (x = 0; x < pixels; x++) {
		dbuf[xx++^4] = (sbuf[x] & 0x000000FF) >>  0;
		dbuf[xx++^4] = (sbuf[x] & 0x0000FF00) >>  8;
		dbuf[xx++^4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf,
		u32 pixels)
{
	unsigned int x;

	for (x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) |
			((sbuf[x] & 0x0000FC00) >> 5) |
			((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

void fl2000_stream_compress(struct usb_device *usb_dev,
		struct drm_framebuffer *fb, void *src)
{
	struct fl2000_fb *cursor;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	unsigned int y;
	void *dst;
	u32 dst_line_len = fb->width * stream->bytes_pix;

	list_rotate_left(&stream->fb_list);
	cursor = list_first_entry(&stream->fb_list, struct fl2000_fb, list);
	dst = cursor->buf;

	for (y = 0; y < fb->height; y++) {
		switch (stream->bytes_pix) {
		case 2:
			fl2000_xrgb888_to_rgb565_line(dst, src, fb->width);
			break;
		case 3:
			fl2000_xrgb888_to_rgb888_line(dst, src, fb->width);
			break;
		default: /* Shouldn't happen */
			break;
		}
		src += fb->pitches[0];
		dst += dst_line_len;
	}
}

int fl2000_stream_mode_set(struct usb_device *usb_dev, int pixels,
		u32 bytes_pix)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	ssize_t size;

	if (!stream)
		return -ENODEV;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(ssize_t)7;

	/* If there are buffers with same size - keep them */
	if (stream->urb->transfer_buffer_length == size &&
			stream->bytes_pix == bytes_pix)
		return 0;

	stream->urb->transfer_buffer_length = size;
	stream->bytes_pix = bytes_pix;

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

	ret = usb_submit_urb(stream->urb, GFP_KERNEL);
	if (ret && ret != -EPERM) {
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

	return ret;
}

void fl2000_stream_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (stream) {
		fl2000_fb_put_buffers(usb_dev, stream);

		usb_free_urb(stream->urb);
		usb_free_urb(stream->zero_len_urb);
	}

	fl2000_fb_list_free(stream);

	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
