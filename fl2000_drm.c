/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm_drm.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_atomic_helper.h>

#define DRM_DRIVER_NAME		"fl2000_drm"
#define DRM_DRIVER_DESC		"USB-HDMI"
#define DRM_DRIVER_DATE		"20181001"

#define DRM_DRIVER_MAJOR	0
#define DRM_DRIVER_MINOR	0
#define DRM_DRIVER_PATCHLEVEL	1

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
	struct drm_device *drm;
};

DEFINE_DRM_GEM_CMA_FOPS(fl2000_drm_driver_fops);

static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME | \
		DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,

	.dumb_create = drm_gem_cma_dumb_create,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.date = DRM_DRIVER_DATE,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl200_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int fl2000_modeset_init(struct drm_device *dev)
{
	int ret = 0;
	struct drm_mode_config *mode_config;

	drm_mode_config_init(dev);
	mode_config = &dev->mode_config;
	mode_config->funcs = &fl200_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = 2560;
	mode_config->min_height = 1;
	mode_config->max_height = 1440;


	drm_mode_config_reset(dev);

	drm_fb_cma_fbdev_init(dev, priv->variant->fb_bpp, 0);

	drm_kms_helper_poll_init(dev);

	return 0;
}

int fl2000_drm_create(struct usb_interface *interface)
{
	int ret = 0;
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	drm = drm_dev_alloc(&fl2000_drm_driver, &usb_dev->dev);
	if (IS_ERR(drm)) {
		return PTR_ERR(drm);
		goto error;
	}

	drm_if = kzalloc(sizeof(*drm_if), GFP_KERNEL);
	if (IS_ERR(drm_if)) {
		dev_err(&interface->dev, "Cannot allocate DRM private " \
				"structure");
		ret = PTR_ERR(drm_if);
		goto error;
	}

	drm_if->interface = interface;
	drm_if->usb_dev = usb_dev;
	drm_if->drm = drm;

	ret = fl2000_modeset_init(drm);
	if (ret < 0)
		goto error;

	drm->dev_private = drm_if;

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto error;

	usb_set_intfdata(interface, drm_if);

	return 0;

error:
	fl2000_drm_destroy(interface);
	return ret;
}

void fl2000_drm_destroy(struct usb_interface *interface)
{

}
