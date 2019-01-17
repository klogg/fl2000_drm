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

int fl2000_debugfs_init(struct drm_minor *minor);

/* List all supported bridges */
static const char *fl2000_supported_bridges[] = {
	"it66121", /* IT66121 driver name*/
};

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
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
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

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = fl2000_debugfs_init,
#endif
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

	dev_info(crtc->dev->dev, "fl2000_mode_valid");

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *cstate,
		struct drm_plane_state *plane_state)
{
	/* TODO: all FL2000DX HW configuration stuff here */

	dev_info(pipe->crtc.dev->dev, "fl2000_display_enable");

	/* Reject USB U1/U2 transitions */
	//regmap_write_bits(regmap, FL2000_USB_LPM, (3<<19), (3<<19));
	//regmap_write_bits(regmap, FL2000_USB_LPM, (3<<20), (3<<20));

	/* Enable wakeup auto reset */
	//regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, (1<<10), (1<<10));
}

void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	/* TODO: disable HW */

	dev_info(pipe->crtc.dev->dev, "fl2000_display_disable");

	/* Accept USB U1/U2 transitions */
	//regmap_write_bits(regmap, FL2000_USB_LPM, (3<<19), ~(3<<19));
	//regmap_write_bits(regmap, FL2000_USB_LPM, (3<<20), ~(3<<20));

	/* Disable wakeup auto reset */
	//regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, (1<<10), ~(1<<10));
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

	dev_info(pipe->crtc.dev->dev, "fl2000_display_update");

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

static void fl2000_drm_release(struct device *dev, void *res)
{
	/* Noop */
}

static int fl2000_bind(struct device *master)
{
	int ret = 0;
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	u64 mask;

	dev_info(master, "Binding FL2000 master component");

	drm_if = devres_find(master, fl2000_drm_release, NULL, NULL);
	if (drm_if == NULL) {
		dev_err(master, "Cannot find DRM private structure");
		return -ENODEV;
	}

	drm = &drm_if->drm;

	ret = drm_dev_init(drm, &fl2000_drm_driver, master);
	if (ret != 0) {
		dev_err(master, "Cannot initialize DRM device (%d)", ret);
		return ret;
	}

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
		dev_err(drm->dev, "Cannot set DRM device DMA mask (%d)", ret);
		return ret;
	}

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe,
			&fl2000_display_funcs, fl2000_pixel_formats,
			ARRAY_SIZE(fl2000_pixel_formats), NULL, NULL);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)", ret);
		return ret;
	}

	/* Attach bridge */
	ret = component_bind_all(master, &drm_if->pipe);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot attach bridge (%d)", ret);
		return ret;
	}

	drm_mode_config_reset(drm);

	ret = drm_fb_cma_fbdev_init(drm, BPP, MAX_CONN);
	if (ret != 0) {
		dev_err(drm->dev, "Cannot initialize CMA framebuffer (%d)", ret);
		return ret;
	}

	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret < 0) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		return ret;
	}

	return 0;
}

static void fl2000_unbind(struct device *master)
{
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;

	dev_info(master, "Unbinding FL2000 master component");

	drm_if = devres_find(master, fl2000_drm_release, NULL, NULL);
	if (drm_if == NULL) return;

	drm = &drm_if->drm;
	if (drm == NULL) return;

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

static void fl2000_match_release(struct device *dev, void *data)
{
	//component_master_del(dev, &fl2000_drm_master_ops);
}

static int fl2000_compare(struct device *dev, void *data)
{
	int i;
	struct i2c_adapter *adapter = data;

	/* Check component's parent (must be I2C adapter) */
	if (dev->parent != &adapter->dev)
		return 0;

	/* Check this is a supported DRM bridge */
	for (i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strcmp(fl2000_supported_bridges[i], dev->driver->name)) {
			dev_info(dev, "Found bridge %s", dev->driver->name);
			return (i + 1); /* Must be not 0 for success */
		}

	return 0;
}

struct i2c_adapter *fl2000_get_i2c_adapter(struct usb_device *usb_dev);

int fl2000_drm_create(struct usb_device *usb_dev)
{
	int ret = 0;
	struct component_match *match = NULL;
	struct fl2000_drm_if *drm_if;
	struct i2c_adapter *adapter;

	adapter = fl2000_get_i2c_adapter(usb_dev);
	if (!adapter) {
		dev_err(&usb_dev->dev, "Cannot find I2C adapter");
		return -ENODEV;
	}

	drm_if = devres_alloc(&fl2000_drm_release, sizeof(*drm_if), GFP_KERNEL);
	if (IS_ERR_OR_NULL(drm_if)) {
		ret = IS_ERR(drm_if) ? PTR_ERR(drm_if) : -ENOMEM;
		dev_err(&usb_dev->dev, "Cannot allocate DRM private structure (%d)", ret);
		return ret;
	}
	devres_add(&usb_dev->dev, drm_if);

	/* Make USB interface master */
	component_match_add_release(&usb_dev->dev, &match,
			fl2000_match_release, fl2000_compare, adapter);

	ret = component_master_add_with_match(&usb_dev->dev,
			&fl2000_drm_master_ops, match);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Cannot register component master (%d)", ret);
		return ret;
	}

	return 0;
}
