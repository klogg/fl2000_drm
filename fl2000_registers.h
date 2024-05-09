/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Based on the official registers description from Fresco Logic
 *
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef __FL2000_REGISTERS_H__
#define __FL2000_REGISTERS_H__

#include <linux/types.h>

#define regmap_reg_default(addr, val)   \
	{                               \
		.reg = addr, .def = val \
	}

/* #### USB Control Registers Bank #### */
/* Some unknown & undocumented FL2000 USB settings */

#define FL2000_USB_CONTROL_OFFSET 0x0000

#define FL2000_USB_LPM_REG (FL2000_USB_CONTROL_OFFSET + 0x70)
union fl2000_usb_lpm_reg {
	u32 val;
	struct {
		u32 __reserved1 : 13;
		u32 magic	: 1;
		u32 __reserved2 : 5;
		u32 u2_reject	: 1;
		u32 u1_reject	: 1;
		u32 __reserved3 : 11;
	} __aligned(4) __packed;
};

#define FL2000_USB_CTRL_REG (FL2000_USB_CONTROL_OFFSET + 0x78)
union fl2000_usb_ctrl_reg {
	u32 val;
	struct {
		u32 __reserved1 : 17;
		u32 wake_nrdy	: 1;
		u32 __reserved2 : 14;
	} __aligned(4) __packed;
};

/* #### VGA Control Registers Bank #### */
/* Taken from the 'FL200DX Memory Mapped Address Space Registers' spec */

#define FL2000_VGA_CONTROL_OFFSET 0x8000

#define FL2000_VGA_STATUS_REG (FL2000_VGA_CONTROL_OFFSET + 0x00)
union fl2000_vga_status_reg {
	u32 val;
	struct {
		u32 vga_status	   : 1;
		u32 vga_error	   : 1; /* read self clear */
		u32 lbuf_halt	   : 1;
		u32 iso_ack	   : 1; /* read self clear */
		u32 td_drop	   : 1; /* read self clear */
		u32 irq_pending	   : 1; /* read self clear */
		u32 pll_status	   : 1;
		u32 dac_status	   : 1;
		u32 lbuf_overflow  : 1;
		u32 lbuf_underflow : 1;
		u32 frame_cnt	   : 16;
		u32 hdmi_event	   : 1; /* read self clear */
		u32 hdmi_status	   : 1;
		u32 edid_status	   : 1;
		u32 monitor_status : 1;
		u32 monitor_event  : 1; /* read self clear */
		u32 edid_event	   : 1; /* read self clear */
	} __aligned(4) __packed;
};

#define FL2000_VGA_CTRL_REG_PXCLK (FL2000_VGA_CONTROL_OFFSET + 0x04)
union fl2000_vga_cntrl_reg_pxclk {
	u32 val;
	struct {
		u32 clear_watermark	 : 1;
		u32 frame_sync		 : 1;
		u32 hsync_polarity	 : 1;
		u32 vsync_polarity	 : 1;
		u32 de_polarity		 : 1;
		u32 mirror_mode		 : 1;
		u32 vga565_mode		 : 1;
		u32 dac_output_en	 : 1;
		u32 vga_timing_en	 : 1;
		u32 use_new_pkt_retry	 : 1;
		u32 ref_select		 : 1;
		u32 dac_px_clk_invert	 : 1;
		u32 clear_lbuf_status	 : 1;
		u32 drop_cnt		 : 1;
		u32 use_vdi_itp_cnt	 : 1;
		u32 __reserved1		 : 1;
		u32 __reserved2		 : 8;
		u32 vga_compress	 : 1;
		u32 vga332_mode		 : 1;
		u32 vga_color_palette_en : 1;
		u32 vga_first_bt_enc_en	 : 1;
		u32 clear_125us_cnt	 : 1;
		u32 disable_halt	 : 1;
		u32 force_de_en		 : 1;
		u32 vga555_mode		 : 1;
	} __aligned(4) __packed;
};

