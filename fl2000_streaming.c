// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
/* FL2000 bulk streaming. Interface 0 altsetting 1 avoids shared endpoint 1. */
#include "fl2000.h"
#include <linux/overflow.h>
#include <linux/unaligned.h>
#include "fl2000_frame.h"

#define FL2000_QUEUE_DEPTH 3
/* Three in-flight frames, a latest frame, and a free conversion target. */
#define FL2000_FRAME_COUNT (FL2000_QUEUE_DEPTH + 2)

struct fl2000_frame {
	struct sg_table *dma_sgt;
	dma_addr_t dma;
	unsigned int size;
	struct page **pages;
	unsigned int nr_pages;
	u8 *vaddr;
	unsigned int users; /* Protected by stream->lock. */
	/* Conversion history is protected by stream->mutex, including busy frames. */
	struct drm_rect dirty;
	bool valid;
};

struct fl2000_transfer {
	struct urb *urb;
	struct sg_table sgt;
	struct fl2000_stream *stream;
	struct fl2000_frame *frame;
	struct fl2000_frame *sg_frame; /* Last pages installed in this URB. */
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct mutex mutex; /* Serializes conversion, modesets and stop/start. */
	spinlock_t lock; /* Only transfer ownership and submission. */
	struct fl2000_frame frames[FL2000_FRAME_COUNT];
	struct fl2000_transfer transfers[FL2000_QUEUE_DEPTH];
	struct fl2000_frame *latest;
	unsigned int buf_size;
	u32 bytes_pix;
	const void *source;
	unsigned int width, height, pitch;
	u32 format;
	bool enabled;
};

static void fl2000_free_frame(struct usb_device *usb_dev, struct fl2000_frame *frame)
{
	unsigned int i;

	if (frame->dma_sgt) {
		usb_free_noncoherent(usb_dev, frame->size, frame->vaddr, DMA_TO_DEVICE,
				     frame->dma_sgt);
		memset(frame, 0, sizeof(*frame));
		return;
	}
	vunmap(frame->vaddr);
	if (frame->pages) {
		for (i = 0; i < frame->nr_pages; i++)
			if (frame->pages[i])
				__free_page(frame->pages[i]);
	}
	kfree(frame->pages);
	memset(frame, 0, sizeof(*frame));
}

static int fl2000_alloc_frame(struct usb_device *usb_dev, struct fl2000_frame *frame,
			      unsigned int size)
{
	unsigned int i;

	frame->size = size;
	frame->vaddr = usb_alloc_noncoherent(usb_dev, size, GFP_KERNEL, &frame->dma, DMA_TO_DEVICE,
					     &frame->dma_sgt);
	if (frame->vaddr) {
		memset(frame->vaddr, 0, size);
		return 0;
	}
	/* Without a contiguous DMA address, retain the page-based SG path. */
	frame->nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	frame->pages = kcalloc(frame->nr_pages, sizeof(*frame->pages), GFP_KERNEL);
	if (!frame->pages)
		return -ENOMEM;
	for (i = 0; i < frame->nr_pages; i++) {
		frame->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!frame->pages[i])
			return -ENOMEM;
	}
	frame->vaddr = vmap(frame->pages, frame->nr_pages, VM_MAP, PAGE_KERNEL);
	return frame->vaddr ? 0 : -ENOMEM;
}

/* Called with IRQs saved and lock held; no allocation or copying here. */
static int fl2000_submit_transfer(struct fl2000_transfer *transfer)
{
	struct fl2000_stream *stream = transfer->stream;
	struct fl2000_frame *frame = stream->latest;
	struct urb *urb = transfer->urb;
	struct scatterlist *sg;
	unsigned int i, remaining = stream->buf_size;
	int ret;

	if (!stream->enabled || !frame)
		return 0;
	frame->users++;
	transfer->frame = frame;
	urb->transfer_buffer_length = stream->buf_size;
	urb->transfer_buffer = frame->vaddr;
	if (frame->dma_sgt) {
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_dma = frame->dma;
		urb->sgt = frame->dma_sgt;
		urb->sg = NULL;
		urb->num_sgs = 0;
	} else {
		urb->transfer_flags &= ~URB_NO_TRANSFER_DMA_MAP;
		urb->sgt = NULL;
		/* USB core owns DMA metadata; never share this table across URBs. */
		if (transfer->sg_frame != frame) {
			for_each_sg(transfer->sgt.sgl, sg, transfer->sgt.orig_nents, i) {
				unsigned int length = min_t(unsigned int, remaining, PAGE_SIZE);

				sg_set_page(sg, frame->pages[i], length, 0);
				remaining -= length;
			}
			transfer->sg_frame = frame;
		}
		urb->sg = transfer->sgt.sgl;
		urb->num_sgs = transfer->sgt.orig_nents;
	}
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		frame->users--;
		transfer->frame = NULL;
		stream->enabled = false;
	}
	return ret;
}

