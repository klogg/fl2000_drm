// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"
#include <drm/drm_atomic.h>
#include "bridge/it66121.h"

#define DRM_DRIVER_NAME "fl2000_drm"
#define DRM_DRIVER_DESC "USB-HDMI"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL20000_MAX_WIDTH  4000
#define FL20000_MAX_HEIGHT 4000

/* Native packed formats avoid colour conversion; XRGB8888 remains a fallback. */
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,
};

/* XXX: Shall this has to come from driver parameters? */
static const char *const fl2000_bridges[] = {
	"it66121",
};

/* Match the upstream IT66121 driver's 24-bit input clock range. */
#define FL2000_MIN_PIXCLOCK 25000000
#define FL2000_MAX_PIXCLOCK 148500000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT 80

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS (10000000000ull * FL2000_BULK_BW_PERCENT / 100 / 8)

static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed, unsigned int pixclock)
{
	unsigned int bytes_pix;
	u64 max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (speed) {
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
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	if (!pixclock)
		return 0;
	/* USB bandwidth is bounded by 1.25 GB/s, even at a one-Hz clock. */
	bytes_pix = (unsigned int)div_u64(max_bw, pixclock);
	switch (bytes_pix) {
	case 0: /* Not enough */
	case 1: /* RGB 332 not supported*/
		return 0;
	case 2: /* RGB 565 */
	case 3: /* RGB 888 */
		break;
	default:
		bytes_pix = 3;
		break;
	}

	return bytes_pix;
}

/* USB carries active pixels only; blanking affects refresh, not frame size. */
static unsigned int fl2000_active_pixel_rate(const struct drm_display_mode *mode)
{
	u64 total = (u64)mode->htotal * mode->vtotal;
	u64 pixels = (u64)mode->clock * 1000 * mode->hdisplay * mode->vdisplay;

	/* Validated timings have active dimensions no larger than their totals. */
	return total ? (unsigned int)div64_u64(pixels + total - 1, total) : 0;
}

static unsigned int fl2000_frame_bytes_pix(enum usb_device_speed speed,
					   const struct drm_display_mode *mode, u32 format)
{
	unsigned int available = fl2000_get_bytes_pix(speed, fl2000_active_pixel_rate(mode));

	if (!available)
		return 0;
	switch (format) {
	case DRM_FORMAT_RGB565:
		return 2;
	case DRM_FORMAT_RGB888:
		return available >= 3 ? 3 : 0;
	case DRM_FORMAT_XRGB8888:
		return available;
	default:
		return 0;
	}
}

struct fl2000_drm_if {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector *connector;
	struct usb_device *usb_dev;
	struct fl2000_bridge bridge;
};

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,

	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static int fl2000_atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	const struct drm_plane *plane;
	const struct drm_plane_state *old_plane;
	const struct drm_plane_state *new_plane;
	int i;

	/* A format switch may change the USB pixel depth. Drain the old stream
	 * and reprogram the device through a full modeset before sending it.
	 */
	for_each_oldnew_plane_in_state(state, plane, old_plane, new_plane, i) {
		struct drm_crtc_state *crtc;

		if (!old_plane->fb || !new_plane->fb || !new_plane->crtc ||
		    old_plane->fb->format->format == new_plane->fb->format->format)
			continue;
		crtc = drm_atomic_get_crtc_state(state, new_plane->crtc);
		if (IS_ERR(crtc))
			return PTR_ERR_OR_ZERO(crtc);
		if (crtc->active)
			crtc->mode_changed = true;
	}
	return drm_atomic_helper_check(drm, state);
}

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = fl2000_atomic_check,
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

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,	 17,  18,  19,
		20,  21,  22,  23,  24,	 25,  26,  27,	28,  29,  30,  31,  32,	 33,  34,  35,
		36,  37,  38,  39,  40,	 41,  42,  43,	44,  45,  46,  47,  48,	 49,  50,  51,
		52,  53,  54,  55,  56,	 57,  58,  59,	60,  61,  62,  63,  64,	 65,  66,  67,
		68,  69,  70,  71,  72,	 73,  74,  75,	76,  77,  78,  79,  80,	 81,  82,  83,
		84,  85,  86,  87,  88,	 89,  90,  91,	92,  93,  94,  95,  96,	 97,  98,  99,
		100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
		116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128
	};
	u32 best_divisor = 0;

	/* Iterate over array */
	for (unsigned int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

/* Detection needs a clock before EDID has supplied a display mode. */
int fl2000_init_clock(struct usb_device *usb_dev)
{
	struct fl2000_pll pll;
	u32 clock;

	if (fl2000_pll_calc((u64)FL2000_MIN_PIXCLOCK * FL2000_PLL_PRECISION, &pll, &clock) >=
	    FL2000_PPM_ERR_MAX)
		return -EINVAL;
	return fl2000_set_pll(usb_dev, &pll);
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;

	if (mode->clock < FL2000_MIN_PIXCLOCK / 1000 || mode->htotal <= 0 || mode->vtotal <= 0 ||
	    (u64)mode->clock * 1000 > FL2000_MAX_PIXCLOCK || mode->hdisplay <= 0 ||
	    mode->vdisplay <= 0 || mode->hdisplay > 4095 || mode->vdisplay > 4095 ||
	    mode->htotal > 4095 || mode->vtotal > 4095 || mode->hsync_start < mode->hdisplay ||
	    mode->hsync_end <= mode->hsync_start || mode->hsync_end > mode->htotal ||
	    mode->hsync_end - mode->hsync_start > 255 || mode->vsync_start < mode->vdisplay ||
	    mode->vsync_end <= mode->vsync_start || mode->vsync_end > mode->vtotal ||
	    mode->vsync_end - mode->vsync_start > 7 ||
	    (mode->flags &
	     (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_3D_MASK)) ||
	    mode->vscan > 1)
		return -EINVAL;

	/* Prefer the original timing, then try -1, +1, ... -10, +10. */
	for (int attempt = 0; attempt <= max_h_adjustment * 2; attempt++) {
		int d = (attempt + 1) / 2;

		if (attempt & 1)
			d = -d;

		if (mode->htotal + d < mode->hsync_end || mode->htotal + d > 4095)
			continue;
		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur. Assume all this was checked before
		 */
		clock_mil_adjusted = clock_mil * (mode->htotal + d) / mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll, &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}

			return 0;
		}
	}

	/* Cannot find PLL configuration that satisfies requirements. */
	return -EINVAL;
}

