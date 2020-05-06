/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);

int fl2000_stream_mode_set(struct usb_device *usb_dev, ssize_t pixels, u32 freq);
void fl2000_stream_compress(struct usb_device *usb_dev,
		struct drm_framebuffer *fb, void *src);
int fl2000_stream_enable(struct usb_device *usb_dev);
void fl2000_stream_disable(struct usb_device *usb_dev);

#define DRM_DRIVER_NAME		"fl2000_drm"
#define DRM_DRIVER_DESC		"USB-HDMI"
#define DRM_DRIVER_DATE		"20181001"

#define DRM_DRIVER_MAJOR	0
#define DRM_DRIVER_MINOR	0
#define DRM_DRIVER_PATCHLEVEL	1

#define MAX_WIDTH		4000
#define MAX_HEIGHT		4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP		32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT		80

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS	(10000000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)

static inline int fl2000_get_bytes_pix(struct usb_device *usb_dev, u32 pixclock)
{
	int bytes_pix;
	u64 max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (usb_dev->speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED_PLUS;
		break;
	default:
		dev_err(&usb_dev->dev, "Unsupported USB bus detected");
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	bytes_pix = max_bw / pixclock;
	switch (bytes_pix) {
	case 0:		/* Not enough */
	case 1:		/* RGB 332 not supported*/
		return 0;
	case 2:		/* RGB 565 */
	case 3:		/* RGB 888 */
		break;
	default:
		bytes_pix = 3;
		break;
	}

	return bytes_pix;
}

/* List all supported bridges */
static const char *fl2000_supported_bridges[] = {
	"it66121", /* IT66121 driver name*/
};

struct fl2000_drm_if {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	bool vblank_enabled;
};

static void fl2000_drm_if_release(struct device *dev, void *res)
{
	/* Noop */
}

static inline struct fl2000_drm_if *fl2000_drm_to_drm_if(struct drm_device *drm)
{
	return container_of(drm, struct fl2000_drm_if, drm);
}

static inline struct fl2000_drm_if *fl2000_pipe_to_drm_if(
		struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct fl2000_drm_if, pipe);
}

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

static void fl2000_drm_release(struct drm_device *drm)
{
	drm_atomic_helper_shutdown(drm);
	drm_mode_config_cleanup(drm);
	drm_dev_fini(drm);
}

static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.lastclose = drm_fb_helper_lastclose,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,
	.release = fl2000_drm_release,

	DRM_GEM_SHMEM_DRIVER_OPS,

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
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static enum drm_mode_status fl2000_display_mode_valid(struct drm_simple_display_pipe *pipe,
		const struct drm_display_mode *mode)
{
	// struct drm_device *drm = crtc->dev;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;
	u32 pixclock = mode->hdisplay * mode->vdisplay * drm_mode_vrefresh(mode);

	/*dev_info(drm->dev, "DRM mode validation: "DRM_MODE_FMT,
			DRM_MODE_ARG(mode));*/

	if (!fl2000_get_bytes_pix(usb_dev, pixclock))
		return MODE_BAD;
	else
		return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *cstate,
		struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;

	dev_info(drm->dev, "fl2000_display_enable");

	/* TODO: Kick off streaming queue */

	drm_crtc_vblank_on(crtc);
}

void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;

	dev_info(drm->dev, "fl2000_display_disable");

	/* TODO: Stop streaming queue */

	drm_crtc_vblank_off(crtc);
}

static int fl2000_display_check(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *plane_state,
		struct drm_crtc_state *crtc_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_framebuffer *fb = plane_state->fb;
	int n;

	n = fb->format->num_planes;
	if (n > 1) {
		/* TODO: Check real buffer area for transmission */
		struct drm_format_name_buf format_name;
		dev_err(drm->dev, "Only single plane RGB framebuffers are " \
				"supported, got %d planes (%s)", n,
				drm_get_format_name(fb->format->format,
						&format_name));
		return -EINVAL;
	}
	return 0;
}

void fl2000_display_vblank(struct usb_device *usb_dev)
{
	struct fl2000_drm_if *drm_if;

	drm_if = devres_find(&usb_dev->dev, fl2000_drm_if_release, NULL, NULL);
	if (!drm_if)
		return;

	if (drm_if->vblank_enabled && drm_if->pipe.crtc.enabled)
		drm_crtc_handle_vblank(&drm_if->pipe.crtc);
}


