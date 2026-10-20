// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */
/* FL2000 bulk streaming. Interface 0 altsetting 1 avoids shared endpoint 1. */
#include "fl2000.h"
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_rect.h>
#include <linux/overflow.h>
#include <linux/unaligned.h>
#include "fl2000_frame.h"

/**
 * DOC: Wire frame representation
 *
 * Input mappings use linear DRM XRGB8888, RGB888 or little-endian RGB565.
 * RGB888 is B, G, R in memory; XRGB8888 adds the unused high byte. XRGB8888
 * converts to the bandwidth-selected 24-bit or 16-bit wire representation.
 * Native formats retain their representation. Swap the two dwords in every
 * eight-byte wire word globally across the frame, not independently per row.
 * Source pitch/crops are respected and final wire padding is zeroed.
 *
 * Damage limits CPU preparation, not USB traffic: the controller receives
 * complete uncompressed frames. Three immutable URBs share retained wire
 * images through explicit references; per-buffer damage history avoids full
 * copies when reusing an image after missed updates.
 */
#define FL2000_QUEUE_DEPTH	 3
#define FL2000_RECOVERY_RETRIES	 3
#define FL2000_RECOVERY_DELAY_MS 100
/* Three in-flight frames, a latest frame, and a free conversion target. */
#define FL2000_FRAME_COUNT (FL2000_QUEUE_DEPTH + 2)

/**
 * struct fl2000_frame - Immutable wire image while submitted
 * @dma_sgt: Persistent USB noncoherent allocation, or NULL for page fallback.
 * @dma: DMA address of the noncoherent allocation.
 * @size: Allocated wire bytes, rounded to eight bytes.
 * @pages: Fallback backing pages, or NULL.
 * @nr_pages: Number of fallback pages.
 * @vaddr: CPU mapping for preparation and USB submission.
 * @users: In-flight URB references, protected by the stream spinlock.
 * @dirty: Accumulated missed source damage, protected by the stream mutex.
 * @valid: Whether conversion history matches the current source layout.
 *
 * Conversion history may change while a frame is busy; its bytes may not.
 * Preparation chooses a frame with no users that is not the retained latest
 * image. Damage accumulates until that frame is available for repair.
 */
struct fl2000_frame {
	struct sg_table *dma_sgt;
	dma_addr_t dma;
	unsigned int size;
	struct page **pages;
	unsigned int nr_pages;
	u8 *vaddr;
	unsigned int users;
	struct drm_rect dirty;
	bool valid;
};

/**
 * struct fl2000_transfer - Reusable URB and its frame reference
 * @urb: Bulk URB reused after completion or synchronous cancellation.
 * @sgt: Per-URB page fallback metadata; never shared with another URB.
 * @stream: Owning stream.
 * @frame: Submitted frame, or NULL when not in flight.
 * @sg_frame: Frame whose pages are currently installed in @sgt.
 *
 * The stream spinlock protects submission and @frame ownership. USB core owns
 * DMA metadata while the URB is submitted. Completion releases the frame
 * reference before selecting the next retained image.
 */
struct fl2000_transfer {
	struct urb *urb;
	struct sg_table sgt;
	struct fl2000_stream *stream;
	struct fl2000_frame *frame;
	struct fl2000_frame *sg_frame;
};

/**
 * struct fl2000_stream_buffers - Prepared replacement transport allocations
 * @frames: Five wire images: three in flight, latest and conversion target.
 * @tables: Separate page fallback tables for the three URBs.
 * @size: Wire frame size, rounded up to eight bytes.
 * @bytes_pix: Wire bytes per pixel, 2 or 3.
 *
 * Owned by pending plane state until fl2000_streaming_mode_set() consumes it.
 * Preparation can fail before atomic state swap; installation allocates nothing.
 */
struct fl2000_stream_buffers {
	struct fl2000_frame frames[FL2000_FRAME_COUNT];
	struct sg_table tables[FL2000_QUEUE_DEPTH];
	unsigned int size;
	u32 bytes_pix;
};

