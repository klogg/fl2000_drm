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
#include <drm/drm_device.h>

#define FL2000_I2C_ADDRESS_HDMI		0x4C
#define FL2000_I2C_ADDRESS_DSUB		0x50
#define FL2000_I2C_ADDRESS_EEPROM	0x54

int fl2000_reg_read(struct usb_device *usb_dev, u32 *data, u16 offset);
int fl2000_reg_write(struct usb_device *usb_dev, u32 *data, u16 offset);

int fl2000_intr_create(struct usb_interface *interface);
void fl2000_intr_destroy(struct usb_interface *interface);

int fl2000_i2c_connect(struct usb_device *usb_dev);
void fl2000_i2c_disconnect(struct usb_device *usb_dev);

#define FL2000_REG_INT_STATUS	0x8000 /* Interrupt status ? */
#define FL2000_REG_FORMAT	0x8004 /* Picture format / Color mode ? */
#define FL2000_REG_H_SYNC1	0x8008 /* h_sync_reg_1 */
#define FL2000_REG_H_SYNC2	0x800C /* h_sync_reg_2 */
#define FL2000_REG_V_SYNC1	0x8010 /* v_sync_reg_1 */
#define FL2000_REG_V_SYNC2	0x8014 /* v_sync_reg_2 */
#define FL2000_REG_8018		0x8018 /* unknown */
#define FL2000_REG_ISO_CTRL	0x801C /* ISO 14 bit value ? */
#define FL2000_REG_I2C_CTRL	0x8020 /* I2C Controller and I2C send */
#define FL2000_REG_I2C_DATA_RD	0x8024 /* I2C read data, 32 bit wide */
#define FL2000_REG_I2C_DATA_WR	0x8028 /* I2C write data, 32 bit wide */
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

#endif /* __FL2000_DRM_H__ */