static void fl2000_data_completion(struct urb *urb)
{
	struct fl2000_transfer *transfer = urb->context;
	struct fl2000_stream *stream = transfer->stream;
	unsigned long flags;
	int ret = 0;
	int status = urb->status;

	spin_lock_irqsave(&stream->lock, flags);
	transfer->frame->users--;
	transfer->frame = NULL;
	if (status || urb->actual_length != stream->buf_size)
		stream->enabled = false;
	else
		ret = fl2000_submit_transfer(transfer);
	spin_unlock_irqrestore(&stream->lock, flags);
	if (ret)
		dev_err_ratelimited(&stream->usb_dev->dev, "Streaming submission failed: %d\n",
				    ret);
	if (status && status != -ENOENT && status != -ECONNRESET && status != -ESHUTDOWN)
		dev_err_ratelimited(&stream->usb_dev->dev, "Streaming transfer failed: %d\n",
				    status);
}

/* mutex held; setting enabled under lock prevents racing resubmission. */
static void fl2000_stop_stream(struct fl2000_stream *stream)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&stream->lock, flags);
	stream->enabled = false;
	spin_unlock_irqrestore(&stream->lock, flags);
	for (i = 0; i < FL2000_QUEUE_DEPTH; i++)
		usb_kill_urb(stream->transfers[i].urb);
}

static void fl2000_streaming_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;
	unsigned int i;

	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	for (i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		usb_free_urb(stream->transfers[i].urb);
		sg_free_table(&stream->transfers[i].sgt);
	}
	for (i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_free_frame(stream->usb_dev, &stream->frames[i]);
	mutex_unlock(&stream->mutex);
}

static struct fl2000_stream *fl2000_find_stream(struct usb_device *usb_dev)
{
	return devres_find(&usb_dev->dev, fl2000_streaming_release, NULL, NULL);
}

static u16 fl2000_rgb565(u32 pixel)
{
	return ((pixel >> 8) & 0xf800) | ((pixel >> 5) & 0x07e0) | ((pixel >> 3) & 0x001f);
}

static u32 fl2000_rgb565_pair(u64 pixels)
{
	u64 values = ((pixels >> 8) & 0x0000f8000000f800ULL) |
		     ((pixels >> 5) & 0x000007e0000007e0ULL) |
		     ((pixels >> 3) & 0x0000001f0000001fULL);

	return (u32)values | (u32)(values >> 16);
}

static void fl2000_convert_line(u8 *dst, const u8 *line, unsigned int width, unsigned int offset,
				unsigned int bytes_pix)
{
	unsigned int x;
	unsigned int count;

	/* Scalar prefix reaches a complete wire word, including cropped rows. */
	while (width && (offset & 7)) {
		u32 pixel = get_unaligned_le32(line);
		unsigned int i;

		if (bytes_pix == 2)
			pixel = fl2000_rgb565(pixel);
		for (i = 0; i < bytes_pix; i++)
			dst[offset++ ^ 4] = pixel >> (8 * i);
		line += 4;
		width--;
	}
	while ((count = fl2000_frame_convert(dst + offset, line, width, bytes_pix))) {
		line += count * 4;
		offset += count * bytes_pix;
		width -= count;
		cond_resched();
	}

	if (bytes_pix == 2) {
		x = 0;
		if ((offset & 3) && width) {
			put_unaligned_le16(fl2000_rgb565(get_unaligned_le32(line)),
					   dst + (offset ^ 4));
			x++;
			offset += 2;
		}
		for (; x + 2 <= width; x += 2, offset += 4) {
			u64 pixels = get_unaligned_le64(line + x * 4);

			put_unaligned_le32(fl2000_rgb565_pair(pixels), dst + (offset ^ 4));
		}
		if (x < width) {
			put_unaligned_le16(fl2000_rgb565(get_unaligned_le32(line + x * 4)),
					   dst + (offset ^ 4));
		}
	} else {
		x = 0;
		/* Odd-width rows may start inside a wire dword. */
		while (x < width && (offset & 3)) {
			u32 pixel = get_unaligned_le32(line + x++ * 4);

			dst[offset++ ^ 4] = pixel;
			dst[offset++ ^ 4] = pixel >> 8;
			dst[offset++ ^ 4] = pixel >> 16;
		}
		for (; x + 4 <= width; x += 4, offset += 12) {
			u32 a = get_unaligned_le32(line + x * 4);
			u32 b = get_unaligned_le32(line + (x + 1) * 4);
			u32 c = get_unaligned_le32(line + (x + 2) * 4);
			u32 d = get_unaligned_le32(line + (x + 3) * 4);

			put_unaligned_le32((a & 0xffffff) | (b << 24), dst + (offset ^ 4));
			put_unaligned_le32(((b >> 8) & 0xffff) | (c << 16),
					   dst + ((offset + 4) ^ 4));
			put_unaligned_le32(((c >> 16) & 0xff) | (d << 8), dst + ((offset + 8) ^ 4));
		}
		for (; x < width; x++) {
			u32 pixel = get_unaligned_le32(line + x * 4);

			dst[offset++ ^ 4] = pixel;
			dst[offset++ ^ 4] = pixel >> 8;
			dst[offset++ ^ 4] = pixel >> 16;
		}
	}
}

