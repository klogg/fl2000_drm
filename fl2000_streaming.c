// SPDX-License-Identifier: GPL-2.0
/*
 * Original driver uses default altsetting (#0) of streaming interface, which allows bursts of bulk
 * transfers of 15x1024 bytes on output. But the HW actually works incorrectly here: it uses same
 * endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification: endpoint
 * addresses can be shared only between alternate settings, not interfaces. In order to workaround
 * this we use isochronous transfers instead of bulk. There is a possibility that we still can use
 * bulk transfers with interface 0, but this is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

/* Triple buffering:
 *  - one buffer for HDMI rendering
 *  - one buffer for USB transmission
 *  - one buffer for DRM/KMS data copy
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

#define FL2000_URB_TIMEOUT 100

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	/* Each buffer journey: render->transmit->wait->... */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* List access from bh and interrupt contexts */
	size_t buf_size;
	u32 bytes_pix;
	struct usb_anchor anchor;
	atomic_t urb_cnt;
	bool enabled;
};

static inline int fl2000_urb_status(struct usb_device *usb_dev, int status, int pipe)
{
	int ret = status;

	switch (status) {
	/* Stalled endpoint */
	case -EPIPE:
		ret = usb_clear_halt(usb_dev, pipe);
		break;
	case -ECONNRESET:
		fallthrough;
	case -ENOENT:
		fallthrough;
	case -ESHUTDOWN:
		/* Not an error */
		ret = 0;
		break;
	default:
		dev_err(&usb_dev->dev, "Nonzero urb status, %d\n", status);
		break;
	}

	return ret;
}

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kcalloc(nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0) /* TODO: Maybe check error? */
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);
	return NULL;
}

static void fl2000_put_buffers(struct list_head *buffers_list)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, buffers_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_get_buffers(struct list_head *buffers_list, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	if (!list_empty(buffers_list)) {
		/* Try fixing non-empty list putting buffers back */
		fl2000_put_buffers(buffers_list);

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, buffers_list);
	}

	return 0;

error:
	fl2000_put_buffers(buffers_list);
	return ret;
}

static void fl2000_data_completion(struct urb *urb);

/* TODO: use anchors more wisely */
static int fl2000_send_stream(struct usb_device *usb_dev, struct fl2000_stream *stream)
{
	if (stream->enabled) do {
		int ret;
		struct fl2000_stream_buf *cur_sb;
		struct fl2000_stream_buf *last_sb;
		struct urb *data_urb;

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			return -ENOMEM;
		}

		spin_lock_irq(&stream->list_lock);

		/* If no buffers are available for immediate transmission - then copy latest
		 * transmission data
		 */
		if (list_empty(&stream->transmit_list)) {
			if (list_empty(&stream->wait_list))
				last_sb = list_last_entry(&stream->render_list, struct fl2000_stream_buf, list);
			else
				last_sb = list_last_entry(&stream->wait_list, struct fl2000_stream_buf, list);
			cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf,
							list);
			if (cur_sb && last_sb) /* Both non-NULL */
				memcpy(cur_sb->vaddr, last_sb->vaddr, stream->buf_size);
		} else {
			cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		}
		list_move_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock(&stream->list_lock);

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, STREAMING_EP), cur_sb,
					(int)stream->buf_size, fl2000_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);

			spin_lock_irq(&stream->list_lock);
			list_move_tail(&cur_sb->list, &stream->render_list);
			spin_unlock(&stream->list_lock);

			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);

			/* NOTE: actually in some cases we can try and resend the URB (-EAGAIN, some -ENOMEM) */
			return ret;
		}
	} while (atomic_dec_and_test(&stream->urb_cnt));

	return 0;
}

