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

#define FL2000_I2C_ADDRESS_DSUB		0x50
#define FL2000_I2C_ADDRESS_EEPROM	0x54

/* Registers are available from everywhere */
int fl2000_reg_read(struct usb_device *usb_dev, u32 *data, u16 offset);
int fl2000_reg_write(struct usb_device *usb_dev, u32 *data, u16 offset);

#define FL2000_REG_INT_STATUS	0x8000 /* Interrupt status ? */
#define FL2000_REG_FORMAT	0x8004 /* Picture format / Color mode ? */
#define FL2000_REG_H_SYNC1	0x8008 /* h_sync_reg_1 */
#define FL2000_REG_H_SYNC2	0x800C /* h_sync_reg_2 */
#define FL2000_REG_V_SYNC1	0x8010 /* v_sync_reg_1 */
#define FL2000_REG_V_SYNC2	0x8014 /* v_sync_reg_2 */
#define FL2000_REG_8018		0x8018 /* unknown */
#define FL2000_REG_ISO_CTRL	0x801C /* ISO 14 bit value ? */
#define FL2000_REG_BUS_CTRL	0x8020 /* I2C/SPI control */
#define FL2000_REG_BUS_DATA_RD	0x8024 /* I2C/SPI read data, 32 bit wide */
#define FL2000_REG_BUS_DATA_WR	0x8028 /* I2C/SPI write data, 32 bit wide */
#define FL2000_REG_PLL		0x802C /* PLL control  */
#define FL2000_REG_8030		0x8030 /* unknown */
#define FL2000_REG_8034		0x8034 /* unknown */
#define FL2000_REG_8038		0x8038 /* unknown */
#define FL2000_REG_INT_CTRL	0x803C /* Interrupt control */
#define FL2000_REG_8040		0x8040 /* unknown */
#define FL2000_REG_8044		0x8044 /* unknown */
#define FL2000_REG_8048		0x8048 /* Application reset */
#define FL2000_REG_804C		0x804C /* unknown */
#define FL2000_REG_8050		0x8050 /* unknown */
#define FL2000_REG_8054		0x8054 /* unknown */
#define FL2000_REG_8058		0x8058 /* unknown */
#define FL2000_REG_805C		0x805C /* unknown */
#define FL2000_REG_8064		0x8064 /* unknown */
#define FL2000_REG_8070		0x8070 /* unknown */
#define FL2000_REG_8074		0x8074 /* unknown */
#define FL2000_REG_8078		0x8078 /* unknown */
#define FL2000_REG_807C		0x807C /* unknown */
#define FL2000_REG_8088		0x8088 /* unknown */
#define FL2000_REG_0070		0x0070 /* unknown */
#define FL2000_REG_0078		0x0078 /* unknown */

typedef union {
	struct {
		u32 addr	:7; /* I2C address */
		u32 cmd		:1;
#define FL2000_CTRL_CMD_WRITE		0
#define FL2000_CTRL_CMD_READ		1
		u32 offset	:8; /* I2C offset, only valid for I2C */
		u32 bus		:1;
#define FL2000_CTRL_BUS_I2C		0
#define FL2000_CTRL_BUS_SPI		1
		u32 spi_erase	:1; /* only valid for SPI */
		u32 res_1	:6;
		u32 data_status	:4; /* mask for failed bytes */
#define FL2000_DATA_STATUS_PASS		0
#define FL2000_DATA_STATUS_FAIL		1
		u32 flags	:3;
#define FL2000_DETECT_MONITOR		(1<<0)
#define FL2000_CONECTION_ENABLE		(1<<2)
		u32 op_status	:1;
#define FL2000_CTRL_OP_STATUS_PROGRESS	0
#define FL2000_CTRL_OP_STATUS_DONE	1
	} __attribute__ ((__packed__));
	u32 w;
} fl2000_bus_control_reg;

typedef union {
	u8 b[sizeof(u32)];
	u32 w;
} fl2000_data_reg;

/* Custom code for DRM bridge autodetection since there is no DT support */
#define I2C_CLASS_HDMI	(1<<9)
#define CONNECTION_SIZE	64
static inline int drm_i2c_bridge_connection_id(char *connection_id,
		struct i2c_adapter *adapter)
{
	return snprintf(connection_id, CONNECTION_SIZE, "%s-bridge",
			adapter->name);
}

#endif /* __FL2000_DRM_H__ */
