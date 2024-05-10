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
	int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct drm_crtc *crtc;
	/* Each buffer journey: render->transmit->wait->... */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* List access from bh and interrupt contexts */
	size_t buf_size;
	int bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct semaphore work_sem;
	bool enabled;
	struct usb_anchor anchor;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	int i;

	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(size_t size)
{
	int i, ret;
	struct fl2000_stream_buf *sb;
	int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kcalloc(nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
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

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb, *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, size_t size)
{
	int i, ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&stream->render_list));

	for (i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;

	if (stream) {
		spin_lock_irq(&stream->list_lock);
		list_move_tail(&cur_sb->list, &stream->render_list);
		spin_unlock(&stream->list_lock);

		drm_crtc_handle_vblank(stream->crtc);

		/* Kick transmit workqueue */
		up(&stream->work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb, *last_sb;
	struct urb *data_urb;

	while (stream->enabled) {
		ret = down_interruptible(&stream->work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			stream->enabled = false;
			return;
		}

		spin_lock_irq(&stream->list_lock);

		/* If no buffers are available for immediate transmission - then copy latest
		 * transmission data
		 */
		if (list_empty(&stream->transmit_list)) {
			if (list_empty(&stream->wait_list))
				last_sb = list_last_entry(&stream->render_list,
							  struct fl2000_stream_buf, list);
			else
				last_sb = list_last_entry(&stream->wait_list,
							  struct fl2000_stream_buf, list);
			cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf,
						  list);
			memcpy(cur_sb->vaddr, last_sb->vaddr, stream->buf_size);
		} else {
			cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf,
						  list);
		}
		list_move_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock(&stream->list_lock);

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			stream->enabled = false;
			return;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			usb_free_urb(data_urb);
			stream->enabled = false;
		}
	}
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int x, xx = 0;

	for (x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int x;

	for (x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	unsigned int y;
	void *dst;
	u32 dst_line_len;

	BUG_ON(list_empty(&stream->render_list));

	spin_lock_irq(&stream->list_lock);

	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	dst = cur_sb->vaddr;
	dst_line_len = width * stream->bytes_pix;

	for (y = 0; y < height; y++) {
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

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	size_t size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(size_t)7;

	stream->bytes_pix = bytes_pix;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	int i;

	BUG_ON(list_empty(&stream->transmit_list));

	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (i = 0; i < FL2000_SB_MIN; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	stream->enabled = false;

	drain_workqueue(stream->work_queue);

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
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 *
 * Return: Operation result
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsettung 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, stream);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->transmit_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->usb_dev = usb_dev;
	stream->crtc = crtc;

	stream->work_queue = create_workqueue("fl2000_stream");
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