/**
 * struct fl2000_stream - Frame history, queue ownership and recovery
 * @usb_dev: USB parent, kept alive by interface resource ownership.
 * @frames: Reusable transport allocations.
 * @transfers: Three reusable bulk transfers.
 * @latest: Retained immutable image selected for subsequent submissions.
 * @buf_size: Rounded wire frame size.
 * @bytes_pix: Wire pixel size, 2 or 3.
 * @source: Source address used to validate conversion history; not owned.
 * @width: Source crop width in pixels.
 * @height: Source crop height in rows.
 * @pitch: Source row stride in bytes.
 * @format: Source DRM fourcc.
 * @recovery: Delayed restart work, serialized by @mutex.
 * @retries: Restart attempts since full completion or explicit enable.
 * @running: Scanout requested; stop clears it before draining the queue.
 * @enabled: Queue may submit; an error clears it while @running can stay
 *           true.
 *
 * Lock order is @mutex before @lock. Completion takes only @lock. The mutex
 * protects source layout, conversion history and frame bytes; @lock protects
 * @latest, frame users, @retries, @running and @enabled. Freeing resources must
 * stop scanout, drop the mutex, then synchronize recovery before freeing URBs.
 */
struct fl2000_stream {
	struct usb_device *usb_dev;
	/** @mutex: Serializes preparation, modesets, start/stop and recovery. */
	struct mutex mutex;
	/** @lock: Protects URB references, latest image and queue/retry state. */
	spinlock_t lock;
	struct fl2000_frame frames[FL2000_FRAME_COUNT];
	struct fl2000_transfer transfers[FL2000_QUEUE_DEPTH];
	struct fl2000_frame *latest;
	unsigned int buf_size;
	u32 bytes_pix;
	const void *source;
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
	u32 format;
	struct delayed_work recovery;
	unsigned int retries;
	bool running;
	bool enabled;
};

/**
 * fl2000_free_frame() - Release either transport allocation backend
 * @usb_dev: USB parent with live adapter resources.
 * @frame: Wire-frame allocation.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_free_frame(struct usb_device *usb_dev, struct fl2000_frame *frame)
{
	if (frame->dma_sgt) {
		usb_free_noncoherent(usb_dev, frame->size, frame->vaddr, DMA_TO_DEVICE,
				     frame->dma_sgt);
		memset(frame, 0, sizeof(*frame));
		return;
	}
	vunmap(frame->vaddr);
	if (frame->pages) {
		for (unsigned int i = 0; i < frame->nr_pages; i++)
			if (frame->pages[i])
				__free_page(frame->pages[i]);
	}
	kfree(frame->pages);
	memset(frame, 0, sizeof(*frame));
}

/**
 * fl2000_alloc_frame() - Allocate a persistent USB image or page fallback
 * @usb_dev: USB parent with live adapter resources.
 * @frame: Wire-frame allocation.
 * @size: Allocation size in bytes.
 *
 * On failure the frame can contain partial allocations; the caller must use
 * fl2000_free_frame() to unwind them.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_alloc_frame(struct usb_device *usb_dev, struct fl2000_frame *frame,
			      unsigned int size)
{
	frame->size = size;
	/* Direct DMA may require contiguous pages; failure has a page fallback. */
	frame->vaddr = usb_alloc_noncoherent(usb_dev, size, GFP_KERNEL | __GFP_NOWARN, &frame->dma,
					     DMA_TO_DEVICE, &frame->dma_sgt);
	if (frame->vaddr) {
		memset(frame->vaddr, 0, size);
		return 0;
	}
	/* Only the page fallback requires controller scatter-gather support. */
	if (!usb_dev->bus->sg_tablesize)
		return -EOPNOTSUPP;
	frame->nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	frame->pages = kcalloc(frame->nr_pages, sizeof(*frame->pages), GFP_KERNEL);
	if (!frame->pages)
		return -ENOMEM;
	for (unsigned int i = 0; i < frame->nr_pages; i++) {
		frame->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!frame->pages[i])
			return -ENOMEM;
	}
	frame->vaddr = vmap(frame->pages, frame->nr_pages, VM_MAP, PAGE_KERNEL);
	return frame->vaddr ? 0 : -ENOMEM;
}

/**
 * fl2000_stream_error() - Stop a failed queue or schedule bounded recovery
 * @stream: Owning bulk stream.
 * @error: USB or frame-transfer error.
 *
 * Cancellation, shutdown, rejected or invalid requests stop scanout intent.
 * Other errors retain intent and retry while the physical device is attached.
 *
 * Context: Caller holds the stream spinlock.
 */