static enum drm_mode_status fl2000_crtc_mode_valid(struct drm_crtc *crtc,
						   const struct drm_display_mode *mode)
{
	const struct fl2000_drm_if *drm_if = container_of(crtc->dev, struct fl2000_drm_if, drm);
	struct drm_display_mode adjusted;
	struct fl2000_pll pll;

	if (fl2000_mode_calc(mode, &adjusted, &pll))
		return MODE_BAD;
	return fl2000_get_bytes_pix(drm_if->usb_dev->speed, fl2000_active_pixel_rate(&adjusted)) ?
		       MODE_OK :
		       MODE_CLOCK_HIGH;
}

static int fl2000_display_check(const struct fl2000_drm_if *drm_if,
				const struct drm_plane_state *plane, struct drm_crtc_state *crtc)
{
	struct fl2000_pll pll;
	int ret;

	/* The adapter has no scanout IRQ. Use DRM's software completion events,
	 * rather than misrepresenting USB transfer completion as physical vblank.
	 */
	crtc->no_vblank = true;
	if ((plane->src_x | plane->src_y | plane->src_w | plane->src_h) & 0xffff)
		return -EINVAL;
	if ((int)(plane->src_w >> 16) != crtc->mode.hdisplay ||
	    (int)(plane->src_h >> 16) != crtc->mode.vdisplay)
		return -EINVAL;
	if (crtc->mode_changed) {
		ret = fl2000_mode_calc(&crtc->mode, &crtc->adjusted_mode, &pll);
		if (ret)
			return ret;
	}
	if (!plane->fb || !fl2000_frame_bytes_pix(drm_if->usb_dev->speed, &crtc->adjusted_mode,
						  plane->fb->format->format))
		return -EINVAL;
	return 0;
}

