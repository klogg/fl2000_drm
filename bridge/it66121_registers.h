/* SPDX-License-Identifier: GPL-2.0 */
/*
 * it66121_registers.h
 *
 * Based on the un-official documentation found on the net and registers
 * description from source code:
 * 	- FL2000DX Linux driver on GitHub
 * 	- RK3188 Android driver on GitHub
 *
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef __FL2000_REGISTERS_H__
#define __FL2000_REGISTERS_H__

#define IT66121_BANK_SIZE		0x100

/* 000 - 02F Common registers, same as 100 - 12F */
#define IT66121_VENDOR_ID_1		0x00
#define IT66121_VENDOR_ID_2		0x01
#define IT66121_DEVICE_ID_1		0x02
#define IT66121_DEVICE_ID_2		0x03
#define IT66121_SW_RESET		0x04
#define IT66121_INT_CONTROL		0x05
#define IT66121_INT_STATUS_1		0x06
typedef union {
	u32 val;
	struct {
		u32 __unused:24;
		u32 aud_overflow:1;
		u32 romacq_noack:1;
		u32 ddc_noack:1;
		u32 ddc_fifo_err:1;
		u32 romacq_bus_hang:1;
		u32 ddc_bus_hang:1;
		u32 rx_sense:1;
		u32 hpd_plug:1;
	} __attribute__ ((aligned, packed));
} it666121_int_status_1_reg;

#define IT66121_INT_STATUS_2		0x07
#define IT66121_INT_STATUS_3		0x08
#define IT66121_INT_MASK_1		0x09
#define IT66121_INT_MASK_2		0x0A
#define IT66121_INT_MASK_3		0x0B
#define IT66121_INT_CLEAR_1		0x0C
#define IT66121_INT_CLEAR_2		0x0D
#define IT66121_SYS_STATUS		0x0E
static const struct reg_field IT66121_SYS_STATUS_int =
		REG_FIELD(IT66121_SYS_STATUS, 7, 7);
static const struct reg_field IT66121_SYS_STATUS_hpd =
		REG_FIELD(IT66121_SYS_STATUS, 6, 6);

#define IT66121_SYS_CONTROL		0x0F

#define IT66121_DDC_CONTROL		0x10
#define IT66121_DDC_MASTER_ROM		(1<<1)
#define IT66121_DDC_MASTER_DDC		(0<<1)
#define IT66121_DDC_MASTER_HOST		(1<<0)
#define IT66121_DDC_MASTER_HDCP		(0<<0)

#define IT66121_DDC_ADDRESS		0x11
#define IT66121_DDC_OFFSET		0x12
#define IT66121_DDC_SIZE		0x13
#define IT66121_DDC_SEGMENT		0x14
#define IT66121_DDC_COMMAND		0x15
#define IT66121_DDC_STATUS		0x16
static const struct reg_field IT66121_DDC_STATUS_done =
		REG_FIELD(IT66121_DDC_STATUS, 7, 7);
static const struct reg_field IT66121_DDC_STATUS_error =
		REG_FIELD(IT66121_DDC_STATUS, 3, 5);

#define IT66121_DDC_RD_FIFO		0x17
/* reserved */
#define IT66121_HDCP_ADDRESS		0x19
/* reserved */
#define IT66121_DDC_BUS_HOLD_TIME	0x1B
#define IT66121_ROM_STATUS		0x1C
/* reserved */
/* reserved */
/* 01F - 2F HDCP registers, ignored */

#define IT66121_BANK_START		0x30

