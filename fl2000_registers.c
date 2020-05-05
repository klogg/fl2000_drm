/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_registers.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define CONTROL_MSG_LEN		4
#define CONTROL_MSG_READ	64
#define CONTROL_MSG_WRITE	65

#define FL2000_HW_RST_MDELAY	10

enum fl2000_regfield_n {
	U1_REJECT,
	U2_REJECT,
	WAKE_NRDY,
	APP_RESET,
	WAKEUP_CLR_EN,
	EDID_DETECT,
	MON_DETECT,
	MAGIC,
	NUM_REGFIELDS
};

struct fl2000_reg_data {
	struct usb_device *usb_dev;
	struct regmap_field *field[NUM_REGFIELDS];
#if defined(CONFIG_DEBUG_FS)
	unsigned int reg_debug_address;
	struct dentry *root_dir, *reg_address_file, *reg_data_file;

#endif
};

static const struct {
	enum fl2000_regfield_n n;
	struct reg_field field;
} fl2000_reg_fields[] = {
	{U1_REJECT, FL2000_USB_LPM_u1_reject},
	{U2_REJECT, FL2000_USB_LPM_u2_reject},
	{WAKE_NRDY, FL2000_USB_CTRL_wake_nrdy},
	{APP_RESET, FL2000_RST_CTRL_REG_app_reset},
	{WAKEUP_CLR_EN, FL2000_VGA_CTRL_REG_3_wakeup_clr_en},
	{EDID_DETECT, FL2000_VGA_I2C_SC_REG_edid_detect},
	{MON_DETECT, FL2000_VGA_I2C_SC_REG_mon_detect},
	{MAGIC, FL2000_USB_LPM_magic}
};

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	int ret;
	struct fl2000_reg_data *reg_data = context;
	struct usb_device *usb_dev = reg_data->usb_dev;
	u16 offset = (u16)reg;
	u32 *data = kmalloc(sizeof(*data), GFP_KERNEL | GFP_DMA);

	BUG_ON(!data);

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0),
			CONTROL_MSG_READ, (USB_DIR_IN | USB_TYPE_VENDOR), 0,
			offset, data, CONTROL_MSG_LEN, USB_CTRL_GET_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN)
			ret = -1;
		else
			ret = 0;
	}

	*val = *data;

	kfree(data);
	return ret;
}

static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	int ret;
	struct fl2000_reg_data *reg_data = context;
	struct usb_device *usb_dev = reg_data->usb_dev;
	u16 offset = (u16)reg;
	u32 *data = kmalloc(sizeof(*data), GFP_KERNEL | GFP_DMA);

	BUG_ON(!data);

	*data = val;

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
			CONTROL_MSG_WRITE, (USB_DIR_OUT | USB_TYPE_VENDOR), 0,
			offset, data, CONTROL_MSG_LEN, USB_CTRL_SET_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN)
			ret = -1;
		else
			ret = 0;
	}

	kfree(data);
	return ret;
}

/* We do not use default values as per documentation because:
 *  a) somehow they differ from real HW
 *  b) on SW reset not all of them are cleared
 */
static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = 0xFFFF,

	.cache_type = REGCACHE_RBTREE,

	.precious_reg = fl2000_reg_precious,
	.volatile_reg = fl2000_reg_volatile,

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.reg_read = fl2000_reg_read,
	.reg_write = fl2000_reg_write,

	.use_single_read = true,
	.use_single_write = true,
};

#if defined(CONFIG_DEBUG_FS)

static int fl2000_debugfs_reg_read(void *data, u64 *value)
{
	int ret;
	unsigned int int_value;
	struct fl2000_reg_data *reg_data = data;
	struct usb_device *usb_dev = reg_data->usb_dev;

	ret = fl2000_reg_read(usb_dev, reg_data->reg_debug_address, &int_value);
	*value = int_value;

	return ret;
}

static int fl2000_debugfs_reg_write(void *data, u64 value)
{
	struct fl2000_reg_data *reg_data = data;
	struct usb_device *usb_dev = reg_data->usb_dev;

	return fl2000_reg_write(usb_dev, reg_data->reg_debug_address, value);
}

DEFINE_SIMPLE_ATTRIBUTE(reg_ops, fl2000_debugfs_reg_read,
		fl2000_debugfs_reg_write, "%08llx\n");

static int fl2000_debugfs_reg_init(struct fl2000_reg_data *reg_data)
{
	struct usb_device *usb_dev = reg_data->usb_dev;

	reg_data->root_dir = debugfs_create_dir("fl2000_regs", NULL);
	if (IS_ERR(reg_data->root_dir))
		return PTR_ERR(reg_data->root_dir);

	reg_data->reg_address_file = debugfs_create_x32("reg_address",
			fl2000_debug_umode, reg_data->root_dir,
			&reg_data->reg_debug_address);
	if (IS_ERR(reg_data->reg_address_file))
		return PTR_ERR(reg_data->reg_address_file);

	reg_data->reg_data_file = debugfs_create_file("reg_data",
			fl2000_debug_umode, reg_data->root_dir, usb_dev,
			&reg_ops);
	if (IS_ERR(reg_data->reg_data_file))
		return PTR_ERR(reg_data->reg_data_file);

	return 0;
}

