/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef __FL2000_DRM_H__
#define __FL2000_DRM_H__

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/usb.h>

struct device;
struct drm_device;
struct drm_rect;
struct i2c_adapter;
struct module;

/**
 * enum fl2000_interface - USB interfaces claimed as one adapter
 * @FL2000_USBIF_AVCONTROL: Register access and bulk endpoint configuration.
 * @FL2000_USBIF_STREAMING: Interface holding the bulk-stream resource
 *                          lifetime.
 * @FL2000_USBIF_INTERRUPT: USB interrupt endpoint resource lifetime.
 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

/**
 * struct fl2000_timings - Controller timing values, excluding PLL encoding
 * @hactive: Active pixels per line.
 * @htotal: Total pixel clocks per line.
 * @hsync_width: Horizontal sync width in pixel clocks.
 * @hstart: Controller active-start encoding: htotal - hsync_start + 1.
 * @vactive: Active lines per frame.
 * @vtotal: Total lines per frame.
 * @vsync_width: Vertical sync width in lines.
 * @vstart: Controller active-start encoding: vtotal - vsync_start + 1.
 * @hsync_positive: True for positive horizontal sync polarity.
 * @vsync_positive: True for positive vertical sync polarity.
 *
 * Values are derived from a validated DRM adjusted mode. Start values are
 * controller encodings, not DRM sync-start coordinates.
 */
struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
	bool hsync_positive;
	bool vsync_positive;
};

/**
 * struct fl2000_pll - Validated PLL register field encodings
 * @prescaler: Prescaler field encoding, selected by the PLL search.
 * @multiplier: Multiplier field encoding.
 * @divisor: Output divider field encoding.
 * @function: Function field sharing the pre-divider byte.
 */
struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

/* Controller I2C poll interval and timeout, in microseconds. */
#define I2C_RDWR_INTERVAL 200
#define I2C_RDWR_TIMEOUT  (256 * 1000)

/* Endpoint numbers; direction is selected by the USB pipe. */
#define STREAMING_EP 1
#define INTERRUPT_EP 3

/* Bulk streaming. */
int fl2000_streaming_create(struct usb_interface *interface);
void fl2000_streaming_destroy(struct usb_interface *interface);
struct fl2000_stream_buffers;
struct fl2000_stream_buffers *fl2000_streaming_prepare(struct usb_device *usb_dev, int pixels,
						       u32 bytes_pix);
void fl2000_streaming_free_buffers(struct usb_device *usb_dev,
				   struct fl2000_stream_buffers *buffers);
int fl2000_streaming_mode_set(struct usb_device *usb_dev, struct fl2000_stream_buffers **prepared);
struct iosys_map;
int fl2000_streaming_update(const struct usb_device *usb_dev, const struct iosys_map *src,
			    bool read_wc, unsigned int height, unsigned int width,
			    unsigned int pitch, u32 format, const struct drm_rect *damage);
int fl2000_streaming_enable(const struct usb_device *usb_dev);
void fl2000_streaming_disable(const struct usb_device *usb_dev);

/* USB interrupt reception and status recovery. */
int fl2000_interrupt_create(struct usb_interface *interface);
void fl2000_interrupt_destroy(struct usb_interface *interface);
int fl2000_enable_interrupt(struct usb_device *usb_dev);
void fl2000_disable_interrupt(const struct usb_device *usb_dev);

/* I2C adapter. */
struct i2c_adapter *fl2000_i2c_adapter(const struct usb_device *usb_dev);
int fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct usb_device *usb_dev);
bool fl2000_i2c_verify_client(const struct usb_device *usb_dev, struct device *client_dev);

/* Controller registers. */
int fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct usb_device *usb_dev);
int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_init_clock(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev, const struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, const struct fl2000_pll *pll);
int fl2000_enable_interrupts(struct usb_device *usb_dev);
int fl2000_check_interrupt(struct usb_device *usb_dev);
int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data);

/* Component master and DRM lifecycle. */
int fl2000_drm_init(struct usb_device *usb_dev);
void fl2000_drm_cleanup(struct usb_device *usb_dev);
int fl2000_drm_hotplug(const struct usb_device *usb_dev);
void fl2000_drm_set_polling(const struct usb_device *usb_dev, bool polling);
int fl2000_drm_suspend(struct usb_device *usb_dev);
int fl2000_drm_resume(struct usb_device *usb_dev);

struct fl2000_stream;
struct fl2000_interrupt;

/**
 * struct fl2000_device - State shared by the claimed USB interfaces
 * @stream: Bulk resources, owned by the streaming interface.
 * @interrupt: Interrupt URB, work and IRQ domain resources.
 * @i2c: Adapter owned by the control interface; NULL before registration.
 * @bridge_owner: Provider module selected under the component framework lock.
 * @i2c_errors: Failed adapter transfers, including IRQ acknowledgment errors.
 * @interfaces: Claimed interfaces, cleared before sibling disconnect
 *              callbacks.
 * @active_interfaces: Interfaces whose subsystem initialization completed.
 * @suspended: Adapter PM transition completed under @lock.
 * @drm: Bound DRM device, or NULL; IRQ work reads it with READ_ONCE().
 *
 * Allocated on the USB parent device. USB core parent locking serializes its
 * initial publication. Independent adapters do not share lifecycle locks.
 * Component removal drains IRQ work before the DRM reference and interface
 * resources can disappear; reading @drm alone does not acquire a reference.
 */
struct fl2000_device {
	/** @lock: Serializes USB lifecycle and PM for this adapter. */
	struct mutex lock;
	struct fl2000_stream *stream;
	struct fl2000_interrupt *interrupt;
	struct i2c_adapter *i2c;
	struct module *bridge_owner;
	atomic_t i2c_errors;
	struct usb_interface *interfaces[FL2000_USBIF_INTERRUPT + 1];
	unsigned long active_interfaces;
	bool suspended;
	struct drm_device *drm;
};

/**
 * fl2000_usb_unavailable() - Check whether USB core has detected device removal
 * @usb_dev: USB parent whose driver resources remain alive.
 *
 * Context: Any context with adapter resource lifetime guaranteed. Does not sleep.
 * Return: True when USB core has marked the physical device detached.
 */
static inline bool fl2000_usb_unavailable(const struct usb_device *usb_dev)
{
	return READ_ONCE(usb_dev->state) == USB_STATE_NOTATTACHED;
}

/**
 * fl2000_usb_error_terminal() - Check whether USB traffic should stop
 * @error: USB submission, completion or control-request result.
 *
 * Cancellation, rejected submission and endpoint shutdown stop the affected
 * queue without automatic recovery. They do not establish adapter removal;
 * an explicit start may retry while the device remains attached.
 *
 * Context: Any context. Does not sleep or change device state.
 * Return: True for cancellation, rejected submission or device-loss errors.
 */
static inline bool fl2000_usb_error_terminal(int error)
{
	switch (error) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
	case -EPERM:
		return true;
	default:
		return false;
	}
}

#endif /* __FL2000_DRM_H__ */
