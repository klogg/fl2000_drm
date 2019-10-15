/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm_drm.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define DRM_DRIVER_NAME		"fl2000_drm"
#define DRM_DRIVER_DESC		"USB-HDMI"
#define DRM_DRIVER_DATE		"20181001"

#define DRM_DRIVER_MAJOR	0
#define DRM_DRIVER_MINOR	0
#define DRM_DRIVER_PATCHLEVEL	1

#define MAX_WIDTH		4000
#define MAX_HEIGHT		4000
#define BPP			32

int fl2000_reset(struct usb_device *usb_dev);
int fl2000_wait(struct usb_device *usb_dev);

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
	struct regmap_field *magic;
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
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.date = DRM_DRIVER_DATE,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,

#if defined(_DISABLED_CONFIG_DEBUG_FS)
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
	struct drm_device *drm = crtc->dev;

	dev_info(drm->dev, "DRM mode validation: "DRM_MODE_FMT,
			DRM_MODE_ARG(mode));

	/* TODO: check mode against USB bulk endpoint bandwidth and other FL2000
	 * HW limitations*/

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *cstate,
		struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct regmap *regmap = fl2000_get_regmap(usb_dev);
	fl2000_vga_ctrl_reg_aclk aclk = {.val = 0};
	u32 mask;

	dev_info(drm->dev, "fl2000_display_enable");

	if (IS_ERR(regmap)) {
		dev_err(drm->dev, "Cannot find regmap (%ld)", PTR_ERR(regmap));
		return;
	}

	/* Disable forcing VGA connect */
	mask = 0;
	aclk.force_vga_connect = false;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);
}

void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;

	dev_info(drm->dev, "fl2000_display_disable");

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

	dev_info(drm->dev, "fl2000_display_update");

	if (fb) {
		dma_addr_t addr = drm_fb_cma_get_gem_addr(fb, pstate, 0);
		int idx;

		/* TODO: Do we really need this? What if it fails? */
		if (!drm_dev_enter(drm, &idx))
			return;

		/* TODO:
		 * Calculate & validate real buffer area for transmission
		 * Schedule transmission of 'addr' over USB */
		printk("%lld", addr);

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

static void fl2000_mode_set(struct drm_encoder *encoder,
		 struct drm_display_mode *mode,
		 struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct fl2000_drm_if *drm_if = container_of(drm, struct fl2000_drm_if,
			drm);
	struct regmap *regmap = fl2000_get_regmap(usb_dev);
	fl2000_vga_hsync_reg1 hsync1 = {.val = 0};
	fl2000_vga_hsync_reg2 hsync2 = {.val = 0};
	fl2000_vga_vsync_reg1 vsync1 = {.val = 0};
	fl2000_vga_vsync_reg2 vsync2 = {.val = 0};
	fl2000_vga_cntrl_reg_pxclk pxclk = {.val = 0};
	fl2000_vga_ctrl_reg_aclk aclk = {.val = 0};
	fl2000_vga_pll_reg pll = {.val = 0};
	fl2000_vga_isoch_reg isoch = {.val = 0};
	u32 mask;

	dev_info(drm->dev, "fl2000_mode_set");

	mask = 0;
	aclk.force_pll_up = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_pll_up);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	/* TODO: Calculate PLL settings */
	pll.val = 0x0020410A; // 32MHz
	regmap_write(regmap, FL2000_VGA_HSYNC_REG1, hsync1.val);

	/* TODO: Reset FL2000 and read back PLL settings for validation */

	/* Generic clock configuration */
	mask = 0;
	aclk.use_pkt_pending = false;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, use_pkt_pending);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	aclk.use_zero_pkt_len = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, use_zero_pkt_len);
	aclk.vga_err_int_en = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, vga_err_int_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	pxclk.dac_output_en = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.drop_cnt = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, drop_cnt);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, pxclk.val);

	mask = 0;
	pxclk.dac_output_en = true;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.clear_watermark = true;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, clear_watermark);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, pxclk.val);

	mask = 0;
	isoch.mframe_cnt = 0;
	fl2000_add_bitmask(mask, fl2000_vga_isoch_reg, mframe_cnt);
	regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, mask, isoch.val);

	/* Timings configuration */
	hsync1.hactive = mode->hdisplay;
	hsync1.htotal = mode->htotal;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG1, hsync1.val);

	vsync1.vactive = mode->vdisplay;
	vsync1.vtotal = mode->vtotal;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG1, vsync1.val);

	hsync2.hsync_width = mode->hsync_end - mode->hsync_start;
	hsync2.hstart = (mode->htotal - mode->hsync_start + 1);
	regmap_write(regmap, FL2000_VGA_HSYNC_REG2, hsync2.val);

	vsync2.vsync_width = mode->vsync_end - mode->vsync_start;
	vsync2.vstart = (mode->vtotal - mode->vsync_start + 1);
	vsync2.start_latency = vsync2.vstart;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG2, vsync2.val);

	/* XXX: This is actually some unknown & undocumented FL2000 USB FE
	 * register setting */
	regmap_field_write(drm_if->magic, true);

	/* Force VGA connect to allow bridge perform its setup */
	mask = 0;
	aclk.force_vga_connect = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);
}

