// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define CONTROL_MSG_LEN	  sizeof(u32)
#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10

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
	struct mutex reg_mutex; /* Serialize control messages for register access */
	void *data;
#if defined(CONFIG_DEBUG_FS)
	unsigned int reg_debug_address;
	struct dentry *root_dir, *reg_data_file;

#endif
};

static const struct {
	enum fl2000_regfield_n n;
	struct reg_field field;
} fl2000_reg_fields[] = { { U1_REJECT, FL2000_USB_LPM_u1_reject },
			  { U2_REJECT, FL2000_USB_LPM_u2_reject },
			  { WAKE_NRDY, FL2000_USB_CTRL_wake_nrdy },
			  { APP_RESET, FL2000_RST_CTRL_REG_app_reset },
			  { WAKEUP_CLR_EN, FL2000_VGA_CTRL_REG_3_wakeup_clr_en },
			  { EDID_DETECT, FL2000_VGA_I2C_SC_REG_edid_detect },
			  { MON_DETECT, FL2000_VGA_I2C_SC_REG_mon_detect },
			  { MAGIC, FL2000_USB_LPM_magic } };

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

	mutex_lock(&reg_data->reg_mutex);

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), CONTROL_MSG_READ,
			      (USB_DIR_IN | USB_TYPE_VENDOR), 0, offset, reg_data->data,
			      CONTROL_MSG_LEN, USB_CTRL_GET_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN)
			ret = -1;
		else
			ret = 0;
	}

	memcpy(val, reg_data->data, CONTROL_MSG_LEN);

	mutex_unlock(&reg_data->reg_mutex);

	return ret;
}

static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	int ret;
	struct fl2000_reg_data *reg_data = context;
	struct usb_device *usb_dev = reg_data->usb_dev;
	u16 offset = (u16)reg;

	mutex_lock(&reg_data->reg_mutex);

	memcpy(reg_data->data, &val, CONTROL_MSG_LEN);

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), CONTROL_MSG_WRITE,
			      (USB_DIR_OUT | USB_TYPE_VENDOR), 0, offset, reg_data->data,
			      CONTROL_MSG_LEN, USB_CTRL_SET_TIMEOUT);
	if (ret > 0) {
		if (ret != CONTROL_MSG_LEN)
			ret = -1;
		else
			ret = 0;
	}

	mutex_unlock(&reg_data->reg_mutex);

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

DEFINE_SIMPLE_ATTRIBUTE(reg_ops, fl2000_debugfs_reg_read, fl2000_debugfs_reg_write, "%08llx\n");

static int fl2000_debugfs_reg_init(struct fl2000_reg_data *reg_data)
{
	struct usb_device *usb_dev = reg_data->usb_dev;

	reg_data->root_dir = debugfs_create_dir("fl2000_regs", NULL);
	if (IS_ERR(reg_data->root_dir))
		return PTR_ERR(reg_data->root_dir);

	debugfs_create_x32("reg_address", fl2000_debug_umode, reg_data->root_dir,
			   &reg_data->reg_debug_address);

	reg_data->reg_data_file = debugfs_create_file("reg_data", fl2000_debug_umode,
						      reg_data->root_dir, usb_dev, &reg_ops);
	if (IS_ERR(reg_data->reg_data_file))
		return PTR_ERR(reg_data->reg_data_file);

	return 0;
}

static void fl2000_debugfs_reg_remove(struct fl2000_reg_data *reg_data)
{
	debugfs_remove(reg_data->reg_data_file);
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

int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_pll_reg pll_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	u32 mask = 0;

	pll_reg.prescaler = pll->prescaler;
	pll_reg.multiplier = pll->multiplier;
	pll_reg.divisor = pll->divisor;
	pll_reg.function = pll->function;
	regmap_write(regmap, FL2000_VGA_PLL_REG, pll_reg.val);

	aclk.force_pll_up = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_pll_up);
	aclk.force_vga_connect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	return 0;
}

int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_hsync_reg1 hsync1 = { .val = 0 };
	union fl2000_vga_hsync_reg2 hsync2 = { .val = 0 };
	union fl2000_vga_vsync_reg1 vsync1 = { .val = 0 };
	union fl2000_vga_vsync_reg2 vsync2 = { .val = 0 };

	hsync1.hactive = timings->hactive;
	hsync1.htotal = timings->htotal;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG1, hsync1.val);

	hsync2.hsync_width = timings->hsync_width;
	hsync2.hstart = timings->hstart;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG2, hsync2.val);

	vsync1.vactive = timings->vactive;
	vsync1.vtotal = timings->vtotal;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG1, vsync1.val);

	vsync2.vsync_width = timings->vsync_width;
	vsync2.vstart = timings->vstart;
	vsync2.start_latency = timings->vstart;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG2, vsync2.val);

	return 0;
}

int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_cntrl_reg_pxclk pxclk = { .val = 0 };
	u32 mask = 0;

	pxclk.dac_output_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.drop_cnt = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, drop_cnt);
	pxclk.vga565_mode = (bytes_pix == 2);
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga565_mode);
	pxclk.vga332_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga332_mode);
	pxclk.vga555_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga555_mode);
	pxclk.vga_compress = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga_compress);
	pxclk.dac_output_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.clear_watermark = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, clear_watermark);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, pxclk.val);

	return 0;
}

