/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm_drm.c
 *
 * (C) Copyright 2019, Artem Mygaiev
 */

#include "fl2000.h"

#include <linux/debugfs.h>
#include <linux/fs.h>


/* NOTE: it might be unsafe / insecure to use allow rw access everyone */
static const umode_t fl2000_debug_umode = 0666;

static u8 i2c_address;	/* I2C bus address that we will talk to */
static u8 i2c_offset;	/* Offset of the register within I2C address */

static int i2c_value_read(void *data, u64 *value)
{
	struct i2c_adapter *i2c_adapter = data;
	return fl2000_i2c_read_dword(i2c_adapter, i2c_address, i2c_offset,
			value);
}

static int i2c_value_write(void *data, u64 value)
{
	struct i2c_adapter *i2c_adapter = data;
	return fl2000_i2c_write_dword(i2c_adapter, i2c_address, i2c_offset,
			value);
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_ops, i2c_value_read, i2c_value_write, "%x\n");

static u32 reg_address;

static int reg_value_read(void *data, u64 *value)
{
	struct usb_device *usb_dev = data;
	return fl2000_reg_read(usb_dev, reg_address, value);
}

static int reg_value_write(void *data, u64 value)
{
	struct usb_device *usb_dev = data;
	return fl2000_reg_write(usb_dev, reg_address, value);
}

DEFINE_SIMPLE_ATTRIBUTE(reg_ops, reg_value_read, reg_value_write, "%llx\n");

int fl2000_debugfs_init(struct drm_minor *minor)
{
	struct dentry *i2c_address_file, *i2c_offset_file, *i2c_data_file;
	struct dentry *reg_address_file, *reg_data_file;
	struct drm_device *drm_dev = minor->dev;

	struct i2c_adapter *i2c_adapter = NULL;
	struct usb_device *usb_dev = NULL;

	i2c_address_file = debugfs_create_x8("i2c_address", fl2000_debug_umode,
			minor->debugfs_root, &i2c_address);

	i2c_offset_file = debugfs_create_x8("i2c_offset", fl2000_debug_umode,
			minor->debugfs_root, &i2c_offset);

	i2c_data_file = debugfs_create_file("i2c_data", fl2000_debug_umode,
			minor->debugfs_root, i2c_adapter, &i2c_ops);

	reg_address_file = debugfs_create_x32("reg_address", fl2000_debug_umode,
			minor->debugfs_root, &reg_address);

	reg_data_file = debugfs_create_file("reg_data", fl2000_debug_umode,
			minor->debugfs_root, usb_dev, &reg_ops);

	return 0;
}
