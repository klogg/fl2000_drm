/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_avcontrol.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2020, Artem Mygaiev
 */

#include "fl2000.h"

/* I2C adapter interface creation */
int fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct usb_device *usb_dev);

/* Register map creation */
int fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct usb_device *usb_dev);

/* DRM device creation */
int fl2000_drm_init(struct usb_device *usb_dev);
void fl2000_drm_cleanup(struct usb_device *usb_dev);

/* Ordered list of "init"/"cleanup functions for sub-devices. Shall be processed
 * forward order on init and backward on cleanup */
static const struct {
	const char *name;
	int (*init_fn)(struct usb_device *usb_dev);
	void (*cleanup_fn)(struct usb_device *usb_dev);
} fl2000_devices[] = {
	{"registers map", fl2000_regmap_init, fl2000_regmap_cleanup},
	{"I2C adapter", fl2000_i2c_init, fl2000_i2c_cleanup},
	{"DRM device", fl2000_drm_init, fl2000_drm_cleanup}
};

struct fl2000_drv {
	bool __device_up[ARRAY_SIZE(fl2000_devices)];
};

static void fl2000_drv_release(struct device *dev, void *res)
{
	/* Noop */
}

void fl2000_avcontrol_destroy(struct usb_interface *interface)
{
	int i;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_drv *drv = devres_find(&usb_dev->dev,
				fl2000_drv_release, NULL, NULL);

	if (drv) {
		for (i = ARRAY_SIZE(fl2000_devices); i > 0; i--)
			if (drv->__device_up[i-1]) {
				(*fl2000_devices[i-1].cleanup_fn)(usb_dev);
				drv->__device_up[i-1] = false;
			}
		devres_release(&usb_dev->dev, fl2000_drv_release, NULL, NULL);
	}
}

int fl2000_avcontrol_create(struct usb_interface *interface)
{
	int i = 0, ret;
	struct fl2000_drv *drv;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	drv = devres_alloc(fl2000_drv_release, sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		dev_err(&usb_dev->dev, "USB data allocation failed");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, drv);

	for (i = 0; i < ARRAY_SIZE(fl2000_devices); i++)
		if (!drv->__device_up[i]) {
			ret = (*fl2000_devices[i].init_fn)(usb_dev);
			if (ret) {
				dev_err(&usb_dev->dev, "Cannot create %s (%d)",
						fl2000_devices[i].name, ret);
				break;
			}
			drv->__device_up[i] = true;
		}

	if (ret)
		fl2000_avcontrol_destroy(interface);

	dev_info(&usb_dev->dev, "AVControl interface up");

	return ret;
}
