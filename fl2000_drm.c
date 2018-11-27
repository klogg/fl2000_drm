/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm_drm.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define DRM_DRIVER_NAME		"fl2000_drm"
#define DRM_DRIVER_DESC		"USB-HDMI"
#define DRM_DRIVER_DATE		"20181001"

#define DRM_DRIVER_MAJOR	0
#define DRM_DRIVER_MINOR	0
#define DRM_DRIVER_PATCHLEVEL	1

#define MAX_WIDTH	4000
#define MAX_HEIGHT	4000
#define BPP		32
#define MAX_CONN	1

/* List all supported bridges */
static const char *fl2000_supported_bridges[] = {
	"it66121",		/* IT66121 driver name*/
};

/* I2C adapter belong to DRM */
struct i2c_adapter *fl2000_i2c_create(struct usb_device *usb_dev);
void fl2000_i2c_destroy(struct i2c_adapter *adapter);

static const u32 fl2000_pixel_formats[] = {
	/* 24-bit RGB le */
	DRM_FORMAT_RGB888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	/* 16-bit RGB le */
	DRM_FORMAT_RGB565,
	/* 15-bit RGB le */
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
};

struct fl2000_drm_if {
	struct i2c_adapter *adapter;
	struct drm_device *drm;
	struct drm_simple_display_pipe pipe;
	struct component_match *match;
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

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static enum drm_mode_status fl2000_mode_valid(struct drm_crtc *crtc,
		const struct drm_display_mode *mode)
{
	/* TODO: check mode against USB bulk endpoint bandwidth */
	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *cstate,
		struct drm_plane_state *plane_state)
{
	/* TODO: all FL2000DX HW configuration stuff here */
}

void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	/* TODO: disable HW */
}

static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *old_pstate)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane *plane = &pipe->plane;
	struct drm_plane_state *pstate = plane->state;
	struct drm_framebuffer *fb = pstate->fb;

	if (fb) {
		//dma_addr_t addr = drm_fb_cma_get_gem_addr(fb, pstate, 0);
		int idx;

		/* TODO: Do we really need this? What if it fails? */
		if (!drm_dev_enter(drm, &idx)) return;

		/* TODO:
		 * Calculate & validate real buffer area for transmission
		 * Schedule transmission of 'addr' over USB */

		drm_dev_exit(idx);
	}

	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&drm->event_lock);
		if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&drm->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.update = fl2000_display_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static int fl2000_bind(struct device *master)
{
	int ret = 0;
	struct usb_interface *interface = container_of(master,
			struct usb_interface, dev);
	struct fl2000_drm_if *drm_if = usb_get_intfdata(interface);
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	u64 mask;

	dev_info(master, "Binding FL2000 master component");

	drm = drm_dev_alloc(&fl2000_drm_driver, master);
	if (IS_ERR_OR_NULL(drm)) {
		dev_err(master, "Cannot allocate DRM device");
		ret = PTR_ERR(drm_if->drm);
		goto error;
	}
	drm->dev_private = drm_if;
	drm_if->drm = drm;

	mask = dma_get_mask(drm->dev);

	drm_mode_config_init(drm);
	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = MAX_HEIGHT;

	ret = dma_set_coherent_mask(drm->dev, mask);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot set DRM device DMA mask");
		goto error;
	}

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe,
			&fl2000_display_funcs, fl2000_pixel_formats,
			ARRAY_SIZE(fl2000_pixel_formats), NULL, NULL);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot configure simple display pipe");
		goto error;
	}

	/* Attach bridge */
	ret = component_bind_all(master, &drm_if->pipe);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot attach bridge");
		goto error;
	}

	drm_mode_config_reset(drm);

	ret = drm_fb_cma_fbdev_init(drm, BPP, MAX_CONN);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot initialize CMA framebuffer");
		goto error;
	}

	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret < 0) {
		dev_err(drm->dev, "Cannot register DRM device");
		goto error;
	}

	return 0;

error:
	return ret;
}

static void fl2000_unbind(struct device *master)
{
	struct usb_interface *interface = container_of(master,
			struct usb_interface, dev);
	struct fl2000_drm_if *drm_if = usb_get_intfdata(interface);
	struct drm_device *drm;

	dev_info(master, "Unbinding FL2000 master component");

	if (IS_ERR_OR_NULL(drm_if)) return;

	drm = drm_if->drm;

	if (IS_ERR_OR_NULL(drm)) return;

	drm_dev_unregister(drm);

	drm_fb_cma_fbdev_fini(drm);

	/* Detach bridge */
	component_unbind_all(master, drm);

	drm_mode_config_cleanup(drm);

	drm_dev_put(drm);
}

static struct component_master_ops fl2000_drm_master_ops = {
	.bind = fl2000_bind,
	.unbind = fl2000_unbind,
};

static int fl2000_compare(struct device *dev, void *data)
{
	int i;
	struct device *master_dev = data;

	/* Check component's parent (must be I2C adapter) */
	if (dev->parent != master_dev)
		return 0;

	/* Check this is a supported DRM bridge */
	for (i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strcmp(fl2000_supported_bridges[i], dev->driver->name)) {
			dev_info(dev, "Found bridge %s", dev->driver->name);
			return (i + 1); /* Must be not 0 for success */
		}

	return 0;
}

void fl2000_drm_destroy(struct usb_interface *interface);

int fl2000_drm_create(struct usb_interface *interface)
{
	int ret = 0;
	struct i2c_adapter *adapter;
	struct fl2000_drm_if *drm_if;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	drm_if = kzalloc(sizeof(*drm_if), GFP_KERNEL);
	if (IS_ERR_OR_NULL(drm_if)) {
		dev_err(&interface->dev, "Cannot allocate DRM private " \
				"structure");
		ret = PTR_ERR(drm_if);
		goto error;
	}
	usb_set_intfdata(interface, drm_if);

	/* Create I2C adapter */
	adapter = fl2000_i2c_create(usb_dev);
	if (IS_ERR_OR_NULL(adapter)) {
		dev_err(&interface->dev, "Cannot register I2C adapter");
		ret = PTR_ERR(adapter);
		goto error;
	}
	drm_if->adapter = adapter;

	/* Make USB interface master */
	component_match_add(&interface->dev, &drm_if->match, fl2000_compare,
			&adapter->dev);

	ret = component_master_add_with_match(&interface->dev,
			&fl2000_drm_master_ops, drm_if->match);
	if (ret != 0) {
		dev_err(&interface->dev, "Cannot register component master");
		goto error;
	}

	return 0;

error:
	fl2000_drm_destroy(interface);
	return ret;
}

void fl2000_drm_destroy(struct usb_interface *interface)
{
	struct fl2000_drm_if *drm_if;

	drm_if = usb_get_intfdata(interface);
	if (drm_if == NULL)
		return;

	fl2000_i2c_destroy(drm_if->adapter);

	component_master_del(&interface->dev, &fl2000_drm_master_ops);

	kfree(drm_if);
}

void fl2000_process_event(struct usb_device *usb_dev)
{
	int ret;
	u32 *status = kmalloc(sizeof(*status), GFP_KERNEL);

	if (IS_ERR_OR_NULL(status)) return;

	ret = fl2000_reg_read(usb_dev, status, FL2000_REG_INT_STATUS);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Cannot read interrupt" \
				"register (%d)", ret);
	} else {
		dev_info(&usb_dev->dev, " *** 0x%4X", *status);
	}

	kfree(status);
}