/* TODO: Possibly we need .check callback as well */
/* It is assumed that FL2000 has bridge either connected to its DPI or
 * implementing analog D-SUB frontend. In this case initialization flow:
 *  1. fl2k mode_set
 *  2. bridge mode_set
 *  3. fl2k enable
 *  4. bridge enable */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.update = fl2000_display_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_mode_set,
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
	struct usb_device *usb_dev = container_of(master, struct usb_device,
			dev);
	struct regmap *regmap = fl2000_get_regmap(usb_dev);
	u64 dma_mask;

	dev_info(master, "Binding FL2000 master component");

	drm_if = devres_alloc(&fl2000_drm_release, sizeof(*drm_if), GFP_KERNEL);
	if (!drm_if) {
		dev_err(&usb_dev->dev, "Cannot allocate DRM private structure");
		return -ENOMEM;
	}
	devres_add(master, drm_if);

	drm = &drm_if->drm;

	drm_if->magic = devm_regmap_field_alloc(&usb_dev->dev, regmap,
			FL2000_USB_LPM_magic);

	ret = drm_dev_init(drm, &fl2000_drm_driver, master);
	if (ret) {
		dev_err(master, "Cannot initialize DRM device (%d)", ret);
		return ret;
	}

	/* For register operations */
	drm->dev_private = usb_dev;

	drm_mode_config_init(drm);
	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = MAX_HEIGHT;

	/* Set DMA mask for DRM device from mask of the 'parent' USB device */
	dma_mask = dma_get_mask(&usb_dev->dev);
	ret = dma_set_coherent_mask(drm->dev, dma_mask);
	if (ret) {
		dev_err(drm->dev, "Cannot set DRM device DMA mask (%d)", ret);
		return ret;
	}

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe,
			&fl2000_display_funcs, fl2000_pixel_formats,
			ARRAY_SIZE(fl2000_pixel_formats), NULL, NULL);
	if (ret) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)",
				ret);
		return ret;
	}

	/* Register 'mode_set' function to operate prior to bridge */
	drm_encoder_helper_add(&drm_if->pipe.encoder, &fl2000_encoder_funcs);

	/* Attach bridge */
	ret = component_bind_all(master, &drm_if->pipe);
	if (ret) {
		dev_err(drm->dev, "Cannot attach bridge (%d)", ret);
		return ret;
	}

	drm_mode_config_reset(drm);

	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		return ret;
	}

	ret = drm_fbdev_generic_setup(drm, BPP);
	if (ret) {
		dev_err(drm->dev, "Cannot initialize framebuffer (%d)", ret);
		return ret;
	}

	fl2000_reset(usb_dev);
	fl2000_wait(usb_dev);

	return 0;
}

static void fl2000_unbind(struct device *master)
{
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;

	dev_info(master, "Unbinding FL2000 master component");

	drm_if = devres_find(master, fl2000_drm_release, NULL, NULL);
	if (!drm_if)
		return;

	drm = &drm_if->drm;
	if (!drm)
		return;

	drm_dev_unregister(drm);

	/* Detach bridge */
	component_unbind_all(master, drm);

	drm_mode_config_cleanup(drm);

	drm_dev_put(drm);

	devm_kfree(master, drm_if);
}

static struct component_master_ops fl2000_drm_master_ops = {
	.bind = fl2000_bind,
	.unbind = fl2000_unbind,
};

static void fl2000_match_release(struct device *dev, void *data)
{
	component_master_del(dev, &fl2000_drm_master_ops);
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
	struct i2c_adapter *adapter;

	adapter = fl2000_get_i2c_adapter(usb_dev);
	if (!adapter) {
		dev_err(&usb_dev->dev, "Cannot find I2C adapter");
		return -ENODEV;
	}

	/* Make USB interface master */
	component_match_add_release(&usb_dev->dev, &match,
			fl2000_match_release, fl2000_compare, adapter);

	ret = component_master_add_with_match(&usb_dev->dev,
			&fl2000_drm_master_ops, match);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot register component master (%d)",
				ret);
		return ret;
	}

	return 0;
}

void fl2000_inter_check(struct usb_device *usb_dev, u32 status)
{
	struct fl2000_drm_if *drm_if = devres_find(&usb_dev->dev,
			fl2000_drm_release, NULL, NULL);
	if (drm_if) {
		drm_kms_helper_hotplug_event(&drm_if->drm);
	}
}
