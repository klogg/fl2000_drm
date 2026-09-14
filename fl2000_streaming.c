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
	u32 bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct semaphore work_sem;
	bool enabled;
	struct usb_anchor anchor;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	sg_free_table(&sb->sgt);
	vfree(sb->vaddr);
	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	struct fl2000_stream_buf *sb;
	struct page **pages;
	unsigned int nr_pages;
	u8 *ptr;
	int ret;
	int i;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->vaddr = vmalloc_32(size);
	if (!sb->vaddr)
		goto error;

	nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	pages = kmalloc_array(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		goto error;

	for (i = 0, ptr = sb->vaddr; i < nr_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);

	ret = sg_alloc_table_from_pages(&sb->sgt, pages, nr_pages, 0, size, GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, size);

	return sb;

error:
	fl2000_free_sb(sb);
	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&stream->render_list));

	for (int i = 0; i < FL2000_SB_NUM; i++) {
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

	UNUSED(dev);

	if (stream->work_queue) {
		fl2000_stream_disable(stream);
		destroy_workqueue(stream->work_queue);
	}
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
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *last_sb;
	struct urb *data_urb;

	while (READ_ONCE(stream->enabled)) {
		ret = down_interruptible(&stream->work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			WRITE_ONCE(stream->enabled, false);
			return;
		}
		if (!READ_ONCE(stream->enabled))
			break;

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
			WRITE_ONCE(stream->enabled, false);
			return;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);
			WRITE_ONCE(stream->enabled, false);
		}
	}
}

/* Weird fl2000 specific dword ordering */
static void fl2000_swap_dword_pairs(void *buf, size_t len)
{
	u32 *words = buf;
	size_t count = len / sizeof(*words);
	size_t i;

	if (WARN_ON_ONCE(len % (2 * sizeof(*words))))
		return;

	for (i = 0; i < count; i += 2)
		swap(words[i], words[i + 1]);
}

void fl2000_stream_compress(struct fl2000_stream *stream, const struct iosys_map *src,
			    struct drm_framebuffer *fb, const struct drm_rect *clip,
			    struct drm_format_conv_state *fmtcnv_state)
{
	struct fl2000_stream_buf *cur_sb;
	struct iosys_map dst;

	BUG_ON(list_empty(&stream->render_list));

	spin_lock_irq(&stream->list_lock);

	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	iosys_map_set_vaddr(&dst, cur_sb->vaddr);

	switch (stream->bytes_pix) {
	case 2:
		drm_fb_xrgb8888_to_rgb565(&dst, NULL, src, fb, clip, fmtcnv_state);
		fl2000_swap_dword_pairs(cur_sb->vaddr, stream->buf_size);
		break;
	case 3:
		drm_fb_xrgb8888_to_rgb888(&dst, NULL, src, fb, clip, fmtcnv_state);
		fl2000_swap_dword_pairs(cur_sb->vaddr, stream->buf_size);
		break;
	default: /* Shouldn't happen */
		break;
	}

	list_move_tail(&cur_sb->list, &stream->transmit_list);
	spin_unlock(&stream->list_lock);
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~7U;

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
	BUG_ON(list_empty(&stream->transmit_list));

	sema_init(&stream->work_sem, 0);
	usb_unpoison_anchored_urbs(&stream->anchor);
	WRITE_ONCE(stream->enabled, true);
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (int i = 0; i < FL2000_SB_MIN; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	WRITE_ONCE(stream->enabled, false);
	up(&stream->work_sem);

	usb_poison_anchored_urbs(&stream->anchor);
	cancel_work_sync(&stream->work);

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock_irq(&stream->list_lock);
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