struct fl2000_plane_state {
	struct drm_shadow_plane_state shadow;
	struct fl2000_stream_buffers *prepared;
};

static struct fl2000_plane_state *fl2000_plane_state(struct drm_plane_state *state)
{
	return container_of(to_drm_shadow_plane_state(state), struct fl2000_plane_state, shadow);
}

static void fl2000_plane_destroy_state(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(plane->dev, struct fl2000_drm_if, drm);
	struct fl2000_plane_state *fl_state = fl2000_plane_state(state);

	fl2000_streaming_free_buffers(drm_if->usb_dev, fl_state->prepared);
	__drm_gem_destroy_shadow_plane_state(&fl_state->shadow);
	kfree(fl_state);
}

static void fl2000_plane_reset(struct drm_plane *plane)
{
	struct fl2000_plane_state *state;

	if (plane->state)
		fl2000_plane_destroy_state(plane, plane->state);
	plane->state = NULL;
	state = kzalloc_obj(*state);
	if (state)
		__drm_gem_reset_shadow_plane(plane, &state->shadow);
}

static struct drm_plane_state *fl2000_plane_duplicate_state(struct drm_plane *plane)
{
	struct fl2000_plane_state *state;

	state = kzalloc_obj(*state);
	if (!state)
		return NULL;
	/* Mappings and prepared buffers belong to a single commit. */
	__drm_gem_duplicate_shadow_plane_state(plane, &state->shadow);
	return &state->shadow.base;
}

static int fl2000_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(plane->dev, struct fl2000_drm_if, drm);
	struct fl2000_plane_state *fl_state = fl2000_plane_state(state);
	struct drm_shadow_plane_state *shadow = &fl_state->shadow;
	const struct drm_crtc_state *crtc;
	const struct drm_display_mode *mode;
	unsigned int bytes_pix;
	int idx;
	int ret;

	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret || !state->fb)
		return ret;
	ret = drm_gem_fb_vmap(state->fb, shadow->map, shadow->data);
	if (ret)
		return ret;
	if (shadow->data[0].is_iomem) {
		drm_gem_fb_vunmap(state->fb, shadow->map);
		return -EOPNOTSUPP;
	}
	crtc = state->crtc ? drm_atomic_get_new_crtc_state(state->state, state->crtc) : NULL;
	if (!crtc || !crtc->active || !drm_atomic_crtc_needs_modeset(crtc))
		return 0;
	if (!drm_dev_enter(plane->dev, &idx)) {
		ret = -ENODEV;
		goto unmap;
	}
	mode = &crtc->adjusted_mode;
	bytes_pix = fl2000_frame_bytes_pix(drm_if->usb_dev->speed, mode, state->fb->format->format);
	fl_state->prepared = fl2000_streaming_prepare(drm_if->usb_dev,
						      mode->hdisplay * mode->vdisplay, bytes_pix);
	ret = PTR_ERR_OR_ZERO(fl_state->prepared);
	if (ret)
		fl_state->prepared = NULL;
	drm_dev_exit(idx);
	if (!ret)
		return 0;
unmap:
	drm_gem_fb_vunmap(state->fb, shadow->map);
	return ret;
}

static void fl2000_cleanup_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(plane->dev, struct fl2000_drm_if, drm);
	struct fl2000_plane_state *fl_state = fl2000_plane_state(state);
	struct drm_shadow_plane_state *shadow = &fl_state->shadow;

	fl2000_streaming_free_buffers(drm_if->usb_dev, fl_state->prepared);
	fl_state->prepared = NULL;
	if (state->fb)
		drm_gem_fb_vunmap(state->fb, shadow->map);
}