static void fl2000_stream_error(struct fl2000_stream *stream, int error)
{
	stream->enabled = false;
	if (!stream->running)
		return;

	switch (error) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
	case -EPERM:
	case -EINVAL:
		goto stop;
	default:
		break;
	}
	if (fl2000_usb_unavailable(stream->usb_dev))
		goto stop;
	if (stream->retries < FL2000_RECOVERY_RETRIES)
		mod_delayed_work(system_wq, &stream->recovery,
				 msecs_to_jiffies(FL2000_RECOVERY_DELAY_MS << stream->retries));
	else
		dev_err_ratelimited(&stream->usb_dev->dev, "Streaming recovery exhausted: %d\n",
				    error);
	return;
stop:
	stream->running = false;
	cancel_delayed_work(&stream->recovery);
}

/**
 * fl2000_submit_transfer() - Take a latest-frame reference and submit one URB
 * @transfer: Reusable bulk transfer.
 *
 * Does not allocate or copy pixels. A failed submit drops the frame reference
 * and schedules recovery when scanout is still requested.
 *
 * Context: Caller holds the stream spinlock with local IRQs saved.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_submit_transfer(struct fl2000_transfer *transfer)
{
	struct fl2000_stream *stream = transfer->stream;
	struct fl2000_frame *frame = stream->latest;
	struct urb *urb = transfer->urb;
	struct scatterlist *sg;
	unsigned int i;
	unsigned int remaining = stream->buf_size;
	int ret;

	if (!stream->enabled || !frame)
		return 0;
	if (fl2000_usb_unavailable(stream->usb_dev)) {
		fl2000_stream_error(stream, -ENODEV);
		return -ENODEV;
	}
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
		fl2000_stream_error(stream, ret);
	}
	return ret;
}

/**
 * fl2000_data_completion() - Release one frame and advance the bulk queue
 * @urb: Completed USB request.
 *
 * Context: USB completion context. Takes the stream spinlock.
 */
static void fl2000_data_completion(struct urb *urb)
{
	struct fl2000_transfer *transfer = urb->context;
	struct fl2000_stream *stream = transfer->stream;
	unsigned long flags;
	int status = urb->status;
	unsigned int actual = urb->actual_length;
	int ret = 0;

	spin_lock_irqsave(&stream->lock, flags);
	transfer->frame->users--;
	transfer->frame = NULL;

	switch (status) {
	case 0:
		break;
	case -ENODEV:
	case -ESHUTDOWN:
		stream->enabled = false;
		stream->running = false;
		cancel_delayed_work(&stream->recovery);
		goto unlock;
	case -ENOENT:
	case -ECONNRESET:
		/* Draining a disabled queue must preserve restart intent. */
		if (stream->enabled) {
			stream->enabled = false;
			stream->running = false;
			cancel_delayed_work(&stream->recovery);
		}
		goto unlock;
	default:
		if (stream->enabled)
			fl2000_stream_error(stream, status);
		dev_err_ratelimited(&stream->usb_dev->dev, "Streaming transfer failed: %d\n",
				    status);
		goto unlock;
	}

	if (actual != stream->buf_size) {
		if (stream->enabled)
			fl2000_stream_error(stream, -EREMOTEIO);
		dev_err_ratelimited(&stream->usb_dev->dev, "Short streaming transfer: %u/%u\n",
				    actual, stream->buf_size);
		goto unlock;
	}
	if (stream->enabled) {
		stream->retries = 0;
		ret = fl2000_submit_transfer(transfer);
	}
unlock:
	spin_unlock_irqrestore(&stream->lock, flags);
	if (ret)
		dev_err_ratelimited(&stream->usb_dev->dev, "Streaming submission failed: %d\n",
				    ret);
}

/**
 * fl2000_drain_stream() - Disable submission and synchronously kill bulk URBs
 * @stream: Owning bulk stream.
 *
 * Preserves scanout intent and the retained latest image for restart.
 *
 * Context: Process context. Caller holds the stream mutex.
 */
static void fl2000_drain_stream(struct fl2000_stream *stream)
{
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	stream->enabled = false;
	spin_unlock_irqrestore(&stream->lock, flags);
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++)
		usb_kill_urb(stream->transfers[i].urb);
}

/**
 * fl2000_stop_stream() - Cancel scanout intent before draining the queue
 * @stream: Owning bulk stream.
 *
 * Does not wait for recovery work holding or waiting on that mutex. Resource
 * release synchronizes recovery separately after dropping the mutex.
 *
 * Context: Process context. Caller holds the stream mutex.
 */
