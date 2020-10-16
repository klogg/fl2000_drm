/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef __FL2000_DRM_H__
#define __FL2000_DRM_H__

#define DEBUG 1

#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/regmap.h>
#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/time.h>
#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_damage_helper.h>

#include "fl2000_registers.h"

#if defined(CONFIG_DEBUG_FS)

/* NOTE: it might be unsafe / insecure to use allow rw access everyone */
#include <linux/debugfs.h>
static const umode_t fl2000_debug_umode = 0666;

#endif /* CONFIG_DEBUG_FS */

/* Custom code for DRM bridge autodetection since there is no DT support */
#define I2C_CLASS_HDMI	BIT(9)

/**
 * fl2000_add_bitmask - Set bitmask for structure field
 *
 * @__mask: Variable to set mask to (assumed u32)
 * @__type: Structure type to use with bitfield (assumed size equal to u32)
 * @__field: Field to set mask for in the '__type' structure
 *
 * Sets bits to 1 in '__mask' variable that correspond to field '__field' of
 * structure type '__type'. Tested only with u32 data types
 */
#define fl2000_add_bitmask(__mask, __type, __field) \
({ \
	union { \
		__type __mask; \
		typeof(__mask) __val; \
	}  __aligned(4) __data; \
	__data.__mask.__field = ~0; \
	(__mask) |= __data.__val; \
})

/* Iterate over array */
#define for_each_array_item(array, idx) \
	for (idx = 0; idx < ARRAY_SIZE(array); idx++)

static inline int fl2000_submit_urb(struct urb *urb)
{
	int ret;
	int attempts = 10;

	do {
		ret = usb_submit_urb(urb, GFP_KERNEL);
		switch (ret) {
		case -ENXIO:
		case -ENOMEM:
			if (attempts--) {
				cond_resched();
				ret = -EAGAIN;
			}
			break;
		default:
			break;
		}
	} while (ret == -EAGAIN);

	return ret;
}

static inline int fl2000_urb_status(struct usb_device *usb_dev, int status, int pipe)
{
	int ret = 0;

	switch (status) {
	/* All went well */
	case 0:
		break;

	/* URB was unlinked or device shutdown in progress, do nothing */
	case -ECONNRESET:
	case -ENOENT:
	case -ENODEV:
		ret = -1;
		break;

	/* Hardware or protocol errors - no recovery, report and do nothing */
	case -ESHUTDOWN:
	case -EPROTO:
	case -EILSEQ:
	case -ETIME:
		dev_err(&usb_dev->dev, "USB hardware unrecoverable error %d", status);
		ret = -1;
		break;

	/* Stalled endpoint */
	case -EPIPE:
		dev_err(&usb_dev->dev, "Pipe %d stalled", pipe);
		ret = usb_clear_halt(usb_dev, pipe);
		if (ret != 0) {
			dev_err(&usb_dev->dev, "Cannot reset endpoint, error %d", ret);
			ret = -1;
		}
		break;

	/* Shutting down */
	case -EPERM:
		dev_err(&usb_dev->dev, "Shutting down interface, URB canceled");
		ret = 0;
		break;

	/* All the rest cases - igonore */
	default:
		break;
	}

	return ret;
}

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

enum fl2000_int_status {
	CLEAR,	/* nothing to do */
	EVENT,	/* sink connection event */
	ERROR,	/* unrecoverable error */
};

/* AVControl transfer task */
int fl2000_avcontrol_create(struct usb_interface *interface);
void fl2000_avcontrol_destroy(struct usb_interface *interface);

/* Stream transfer task */
int fl2000_stream_create(struct usb_interface *interface);
void fl2000_stream_destroy(struct usb_interface *interface);

/* Streaming interface */
int fl2000_stream_mode_set(struct usb_device *usb_dev, int pixels, u32 bytes_pix);
void fl2000_stream_compress(struct usb_device *usb_dev, struct drm_framebuffer *fb, void *src);
int fl2000_stream_enable(struct usb_device *usb_dev);
void fl2000_stream_disable(struct usb_device *usb_dev);

/* Interrupt polling task */
int fl2000_intr_create(struct usb_interface *interface);
void fl2000_intr_destroy(struct usb_interface *interface);

/* I2C adapter interface creation */
int fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct usb_device *usb_dev);

/* Register map creation */
int fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct usb_device *usb_dev);

/* Registers interface */
int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll);
int fl2000_enable_interrupts(struct usb_device *usb_dev);
enum fl2000_int_status fl2000_check_interrupt(struct usb_device *usb_dev);

/* DRM device creation */
int fl2000_drm_init(struct usb_device *usb_dev);
void fl2000_drm_cleanup(struct usb_device *usb_dev);

/* DRM interface */
void fl2000_display_vblank(struct usb_device *usb_dev);
void fl2000_display_event_check(struct usb_device *usb_dev);

#endif /* __FL2000_DRM_H__ */
