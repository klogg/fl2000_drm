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

#define FL2000_FB_NUM		2

#define FL2000_URB_TIMEOUT	100

void fl2000_display_vblank(struct usb_device *usb_dev);

struct fl2000_fb {
	struct list_head list;
	void *buf;
	struct sg_table *sgt;
};

struct fl2000_stream {
	struct urb *zero_len_urb;
	struct usb_sg_request data_request;
	struct usb_device *usb_dev;
	/* TODO: Add spinlock for list protection */
	struct list_head fb_list;
	size_t buf_size;
	int bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct timer_list sg_timer;
};

/* Copied from drivers/gpu/drm/virtio/virtgpu_vq.c
 * (C) David Riley <davidriley@chromium.org> */
static struct sg_table *vmalloc_to_sgt(char *data, uint32_t size, int *sg_ents)
{
	int ret, s, i;
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct page *pg;

	if (WARN_ON(!PAGE_ALIGNED(data)))
		return NULL;

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;

	*sg_ents = DIV_ROUND_UP(size, PAGE_SIZE);
	ret = sg_alloc_table(sgt, *sg_ents, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return NULL;
	}

	for_each_sg(sgt->sgl, sg, *sg_ents, i) {
		pg = vmalloc_to_page(data);
		if (!pg) {
			sg_free_table(sgt);
			kfree(sgt);
			return NULL;
		}

		s = min_t(int, PAGE_SIZE, size);
		sg_set_page(sg, pg, s, 0);

		size -= s;
		data += s;
	}

	return sgt;
}

static int fl2000_fb_get_buffers(struct usb_device *usb_dev,
		struct fl2000_stream *stream, size_t size)
{
	int i;
	struct fl2000_fb *cursor;
	int sg_ents;

	if (!list_empty(&stream->fb_list))
		return -1;

	for (i = 0; i < FL2000_FB_NUM; i++) {
		cursor = kzalloc(sizeof(struct fl2000_fb), GFP_KERNEL);
		if (!cursor)
			return -ENOMEM;

		INIT_LIST_HEAD(&cursor->list);
		list_add(&cursor->list, &stream->fb_list);

		cursor->buf = kvmalloc(size, GFP_KERNEL | GFP_DMA);
		if (!cursor->buf)
			return -ENOMEM;

		if (is_vmalloc_addr(cursor->buf)) {
			cursor->sgt = vmalloc_to_sgt(cursor->buf, size, &sg_ents);
			if (!cursor->sgt)
				return -ENOMEM;

			/* Large buffers can be sent only via scatterlists.
			 * Device expects a single URB for bulk data transfer
			 * so if host controller cannot do scatter-gather DMA
			 * driver wont work
			 * */
			if (sg_ents > 1 && !usb_dev->bus->sg_tablesize)
				return -EIO;
		} else
			cursor->sgt = NULL;
	}

	return 0;
}

static void fl2000_fb_put_buffers(struct usb_device *usb_dev,
		struct fl2000_stream *stream)
{
	struct fl2000_fb *cursor, *temp;
	list_for_each_entry_safe(cursor, temp, &stream->fb_list, list) {
		if (cursor->sgt) {
			sg_free_table(cursor->sgt);
			kfree(cursor->sgt);
			cursor->sgt = NULL;
		}

		if (cursor->buf) {
			kvfree(cursor->buf);
			cursor->buf = NULL;
		}

		list_del(&cursor->list);
		kfree(cursor);
	}
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	/* Noop */
}

static void fl2000_sg_timed_out(struct timer_list *timer)
{
	struct fl2000_stream *stream = from_timer(stream, timer, sg_timer);

	dev_err(&stream->usb_dev->dev, "Data transfer timed out");

	usb_sg_cancel(&stream->data_request);
}

static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream,
			work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_fb *cursor;

	cursor = list_last_entry(&stream->fb_list, struct fl2000_fb, list);
	if (!cursor)
		return;

	if (cursor->sgt) {
		ret = usb_sg_init(&stream->data_request, stream->usb_dev,
				usb_sndbulkpipe(usb_dev, 1), 0,
				cursor->sgt->sgl, cursor->sgt->nents,
				stream->buf_size, GFP_KERNEL);
		if (ret)
			return;

		stream->sg_timer.expires = jiffies +
				msecs_to_jiffies(FL2000_URB_TIMEOUT);
		add_timer(&stream->sg_timer);

		usb_sg_wait(&stream->data_request);

		if (!del_timer_sync(&stream->sg_timer))
			ret = -ETIME;
		else
			ret = stream->data_request.status;
	} else {
		int actual_length;
		ret = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, 1),
			cursor->buf, stream->buf_size, &actual_length,
			FL2000_URB_TIMEOUT);
	}

	ret = fl2000_urb_status(usb_dev, ret, usb_sndbulkpipe(usb_dev, 1));
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

	ret = fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	if (ret) {
		/* TODO: Process EPERM correctly */
		dev_err(&usb_dev->dev, "Stopping streaming");
		return;
	}

	/* Process vblank */
	fl2000_display_vblank(stream->usb_dev);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	queue_work(stream->work_queue, &stream->work);
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

	list_rotate_left(&stream->fb_list);
}

int fl2000_stream_mode_set(struct usb_device *usb_dev, int pixels,
		u32 bytes_pix)
{
	int ret;
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);
	size_t size;

	if (!stream)
		return -ENODEV;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(size_t)7;

	stream->bytes_pix = bytes_pix;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->fb_list))
		fl2000_fb_put_buffers(usb_dev, stream);

	/* Allocate new buffers */
	ret = fl2000_fb_get_buffers(usb_dev, stream, size);
	if (ret) {
		fl2000_fb_put_buffers(usb_dev, stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return -ENODEV;

	INIT_WORK(&stream->work, &fl2000_stream_work);
	queue_work(stream->work_queue, &stream->work);

	return 0;
}

void fl2000_stream_disable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	cancel_work_sync(&stream->work);

	usb_sg_cancel(&stream->data_request);

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

	INIT_LIST_HEAD(&stream->fb_list);

	stream->zero_len_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!stream->zero_len_urb) {
		dev_err(&usb_dev->dev, "Allocate zero length URB failed");
		devres_free(stream);
		return -ENOMEM;
	}

	stream->work_queue = create_workqueue("fl2000_stream");
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		usb_free_urb(stream->zero_len_urb);
		devres_free(stream);
		return -ENOMEM;
	}

	usb_fill_bulk_urb(stream->zero_len_urb, usb_dev,
			usb_sndbulkpipe(usb_dev, 1),
			NULL, 0,
			fl2000_stream_zero_len_completion, NULL);

	stream->usb_dev = usb_dev;

	timer_setup(&stream->sg_timer, fl2000_sg_timed_out, 0);

	return ret;
}

void fl2000_stream_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_stream *stream = devres_find(&usb_dev->dev,
			fl2000_stream_release, NULL, NULL);

	if (!stream)
		return;

	destroy_workqueue(stream->work_queue);

	usb_free_urb(stream->zero_len_urb);

	fl2000_fb_put_buffers(usb_dev, stream);

	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