/* TODO: Support ISO configuration */
int fl2000_set_transfers(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	union fl2000_vga_isoch_reg isoch = { .val = 0 };
	u32 mask;

	mask = 0;
	aclk.use_pkt_pending = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_pkt_pending);
	aclk.use_zero_td = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_td);
	aclk.use_zero_pkt_len = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_pkt_len);
	aclk.vga_err_int_en = true;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	isoch.mframe_cnt = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_isoch_reg, mframe_cnt);
	regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, mask, isoch.val);

	return 0;
}

int fl2000_reset(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg_data *reg_data =
		devres_find(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);

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
	struct fl2000_reg_data *reg_data =
		devres_find(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);

	/* XXX: This is actually some unknown & undocumented FL2000 USB AFE register setting */
	ret = regmap_field_write(reg_data->field[MAGIC], true);
	if (ret)
		return -EIO;

	return 0;
}

int fl2000_usb_magic(struct usb_device *usb_dev)
{
	int ret;
	struct fl2000_reg_data *reg_data =
		devres_find(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);

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

int fl2000_enable_interrupts(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	union fl2000_vga_ctrl2_reg_axclk axclk = { .val = 0 };
	u32 mask;

	mask = 0;
	aclk.vga_err_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, vga_err_int_en);
	aclk.lbuf_err_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, lbuf_err_int_en);
	aclk.edid_mon_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, edid_mon_int_en);
	aclk.edid_mon_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, edid_mon_int_en);
	aclk.feedback_int_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, feedback_int_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	axclk.hdmi_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl2_reg_axclk, hdmi_int_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL2_REG_ACLK, mask, axclk.val);

	return 0;
}

int fl2000_check_interrupt(struct usb_device *usb_dev)
{
	int ret;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_status_reg status;
	u32 mask = 0;

	/* Process interrupt */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot read interrupt register (%d)", ret);
		return ret;
	}

	if (status.hdmi_event || status.monitor_event || status.edid_event) {
		dev_info(&usb_dev->dev, "Connection event 0x%X", status.val);
		fl2000_display_event_check(usb_dev);
	}

	/* LBUF issues are recoverable */
	if (status.lbuf_overflow) {
		dev_err(&usb_dev->dev, "LBUF overflow detected!");
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_overflow);
	}
	if (status.lbuf_underflow) {
		dev_err(&usb_dev->dev, "LBUF underflow detected!");
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_underflow);
	}
	regmap_write_bits(regmap, FL2000_VGA_STATUS_REG, mask, status.val);

	if (status.lbuf_halt) {
		/* TODO: Reset LBUF using regmap_field for lbuf_sw_rst */
		dev_err(&usb_dev->dev, "LBUF halted!");
	}

	if (status.vga_error) {
		/* TODO: Don't know how to recover here */
		dev_err(&usb_dev->dev, "VGA error detected!");
	}

	return 0;
}

int fl2000_regmap_init(struct usb_device *usb_dev)
{
	int i, ret;
	struct fl2000_reg_data *reg_data;
	struct regmap *regmap;
	union fl2000_vga_status_reg status;

	reg_data = devres_alloc(&fl2000_reg_data_release, sizeof(*reg_data), GFP_KERNEL);
	if (!reg_data) {
		dev_err(&usb_dev->dev, "Registers data allocation failed");
		return -ENOMEM;
	}
	devres_add(&usb_dev->dev, reg_data);

	mutex_init(&reg_data->reg_mutex);

	reg_data->usb_dev = usb_dev;

	reg_data->data = kmalloc(CONTROL_MSG_LEN, GFP_KERNEL | GFP_DMA);

	regmap = devm_regmap_init(&usb_dev->dev, NULL, reg_data, &fl2000_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&usb_dev->dev, "Registers map failed (%ld)", PTR_ERR(regmap));
		devres_release(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);
		return PTR_ERR(regmap);
	}

	for (i = 0; i < ARRAY_SIZE(fl2000_reg_fields); i++) {
		enum fl2000_regfield_n n = fl2000_reg_fields[i].n;

		reg_data->field[n] =
			devm_regmap_field_alloc(&usb_dev->dev, regmap, fl2000_reg_fields[i].field);
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
	struct fl2000_reg_data *reg_data =
		devres_find(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);

	fl2000_debugfs_reg_remove(reg_data);

	for (i = 0; i < ARRAY_SIZE(fl2000_reg_fields); i++) {
		enum fl2000_regfield_n n = fl2000_reg_fields[i].n;

		devm_regmap_field_free(&usb_dev->dev, reg_data->field[n]);
	}

	/* XXX: Current regmap implementation missing some kind of a devm_regmap_destroy() call that
	 * would work similarly to *_find()
	 */

	kfree(reg_data->data);

	if (reg_data)
		devres_release(&usb_dev->dev, fl2000_reg_data_release, NULL, NULL);
}