#define FL2000_VGA_HSYNC_REG1 (FL2000_VGA_CONTROL_OFFSET + 0x08)
union fl2000_vga_hsync_reg1 {
	u32 val;
	struct {
		u32 htotal	: 12;
		u32 __reserved1 : 4;
		u32 hactive	: 12;
		u32 __reserved2 : 4;
	} __aligned(4) __packed;
};

#define FL2000_VGA_HSYNC_REG2 (FL2000_VGA_CONTROL_OFFSET + 0x0C)
union fl2000_vga_hsync_reg2 {
	u32 val;
	struct {
		u32 hstart	: 12;
		u32 __reserved1 : 4;
		u32 hsync_width : 8;
		u32 __reserved2 : 8;
	} __aligned(4) __packed;
};

#define FL2000_VGA_VSYNC_REG1 (FL2000_VGA_CONTROL_OFFSET + 0x10)
union fl2000_vga_vsync_reg1 {
	u32 val;
	struct {
		u32 vtotal	: 12;
		u32 __reserved1 : 4;
		u32 vactive	: 12;
		u32 __reserved2 : 4;
	} __aligned(4) __packed;
};

#define FL2000_VGA_VSYNC_REG2 (FL2000_VGA_CONTROL_OFFSET + 0x14)
union fl2000_vga_vsync_reg2 {
	u32 val;
	struct {
		u32 vstart	  : 12;
		u32 __reserved1	  : 4;
		u32 vsync_width	  : 3;
		u32 __reserved2	  : 1;
		u32 start_latency : 10;
		u32 __reserved3	  : 1;
		u32 buf_error_en  : 1;
	} __aligned(4) __packed;
};

#define FL2000_VGA_TEST_REG  (FL2000_VGA_CONTROL_OFFSET + 0x18)
#define FL2000_VGA_ISOCH_REG (FL2000_VGA_CONTROL_OFFSET + 0x1C)
union fl2000_vga_isoch_reg {
	u32 val;
	struct {
		u32 start_mframe_cnt   : 14;
		u32 use_mframe_match   : 1;
		u32 use_zero_len_frame : 1;
		u32 mframe_cnt	       : 14;
		u32 mframe_cnt_update  : 1;
		u32 __reserved	       : 1;
	} __aligned(4) __packed;
};

#define FL2000_VGA_I2C_SC_REG (FL2000_VGA_CONTROL_OFFSET + 0x20)
union fl2000_vga_i2c_sc_reg {
	u32 val;
	struct {
		u32 i2c_addr	   : 7;
		u32 i2c_cmd	   : 1;
		u32 i2c_offset	   : 8;
		u32 vga_status	   : 8;
		u32 i2c_status	   : 4;
		u32 monitor_detect : 1;
		u32 i2c_ready	   : 1;
		u32 edid_detect	   : 1;
		u32 i2c_done	   : 1;
	} __aligned(4) __packed;
};

#define FL2000_VGA_I2C_RD_REG (FL2000_VGA_CONTROL_OFFSET + 0x24)
#define FL2000_VGA_I2C_WR_REG (FL2000_VGA_CONTROL_OFFSET + 0x28)

#define FL2000_VGA_PLL_REG (FL2000_VGA_CONTROL_OFFSET + 0x2C)
union fl2000_vga_pll_reg {
	u32 val;
	struct {
		u32 divisor	    : 8;
		u32 prescaler	    : 2;
		u32 __reserved1	    : 3;
		u32 function	    : 2;
		u32 __reserved2	    : 1;
		u32 multiplier	    : 8;
		u32 test_io	    : 1;
		u32 cfg_dac_pwrdown : 1;
		u32 force_dac_pwrup : 1;
		u32 __reserved3	    : 5;
	} __aligned(4) __packed;
};

#define FL2000_VGA_LBUF_REG (FL2000_VGA_CONTROL_OFFSET + 0x30)
union fl2000_vga_lbuf_reg {
	u32 val;
	struct {
		u32 lbuf_watermark_assert_rdy : 15;
		u32 __reserved		      : 17;
	} __aligned(4) __packed;
};

