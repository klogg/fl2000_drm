/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_registers.h
 *
 * Based on the official registers description from Fresco Logic
 *
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef __FL2000_REGISTERS_H__
#define __FL2000_REGISTERS_H__

#define regmap_reg_default(addr, val) {.reg = addr, .def = val}

/* #### USB Control Registers Bank #### */
#define FL2000_USB_CONTROL_OFFSET	0x0000

#define FL2000_USB_LPM			(FL2000_USB_CONTROL_OFFSET + 0x70)
static const struct reg_field FL2000_USB_LPM_u2_reject =
		REG_FIELD(FL2000_USB_LPM, 19, 19);
static const struct reg_field FL2000_USB_LPM_u1_reject =
		REG_FIELD(FL2000_USB_LPM, 20, 20);

#define FL2000_USB_CTRL			(FL2000_USB_CONTROL_OFFSET + 0x78)
static const struct reg_field FL2000_USB_CTRL_wake_nrdy =
		REG_FIELD(FL2000_USB_CTRL, 17, 17);

/* #### VGA Control Registers Bank #### */
#define FL2000_VGA_CONTROL_OFFSET	0x8000

#define FL2000_VGA_STATUS_REG		(FL2000_VGA_CONTROL_OFFSET + 0x00)
/* Implement structure here because self-clearing fields cannot be read
 * with sequential access to reg_fields */
typedef union {
	u32 val;
	struct {
		u32 vga_status:1;
		u32 vga_error:1;	/* read self clear */
		u32 line_buf_halt:1;
		u32 iso_ack:1;		/* read self clear */
		u32 td_drop:1;		/* read self clear */
		u32 irq_pending:1;	/* read self clear */
		u32 pll_status:1;
		u32 dac_status:1;
		u32 lbuf_overflow:1;
		u32 lbuf_underflow:1;
		u32 frame_count:16;
		u32 hdmi_event:1;	/* read self clear */
		u32 hdmi_status:1;
		u32 edid_status:1;
		u32 monitor_status:1;
		u32 monitor_event:1;	/* read self clear */
		u32 edid_event:1;	/* read self clear */
	} __attribute__ ((aligned, packed));
} fl2000_vga_status_reg;

#define FL2000_VGA_CTRL_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x04)
#define FL2000_VGA_HSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x08)
#define FL2000_VGA_HSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x0C)
#define FL2000_VGA_VSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x10)
#define FL2000_VGA_VSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x14)
#define FL2000_VGA_TEST_REG		(FL2000_VGA_CONTROL_OFFSET + 0x18)
#define FL2000_VGA_ISOCH_REG		(FL2000_VGA_CONTROL_OFFSET + 0x1C)
#define FL2000_VGA_I2C_SC_REG		(FL2000_VGA_CONTROL_OFFSET + 0x20)
/* Implement structure here because during I2C xfers there would be too many
 * slow USB control exchanges caused by independent setting of reg_fields */
typedef union {
	u32 val;
	struct {
		u32 i2c_addr:7;
		u32 i2c_cmd:1;
		u32 i2c_offset:8;
		u32 vga_status:8;
		u32 i2c_status:4;
		u32 monitor_detect:1;
		u32 i2c_ready:1;
		u32 edid_detect:1;
		u32 i2c_done:1;
	} __attribute__ ((aligned, packed));
} fl2000_vga_i2c_sc_reg;
static const struct reg_field FL2000_VGA_I2C_SC_REG_edid_detect =
		REG_FIELD(FL2000_VGA_I2C_SC_REG, 30, 30);
static const struct reg_field FL2000_VGA_I2C_SC_REG_monitor_detect =
		REG_FIELD(FL2000_VGA_I2C_SC_REG, 28, 28);


#define FL2000_VGA_I2C_RD_REG		(FL2000_VGA_CONTROL_OFFSET + 0x24)
#define FL2000_VGA_I2C_WR_REG		(FL2000_VGA_CONTROL_OFFSET + 0x28)
#define FL2000_VGA_PLL_REG		(FL2000_VGA_CONTROL_OFFSET + 0x2C)
#define FL2000_VGA_LBUF_REG		(FL2000_VGA_CONTROL_OFFSET + 0x30)
#define FL2000_VGA_HI_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x34)
#define FL2000_VGA_LO_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x38)
#define FL2000_VGA_CTRL_REG_ACLK	(FL2000_VGA_CONTROL_OFFSET + 0x3C)
#define FL2000_VGA_PXCLK_CNT_REG	(FL2000_VGA_CONTROL_OFFSET + 0x40)
#define FL2000_VGA_VCNT_REG		(FL2000_VGA_CONTROL_OFFSET + 0x44)
#define FL2000_RST_CTRL_REG		(FL2000_VGA_CONTROL_OFFSET + 0x48)
static const struct reg_field FL2000_RST_CTRL_REG_app_reset =
		REG_FIELD(FL2000_RST_CTRL_REG, 15, 15);