static void fl2000_convert_frame(u8 *dst, const void *src, unsigned int height, unsigned int width,
				 unsigned int pitch, unsigned int bytes_pix, unsigned int buf_size)
{
	unsigned int size = width * height * bytes_pix;
	unsigned int y;

	memset(dst + (size & ~7U), 0, buf_size - (size & ~7U));
	for (y = 0; y < height;) {
		unsigned int rows = 1;

		/* Batch contiguous rows without extending a preparation span. */
		if (pitch == width * 4)
			rows = min(height - y, max(1U, 4096U / width));
		fl2000_convert_line(dst, (const u8 *)src + (size_t)y * pitch, width * rows,
				    y * width * bytes_pix, bytes_pix);
		y += rows;
		cond_resched();
	}
}

static void fl2000_reorder_line(u8 *dst, const u8 *line, unsigned int remaining,
				unsigned int offset)
{
	while (remaining && (offset & 7)) {
		dst[offset++ ^ 4] = *line++;
		remaining--;
	}
	while (remaining >= 8) {
		unsigned int prepared = fl2000_frame_reorder(dst + offset, line, remaining);
		unsigned int count = min_t(unsigned int, remaining & ~7U, 65536);
		unsigned int i;

		if (prepared) {
			line += prepared;
			offset += prepared;
			remaining -= prepared;
			cond_resched();
			continue;
		}

		for (i = 0; i < count; i += 8) {
			u64 word = get_unaligned_le64(line + i);

			put_unaligned_le64((word << 32) | (word >> 32), dst + offset + i);
		}
		line += count;
		offset += count;
		remaining -= count;
		cond_resched();
	}
	while (remaining) {
		dst[offset++ ^ 4] = *line++;
		remaining--;
	}
}

static void fl2000_reorder_frame(u8 *dst, const void *src, unsigned int height, unsigned int width,
				 unsigned int pitch, unsigned int bytes_pix, unsigned int buf_size)
{
	unsigned int size = width * height * bytes_pix;
	unsigned int row_bytes = width * bytes_pix;
	unsigned int y;

	memset(dst + (size & ~7U), 0, buf_size - (size & ~7U));
	if (pitch == row_bytes) {
		row_bytes = size;
		height = 1;
	}
	for (y = 0; y < height; y++) {
		fl2000_reorder_line(dst, (const u8 *)src + (size_t)y * pitch, row_bytes,
				    y * row_bytes);
		cond_resched();
	}
}

static unsigned int fl2000_source_bytes_pix(u32 format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
		return 4;
	case DRM_FORMAT_RGB888:
		return 3;
	case DRM_FORMAT_RGB565:
		return 2;
	default:
		return 0;
	}
}

static void fl2000_damage_add(struct drm_rect *pending, const struct drm_rect *damage)
{
	if (pending->x1 >= pending->x2 || pending->y1 >= pending->y2) {
		*pending = *damage;
		return;
	}
	pending->x1 = min(pending->x1, damage->x1);
	pending->y1 = min(pending->y1, damage->y1);
	pending->x2 = max(pending->x2, damage->x2);
	pending->y2 = max(pending->y2, damage->y2);
}