#define FL2000_VGA_HI_MARK (FL2000_VGA_CONTROL_OFFSET + 0x34)
union fl2000_vga_hi_mark {
	u32 val;
	struct {
		u32 lbuf_high_watermark : 17;
		u32 __reserved		: 15;
	} __aligned(4) __packed;
};

#define FL2000_VGA_LO_MARK (FL2000_VGA_CONTROL_OFFSET + 0x38)
union fl2000_vga_lo_mark {
	u32 val;
	struct {
		u32 lbuf_low_watermark : 17;
		u32 __reserved	       : 15;
	} __aligned(4) __packed;
};

#define FL2000_VGA_CTRL_REG_ACLK (FL2000_VGA_CONTROL_OFFSET + 0x3C)
union fl2000_vga_ctrl_reg_aclk {
	u32 val;
	struct {
		u32 cfg_timing_reset_n	  : 1;
		u32 plh_block_en	  : 1;
		u32 edid_mon_int_en	  : 1;
		u32 ext_mon_int_en	  : 1;
		u32 vga_status_self_clear : 1;
		u32 pll_lock_time	  : 5;
		u32 pll_fast_timeout_en	  : 1;
		u32 ppe_block_em	  : 1;
		u32 pll_timer_en	  : 1;
		u32 feedback_int_en	  : 1;
		u32 clr_125us_counter	  : 1;
		u32 ccs_pd_dis		  : 1;
		u32 standby_en		  : 1;
		u32 force_loopback	  : 1;
		u32 lbuf_drop_frame_en	  : 1;
		u32 lbuf_vde_rst_en	  : 1;
		u32 lbuf_sw_rst		  : 1;
		u32 lbuf_err_int_en	  : 1;
		u32 biac_en		  : 1;
		u32 pxclk_in_en		  : 1;
		u32 vga_err_int_en	  : 1;
		u32 force_vga_connect	  : 1;
		u32 force_pll_up	  : 1;
		u32 use_zero_td		  : 1;
		u32 use_zero_pkt_len	  : 1;
		u32 use_pkt_pending	  : 1;
		u32 pll_dac_pd_usbp3_en	  : 1;
		u32 pll_dac_pd_novga_en	  : 1;
	} __aligned(4) __packed;
};

#define FL2000_VGA_PXCLK_CNT_REG (FL2000_VGA_CONTROL_OFFSET + 0x40)
union fl2000_vga_pxclk_cnt_reg_reg {
	u32 val;
	struct {
		u32 pix_clock_count : 28;
		u32 __reserved	    : 4;
	} __aligned(4) __packed;
};

#define FL2000_VGA_VCNT_REG (FL2000_VGA_CONTROL_OFFSET + 0x44)
union fl2000_vga_vcnt_reg {
	u32 val;
	struct {
		u32 max_aclk_count	: 15;
		u32 max_aclk_count_hit	: 1;
		u32 max_lbuf_accumulate : 16;
	} __aligned(4) __packed;
};

#define FL2000_RST_CTRL_REG (FL2000_VGA_CONTROL_OFFSET + 0x48)
union fl2000_rst_cntrl_reg {
	u32 val;
	struct {
		u32 dis_hot_rst2_port	: 1;
		u32 dis_warm_rst2_port	: 1;
		u32 dis_hot_reset_pipe	: 1;
		u32 dis_warm_reset_pipe : 1;
		u32 dis_hot_reset_pix	: 1;
		u32 dis_warm_reset_pix	: 1;
		u32 dis_usb2_reset_pix	: 1;
		u32 dis_pll_reset_pix	: 1;
		u32 dis_sw_reset_pix	: 1;
		u32 dis_usb2_reset_buf	: 1;
		u32 dis_sw_reset_buf	: 1;
		u32 dis_lbuf_reset_pix	: 1;
		u32 dis_hot_reset_port	: 1;
		u32 dis_warm_reset_port : 1;
		u32 set_slow_clk_predft : 1;
		u32 sw_reset		: 1;
		u32 frame_first_itp_wl	: 16;
	} __aligned(4) __packed;
};