static void fl2000_stop_stream(struct fl2000_stream *stream)
{
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	stream->running = false;
	spin_unlock_irqrestore(&stream->lock, flags);
	cancel_delayed_work(&stream->recovery);
	fl2000_drain_stream(stream);
}

/**
 * fl2000_streaming_release() - Synchronize recovery before freeing USB buffers
 * @dev: Resource owner; unused.
 * @res: Stream whose external callers have been quiesced.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_streaming_release(struct device *dev __always_unused, void *res)
{
	struct fl2000_stream *stream = res;

	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	mutex_unlock(&stream->mutex);
	cancel_delayed_work_sync(&stream->recovery);
	mutex_lock(&stream->mutex);
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		usb_free_urb(stream->transfers[i].urb);
		sg_free_table(&stream->transfers[i].sgt);
	}
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_free_frame(stream->usb_dev, &stream->frames[i]);
	mutex_unlock(&stream->mutex);
}

/**
 * fl2000_find_stream() - Borrow the interface-owned stream pointer
 * @usb_dev: USB parent with live adapter resources.
 *
 * Context: Any context. Does not sleep.
 * Return: Stream resource or NULL when not initialized.
 */
static struct fl2000_stream *fl2000_find_stream(const struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	return device->stream;
}

/**
 * fl2000_rgb565() - Quantize one XRGB8888 word to RGB565
 * @pixel: XRGB8888 pixel in a CPU-endian word.
 *
 * Context: Any context. Does not sleep.
 * Return: Packed RGB565 pixel.
 */
static u16 fl2000_rgb565(u32 pixel)
{
	return ((pixel >> 8) & 0xf800) | ((pixel >> 5) & 0x07e0) | ((pixel >> 3) & 0x001f);
}

/**
 * fl2000_rgb565_pair() - Quantize two XRGB8888 pixels using integer lanes
 * @pixels: Two XRGB8888 pixels in a little-endian 64-bit word.
 *
 * Context: Any context. Does not sleep.
 * Return: Two RGB565 words packed in source pixel order.
 */
static u32 fl2000_rgb565_pair(u64 pixels)
{
	u64 values = ((pixels >> 8) & 0x0000f8000000f800ULL) |
		     ((pixels >> 5) & 0x000007e0000007e0ULL) |
		     ((pixels >> 3) & 0x0000001f0000001fULL);

	return (u32)values | (u32)(values >> 16);
}

/**
 * fl2000_convert_line() - Convert a pixel span into globally ordered wire bytes
 * @dst: Writable destination wire buffer.
 * @line: Source row/crop mapping.
 * @width: Active pixel count per row.
 * @offset: Byte offset in the full destination wire frame.
 * @bytes_pix: Wire bytes per pixel, 2 or 3.
 *
 * Context: Process context. May reschedule between bounded backend spans.
 */
static void fl2000_convert_line(u8 *dst, const u8 *line, unsigned int width, unsigned int offset,
				unsigned int bytes_pix)
{
	unsigned int x;
	unsigned int count;

	/* Scalar prefix reaches a complete wire word, including cropped rows. */
	while (width && (offset & 7)) {
		u32 pixel = get_unaligned_le32(line);

		if (bytes_pix == 2)
			pixel = fl2000_rgb565(pixel);
		for (unsigned int i = 0; i < bytes_pix; i++)
			dst[offset++ ^ 4] = (u8)(pixel >> (8 * i));
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

			dst[offset++ ^ 4] = (u8)pixel;
			dst[offset++ ^ 4] = (u8)(pixel >> 8);
			dst[offset++ ^ 4] = (u8)(pixel >> 16);
		}
	}
}