static void fl2000_debugfs_reg_remove(struct fl2000_reg_data *reg_data)
{
	debugfs_remove(reg_data->reg_data_file);
	debugfs_remove(reg_data->reg_address_file);
	debugfs_remove(reg_data->root_dir);
}

#else /* CONFIG_DEBUG_FS */

#define fl2000_debugfs_reg_init(reg_data)
#define fl2000_debugfs_reg_remove(reg_data)

#endif /* CONFIG_DEBUG_FS */

static void fl2000_reg_data_release(struct device *dev, void *res)
{
	/* Noop */
}

int fl2000_reset(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg_data *reg_data = devres_find(&usb_dev->dev,
			fl2000_reg_data_release, NULL, NULL);

	if (!reg_data) {
		dev_err(&usb_dev->dev, "Device resources not found");
		return -ENOMEM;
	}

	ret = regmap_field_write(reg_data->field[APP_RESET], true);
	if (ret)
		return -EIO;

	msleep(FL2000_HW_RST_MDELAY);

	return 0;
}

int fl2000_afe_magic(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg_data *reg_data = devres_find(&usb_dev->dev,
			fl2000_reg_data_release, NULL, NULL);;

	/* XXX: This is actually some unknown & undocumented FL2000 USB AFE
	 * register setting */
	ret = regmap_field_write(reg_data->field[MAGIC], true);
	if (ret)
		return -EIO;

	return 0;
}

int fl2000_usb_magic(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg_data *reg_data = devres_find(&usb_dev->dev,
			fl2000_reg_data_release, NULL, NULL);;

	ret = regmap_field_write(reg_data->field[EDID_DETECT], true);
	if (ret)
		return -EIO;
	ret = regmap_field_write(reg_data->field[MON_DETECT], true);
	if (ret)
		return -EIO;
	ret = regmap_field_write(reg_data->field[WAKEUP_CLR_EN], false);
	if (ret)
		return -EIO;
	ret = regmap_field_write(reg_data->field[U1_REJECT], true);
	if (ret)
		return -EIO;
	ret = regmap_field_write(reg_data->field[U2_REJECT], true);
	if (ret)
		return -EIO;
	ret = regmap_field_write(reg_data->field[WAKE_NRDY], false);
	if (ret)
		return -EIO;

	return 0;
}

int fl2000_regmap_init(struct usb_device *usb_dev)
{
	int i, ret;
	struct fl2000_reg_data *reg_data;
	struct regmap *regmap;
	fl2000_vga_status_reg status;

	reg_data = devres_alloc(&fl2000_reg_data_release, sizeof(*reg_data),
			GFP_KERNEL);
	if (!reg_data) {
		dev_err(&usb_dev->dev, "Registers data allocation failed");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, reg_data);

	reg_data->usb_dev = usb_dev;

	regmap = devm_regmap_init(&usb_dev->dev, NULL, reg_data,
			&fl2000_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&usb_dev->dev, "Registers map failed (%ld)",
				PTR_ERR(regmap));
		devres_release(&usb_dev->dev, fl2000_reg_data_release,
				NULL, NULL);
		return PTR_ERR(regmap);
	}

	for (i = 0; i < ARRAY_SIZE(fl2000_reg_fields); i++) {
		enum fl2000_regfield_n n = fl2000_reg_fields[i].n;
		reg_data->field[n] = devm_regmap_field_alloc(
				&usb_dev->dev, regmap,
				fl2000_reg_fields[i].field);
		if (IS_ERR(reg_data->field[n])) {
			/* TODO: Release what was allocated before error */
			return PTR_ERR(reg_data->field[n]);
		}
	}

	ret = fl2000_debugfs_reg_init(reg_data);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot create debug entry (%d)", ret);
		return ret;
	}

	/* TODO: Move initial reset to higher level initialization function */
	ret = fl2000_reset(usb_dev);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot reset device (%d)", ret);
		return ret;
	}

	/* TODO: Split interrupt processing into DRM and register parts */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot reset interrupts (%d)", ret);
		return ret;
	}

	dev_info(&usb_dev->dev, "Configured FL2000 registers");
	return 0;
}

void fl2000_regmap_cleanup(struct usb_device *usb_dev)
{
	int i;
	struct fl2000_reg_data *reg_data = devres_find(&usb_dev->dev,
			fl2000_reg_data_release, NULL, NULL);

	fl2000_debugfs_reg_remove(reg_data);

	for (i = 0; i < ARRAY_SIZE(fl2000_reg_fields); i++) {
		enum fl2000_regfield_n n = fl2000_reg_fields[i].n;
		devm_regmap_field_free(&usb_dev->dev, reg_data->field[n]);
	}

	/* XXX: Current regmap implementation missing some kind of a
	 * devm_regmap_destroy() call that would work similarly to *_find() */

	if (reg_data)
		devres_release(&usb_dev->dev, fl2000_reg_data_release,
				NULL, NULL);
}
