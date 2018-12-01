/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm.h
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
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
#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_crtc_helper.h>

/* Custom code for DRM bridge autodetection since there is no DT support */
#define I2C_CLASS_HDMI	(1<<9)

/* #### U1/U2 Control Registers Bank #### */
static const char * const fl2000_u1u2_control_regs = "u1u2_control";

#define FL2000_U1U2_CONTROL_OFFSET	0x0000

#define FL2000_REG_0070			(FL2000_U1U2_CONTROL_OFFSET + 0x70)
#define FL2000_REG_0078			(FL2000_U1U2_CONTROL_OFFSET + 0x78)

/* #### VGA Control Registers Bank #### */
static const char * const fl2000_vga_control_regs = "vga_control";

#define FL2000_VGA_CONTROL_OFFSET	0x8000

#define FL2000_VGA_STATUS_REG		(FL2000_VGA_CONTROL_OFFSET + 0x00) /* precious, volatile */
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_addr = REG_FIELD(FL2000_VGA_STATUS_REG, 0, 6);
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_cmd = REG_FIELD(FL2000_VGA_STATUS_REG, 7, 7);
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_offset = REG_FIELD(FL2000_VGA_STATUS_REG, 8, 15);
static const struct reg_field FL2000_VGA_STATUS_REG_vga_status = REG_FIELD(FL2000_VGA_STATUS_REG, 16, 23); /* readonly */
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_status = REG_FIELD(FL2000_VGA_STATUS_REG, 24, 27);
static const struct reg_field FL2000_VGA_STATUS_REG_monitor_detect = REG_FIELD(FL2000_VGA_STATUS_REG, 28, 28);
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_ready = REG_FIELD(FL2000_VGA_STATUS_REG, 29, 29); /* readonly */
static const struct reg_field FL2000_VGA_STATUS_REG_edid_detect = REG_FIELD(FL2000_VGA_STATUS_REG, 30, 30);
static const struct reg_field FL2000_VGA_STATUS_REG_i2c_done = REG_FIELD(FL2000_VGA_STATUS_REG, 31, 31);

#define FL2000_VGA_CTRL_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x04) /* volatile */
#define FL2000_VGA_HSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x08)
#define FL2000_VGA_HSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x0C)
#define FL2000_VGA_VSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x10)
#define FL2000_VGA_VSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x14)
#define FL2000_VGA_TEST_REG		(FL2000_VGA_CONTROL_OFFSET + 0x18)
#define FL2000_VGA_ISOCH_REG		(FL2000_VGA_CONTROL_OFFSET + 0x1C) /* volatile */
#define FL2000_VGA_I2C_SC_REG		(FL2000_VGA_CONTROL_OFFSET + 0x20) /* volatile */
#define FL2000_VGA_I2C_RD_REG		(FL2000_VGA_CONTROL_OFFSET + 0x24) /* volatile */
#define FL2000_VGA_I2C_WR_REG		(FL2000_VGA_CONTROL_OFFSET + 0x28)
#define FL2000_VGA_PLL_REG		(FL2000_VGA_CONTROL_OFFSET + 0x2C)
#define FL2000_VGA_LBUF_REG		(FL2000_VGA_CONTROL_OFFSET + 0x30)
#define FL2000_VGA_HI_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x34)
#define FL2000_VGA_LO_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x38)
#define FL2000_VGA_CTRL_REG_ACLK	(FL2000_VGA_CONTROL_OFFSET + 0x3C)
#define FL2000_VGA_PXCLK_CNT_REG	(FL2000_VGA_CONTROL_OFFSET + 0x40) /* volatile */
#define FL2000_VGA_VCNT_REG		(FL2000_VGA_CONTROL_OFFSET + 0x44) /* volatile */
#define FL2000_RST_CTRL_REG		(FL2000_VGA_CONTROL_OFFSET + 0x48) /* volatile */
#define FL2000_BIAC_CTRL1_REG		(FL2000_VGA_CONTROL_OFFSET + 0x4C)
#define FL2000_BIAC_CTRL2_REG		(FL2000_VGA_CONTROL_OFFSET + 0x50)
#define FL2000_BIAC_STATUS_REG		(FL2000_VGA_CONTROL_OFFSET + 0x54) /* volatile */
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x58) */
#define FL2000_VGA_PLT_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x5C)
#define FL2000_VGA_PLT_RADDR_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x60)
#define FL2000_VGA_CTRL2_REG_ACLK	(FL2000_VGA_CONTROL_OFFSET + 0x64)
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x68) */
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x6C) */
#define FL2000_TEST_CNTL_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x70) /* volatile */
#define FL2000_TEST_CNTL_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x74) /* volatile */
#define FL2000_TEST_CNTL_REG3		(FL2000_VGA_CONTROL_OFFSET + 0x78)
#define FL2000_TEST_STAT1		(FL2000_VGA_CONTROL_OFFSET + 0x7C) /* volatile */
#define FL2000_TEST_STAT2		(FL2000_VGA_CONTROL_OFFSET + 0x80) /* volatile */
#define FL2000_TEST_STAT3		(FL2000_VGA_CONTROL_OFFSET + 0x84) /* volatile */
#define FL2000_VGA_CTRL_REG_3		(FL2000_VGA_CONTROL_OFFSET + 0x88)
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x8C) */

#endif /* __FL2000_DRM_H__ */