static int fl2000_copy_frame(struct fl2000_drm_if *drm_if, struct drm_plane_state *state,
			     const struct drm_rect *damage)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_rect visible;
	size_t offset;
	int idx;
	int ret;

	if (!fb || !drm_dev_enter(&drm_if->drm, &idx))
		return -ENODEV;
	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		goto exit;
	offset = (size_t)(state->src_y >> 16) * fb->pitches[0] +
		 (state->src_x >> 16) * fb->format->cpp[0];
	if (damage) {
		/* Damage is in framebuffer coordinates; conversion starts at the crop. */
		visible = *damage;
		visible.x1 -= state->src_x >> 16;
		visible.x2 -= state->src_x >> 16;
		visible.y1 -= state->src_y >> 16;
		visible.y2 -= state->src_y >> 16;
		damage = &visible;
	}
	ret = fl2000_streaming_update(drm_if->usb_dev, (u8 *)shadow->data[0].vaddr + offset,
				      state->src_h >> 16, state->src_w >> 16, fb->pitches[0],
				      fb->format->format, damage);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
exit:
	drm_dev_exit(idx);
	return ret;
}

static int fl2000_configure_mode(struct fl2000_drm_if *drm_if, const struct drm_display_mode *mode,
				 u32 format)
{
	struct usb_device *usb_dev = drm_if->usb_dev;
	struct fl2000_pll pll;
	struct fl2000_timings timings;
	u32 clock;
	unsigned int bytes_pix;
	int ret;

	if (fl2000_pll_calc((u64)mode->clock * 1000 * FL2000_PLL_PRECISION, &pll, &clock) >=
	    FL2000_PPM_ERR_MAX)
		return -EINVAL;
	bytes_pix = fl2000_frame_bytes_pix(usb_dev->speed, mode, format);
	if (!bytes_pix)
		return -EINVAL;
	/* Reset before programming: the cache must describe the current hardware. */
	ret = fl2000_reset(usb_dev);
	if (ret)
		return ret;
	ret = fl2000_set_pll(usb_dev, &pll);
	if (ret)
		return ret;
	timings = (struct fl2000_timings){
		.hactive = mode->hdisplay,
		.htotal = mode->htotal,
		.hsync_width = mode->hsync_end - mode->hsync_start,
		.hstart = mode->htotal - mode->hsync_start + 1,
		.vactive = mode->vdisplay,
		.vtotal = mode->vtotal,
		.vsync_width = mode->vsync_end - mode->vsync_start,
		.vstart = mode->vtotal - mode->vsync_start + 1,
		.hsync_positive = !!(mode->flags & DRM_MODE_FLAG_PHSYNC),
		.vsync_positive = !!(mode->flags & DRM_MODE_FLAG_PVSYNC),
	};
	ret = fl2000_usb_magic(usb_dev);
	if (!ret)
		ret = fl2000_set_timings(usb_dev, &timings);
	if (!ret)
		ret = fl2000_set_pixfmt(usb_dev, bytes_pix);
	if (!ret)
		ret = fl2000_set_transfers(usb_dev);
	if (!ret)
		ret = fl2000_enable_interrupts(usb_dev);
	if (!ret)
		ret = fl2000_afe_magic(usb_dev);
	return ret;
}

static int fl2000_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	int ret;

	ret = drm_crtc_helper_atomic_check(crtc, state);
	if (ret)
		return ret;
	/* CRTC-only commits must also validate and update the primary plane. */
	return drm_atomic_add_affected_planes(state, crtc);
}