/**
 * fl2000_convert_frame() - Prepare a complete XRGB8888 wire image
 * @dst: Writable destination wire buffer.
 * @src: Readable source mapping.
 * @height: Active row count.
 * @width: Active pixel count per row.
 * @pitch: Source row stride in bytes.
 * @bytes_pix: Wire bytes per pixel, 2 or 3.
 * @buf_size: Padded destination frame size in bytes.
 *
 * Zeroes final wire padding. Contiguous rows can share bounded conversion
 * spans; source pitch and global eight-byte wire ordering are preserved.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_convert_frame(u8 *dst, const void *src, unsigned int height, unsigned int width,
				 unsigned int pitch, unsigned int bytes_pix, unsigned int buf_size)
{
	unsigned int size = width * height * bytes_pix;

	memset(dst + (size & ~7U), 0, buf_size - (size & ~7U));
	for (unsigned int y = 0; y < height;) {
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

/**
 * fl2000_reorder_line() - Swap dword halves while copying native-format bytes
 * @dst: Writable destination wire buffer.
 * @line: Source row/crop mapping.
 * @remaining: Number of source bytes to copy.
 * @offset: Byte offset in the full destination wire frame.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_reorder_line(u8 *dst, const u8 *line, unsigned int remaining,
				unsigned int offset)
{
	while (remaining && (offset & 7)) {
		dst[offset++ ^ 4] = *line++;
		remaining--;
	}
	while (remaining >= 8) {
		unsigned int count = min_t(unsigned int, remaining & ~7U, 65536);

		for (unsigned int i = 0; i < count; i += 8) {
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

/**
 * fl2000_reorder_frame() - Prepare a complete native-format wire image
 * @dst: Writable destination wire buffer.
 * @src: Readable source mapping.
 * @height: Active row count.
 * @width: Active pixel count per row.
 * @pitch: Source row stride in bytes.
 * @bytes_pix: Wire bytes per pixel, 2 or 3.
 * @buf_size: Padded destination frame size in bytes.
 *
 * Skips source row padding and zeroes final wire padding.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_reorder_frame(u8 *dst, const void *src, unsigned int height, unsigned int width,
				 unsigned int pitch, unsigned int bytes_pix, unsigned int buf_size)
{
	unsigned int size = width * height * bytes_pix;
	unsigned int row_bytes = width * bytes_pix;

	memset(dst + (size & ~7U), 0, buf_size - (size & ~7U));
	if (pitch == row_bytes) {
		row_bytes = size;
		height = 1;
	}
	for (unsigned int y = 0; y < height; y++) {
		fl2000_reorder_line(dst, (const u8 *)src + (size_t)y * pitch, row_bytes,
				    y * row_bytes);
		cond_resched();
	}
}

/**
 * fl2000_source_bytes_pix() - Resolve the supported framebuffer pixel size
 * @format: Source DRM fourcc.
 *
 * Context: Any context. Does not sleep.
 * Return: 4, 3 or 2 for supported formats, otherwise 0.
 */
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

/**
 * fl2000_damage_add() - Union nonempty damage into a pending rectangle
 * @pending: Accumulated rectangle; an empty rectangle accepts the first
 *           damage.
 * @damage: Pixel damage rectangle.
 *
 * Context: Any context. Does not sleep.
 */
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

/**
 * fl2000_convert_damage() - Repair damaged pixels without changing wire order
 * @dst: Writable destination wire buffer.
 * @src: Readable source mapping.
 * @width: Active pixel count per row.
 * @pitch: Source row stride in bytes.
 * @source_bytes: Source bytes per pixel.
 * @bytes_pix: Wire bytes per pixel, 2 or 3.
 * @damage: Crop-relative rectangle wholly inside the source layout.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_convert_damage(u8 *dst, const void *src, unsigned int width, unsigned int pitch,
				  unsigned int source_bytes, unsigned int bytes_pix,
				  const struct drm_rect *damage)
{
	unsigned int count = damage->x2 - damage->x1;

	for (int y = damage->y1; y < damage->y2; y++) {
		const u8 *line = (const u8 *)src + (size_t)y * pitch + damage->x1 * source_bytes;
		unsigned int offset = (y * width + damage->x1) * bytes_pix;

		if (source_bytes == 4)
			fl2000_convert_line(dst, line, count, offset, bytes_pix);
		else
			fl2000_reorder_line(dst, line, count * bytes_pix, offset);
		cond_resched();
	}
}

/**
 * fl2000_frame_layout_valid() - Check crop geometry and installed wire size
 * @stream: Owning bulk stream.
 * @src: Readable source mapping.
 * @height: Active row count.
 * @width: Active pixel count per row.
 * @pitch: Source row stride in bytes.
 * @source_bytes: Source bytes per pixel.
 *
 * Context: Any context. Does not sleep.
 * Return: True if the mapping and geometry fit the installed stream layout.
 */