/* 030 - 0FF Bank 0 */
/* 030 - 057 HDCP registers, ignored */
#define IT66121_MCLK_CONTROL		0x058
#define IT66121_PLL_CONTROL		0x059
#define IT66121_CLK_POWER_CONTROL	0x05A
#define IT66121_OS_FREQ_NUM_2		0x05B
#define IT66121_OS_FREQ_NUM_1		0x05C
/* reserved */
#define IT66121_TX_CLK_COUNT		0x05E
#define IT66121_PLL_LOCK_STATUS		0x05F
#define IT66121_AUDIO_FREQ_COUNT	0x060
#define IT66121_AFE_DRV_CONTROL		0x061
#define IT66121_AFE_XP_CONTROL		0x062
#define IT66121_AFE_IP_CONTROL_2	0x063
#define IT66121_AFE_IP_CONTROL_1	0x064
#define IT66121_AFE_RING_CONTROL	0x065
#define IT66121_AFE_DRV_HS		0x066
/* reserved */
#define IT66121_AFE_IP_CONTROL_3	0x068
#define IT66121_AFE_PAT_RSTB		0x069
#define IT66121_AFE_XP_TEST		0x06A
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* reserved */
#define IT66121_INPUT_MODE		0x070
#define IT66121_INPUT_IO_CONTROL	0x071
#define IT66121_INPUT_COLOR_CONV	0x072
#define IT66121_Y_BLANK_LEVEL		0x073
#define IT66121_C_BLANK_LEVEL		0x074
#define IT66121_RGB_BLANK_LEVEL		0x075
#define IT66121_MATRIX_11V_1		0x076
#define IT66121_MATRIX_11V_2		0x077
#define IT66121_MATRIX_12V_1		0x078
#define IT66121_MATRIX_12V_2		0x079
#define IT66121_MATRIX_13V_1		0x07A
#define IT66121_MATRIX_13V_2		0x07B
#define IT66121_MATRIX_21V_1		0x07C
#define IT66121_MATRIX_21V_2		0x07D
#define IT66121_MATRIX_22V_1		0x07E
#define IT66121_MATRIX_22V_2		0x07F
#define IT66121_MATRIX_23V_1		0x080
#define IT66121_MATRIX_23V_2		0x081
#define IT66121_MATRIX_31V_1		0x082
#define IT66121_MATRIX_31V_2		0x083
#define IT66121_MATRIX_32V_1		0x084
#define IT66121_MATRIX_32V_2		0x085
#define IT66121_MATRIX_33V_1		0x086
#define IT66121_MATRIX_33V_2		0x087
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* reserved */
#define IT66121_PCI2C_CEC_ADDRESS	0x08D
/* reserved */
/* reserved */
/* 090 - 0BF Pattern generation registers, ignored */
#define IT66121_HDMI_MODE		0x0C0
#define IT66121_HDMI_AV_MUTE		0x0C1
#define IT66121_HDMI_BLACK_SRC		0x0C2
#define IT66121_HDMI_OESS_PREIOD	0x0C3
/* reserved */
#define IT66121_HDMI_AUDIO_CTS		0x0C5
#define IT66121_HDMI_GEN_CTRL_PACKET	0x0C6
/* reserved */
/* reserved */
#define IT66121_HDMI_NULL_PACKET	0x0C9
#define IT66121_HDMI_ACP_PACKET		0x0CA
/* reserved */
/* reserved */
#define IT66121_HDMI_AVI_INFO_PACKET	0x0CD
#define IT66121_HDMI_AUDIO_INFO_PACKET	0x0CE
/* reserved */
#define IT66121_HDMI_MPEG_INFO_PACKET	0x0D0
#define IT66121_HDMI_VIDEO_PARAM_STATUS	0x0D1
#define IT66121_HDMI_3D_INFO_PACKET	0x0D2
/* reserved */
/* reserved */
/* reserved */
/* reserved */
#define IT66121_HDMI_PCLK_CONTROL	0x0D7
#define IT66121_HDMI_PCLK_COUNT		0x0D8
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* reserved */
/* E0 - E7 Audio channel registers, ignored */
#define IT66121_EXT_INT_CONTROL		0x0E8
/* reserved */
/* reserved */
/* reserved */
#define IT66121_EXT_INT_MASK		0x0EC
/* reserved */
#define IT66121_EXT_INT_STATUS_1	0x0EE
/* reserved */
#define IT66121_EXT_INT_STATUS_2	0x0F0
/* reserved */
/* reserved */
/* 0F3 - 0F8 Test registers, ignored */
/* 0F9 - 0FF undefined*/

/* 130 - 1BF Bank 1 */
/* 130 - 18F HDMI packet content registers, ignored */
/* 190 - 1BF Audio channel status registers, ignored */

#define IT66121_BANK_END		0x1FF

/* CEC registers ignored (must be configured as a separate regmap / device) */

#endif /* __FL2000_REGISTERS_H__ */