static void fb2000_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	int idx, ret;
	struct drm_device *drm = fb->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct dma_buf_attachment *import_attach = gem->import_attach;
	void *vaddr;

	if (!drm_dev_enter(fb->dev, &idx)) {
		dev_err(drm->dev, "DRM enter failed!");
		return;
	}

	vaddr = drm_gem_shmem_vmap(gem);
	if (!vaddr) {
		dev_err(drm->dev, "FB vmap failed!");
		return;
	}

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return;
	}

	fl2000_stream_compress(usb_dev, fb, vaddr);

	if (import_attach)
		dma_buf_end_cpu_access(import_attach->dmabuf, DMA_FROM_DEVICE);

	drm_gem_shmem_vunmap(drm_gem_fb_get_obj(fb, 0), vaddr);

	drm_dev_exit(idx);
}


static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
		struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		fb2000_dirty(state->fb, &rect);

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

static int fl2000_display_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct fl2000_drm_if *drm_if;

	drm_if = devres_find(&usb_dev->dev, fl2000_drm_if_release, NULL, NULL);
	if (!drm_if)
		return -ENODEV;

	drm_if->vblank_enabled = true;

	return 0;
}

static void fl2000_display_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct fl2000_drm_if *drm_if;

	drm_if = devres_find(&usb_dev->dev, fl2000_drm_if_release, NULL, NULL);
	if (!drm_if)
		return;

	drm_if->vblank_enabled = false;
}

/* Logical pipe management (no HW configuration here) */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_display_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.check = fl2000_display_check,
	.update = fl2000_display_update,
	.enable_vblank = fl2000_display_enable_vblank,
	.disable_vblank = fl2000_display_disable_vblank,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

/* TODO: Move registers operation to registers.c */
static void fl2000_output_mode_set(struct drm_encoder *encoder,
		 struct drm_display_mode *mode,
		 struct drm_display_mode *adjusted_mode)
{
	int bytes_pix;
	struct drm_device *drm = encoder->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	fl2000_vga_hsync_reg1 hsync1 = {.val = 0};
	fl2000_vga_hsync_reg2 hsync2 = {.val = 0};
	fl2000_vga_vsync_reg1 vsync1 = {.val = 0};
	fl2000_vga_vsync_reg2 vsync2 = {.val = 0};
	fl2000_vga_cntrl_reg_pxclk pxclk = {.val = 0};
	fl2000_vga_ctrl_reg_aclk aclk = {.val = 0};
	fl2000_vga_pll_reg pll = {.val = 0};
	fl2000_vga_isoch_reg isoch = {.val = 0};
	u32 mask;
	u32 pixclock = mode->hdisplay * mode->vdisplay * drm_mode_vrefresh(mode);

	bytes_pix = fl2000_get_bytes_pix(usb_dev,pixclock);

	mask = 0;
	aclk.force_pll_up = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_pll_up);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	/* TODO: Calculate PLL settings */
	pll.val = 0x0020410A; // 32MHz
	regmap_write(regmap, FL2000_VGA_PLL_REG, pll.val);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);
	regmap_read(regmap, FL2000_VGA_PLL_REG, &pll.val);

	/* Generic clock configuration */
	mask = 0;
	aclk.use_pkt_pending = false;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, use_pkt_pending);
	aclk.use_zero_pkt_len = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, use_zero_pkt_len);
	aclk.vga_err_int_en = true;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	pxclk.dac_output_en = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.drop_cnt = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, drop_cnt);
	pxclk.vga565_mode = (bytes_pix == 2);
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, vga565_mode);
	pxclk.vga332_mode = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, vga332_mode);
	pxclk.vga555_mode = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, vga555_mode);
	pxclk.vga_compress = false;
	fl2000_add_bitmask(mask, fl2000_vga_cntrl_reg_pxclk, vga_compress);
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

	hsync2.hsync_width = mode->hsync_end - mode->hsync_start;
	hsync2.hstart = (mode->htotal - mode->hsync_start + 1);
	regmap_write(regmap, FL2000_VGA_HSYNC_REG2, hsync2.val);

	vsync1.vactive = mode->vdisplay;
	vsync1.vtotal = mode->vtotal;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG1, vsync1.val);

	vsync2.vsync_width = mode->vsync_end - mode->vsync_start;
	vsync2.vstart = (mode->vtotal - mode->vsync_start + 1);
	vsync2.start_latency = vsync2.vstart;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG2, vsync2.val);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(usb_dev, mode->hdisplay * mode->vdisplay,
			bytes_pix);

	/* Force VGA connect to allow bridge perform its setup */
	mask = 0;
	aclk.force_vga_connect = true;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);
}

