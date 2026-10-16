/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef __FL2000_DRM_H__
#define __FL2000_DRM_H__

#include <linux/module.h>
#include <linux/types.h>
#include <linux/math64.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <drm/drm_gem.h>
#include <drm/drm_vblank.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_damage_helper.h>

#include "fl2000_registers.h"

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

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

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

/* Timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL 200
#define I2C_RDWR_TIMEOUT  (256 * 1000)

/* Endpoints we want to use for Bulk streaming and Interrupt transfers */
#define STREAMING_EP 1
#define INTERRUPT_EP 3

/* Streaming transfer task creation */
int fl2000_streaming_create(struct usb_interface *interface);
void fl2000_streaming_destroy(struct usb_interface *interface);
/* ... and interface */
struct fl2000_stream_buffers;
struct fl2000_stream_buffers *fl2000_streaming_prepare(struct usb_device *usb_dev, int pixels,
						       u32 bytes_pix);
void fl2000_streaming_free_buffers(struct usb_device *usb_dev,
				   struct fl2000_stream_buffers *buffers);
int fl2000_streaming_mode_set(struct usb_device *usb_dev, struct fl2000_stream_buffers **prepared);
int fl2000_streaming_update(struct usb_device *usb_dev, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch, u32 format,
			    const struct drm_rect *damage);
int fl2000_streaming_enable(struct usb_device *usb_dev);
void fl2000_streaming_disable(struct usb_device *usb_dev);

/* Interrupt polling task creation */
int fl2000_interrupt_create(struct usb_interface *interface);
void fl2000_interrupt_destroy(struct usb_interface *interface);
/* ... and interface */
int fl2000_enable_interrupt(struct usb_device *usb_dev);
int fl2000_disable_interrupt(struct usb_device *usb_dev);

/* I2C adapter interface creation */
struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *usb_dev);
int fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct usb_device *usb_dev);
/* ... and interface*/
bool fl2000_i2c_verify_client(struct usb_device *usb_dev, struct device *client_dev,
			      const char *client_name);

/* Register map creation */
int fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct usb_device *usb_dev);
/* ... and interface */
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

/* DRM device creation */
int fl2000_drm_init(struct usb_device *usb_dev);
void fl2000_drm_cleanup(struct usb_device *usb_dev);
/* ... and interface */
int fl2000_drm_hotplug(const struct usb_device *usb_dev);
void fl2000_drm_set_polling(const struct usb_device *usb_dev, bool polling);
int fl2000_drm_suspend(struct usb_device *usb_dev);
int fl2000_drm_resume(struct usb_device *usb_dev);

struct fl2000_stream;
struct fl2000_interrupt;

struct fl2000_device {
	struct mutex lock; /* Serializes lifecycle and PM for this adapter. */
	struct fl2000_stream *stream;
	struct fl2000_interrupt *interrupt;
	struct i2c_adapter *i2c;
	struct usb_interface *interfaces[FL2000_USBIF_INTERRUPT + 1];
	unsigned long active_interfaces;
	bool master_registered;
	bool suspended;
	struct drm_device *drm;
};

#endif /* __FL2000_DRM_H__ */