static int fl2000_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(plane->dev, struct fl2000_drm_if, drm);
	struct drm_plane_state *new_plane = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc = drm_atomic_get_new_crtc_state(state, &drm_if->crtc);
	int ret;

	ret = drm_atomic_helper_check_plane_state(new_plane, crtc, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret || !new_plane->visible)
		return ret;
	return fl2000_display_check(drm_if, new_plane, crtc);
}

static void fl2000_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(crtc->dev, struct fl2000_drm_if, drm);
	struct drm_plane_state *plane = drm_atomic_get_new_plane_state(state, &drm_if->plane);
	const struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int idx;
	int ret;

	if (!drm_dev_enter(&drm_if->drm, &idx))
		return;
	ret = fl2000_streaming_mode_set(drm_if->usb_dev, &fl2000_plane_state(plane)->prepared);
	if (!ret)
		ret = fl2000_configure_mode(drm_if, &crtc_state->adjusted_mode,
					    plane->fb->format->format);
	if (!ret)
		ret = fl2000_copy_frame(drm_if, plane, NULL);
	if (!ret)
		ret = fl2000_streaming_enable(drm_if->usb_dev);
	if (ret) {
		fl2000_streaming_disable(drm_if->usb_dev);
		dev_err(&drm_if->usb_dev->dev, "Display enable failed: %d\n", ret);
	}

	drm_dev_exit(idx);
}

static void fl2000_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state __always_unused)
{
	struct fl2000_drm_if *drm_if = container_of(crtc->dev, struct fl2000_drm_if, drm);

	fl2000_streaming_disable(drm_if->usb_dev);
}

static void fl2000_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct fl2000_drm_if *drm_if = container_of(plane->dev, struct fl2000_drm_if, drm);
	const struct drm_crtc_state *crtc = drm_atomic_get_new_crtc_state(state, &drm_if->crtc);
	const struct drm_plane_state *old_plane = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_plane = drm_atomic_get_new_plane_state(state, plane);
	struct drm_rect damage;
	int idx;
	int ret;

	/* The enable callback configures and copies the first frame on modesets. */
	if (!crtc->active || crtc->mode_changed || !new_plane->fb)
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		return;
	if (old_plane->fb != new_plane->fb ||
	    drm_atomic_helper_damage_merged(old_plane, new_plane, &damage)) {
		/* New framebuffers must rebuild each buffer, even with explicit clips. */
		ret = fl2000_copy_frame(drm_if, new_plane,
					old_plane->fb != new_plane->fb ? NULL : &damage);
		if (!ret)
			ret = fl2000_streaming_enable(drm_if->usb_dev);
		if (ret)
			dev_err_ratelimited(plane->dev->dev, "Frame conversion failed: %d\n", ret);
	}
	drm_dev_exit(idx);
}

static const struct drm_crtc_funcs fl2000_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs fl2000_crtc_helper_funcs = {
	.mode_valid = fl2000_crtc_mode_valid,
	.atomic_check = fl2000_crtc_atomic_check,
	.atomic_enable = fl2000_crtc_atomic_enable,
	.atomic_disable = fl2000_crtc_atomic_disable,
};

static const struct drm_plane_funcs fl2000_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = fl2000_plane_reset,
	.atomic_duplicate_state = fl2000_plane_duplicate_state,
	.atomic_destroy_state = fl2000_plane_destroy_state,
};

static const struct drm_plane_helper_funcs fl2000_plane_helper_funcs = {
	.prepare_fb = fl2000_prepare_fb,
	.cleanup_fb = fl2000_cleanup_fb,
	.atomic_check = fl2000_plane_atomic_check,
	.atomic_update = fl2000_plane_atomic_update,
};