static void fl2000_data_completion(struct urb *urb)
{
	int ret;
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;

	spin_lock_irq(&stream->list_lock);
	list_move_tail(&cur_sb->list, &stream->render_list);
	spin_unlock(&stream->list_lock);

	atomic_inc(stream->urb_cnt);

	fl2000_drm_vblank(usb_dev);

	ret = fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	if (ret == 0)
		ret = fl2000_send_stream(usb_dev, stream);
	/* TODO: Signal fault to system and start shutdown of usb_dev in case of non-0 'ret' */

	usb_unanchor_urb(urb);
	usb_free_urb(urb);
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int xx = 0;

	for (unsigned int x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

static void fl2000_streaming_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	UNUSED(dev);

	fl2000_streaming_disable(stream);

	fl2000_put_buffers(&stream->render_list);
}

/**
 * fl2000_streaming_compress() - compress XRGB888 data to RGB565 or RGB888
 * 
 * @usb_dev: USB device
 * @src: Source buffer
 * @height: Image height
 * @width: Image width
 * @pitch: Image pitch
 */
void fl2000_streaming_compress(struct usb_device *usb_dev, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	void *dst;
	u32 dst_line_len;
	struct fl2000_stream *stream;

	stream = devres_find(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot find streaming context");
		return;
	}

	if (list_empty(&stream->render_list)
		return;

	spin_lock_irq(&stream->list_lock);

	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	dst = cur_sb->vaddr;
	dst_line_len = width * stream->bytes_pix;

	for (unsigned int y = 0; y < height; y++) {
		switch (stream->bytes_pix) {
		case 2:
			fl2000_xrgb888_to_rgb565_line(dst, src, width);
			break;
		case 3:
			fl2000_xrgb888_to_rgb888_line(dst, src, width);
			break;
		default: /* Shouldn't happen */
			break;
		}
		src += pitch;
		dst += dst_line_len;
	}

	list_move_tail(&cur_sb->list, &stream->transmit_list);
	spin_unlock(&stream->list_lock);
}

/**
 * fl2000_streaming_mode_set() - streaming mode setup
 * 
 * @usb_dev: USB device
 * @pixels: Number of pixels in a line
 * @bytes_pix: Bytes per pixel
 * 
 * @returns: Operation result
 */
int fl2000_streaming_mode_set(struct usb_device *usb_dev, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;
	struct fl2000_stream *stream;

	stream = devres_find(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot find streaming context");
		return -ENODEV;
	}

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(unsigned int)7;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Allocate new buffers possibly releasing old ones */
	ret = fl2000_get_buffers(&stream->render_list, size);
	if (ret) {
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;
	stream->bytes_pix = bytes_pix;

	return 0;
}

/**
 * fl2000_streaming_enable() - streaming processing start
 * 
 * @usb_dev: USB device
 * 
 * @returns: Operation result
 */
int fl2000_streaming_enable(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_stream *stream;

	stream = devres_find(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot find streaming context");
		return -ENODEV;
	}

	if (list_empty(&stream->transmit_list)) {
		dev_err(&usb_dev->dev, "No buffers for streaming");
		return -ENOMEM;
	}

	if (atomic_read(&stream->urb_cnt) != FL2000_SB_MIN) {
		dev_err(&usb_dev->dev, "URBs are not released");
		return -EBUSY;
	}

	stream->enabled = true;
	ret = fl2000_send_stream(usb_dev, stream);
	/* TODO: Signal fault to system and start shutdown of usb_dev in case of non-0 'ret' */

	return ret;
}

/**
 * fl2000_streaming_disable() - streaming processing stop
 * 
 * @usb_dev: USB device
 */
void fl2000_streaming_disable(struct usb_device *usb_dev)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream *stream;

	stream = devres_find(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot find streaming context");
		return -ENODEV;
	}

	stream->enabled = false;

	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000))
		usb_kill_anchored_urbs(&stream->anchor);

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock(&stream->list_lock);

	atomic_set(&stream->urb_cnt, FL2000_SB_MIN);
}

/**
 * fl2000_streaming_create() - streaming processing context creation
 * 
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 * 
 * @interface:	streaming transfers interface
 * 
 * @returns: Operation result
 */
int fl2000_streaming_create(struct usb_interface *interface)
{
	int ret;
	struct fl2000_stream *stream;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	stream = devres_alloc(&fl2000_streaming_release, sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		dev_err(&interface->dev, "Cannot allocate stream private structure");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->transmit_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	init_usb_anchor(&stream->anchor);

	stream->urb_cnt = ATOMIC_INIT(FL2000_SB_MIN);
	stream->enabled = false;

	devres_add(&usb_dev->dev, stream);

	return 0;
}

/**
 * fl2000_streaming_destroy() - streaming processing context destruction
 * 
 * @interface:	streaming transfers interface
 */
void fl2000_streaming_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	devres_release(&usb_dev->dev, fl2000_streaming_release, NULL, NULL);
}