#define FL2000_BIAC_CTRL1_REG (FL2000_VGA_CONTROL_OFFSET + 0x4C)
union fl2000_cfg_biac_ctrl1_reg {
	u32 val;
	struct {
		u32 cfg_biac_ctrl_lo8	: 8;
		u32 cfg_biac_frame_mult : 8;
		u32 cfg_biac_125us_mult : 16;
	} __aligned(4) __packed;
};

#define FL2000_BIAC_CTRL2_REG (FL2000_VGA_CONTROL_OFFSET + 0x50)
union fl2000_cfg_biac_ctrl2_reg {
	u32 val;
	struct {
		u32 cfg_biac_ctrl_hi16 : 16;
		u32 __reserved	       : 16;
	} __aligned(4) __packed;
};

#define FL2000_BIAC_STATUS_REG (FL2000_VGA_CONTROL_OFFSET + 0x54)
union fl2000_cfg_biac_status_reg {
	u32 val;
	struct {
		u32 current_status : 16;
		u32 current_value  : 16;
	} __aligned(4) __packed;
};

/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x58) */

#define FL2000_VGA_PLT_REG_PXCLK (FL2000_VGA_CONTROL_OFFSET + 0x5C)
union fl2000_vga_plt_reg_pxclk {
	u32 val;
	struct {
		u32 palette_ram_wr_data : 24;
		u32 __reserved		: 8;
	} __aligned(4) __packed;
};

#define FL2000_VGA_PLT_RADDR_REG_PXCLK (FL2000_VGA_CONTROL_OFFSET + 0x60)
union fl2000_vga_plt_rdaddr_reg_pxclk {
	u32 val;
	struct {
		u32 palette_ram_rd_addr	      : 8;
		u32 last_frame_lbuf_watermark : 16;
		u32 __reserved		      : 8;
	} __aligned(4) __packed;
};

#define FL2000_VGA_CTRL2_REG_ACLK (FL2000_VGA_CONTROL_OFFSET + 0x64)
union fl2000_vga_ctrl2_reg_axclk {
	u32 val;
	struct {
		u32 pll_powerdown_detect_en : 1;
		u32 mstor_blksize_ptr_width : 3;
		u32 mstor_blk_count	    : 12;
		u32 spi_wr_en		    : 1;
		u32 detect_pins_debounce_en : 1;
		u32 hdmi_int_en		    : 1;
		u32 hdmi_int_active_high    : 1;
		u32 spi_en		    : 1;
		u32 __reserved		    : 2;
		u32 sw_prod_rev		    : 8;
	} __aligned(4) __packed;
};

/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x68) */
/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x6C) */

#define FL2000_TEST_CNTL_REG1 (FL2000_VGA_CONTROL_OFFSET + 0x70)
#define FL2000_TEST_CNTL_REG2 (FL2000_VGA_CONTROL_OFFSET + 0x74)
#define FL2000_TEST_CNTL_REG3 (FL2000_VGA_CONTROL_OFFSET + 0x78)
#define FL2000_TEST_STAT1     (FL2000_VGA_CONTROL_OFFSET + 0x7C)
#define FL2000_TEST_STAT2     (FL2000_VGA_CONTROL_OFFSET + 0x80)
#define FL2000_TEST_STAT3     (FL2000_VGA_CONTROL_OFFSET + 0x84)

#define FL2000_VGA_CTRL_REG_3 (FL2000_VGA_CONTROL_OFFSET + 0x88)
union fl2000_vga_ctrl_reg_3 {
	u32 val;
	struct {
		u32 __reserved1	  : 10;
		u32 wakeup_clr_en : 1;
		u32 __reserved2	  : 21;
	} __aligned(4) __packed;
};

/* undefined				(FL2000_VGA_CONTROL_OFFSET + 0x8C) */

/* List of registers that shall not be read automatically (e.g. for cache update due to presence of
 * self-clear bits
 */
static inline bool FL2000_REG_PRECIOUS(u32 reg)
{
	if (reg == FL2000_VGA_STATUS_REG)
		return true;
	else
		return false;
}

/* List of volatile registers that shall not be cached */
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
	case FL2000_VGA_PLT_RADDR_REG_PXCLK:
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