static bool fl2000_frame_layout_valid(const struct fl2000_stream *stream, const void *src,
				      unsigned int height, unsigned int width, unsigned int pitch,
				      unsigned int source_bytes)
{
	unsigned int size;

	return src && width && height && width <= INT_MAX && height <= INT_MAX && source_bytes &&
	       width <= pitch / source_bytes &&
	       (source_bytes == 4 || source_bytes == stream->bytes_pix) &&
	       !check_mul_overflow(width, height, &size) &&
	       !check_mul_overflow(size, stream->bytes_pix, &size) && stream->bytes_pix &&
	       ALIGN(size, 8) == stream->buf_size;
}

/**
 * fl2000_find_free_frame() - Choose an unsubmitted frame other than latest
 * @stream: Owning bulk stream.
 *
 * Context: Caller holds the stream mutex; takes the spinlock to inspect
 *          references.
 * Return: Borrowed frame, or NULL if none is available.
 */
static struct fl2000_frame *fl2000_find_free_frame(struct fl2000_stream *stream)
{
	struct fl2000_frame *frame = NULL;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) {
		if (!stream->frames[i].users && &stream->frames[i] != stream->latest) {
			frame = &stream->frames[i];
			break;
		}
	}
	spin_unlock_irqrestore(&stream->lock, flags);
	return frame;
}

/**
 * fl2000_streaming_update() - Prepare a retained image from source damage
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 * @src: CPU mapping of the crop origin; valid for this call only.
 * @height: Crop height in rows.
 * @width: Crop width in pixels.
 * @pitch: Source row stride in bytes, including padding.
 * @format: Linear XRGB8888, RGB888 or RGB565 DRM fourcc.
 * @damage: Crop-relative pixel rectangle, or NULL for the full crop.
 *
 * Copies/converts only regions missed by the chosen free wire buffer. Tracks
 * missed damage even when all candidates are busy. Layout changes invalidate
 * history. Does not modify submitted buffers, retain the source mapping,
 * submit USB traffic or start a stopped stream.
 *
 * Context: Process context. Takes the stream mutex; may reschedule during
 *          copying.
 * Return: 0 on success or empty damage, -EBUSY without a free frame, or an
 *         error.
 */
int fl2000_streaming_update(const struct usb_device *usb_dev, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch, u32 format,
			    const struct drm_rect *damage)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	struct fl2000_frame *frame;
	struct drm_rect full = { 0, 0, width, height };
	unsigned int source_bytes = fl2000_source_bytes_pix(format);
	unsigned long flags;
	int ret = 0;

	if (!stream)
		return -ENODEV;
	mutex_lock(&stream->mutex);
	if (!fl2000_frame_layout_valid(stream, src, height, width, pitch, source_bytes)) {
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
		for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++)
			stream->frames[i].valid = false;
		stream->source = src;
		stream->width = width;
		stream->height = height;
		stream->pitch = pitch;
		stream->format = format;
	}
	/* Preserve damage even if every candidate is currently in flight. */
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_damage_add(&stream->frames[i].dirty, damage);
	frame = fl2000_find_free_frame(stream);
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

/**
 * fl2000_streaming_free_buffers() - Release an uninstalled or retired buffer set
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 * @buffers: Owned buffer set, or NULL.
 *
 * Must not be used on buffers still owned by the active stream.
 *
 * Context: Process context. May sleep.
 */
void fl2000_streaming_free_buffers(struct usb_device *usb_dev,
				   struct fl2000_stream_buffers *buffers)
{
	if (!buffers)
		return;
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++)
		fl2000_free_frame(usb_dev, &buffers->frames[i]);
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++)
		sg_free_table(&buffers->tables[i]);
	kfree(buffers);
}

/**
 * fl2000_streaming_prepare() - Allocate a replacement wire-buffer set
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 * @pixels: Active pixel count; must be positive.
 * @bytes_pix: Wire pixel size: 2 for RGB565 or 3 for RGB888.
 *
 * Does not modify the active stream. The caller owns the returned set until
 * installation, or frees it with fl2000_streaming_free_buffers().
 *
 * Context: Process context. May sleep.
 * Return: An owned buffer set, or ERR_PTR() on invalid size or allocation
 *         failure.
 */