#define FL2000_BIAC_CTRL1_REG		(FL2000_VGA_CONTROL_OFFSET + 0x4C)
#define FL2000_BIAC_CTRL2_REG		(FL2000_VGA_CONTROL_OFFSET + 0x50)
#define FL2000_BIAC_STATUS_REG		(FL2000_VGA_CONTROL_OFFSET + 0x54)
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x58) */
#define FL2000_VGA_PLT_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x5C)
#define FL2000_VGA_PLT_RADDR_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x60)
#define FL2000_VGA_CTRL2_REG_ACLK	(FL2000_VGA_CONTROL_OFFSET + 0x64)
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x68) */
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x6C) */
#define FL2000_TEST_CNTL_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x70)
#define FL2000_TEST_CNTL_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x74)
#define FL2000_TEST_CNTL_REG3		(FL2000_VGA_CONTROL_OFFSET + 0x78)
#define FL2000_TEST_STAT1		(FL2000_VGA_CONTROL_OFFSET + 0x7C)
#define FL2000_TEST_STAT2		(FL2000_VGA_CONTROL_OFFSET + 0x80)
#define FL2000_TEST_STAT3		(FL2000_VGA_CONTROL_OFFSET + 0x84)
#define FL2000_VGA_CTRL_REG_3		(FL2000_VGA_CONTROL_OFFSET + 0x88)
static const struct reg_field FL2000_VGA_CTRL_REG_3_wakeup_clear_en =
		REG_FIELD(FL2000_VGA_CTRL_REG_3, 10, 10);

/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x8C) */

static const struct reg_default fl2000_reg_defaults[] = {
	regmap_reg_default(FL2000_VGA_STATUS_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_PXCLK,		0x0010119C),
	regmap_reg_default(FL2000_VGA_HSYNC_REG1,		0x02800320),
	regmap_reg_default(FL2000_VGA_HSYNC_REG2,		0x00600089),
	regmap_reg_default(FL2000_VGA_VSYNC_REG1,		0x01E0020D),
	regmap_reg_default(FL2000_VGA_VSYNC_REG2,		0x0002001C),
	regmap_reg_default(FL2000_VGA_TEST_REG,			0x00000006),
	regmap_reg_default(FL2000_VGA_ISOCH_REG,		0x00850000),
	regmap_reg_default(FL2000_VGA_I2C_SC_REG,		0x80000000),
	regmap_reg_default(FL2000_VGA_I2C_RD_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_I2C_WR_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLL_REG,			0x003F6119),
	regmap_reg_default(FL2000_VGA_LBUF_REG,			0x23300001),
	regmap_reg_default(FL2000_VGA_HI_MARK,			0x00000000),
	regmap_reg_default(FL2000_VGA_LO_MARK,			0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_ACLK,		0x00000000),
	regmap_reg_default(FL2000_VGA_PXCLK_CNT_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_VCNT_REG,			0x00000000),
	regmap_reg_default(FL2000_RST_CTRL_REG,			0x00000100),
	regmap_reg_default(FL2000_BIAC_CTRL1_REG,		0x00A00120),
	regmap_reg_default(FL2000_BIAC_CTRL2_REG,		0x00000000),
	regmap_reg_default(FL2000_BIAC_STATUS_REG,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLT_REG_PXCLK,		0x00000000),
	regmap_reg_default(FL2000_VGA_PLT_RADDR_REG_PXCLK,	0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL2_REG_ACLK,		0x00000000),
	regmap_reg_default(FL2000_TEST_CNTL_REG1,		0xC0003C20),
	regmap_reg_default(FL2000_TEST_CNTL_REG1,		0x00000C04),
	regmap_reg_default(FL2000_TEST_CNTL_REG3,		0x00000000),
	regmap_reg_default(FL2000_TEST_STAT1,			0x00000000),
	regmap_reg_default(FL2000_TEST_STAT2,			0x00000000),
	regmap_reg_default(FL2000_TEST_STAT3,			0x00000000),
	regmap_reg_default(FL2000_VGA_CTRL_REG_3,		0x00000488),
};

/*
 * List of registers that shall not be read automatically (e.g. for cache update
 * due to presence of self-clear bits
 * */
static inline bool FL2000_REG_PRECIOUS(u32 reg)
{
	switch (reg) {
	case FL2000_VGA_STATUS_REG:
		return true;
	default:
		return false;
	}
}

/*
 * List of volatile registers that shall not be cached
 * */
static inline bool FL2000_REG_VOLATILE(u32 reg)
{
	switch (reg) {
	case FL2000_VGA_STATUS_REG:
	case FL2000_VGA_CTRL_REG_PXCLK:
	case FL2000_VGA_ISOCH_REG:
	case FL2000_VGA_I2C_SC_REG:
	case FL2000_VGA_I2C_RD_REG:
	case FL2000_VGA_PXCLK_CNT_REG:
	case FL2000_VGA_VCNT_REG:
	case FL2000_RST_CTRL_REG:
	case FL2000_BIAC_STATUS_REG:
	case FL2000_TEST_CNTL_REG1:
	case FL2000_TEST_CNTL_REG2:
	case FL2000_TEST_STAT1:
	case FL2000_TEST_STAT2:
	case FL2000_TEST_STAT3:
		return true;
	default:
		return false;
	}
}

#endif /* __FL2000_REGISTERS_H__ */
