// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

/* Protected by internal regmap mutex */
static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	int ret;
	u32 *usb_rw_data;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;

	usb_rw_data = kmalloc(sizeof(*usb_rw_data), GFP_KERNEL | GFP_DMA);
	if (!usb_rw_data)
		return -ENOMEM;

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), CONTROL_MSG_READ,
			      (USB_DIR_IN | USB_TYPE_VENDOR), 0, offset, usb_rw_data, sizeof(u32),
			      USB_CTRL_GET_TIMEOUT);
	if (ret > 0) {
		if (ret != sizeof(u32))
			ret = -1;
		else
			ret = 0;
	}

	*val = *usb_rw_data;

	kfree(usb_rw_data);
	return ret;
}

/* Protected by internal regmap mutex */
static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	int ret;
	u32 *usb_rw_data;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;

	usb_rw_data = kmalloc(sizeof(*usb_rw_data), GFP_KERNEL | GFP_DMA);
	if (!usb_rw_data)
		return -ENOMEM;

	*usb_rw_data = val;

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), CONTROL_MSG_WRITE,
			      (USB_DIR_OUT | USB_TYPE_VENDOR), 0, offset, usb_rw_data, sizeof(u32),
			      USB_CTRL_SET_TIMEOUT);
	if (ret > 0) {
		if (ret != sizeof(u32))
			ret = -1;
		else
			ret = 0;
	}

	kfree(usb_rw_data);
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
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_rst_cntrl_reg rst_cntrl_reg = { .val = 0 };
	u32 mask = 0;

	rst_cntrl_reg.sw_reset = true;
	fl2000_add_bitmask(mask, union fl2000_rst_cntrl_reg, sw_reset);
	regmap_write_bits(regmap, FL2000_RST_CTRL_REG, mask, rst_cntrl_reg.val);

	msleep(FL2000_HW_RST_MDELAY);

	return 0;
}

int fl2000_afe_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	u32 mask = 0;

	usb_lpm_reg.magic = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, magic);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	return 0;
}

int fl2000_usb_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_i2c_sc_reg vga_i2c_sc_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_3 vga_ctrl_reg_3 = { .val = 0 };
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	union fl2000_usb_ctrl_reg usb_ctrl_reg = { .val = 0 };
	u32 mask;

	mask = 0;
	vga_i2c_sc_reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	vga_i2c_sc_reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);
	regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, vga_i2c_sc_reg.val);

	mask = 0;
	vga_ctrl_reg_3.wakeup_clr_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_3, wakeup_clr_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, mask, vga_ctrl_reg_3.val);

	mask = 0;
	usb_lpm_reg.u1_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u1_reject);
	usb_lpm_reg.u2_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u2_reject);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	mask = 0;
	usb_ctrl_reg.wake_nrdy = false;
	fl2000_add_bitmask(mask, union fl2000_usb_ctrl_reg, wake_nrdy);
	regmap_write_bits(regmap, FL2000_USB_CTRL_REG, mask, usb_ctrl_reg.val);

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
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_status_reg status;
	int ret;
	int sink_event = 0;
	u32 mask = 0;

	/* Process interrupt */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret)
		return 0; /* XXX: Cannot report error here */

	if (status.hdmi_event || status.monitor_event || status.edid_event)
		sink_event = 1;

	/* LBUF issues are recoverable */
	if (status.lbuf_overflow)
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_overflow);
	if (status.lbuf_underflow)
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_underflow);
	regmap_write_bits(regmap, FL2000_VGA_STATUS_REG, mask, status.val);

	/* TODO: Reset LBUF using regmap_field if (status.lbuf_halt) */

	/* TODO: Don't know how to recover if (status.vga_error) */

	return sink_event;
}

int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data)
{
	int ret;
	union fl2000_vga_i2c_sc_reg reg = { .val = 0 };
	u32 mask = 0;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret)
			return -EIO;
	}

	/* This bit always reads back as 0, so we need to restore it back. Though not quite
	 * sure if we need to enable monitor detection circuit for HDMI use case
	 */
	reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);

	reg.i2c_status = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_status);
	reg.i2c_addr = addr;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_addr);
	reg.i2c_cmd = read;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_cmd);
	reg.i2c_offset = offset;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_offset);
	reg.i2c_done = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_done);

	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
	if (ret)
		return -EIO;

	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, reg.val, reg.i2c_done,
				       I2C_RDWR_INTERVAL, I2C_RDWR_TIMEOUT);
	/* This shouldn't normally happen: there's internal 256ms HW timeout on I2C operations and
	 * USB must be always available so no I/O errors. But if it happens we are probably in
	 * irreversible HW issue
	 */
	if (ret || reg.i2c_status != 0)
		return -EIO;

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret)
			return -EIO;
	}

	return 0;
}

struct regmap *fl2000_regmap_init(struct usb_device *usb_dev)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&usb_dev->dev, NULL, usb_dev, &fl2000_regmap_config);
	if (IS_ERR(regmap))
		dev_err(&usb_dev->dev, "Registers map failed (%ld)", PTR_ERR(regmap));

	return regmap;
}
