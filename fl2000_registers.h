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

#include <linux/types.h>

#define regmap_reg_default(addr, val) {.reg = addr, .def = val}

/* #### USB Control Registers Bank #### */
#define FL2000_USB_CONTROL_OFFSET	0x0000

#define FL2000_USB_LPM			(FL2000_USB_CONTROL_OFFSET + 0x70)
static const struct reg_field FL2000_USB_LPM_magic =
		REG_FIELD(FL2000_USB_LPM, 13, 13);
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
		u32 lbuf_halt:1;
		u32 iso_ack:1;		/* read self clear */
		u32 td_drop:1;		/* read self clear */
		u32 irq_pending:1;	/* read self clear */
		u32 pll_status:1;
		u32 dac_status:1;
		u32 lbuf_overflow:1;
		u32 lbuf_underflow:1;
		u32 frame_cnt:16;
		u32 hdmi_event:1;	/* read self clear */
		u32 hdmi_status:1;
		u32 edid_status:1;
		u32 monitor_status:1;
		u32 monitor_event:1;	/* read self clear */
		u32 edid_event:1;	/* read self clear */
	} __attribute__ ((aligned, packed));
} fl2000_vga_status_reg;

#define FL2000_VGA_CTRL_REG_PXCLK	(FL2000_VGA_CONTROL_OFFSET + 0x04)
typedef union {
	u32 val;
	struct {
		u32 clear_watermark:1;
		u32 frame_sync:1;
		u32 hsync_polarity:1;
		u32 vsync_polarity:1;
		u32 de_polarity:1;
		u32 mirror_mode:1;
		u32 vga565_mode:1;
		u32 dac_output_en:1;
		u32 vga_timing_en:1;
		u32 use_new_pkt_retry:1;
		u32 ref_select:1;
		u32 dac_px_clk_invert:1;
		u32 clear_lbuf_status:1;
		u32 drop_cnt:1;
		u32 use_vdi_itp_cnt:1;
		u32 __reserved1:1;
		u32 __reserved2:8;
		u32 vga_compress:1;
		u32 vga332_mode:1;
		u32 vga_color_palette_en:1;
		u32 vga_first_bt_enc_en:1;
		u32 clear_125us_cnt:1;
		u32 disable_halt:1;
		u32 force_de_en:1;
		u32 vga555_mode:1;
	} __attribute__ ((aligned, packed));
} fl2000_vga_cntrl_reg_pxclk;

#define FL2000_VGA_HSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x08)
typedef union {
	u32 val;
	struct {
		u32 htotal:12;
		u32 __reserved1:4;
		u32 hactive:12;
		u32 __reserved2:4;
	} __attribute__ ((aligned, packed));
} fl2000_vga_hsync_reg1;

#define FL2000_VGA_HSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x0C)
typedef union {
	u32 val;
	struct {
		u32 hstart:12;
		u32 __reserved1:4;
		u32 hsync_width:8;
		u32 __reserved2:8;
	} __attribute__ ((aligned, packed));
} fl2000_vga_hsync_reg2;

#define FL2000_VGA_VSYNC_REG1		(FL2000_VGA_CONTROL_OFFSET + 0x10)
typedef union {
	u32 val;
	struct {
		u32 vtotal:12;
		u32 __reserved1:4;
		u32 vactive:12;
		u32 __reserved2:4;
	} __attribute__ ((aligned, packed));
} fl2000_vga_vsync_reg1;

#define FL2000_VGA_VSYNC_REG2		(FL2000_VGA_CONTROL_OFFSET + 0x14)
typedef union {
	u32 val;
	struct {
		u32 vstart:12;
		u32 __reserved1:4;
		u32 vsync_width:3;
		u32 __reserved2:1;
		u32 start_latency:10;
		u32 __reserved3:1;
		u32 buf_error_en:1;
	} __attribute__ ((aligned, packed));
} fl2000_vga_vsync_reg2;

#define FL2000_VGA_TEST_REG		(FL2000_VGA_CONTROL_OFFSET + 0x18)
#define FL2000_VGA_ISOCH_REG		(FL2000_VGA_CONTROL_OFFSET + 0x1C)
typedef union {
	u32 val;
	struct {
		u32 start_mframe_cnt:14;
		u32 use_mframe_match:1;
		u32 use_zero_len_frame:1;
		u32 mframe_cnt:14;
		u32 mframe_cnt_update:1;
		u32 __reserved:1;
	} __attribute__ ((aligned, packed));
} fl2000_vga_isoch_reg;

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
static const struct reg_field FL2000_VGA_I2C_SC_REG_mon_detect =
		REG_FIELD(FL2000_VGA_I2C_SC_REG, 28, 28);

#define FL2000_VGA_I2C_RD_REG		(FL2000_VGA_CONTROL_OFFSET + 0x24)
#define FL2000_VGA_I2C_WR_REG		(FL2000_VGA_CONTROL_OFFSET + 0x28)
#define FL2000_VGA_PLL_REG		(FL2000_VGA_CONTROL_OFFSET + 0x2C)
typedef union {
	u32 val;
	struct {
		u32 post_div:8;
		u32 pre_div:8;
		u32 mul:8;
		u32 test_io:1;
		u32 cfg_dac_pwrdown:1;
		u32 force_dac_pwrup:1;
		u32 __reserved:5;
	} __attribute__ ((aligned, packed));
} fl2000_vga_pll_reg;

#define FL2000_VGA_LBUF_REG		(FL2000_VGA_CONTROL_OFFSET + 0x30)
#define FL2000_VGA_HI_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x34)
#define FL2000_VGA_LO_MARK		(FL2000_VGA_CONTROL_OFFSET + 0x38)
#define FL2000_VGA_CTRL_REG_ACLK	(FL2000_VGA_CONTROL_OFFSET + 0x3C)
typedef union {
	u32 val;
	struct {
		u32 cfg_timing_reset_n:1;
		u32 plh_block_en:1;
		u32 edid_mon_int_en:1;
		u32 ext_mon_int_en:1;
		u32 vga_status_self_clear:1;
		u32 pll_lock_time:5;
		u32 pll_fast_timeout_en:1;
		u32 ppe_block_em:1;
		u32 pll_timer_en:1;
		u32 feedback_int_en:1;
		u32 clr_125us_counter:1;
		u32 ccs_pd_dis:1;
		u32 standby_en:1;
		u32 force_loopback:1;
		u32 lbuf_drop_frame_en:1;
		u32 lbuf_vde_rst_en:1;
		u32 lbuf_sw_rst:1;
		u32 lbuf_err_int_en:1;
		u32 biac_en:1;
		u32 pxclk_in_en:1;
		u32 vga_err_int_en:1;
		u32 force_vga_connect:1;
		u32 force_pll_up:1;
		u32 use_zero_td:1;
		u32 use_zero_pkt_len:1;
		u32 use_pkt_pending:1;
		u32 pll_dac_pd_usbp3_en:1;
		u32 pll_dac_pd_novga_en:1;
	} __attribute__ ((aligned, packed));
} fl2000_vga_ctrl_reg_aclk;

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
static const struct reg_field FL2000_VGA_CTRL_REG_3_wakeup_clr_en =
		REG_FIELD(FL2000_VGA_CTRL_REG_3, 10, 10);

/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x8C) */

/*
 * List of registers that shall not be read automatically (e.g. for cache update
 * due to presence of self-clear bits
 * */
static inline bool FL2000_REG_PRECIOUS(u32 reg)
{
	if (reg == FL2000_VGA_STATUS_REG)
		return true;
	else
		return false;
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