static void fl2000_convert_damage(u8 *dst, const void *src, unsigned int width, unsigned int pitch,
				  unsigned int source_bytes, unsigned int bytes_pix,
				  const struct drm_rect *damage)
{
	unsigned int count = damage->x2 - damage->x1;
	int y;

	for (y = damage->y1; y < damage->y2; y++) {
		const u8 *line = (const u8 *)src + (size_t)y * pitch + damage->x1 * source_bytes;
		unsigned int offset = (y * width + damage->x1) * bytes_pix;

		if (source_bytes == 4)
			fl2000_convert_line(dst, line, count, offset, bytes_pix);
		else
			fl2000_reorder_line(dst, line, count * bytes_pix, offset);
		cond_resched();
	}
}

int fl2000_streaming_update(struct usb_device *usb_dev, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch, u32 format,
			    const struct drm_rect *damage)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	struct fl2000_frame *frame = NULL;
	struct drm_rect full = { 0, 0, width, height };
	unsigned int size, i;
	unsigned int source_bytes = fl2000_source_bytes_pix(format);
	unsigned long flags;
	int ret = 0;

	if (!stream)
		return -ENODEV;
	mutex_lock(&stream->mutex);
	if (!src || !width || !height || width > INT_MAX || height > INT_MAX || !source_bytes ||
	    width > pitch / source_bytes ||
	    (source_bytes != 4 && source_bytes != stream->bytes_pix) ||
	    check_mul_overflow(width, height, &size) ||
	    check_mul_overflow(size, stream->bytes_pix, &size) || !stream->bytes_pix ||
	    ALIGN(size, 8) != stream->buf_size) {
		ret = -EINVAL;
		goto out;
	}
	if (!damage)
		damage = &full;
	if (damage->x1 < 0 || damage->y1 < 0 || damage->x2 > (int)width ||
	    damage->y2 > (int)height || damage->x1 > damage->x2 || damage->y1 > damage->y2) {
		ret = -EINVAL;
		goto out;
	}
	if (damage->x1 == damage->x2 || damage->y1 == damage->y2)
		goto out;
	/* A different crop, mapping, layout or format invalidates every cached image. */
	if (stream->source != src || stream->width != width || stream->height != height ||
	    stream->pitch != pitch || stream->format != format) {
		for (i = 0; i < FL2000_FRAME_COUNT; i++)
			stream->frames[i].valid = false;
		stream->source = src;
		stream->width = width;
		stream->height = height;
		stream->pitch = pitch;
		stream->format = format;
	}
	/* Preserve damage even if every candidate is currently in flight. */
	for (i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_damage_add(&stream->frames[i].dirty, damage);
	spin_lock_irqsave(&stream->lock, flags);
	for (i = 0; i < FL2000_FRAME_COUNT; i++) {
		if (!stream->frames[i].users && &stream->frames[i] != stream->latest) {
			frame = &stream->frames[i];
			break;
		}
	}
	spin_unlock_irqrestore(&stream->lock, flags);
	if (!frame) {
		ret = -EBUSY;
		goto out;
	}

	if (!frame->valid || (frame->dirty.x1 == 0 && frame->dirty.y1 == 0 &&
			      frame->dirty.x2 == (int)width && frame->dirty.y2 == (int)height)) {
		if (source_bytes == 4)
			fl2000_convert_frame(frame->vaddr, src, height, width, pitch,
					     stream->bytes_pix, stream->buf_size);
		else
			fl2000_reorder_frame(frame->vaddr, src, height, width, pitch,
					     stream->bytes_pix, stream->buf_size);
	} else {
		fl2000_convert_damage(frame->vaddr, src, width, pitch, source_bytes,
				      stream->bytes_pix, &frame->dirty);
	}
	frame->valid = true;
	frame->dirty = (struct drm_rect){ 0 };
	spin_lock_irqsave(&stream->lock, flags);
	stream->latest = frame;
	spin_unlock_irqrestore(&stream->lock, flags);
out:
	mutex_unlock(&stream->mutex);
	return ret;
}