static const struct drm_encoder_funcs fl2000_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int fl2000_pipeline_init(struct fl2000_drm_if *drm_if)
{
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct drm_device *drm = &drm_if->drm;
	int ret;

	ret = drm_universal_plane_init(drm, &drm_if->plane, 0, &fl2000_plane_funcs,
				       fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
				       modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&drm_if->plane, &fl2000_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&drm_if->plane);
	ret = drm_crtc_init_with_planes(drm, &drm_if->crtc, &drm_if->plane, NULL,
					&fl2000_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&drm_if->crtc, &fl2000_crtc_helper_funcs);
	ret = drm_encoder_init(drm, &drm_if->encoder, &fl2000_encoder_funcs, DRM_MODE_ENCODER_TMDS,
			       NULL);
	if (ret)
		return ret;
	drm_if->encoder.possible_crtcs = drm_crtc_mask(&drm_if->crtc);
	return 0;
}

int fl2000_drm_hotplug(const struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	struct drm_device *drm = READ_ONCE(device->drm);
	int idx;
	int ret = -ENODEV;

	if (drm && drm_dev_enter(drm, &idx)) {
		struct fl2000_drm_if *drm_if = container_of(drm, struct fl2000_drm_if, drm);

		ret = drm_if->bridge.interrupt ? drm_if->bridge.interrupt(drm_if->bridge.bridge) :
						 0;
		if (!ret) {
			/* Update cached status even without periodic polling. */
			if (!drm_helper_hpd_irq_event(drm))
				drm_kms_helper_hotplug_event(drm);
		}
		drm_dev_exit(idx);
	}
	return ret;
}

void fl2000_drm_set_polling(const struct usb_device *usb_dev, bool polling)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	struct drm_device *drm = READ_ONCE(device->drm);
	int idx;

	if (drm && drm_dev_enter(drm, &idx)) {
		struct fl2000_drm_if *drm_if = container_of(drm, struct fl2000_drm_if, drm);
		struct drm_connector *connector;

		mutex_lock(&drm->mode_config.mutex);
		connector = drm_if->bridge.connector ? *drm_if->bridge.connector : NULL;
		if (connector)
			connector->polled = polling ? DRM_CONNECTOR_POLL_CONNECT |
							      DRM_CONNECTOR_POLL_DISCONNECT :
						      DRM_CONNECTOR_POLL_HPD;
		mutex_unlock(&drm->mode_config.mutex);
		if (polling)
			drm_kms_helper_poll_enable(drm);
		drm_dev_exit(idx);
	}
}

static int fl2000_drm_bind(struct device *master)
{
	struct usb_device *usb_dev = to_usb_device(master);
	struct fl2000_device *device = dev_get_drvdata(master);
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct device *dma_dev;
	struct drm_connector *connector;
	int ret;

	drm_if = __drm_dev_alloc(master, &fl2000_drm_driver, sizeof(*drm_if),
				 offsetof(struct fl2000_drm_if, drm));
	if (IS_ERR(drm_if))
		return PTR_ERR_OR_ZERO(drm_if);
	drm = &drm_if->drm;
	drm_if->usb_dev = usb_dev;
	ret = drmm_mode_config_init(drm);
	if (ret)
		goto put;
	drm->mode_config.funcs = &fl2000_mode_config_funcs;
	drm->mode_config.min_width = 1;
	drm->mode_config.max_width = FL20000_MAX_WIDTH;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_height = FL20000_MAX_HEIGHT;
	/* Imported GEM buffers are mapped by the USB controller, not the dongle. */
	dma_dev = usb_intf_get_dma_device(usb_ifnum_to_if(usb_dev, FL2000_USBIF_AVCONTROL));
	if (dma_dev) {
		drm_dev_set_dma_dev(drm, dma_dev);
		put_device(dma_dev);
	}
	ret = fl2000_pipeline_init(drm_if);
	if (ret)
		goto put;
	ret = component_bind_all(master, &drm_if->bridge);
	if (ret)
		goto put;
	ret = drm_bridge_attach(&drm_if->encoder, drm_if->bridge.bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		goto unbind;
	connector = fl2000_bridge_connector_init(drm, &drm_if->encoder, drm_if->bridge.bridge);
	if (IS_ERR(connector)) {
		ret = PTR_ERR_OR_ZERO(connector);
		goto detach_encoder;
	}
	drm_if->connector = connector;
	ret = drm_connector_attach_encoder(connector, &drm_if->encoder);
	if (ret)
		goto detach_encoder;
	*drm_if->bridge.connector = connector;
	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);
	WRITE_ONCE(device->drm, drm);
	ret = fl2000_enable_interrupts(usb_dev);
	if (ret)
		goto detach;
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto detach;
	/* Initial fbdev setup resets the device while configuring its mode. */
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);
	ret = fl2000_enable_interrupt(usb_dev);
	if (ret) {
		drm_dev_unplug(drm);
		drm_atomic_helper_shutdown(drm);
		fl2000_streaming_disable(usb_dev);
		goto detach;
	}

	return 0;
