/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

/* TODO: Move registers operation to registers.c */

#include "fl2000.h"

int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev,
		struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll);

int fl2000_stream_mode_set(struct usb_device *usb_dev, size_t pixels, u32 freq);
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

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL20000_MAX_WIDTH	4000
#define FL20000_MAX_HEIGHT	4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP		32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Maximum pixel clock set to 500MHz. It is hard to get more or lesss precies
 * PLL configuration for higher clock */
#define FL2000_MAX_PIXCLOCK	500000000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION	1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL		10000000	/* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN	62500000	/* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX	1000000000	/* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX	500

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT		80

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS	(10000000000ull * 100 / \
						FL2000_BULK_BW_PERCENT / 8)

static u32 fl2000_get_bytes_pix(struct usb_device *usb_dev, u32 pixclock)
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

DEFINE_DRM_GEM_SHMEM_FOPS(fl2000_drm_driver_fops);

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

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = (u64)vco_clk * FL2000_PLL_PRECISION / divisor;
	u64 pll_clk_err;

	/* Not using abs() here to avoid possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return pll_clk_err / (clock_mil / FL2000_PLL_PRECISION);
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock)
{
	static const u32 prescaler[] = {
			1,
			2
	};
	static const u32 multiplier[] = {
			  1,   2,   3,   4,   5,   6,   7,   8,   9,  10,
			 11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
			 21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
			 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
			 41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
			 51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
			 61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
			 71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
			 81,  82,  83,  84,  85,  86,  87,  88,  89,  90,
			 91,  92,  93,  94,  95,  96,  97,  98,  99, 100,
			101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
			111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
			121, 122, 123, 124, 125, 126, 127, 128
	};
	static const u32 divisor[] = {
			       2,        4,        6,   7,   8,   9,  10,
			 11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
			 21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
			 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
			 41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
			 51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
			 61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
			 71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
			 81,  82,  83,  84,  85,  86,  87,  88,  89,  90,
			 91,  92,  93,  94,  95,  96,  97,  98,  99, 100,
			101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
			111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
			121, 122, 123, 124, 125, 126, 127, 128
	};
	unsigned int prescaler_idx, multiplier_idx, divisor_idx;
	u64 min_ppm_err = (u64)(-1);

	for_each_array_item(prescaler, prescaler_idx) {
		for_each_array_item(multiplier, multiplier_idx) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler[prescaler_idx] *
					multiplier[multiplier_idx];

			if (vco_clk < FL2000_VCOCLOCK_MIN ||
					vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			for_each_array_item(divisor, divisor_idx) {
				u64 ppm_err = fl2000_pll_ppm_err(clock_mil,
						vco_clk, divisor[divisor_idx]);

				if (ppm_err > min_ppm_err)
					continue;

				min_ppm_err = ppm_err;

				pll->prescaler = prescaler[prescaler_idx];
				pll->multiplier = multiplier[multiplier_idx];
				pll->divisor = divisor[divisor_idx];
				pll->function = vco_clk < 125000000 ? 0 :
						vco_clk < 250000000 ? 1 :
						vco_clk < 500000000 ? 2 : 3;
				*clock = vco_clk / divisor[divisor_idx];

				/* Stop if found exact setting */
				if (ppm_err == 0)
					return 0;
			}
		}
	}

	/* No exact PLL settings found for requested clock */
	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	static const int h_adjust[] = {
			 0,
			 1,  -1,
			 2,  -2,
			 3,  -3,
			 4,  -4,
			 5,  -5,
			 6,  -6,
			 7,  -7,
			 8,  -8,
			 9,  -9,
			10, -10
	};
	unsigned int h_adjust_idx;
	u64 ppm_err;
	u32 clock_adjusted;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -1;

	/* Try to match pixel clock slightly adjusting htotal value */
	for_each_array_item(h_adjust, h_adjust_idx) {
		u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
		int adjust = h_adjust[h_adjust_idx];

		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get
		 * 10^15Hz. Assume maximum htotal is 10000 pix (no way) we get
		 * 10^19 max value and using u64 which is 1.8*10^19 no overflow
		 * can occur. Assume all this was checked before */
		if (adjust != 0)
			clock_mil = clock_mil * ((s64)mode->htotal + adjust) /
								mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil, pll, &clock_adjusted);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += h_adjust[h_adjust_idx];
				adjusted_mode->clock = clock_adjusted;
			}

			return 0;
		}

	}

	/* Cannot find PLL configuration that satisfy requirements */
	return -1;
}

static enum drm_mode_status fl2000_display_mode_valid(struct drm_crtc *crtc,
		const struct drm_display_mode *mode)
{
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;

	/* Get PLL configuration and check if mode adjustments needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll))
		return MODE_BAD;

	if (!fl2000_get_bytes_pix(usb_dev, adjusted_mode.clock))
		return MODE_BAD;

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
		struct drm_crtc_state *cstate,
		struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;

	fl2000_stream_enable(usb_dev);

	drm_crtc_vblank_on(crtc);
}

void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct usb_device *usb_dev = drm->dev_private;

	fl2000_stream_disable(usb_dev);

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

static void fl2000_output_mode_set(struct drm_encoder *encoder,
		 struct drm_display_mode *mode,
		 struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct usb_device *usb_dev = drm->dev_private;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	u32 bytes_pix;

	/* Get PLL configuration and cehc if mode adjustments needed */
	if (fl2000_mode_calc(mode, adjusted_mode, &pll))
		return;

	/* Check how many bytes per pixel shall be used with adjusted clock */
	bytes_pix = fl2000_get_bytes_pix(usb_dev, adjusted_mode->clock);
	if (!bytes_pix)
		return;

	dev_info(drm->dev, "Mode requested:  "DRM_MODE_FMT, DRM_MODE_ARG(mode));
	dev_info(drm->dev, "Mode configured: "DRM_MODE_FMT,
			DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end -
			adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end -
			adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(usb_dev, mode->hdisplay * mode->vdisplay,
			bytes_pix);
}

/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_output_mode_set,
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
	mode_config->max_width = FL20000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL20000_MAX_HEIGHT;

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