int fl2000_streaming_mode_set(struct usb_device *usb_dev, int pixels, u32 bytes_pix)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	unsigned int size, i;
	bool needs_sg = false;
	int ret = 0;

	if (!stream)
		return -ENODEV;
	if (pixels <= 0 || (bytes_pix != 2 && bytes_pix != 3) ||
	    check_mul_overflow((unsigned int)pixels, bytes_pix, &size) || size > INT_MAX - 7)
		return -EINVAL;
	size = ALIGN(size, 8);
	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	stream->latest = NULL;
	stream->source = NULL;
	for (i = 0; i < FL2000_FRAME_COUNT; i++) {
		stream->frames[i].valid = false;
		stream->frames[i].dirty = (struct drm_rect){ 0 };
	}
	stream->bytes_pix = bytes_pix;
	if (stream->buf_size == size)
		goto out;
	stream->latest = NULL;
	stream->buf_size = 0;
	for (i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_free_frame(stream->usb_dev, &stream->frames[i]);
	for (i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		sg_free_table(&stream->transfers[i].sgt);
		stream->transfers[i].sg_frame = NULL;
	}
	for (i = 0; i < FL2000_FRAME_COUNT; i++) {
		ret = fl2000_alloc_frame(usb_dev, &stream->frames[i], size);
		if (ret)
			break;
		needs_sg |= !stream->frames[i].dma_sgt;
	}
	if (!ret && needs_sg) {
		unsigned int pages = DIV_ROUND_UP(size, PAGE_SIZE);

		if (pages > usb_dev->bus->sg_tablesize)
			ret = -E2BIG;
		for (i = 0; !ret && i < FL2000_QUEUE_DEPTH; i++)
			ret = sg_alloc_table(&stream->transfers[i].sgt, pages, GFP_KERNEL);
	}
	if (ret) {
		for (i = 0; i < FL2000_FRAME_COUNT; i++)
			fl2000_free_frame(stream->usb_dev, &stream->frames[i]);
		for (i = 0; i < FL2000_QUEUE_DEPTH; i++)
			sg_free_table(&stream->transfers[i].sgt);
	} else {
		stream->buf_size = size;
	}
out:
	mutex_unlock(&stream->mutex);
	return ret;
}

int fl2000_streaming_enable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	unsigned long flags;
	unsigned int i;
	bool enabled;
	int ret = 0;

	if (!stream)
		return -ENODEV;
	mutex_lock(&stream->mutex);
	spin_lock_irqsave(&stream->lock, flags);
	enabled = stream->enabled;
	spin_unlock_irqrestore(&stream->lock, flags);
	if (enabled)
		goto out;
	/* Drain stopped/error transfers before reusing URBs or resetting the pipe. */
	fl2000_stop_stream(stream);
	if (!stream->latest) {
		ret = -EINVAL;
		goto out;
	}
	ret = usb_clear_halt(usb_dev, usb_sndbulkpipe(usb_dev, STREAMING_EP));
	if (ret)
		goto out;
	spin_lock_irqsave(&stream->lock, flags);
	stream->enabled = true;
	for (i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		ret = fl2000_submit_transfer(&stream->transfers[i]);
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&stream->lock, flags);
	if (ret)
		fl2000_stop_stream(stream);
out:
	mutex_unlock(&stream->mutex);
	return ret;
}

void fl2000_streaming_disable(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);

	if (!stream)
		return;
	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	mutex_unlock(&stream->mutex);
}

int fl2000_streaming_create(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct usb_interface *control = usb_ifnum_to_if(usb_dev, FL2000_USBIF_AVCONTROL);
	struct usb_endpoint_descriptor *ep;
	struct fl2000_stream *stream;
	unsigned int i;
	int ret;

	if (!control || usb_find_bulk_out_endpoint(control->cur_altsetting, &ep) ||
	    usb_endpoint_num(ep) != STREAMING_EP || !usb_dev->bus->sg_tablesize)
		return -ENODEV;
	stream = devres_alloc(fl2000_streaming_release, sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;
	stream->usb_dev = usb_dev;
	mutex_init(&stream->mutex);
	spin_lock_init(&stream->lock);
	for (i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		struct fl2000_transfer *transfer = &stream->transfers[i];

		transfer->stream = stream;
		transfer->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!transfer->urb) {
			ret = -ENOMEM;
			goto fail;
		}
		usb_fill_bulk_urb(transfer->urb, usb_dev, usb_sndbulkpipe(usb_dev, STREAMING_EP),
				  NULL, 0, fl2000_data_completion, transfer);
		transfer->urb->transfer_flags = URB_ZERO_PACKET;
	}
	devres_add(&usb_dev->dev, stream);
	return 0;
fail:
	fl2000_streaming_release(&usb_dev->dev, stream);
	devres_free(stream);
	return ret;
}

void fl2000_streaming_destroy(struct usb_interface *interface)
{
	devres_release(&interface_to_usbdev(interface)->dev, fl2000_streaming_release, NULL, NULL);
}