static void fl2000_output_enable(struct drm_encoder *encoder)
{
	struct drm_device *drm = encoder->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	fl2000_vga_ctrl_reg_aclk aclk = {.val = 0};
	u32 mask;

	dev_info(drm->dev, "fl2000_output_enable");

	if (IS_ERR(regmap)) {
		dev_err(drm->dev, "Cannot find regmap (%ld)", PTR_ERR(regmap));
		return;
	}

	/* Disable forcing VGA connect */
	mask = 0;
	aclk.force_vga_connect = false;
	fl2000_add_bitmask(mask, fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	fl2000_stream_enable(usb_dev);
}

static void fl2000_output_disable(struct drm_encoder *encoder)
{
	struct drm_device *drm = encoder->dev;
	struct usb_device *usb_dev = drm->dev_private;

	dev_info(drm->dev, "fl2000_output_disable");

	/* TODO: disable HW */

	fl2000_stream_disable(usb_dev);
}

/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_output_mode_set,
	.enable = fl2000_output_enable,
	.disable = fl2000_output_disable,
};

static int fl2000_bind(struct device *master)
{
	int ret = 0;
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	struct usb_device *usb_dev = container_of(master, struct usb_device,
			dev);
	u64 dma_mask;

	dev_info(master, "Binding FL2000 master component");

	drm_if = devres_alloc(&fl2000_drm_if_release, sizeof(*drm_if),
			GFP_KERNEL);
	if (!drm_if) {
		dev_err(&usb_dev->dev, "Cannot allocate DRM private structure");
		return -ENOMEM;
	}
	devres_add(master, drm_if);

	drm = &drm_if->drm;
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

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		dev_err(drm->dev, "Failed to initialize %d VBLANK(s) (%d)",
				drm->mode_config.num_crtc, ret);
		return ret;
	}

	drm_kms_helper_poll_init(drm);

	drm_plane_enable_fb_damage_clips(&drm_if->pipe.plane);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		return ret;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	ret = drm_fbdev_generic_setup(drm, FL2000_FB_BPP);
	if (ret) {
		dev_err(drm->dev, "Cannot initialize framebuffer (%d)", ret);
		return ret;
	}

	return 0;
}

static void fl2000_unbind(struct device *master)
{
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;

	dev_info(master, "Unbinding FL2000 master component");

	drm_if = devres_find(master, fl2000_drm_if_release, NULL, NULL);
	if (!drm_if)
		return;

	drm = &drm_if->drm;
	if (!drm)
		return;

	/* Detach bridge */
	component_unbind_all(master, drm);

	/* Prepare to DRM device shutdown */
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_dev_put(drm);
}

static struct component_master_ops fl2000_drm_master_ops = {
	.bind = fl2000_bind,
	.unbind = fl2000_unbind,
};

static void fl2000_match_release(struct device *dev, void *data)
{
	/* Noop */
}

static int fl2000_compare(struct device *dev, void *data)
{
	int i;
	struct usb_device *usb_dev = data;

	/* Check component's parent (must be our USB device) */
	if (dev->parent->parent != &usb_dev->dev)
		return 0;

	/* Check this is a supported DRM bridge */
	for (i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strcmp(fl2000_supported_bridges[i], dev->driver->name)) {
			dev_info(dev, "Found bridge %s", dev->driver->name);
			return (i + 1); /* Must be not 0 for success */
		}

	return 0;
}

/* TODO: Move registers operation to registers.c */
void fl2000_inter_check(struct usb_device *usb_dev)
{
	int ret;
	fl2000_vga_status_reg status;
	struct fl2000_drm_if *drm_if = devres_find(&usb_dev->dev,
			fl2000_drm_if_release, NULL, NULL);
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	if (!drm_if || !regmap)
		return;

	/* Process interrupt */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot read interrupt register (%d)",
				ret);
	} else {
		dev_info(&usb_dev->dev, "FL2000 interrupt 0x%X", status.val);
		if (status.hdmi_event || status.monitor_event ||
				status.edid_event) {
			drm_kms_helper_hotplug_event(&drm_if->drm);
		}
	}
}

int fl2000_drm_init(struct usb_device *usb_dev)
{
	int ret = 0;
	struct component_match *match = NULL;

	/* Make USB interface master */
	component_match_add_release(&usb_dev->dev, &match,
			fl2000_match_release, fl2000_compare, usb_dev);

	ret = component_master_add_with_match(&usb_dev->dev,
			&fl2000_drm_master_ops, match);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot register component master (%d)",
				ret);
		return ret;
	}

	return 0;
}

void fl2000_drm_cleanup(struct usb_device *usb_dev)
{
	component_master_del(&usb_dev->dev, &fl2000_drm_master_ops);
}