struct fl2000_stream_buffers *fl2000_streaming_prepare(struct usb_device *usb_dev, int pixels,
						       u32 bytes_pix)
{
	struct fl2000_stream_buffers *buffers;
	unsigned int size;
	unsigned int pages;
	bool needs_sg = false;
	int ret;

	if (pixels <= 0 || (bytes_pix != 2 && bytes_pix != 3) ||
	    check_mul_overflow((unsigned int)pixels, bytes_pix, &size) || size > INT_MAX - 7)
		return ERR_PTR(-EINVAL);
	buffers = kzalloc_obj(*buffers);
	if (!buffers)
		return ERR_PTR(-ENOMEM);
	buffers->size = ALIGN(size, 8);
	buffers->bytes_pix = bytes_pix;
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) {
		ret = fl2000_alloc_frame(usb_dev, &buffers->frames[i], buffers->size);
		if (ret)
			goto fail;
		needs_sg = needs_sg || !buffers->frames[i].dma_sgt;
	}
	if (needs_sg) {
		pages = DIV_ROUND_UP(buffers->size, PAGE_SIZE);
		if (pages > usb_dev->bus->sg_tablesize) {
			ret = -E2BIG;
			goto fail;
		}
		for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
			ret = sg_alloc_table(&buffers->tables[i], pages, GFP_KERNEL);
			if (ret)
				goto fail;
		}
	}
	return buffers;
fail:
	fl2000_streaming_free_buffers(usb_dev, buffers);
	return ERR_PTR(ret);
}

/**
 * fl2000_streaming_mode_set() - Consume prepared buffers after draining scanout
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 * @prepared: Owned buffer set pointer; set to NULL on success.
 *
 * Stops scanout, exchanges buffer ownership and frees the retired set without
 * allocating. Leaves @prepared unchanged on failure. A new image must be
 * prepared and enabled before scanout resumes.
 *
 * Context: Process context. Takes the stream mutex and drains URBs.
 * Return: 0 on success or a negative error code.
 */
int fl2000_streaming_mode_set(struct usb_device *usb_dev, struct fl2000_stream_buffers **prepared)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	struct fl2000_stream_buffers *buffers = *prepared;

	if (!stream)
		return -ENODEV;
	if (!buffers)
		return -EINVAL;
	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	stream->latest = NULL;
	stream->source = NULL;
	/* No allocations after atomic state swap. Retire the old set below. */
	for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++)
		swap(stream->frames[i], buffers->frames[i]);
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		swap(stream->transfers[i].sgt, buffers->tables[i]);
		stream->transfers[i].sg_frame = NULL;
	}
	stream->buf_size = buffers->size;
	stream->bytes_pix = buffers->bytes_pix;
	*prepared = NULL;
	mutex_unlock(&stream->mutex);
	fl2000_streaming_free_buffers(usb_dev, buffers);
	return 0;
}

/**
 * fl2000_start_stream() - Drain old transfers and resubmit the retained image
 * @stream: Owning bulk stream.
 *
 * Recheck scanout intent after draining URBs and clearing the halt; a shutdown
 * completion can stop the queue during drainage.
 *
 * Context: Process context. Caller holds the stream mutex.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_start_stream(struct fl2000_stream *stream)
{
	unsigned long flags;
	int ret;

	fl2000_drain_stream(stream);
	if (fl2000_usb_unavailable(stream->usb_dev)) {
		ret = -ENODEV;
		goto recover;
	}
	if (!stream->latest)
		return -EINVAL;
	ret = usb_clear_halt(stream->usb_dev, usb_sndbulkpipe(stream->usb_dev, STREAMING_EP));
	if (ret)
		goto recover;
	spin_lock_irqsave(&stream->lock, flags);
	/* A shutdown completion during drainage can withdraw restart intent. */
	if (!stream->running) {
		ret = -ECANCELED;
		goto unlock;
	}
	stream->enabled = true;
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
		ret = fl2000_submit_transfer(&stream->transfers[i]);
		if (ret)
			break;
	}
unlock:
	spin_unlock_irqrestore(&stream->lock, flags);
	if (ret)
		fl2000_drain_stream(stream);
	return ret;
recover:
	spin_lock_irqsave(&stream->lock, flags);
	fl2000_stream_error(stream, ret);
	spin_unlock_irqrestore(&stream->lock, flags);
	return ret;
}