detach:
	drm_kms_helper_poll_fini(drm);
	WRITE_ONCE(device->drm, NULL);
detach_encoder:
	drm_encoder_cleanup(&drm_if->encoder);
	fl2000_bridge_connector_detach(drm_if->connector);
unbind:
	component_unbind_all(master, &drm_if->bridge);
put:
	drm_dev_put(drm);
	return ret;
}

static void fl2000_drm_unbind(struct device *master)
{
	struct usb_device *usb_dev = to_usb_device(master);
	struct fl2000_device *device = dev_get_drvdata(master);
	struct drm_device *drm = device->drm;
	struct fl2000_drm_if *drm_if;

	if (!drm)
		return;
	drm_if = container_of(drm, struct fl2000_drm_if, drm);
	fl2000_disable_interrupt(usb_dev);
	drm_kms_helper_poll_fini(drm);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	fl2000_streaming_disable(usb_dev);
	/* Encoder cleanup owns bridge detachment; do it before its I2C driver can go away. */
	drm_encoder_cleanup(&drm_if->encoder);
	fl2000_bridge_connector_detach(drm_if->connector);
	component_unbind_all(master, &drm_if->bridge);
	WRITE_ONCE(device->drm, NULL);
	drm_dev_put(drm);
}

static const struct component_master_ops fl2000_master_ops = {
	.bind = fl2000_drm_bind,
	.unbind = fl2000_drm_unbind,
};

static int fl2000_component_compare(struct device *client_dev, void *data)
{
	return fl2000_i2c_verify_client(data, client_dev, fl2000_bridges[0]);
}

int fl2000_drm_init(struct usb_device *usb_dev)
{
	struct component_match *match = NULL;

	component_match_add(&usb_dev->dev, &match, fl2000_component_compare, usb_dev);
	if (IS_ERR(match))
		return PTR_ERR_OR_ZERO(match);
	return component_master_add_with_match(&usb_dev->dev, &fl2000_master_ops, match);
}

void fl2000_drm_cleanup(struct usb_device *usb_dev)
{
	fl2000_disable_interrupt(usb_dev);
	component_master_del(&usb_dev->dev, &fl2000_master_ops);
}

int fl2000_drm_suspend(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	int ret;

	fl2000_disable_interrupt(usb_dev);
	ret = device->drm ? drm_mode_config_helper_suspend(device->drm) : 0;
	if (!ret)
		fl2000_streaming_disable(usb_dev);
	else if (device->drm)
		fl2000_enable_interrupt(usb_dev);
	return ret;
}

int fl2000_drm_resume(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	int ret;

	if (!regmap)
		return 0;
	ret = fl2000_reset(usb_dev);
	if (!ret)
		ret = fl2000_init_clock(usb_dev);
	if (!ret)
		ret = fl2000_usb_magic(usb_dev);
	if (!ret && device->drm)
		ret = fl2000_enable_interrupt(usb_dev);
	if (!ret && device->drm)
		ret = drm_mode_config_helper_resume(device->drm);
	if (ret)
		fl2000_disable_interrupt(usb_dev);
	return ret;
}