/**
 * fl2000_recover_stream() - Run a bounded restart if scanout is still requested
 * @work: Scheduled subsystem work item.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_recover_stream(struct work_struct *work)
{
	struct fl2000_stream *stream =
		container_of(to_delayed_work(work), struct fl2000_stream, recovery);
	unsigned long flags;
	bool retry;
	int ret;

	mutex_lock(&stream->mutex);
	spin_lock_irqsave(&stream->lock, flags);
	retry = stream->running && !stream->enabled && stream->latest &&
		stream->retries < FL2000_RECOVERY_RETRIES;
	if (retry)
		stream->retries++;
	spin_unlock_irqrestore(&stream->lock, flags);
	if (retry) {
		ret = fl2000_start_stream(stream);
		if (ret)
			dev_err_ratelimited(&stream->usb_dev->dev,
					    "Streaming recovery failed: %d\n", ret);
	}
	mutex_unlock(&stream->mutex);
}

/**
 * fl2000_streaming_enable() - Start or retain scanout of the latest image
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 *
 * Requires an installed buffer set and a prepared latest image. Enabling a
 * stopped queue resets its retry budget. Recoverable errors can schedule
 * delayed restarts even when this call returns an error.
 *
 * Context: Process context. Takes the stream mutex and may drain old URBs.
 * Return: 0 on success or a negative error code.
 */
int fl2000_streaming_enable(const struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);
	unsigned long flags;
	bool enabled;
	int ret = 0;

	if (!stream)
		return -ENODEV;
	mutex_lock(&stream->mutex);
	spin_lock_irqsave(&stream->lock, flags);
	enabled = stream->enabled;
	stream->running = true;
	if (!enabled)
		stream->retries = 0;
	spin_unlock_irqrestore(&stream->lock, flags);
	if (!enabled) {
		cancel_delayed_work(&stream->recovery);
		ret = fl2000_start_stream(stream);
	}
	mutex_unlock(&stream->mutex);
	return ret;
}

/**
 * fl2000_streaming_disable() - Stop submission and suppress delayed restarts
 * @usb_dev: Adapter whose streaming resources remain alive for the call.
 *
 * Drains all submitted URBs. Pending recovery is canceled; an already blocked
 * worker may still run but observes stopped scanout and submits nothing.
 * Resource destruction additionally synchronizes that worker before freeing.
 *
 * Context: Process context. Takes the stream mutex; cannot run from recovery
 *          work.
 */
void fl2000_streaming_disable(const struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = fl2000_find_stream(usb_dev);

	if (!stream)
		return;
	mutex_lock(&stream->mutex);
	fl2000_stop_stream(stream);
	mutex_unlock(&stream->mutex);
}

/**
 * fl2000_streaming_create() - Create reusable bulk URBs
 * @interface: Claimed streaming interface; control endpoint setup is
 *             complete.
 *
 * Attaches release-managed resources to the USB parent. Frame allocations are
 * separate and supplied by atomic preparation.
 *
 * Context: Process context. Caller serializes interface lifecycle.
 * Return: 0 on success or a negative error code.
 */
int fl2000_streaming_create(struct usb_interface *interface)
{
	struct fl2000_device *device = dev_get_drvdata(&interface_to_usbdev(interface)->dev);
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct usb_interface *control = usb_ifnum_to_if(usb_dev, FL2000_USBIF_AVCONTROL);
	struct usb_endpoint_descriptor *ep;
	struct fl2000_stream *stream;
	int ret;

	if (!control || usb_find_bulk_out_endpoint(control->cur_altsetting, &ep) ||
	    usb_endpoint_num(ep) != STREAMING_EP)
		return -ENODEV;
	stream = devres_alloc(fl2000_streaming_release, sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;
	stream->usb_dev = usb_dev;
	INIT_DELAYED_WORK(&stream->recovery, fl2000_recover_stream);
	mutex_init(&stream->mutex);
	spin_lock_init(&stream->lock);
	for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
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
	device->stream = stream;
	return 0;
fail:
	fl2000_streaming_release(&usb_dev->dev, stream);
	devres_free(stream);
	return ret;
}

/**
 * fl2000_streaming_destroy() - Drain and release bulk resources
 * @interface: Streaming interface whose component master is already removed.
 *
 * Context: Process context. Caller excludes updates and interface
 *          reinitialization.
 */
void fl2000_streaming_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	devres_release(&usb_dev->dev, fl2000_streaming_release, NULL, NULL);
	device->stream = NULL;
}
