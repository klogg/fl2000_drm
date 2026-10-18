// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/usb.h>
#include "fl2000_registers.h"
#include <linux/bitfield.h>

#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10
/* Vendor detection restart sequence waits for EDID detection to settle. */
#define FL2000_MONITOR_DETECT_MDELAY 750

/**
 * fl2000_reg_precious() - Exclude destructive status from incidental regmap reads
 * @dev: Regmap device argument; unused.
 * @reg: Register offset.
 *
 * Context: Any context. Does not sleep.
 * Return: True for the controller status register.
 */
static bool fl2000_reg_precious(struct device *dev __always_unused, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

/**
 * fl2000_reg_volatile() - Select registers that must bypass the cache
 * @dev: Regmap device argument; unused.
 * @reg: Register offset.
 *
 * Context: Any context. Does not sleep.
 * Return: True for volatile registers, false for cacheable controls.
 */
static bool fl2000_reg_volatile(struct device *dev __always_unused, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

/**
 * fl2000_reg_read() - Read an exact little-endian USB register word
 * @context: USB parent used for the vendor request.
 * @reg: Register offset.
 * @val: CPU-endian output; unchanged on failure.
 *
 * USB helpers own DMA-safe staging buffers, so the local word may be on stack.
 *
 * Context: Process context. Regmap holds its mutex.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct usb_device *usb_dev = context;
	__le32 data;
	int ret;

	ret = usb_control_msg_recv(usb_dev, 0, CONTROL_MSG_READ, USB_DIR_IN | USB_TYPE_VENDOR, 0,
				   (u16)reg, &data, sizeof(data), USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (!ret)
		*val = le32_to_cpu(data);
	return ret;
}

/**
 * fl2000_reg_write() - Write a little-endian USB register word
 * @context: USB parent used for the vendor request.
 * @reg: Register offset.
 * @val: CPU-endian register value.
 *
 * Context: Process context. Regmap holds its mutex.
 * Return: 0 on success or a negative error code.
 */
static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct usb_device *usb_dev = context;
	__le32 data = cpu_to_le32(val);

	return usb_control_msg_send(usb_dev, 0, CONTROL_MSG_WRITE, USB_DIR_OUT | USB_TYPE_VENDOR, 0,
				    (u16)reg, &data, sizeof(data), USB_CTRL_SET_TIMEOUT,
				    GFP_KERNEL);
}

/**
 * var fl2000_regmap_config - Controller caching and vendor USB register access
 *
 * Observed values differ from the manual, and soft reset preserves registers.
 * Do not seed the cache with documented reset defaults.
 */
static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = FL2000_MAX_REGISTER,

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

/**
 * fl2000_set_pll() - Program the PLL and clear forced connection
 * @usb_dev: USB parent whose interface resources remain alive.
 * @pll: Validated register field encodings from the PLL search.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_set_pll(struct usb_device *usb_dev, const struct fl2000_pll *pll)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 value;
	int ret;

	value = FIELD_PREP(FL2000_VGA_PLL_PRESCALER, pll->prescaler) |
		FIELD_PREP(FL2000_VGA_PLL_MULTIPLIER, pll->multiplier) |
		FIELD_PREP(FL2000_VGA_PLL_DIVISOR, pll->divisor) |
		FIELD_PREP(FL2000_VGA_PLL_FUNCTION, pll->function);
	ret = regmap_write(regmap, FL2000_VGA_PLL_REG, value);
	if (ret)
		return ret;

	/* Let monitor detection report unplugging instead of forcing connected. */
	return regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK,
				 FL2000_VGA_CTRL_ACLK_FORCE_PLL_UP |
					 FL2000_VGA_CTRL_ACLK_FORCE_VGA_CONNECT,
				 FL2000_VGA_CTRL_ACLK_FORCE_PLL_UP);
}

/**
 * fl2000_set_timings() - Program adjusted display timing fields
 * @usb_dev: USB parent whose interface resources remain alive.
 * @timings: Validated controller encodings and sync polarities.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_set_timings(struct usb_device *usb_dev, const struct fl2000_timings *timings)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 value;
	u32 mask = FL2000_VGA_CTRL_PXCLK_HSYNC_POLARITY | FL2000_VGA_CTRL_PXCLK_VSYNC_POLARITY;
	int ret;

	value = FIELD_PREP(FL2000_VGA_HSYNC1_HACTIVE, timings->hactive) |
		FIELD_PREP(FL2000_VGA_HSYNC1_HTOTAL, timings->htotal);
	ret = regmap_write(regmap, FL2000_VGA_HSYNC_REG1, value);
	if (ret)
		return ret;

	value = FIELD_PREP(FL2000_VGA_HSYNC2_HSYNC_WIDTH, timings->hsync_width) |
		FIELD_PREP(FL2000_VGA_HSYNC2_HSTART, timings->hstart);
	ret = regmap_write(regmap, FL2000_VGA_HSYNC_REG2, value);
	if (ret)
		return ret;

	value = FIELD_PREP(FL2000_VGA_VSYNC1_VACTIVE, timings->vactive) |
		FIELD_PREP(FL2000_VGA_VSYNC1_VTOTAL, timings->vtotal);
	ret = regmap_write(regmap, FL2000_VGA_VSYNC_REG1, value);
	if (ret)
		return ret;

	value = FIELD_PREP(FL2000_VGA_VSYNC2_VSYNC_WIDTH, timings->vsync_width) |
		FIELD_PREP(FL2000_VGA_VSYNC2_VSTART, timings->vstart) |
		FIELD_PREP(FL2000_VGA_VSYNC2_START_LATENCY,
			   min_t(u32, timings->vstart, FIELD_MAX(FL2000_VGA_VSYNC2_START_LATENCY)));
	ret = regmap_write(regmap, FL2000_VGA_VSYNC_REG2, value);
	if (ret)
		return ret;

	value = FIELD_PREP(FL2000_VGA_CTRL_PXCLK_HSYNC_POLARITY, timings->hsync_positive) |
		FIELD_PREP(FL2000_VGA_CTRL_PXCLK_VSYNC_POLARITY, timings->vsync_positive);
	return regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, value);
}

/**
 * fl2000_set_pixfmt() - Select the wire pixel representation
 * @usb_dev: USB parent whose interface resources remain alive.
 * @bytes_pix: Validated wire pixel size: 2 for RGB565 or 3 for RGB888.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask = FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN | FL2000_VGA_CTRL_PXCLK_DROP_CNT |
		   FL2000_VGA_CTRL_PXCLK_VGA565_MODE | FL2000_VGA_CTRL_PXCLK_VGA332_MODE |
		   FL2000_VGA_CTRL_PXCLK_VGA555_MODE | FL2000_VGA_CTRL_PXCLK_VGA_COMPRESS |
		   FL2000_VGA_CTRL_PXCLK_CLEAR_WATERMARK;
	u32 value = FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN | FL2000_VGA_CTRL_PXCLK_CLEAR_WATERMARK;

	if (bytes_pix == 2)
		value |= FL2000_VGA_CTRL_PXCLK_VGA565_MODE;
	return regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, value);
}

/**
 * fl2000_set_transfers() - Configure bulk frame-boundary signaling
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_set_transfers(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask = FL2000_VGA_CTRL_ACLK_USE_PKT_PENDING | FL2000_VGA_CTRL_ACLK_USE_ZERO_TD |
		   FL2000_VGA_CTRL_ACLK_USE_ZERO_PKT_LEN | FL2000_VGA_CTRL_ACLK_VGA_ERR_INT_EN;
	int ret;

	ret = regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask,
				FL2000_VGA_CTRL_ACLK_USE_ZERO_PKT_LEN |
					FL2000_VGA_CTRL_ACLK_VGA_ERR_INT_EN);
	if (ret)
		return ret;

	return regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, FL2000_VGA_ISOCH_MFRAME_CNT, 0);
}

/**
 * fl2000_reset() - Reset the controller and invalidate cached registers
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Takes the adapter bus lock when registered, then accesses the controller
 * regmap. This excludes complete I2C transfers, not multi-transfer bridge DDC
 * sequences. Call only before adapter registration or during system resume,
 * after display, polling and IRQ work have been quiesced by suspend and before
 * child devices and userspace resume. Do not call from runtime modesets or
 * bridge I2C operations, or wait for IRQ work while holding the bus lock.
 *
 * Context: Process context. Caller must not already hold the adapter bus
 *          lock.
 * Return: 0 on success or a negative error code.
 */
int fl2000_reset(struct usb_device *usb_dev)
{
	struct i2c_adapter *adapter = fl2000_i2c_adapter(usb_dev);
	int ret;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	/* Initial probe has no adapter or concurrent bridge traffic. */
	if (adapter)
		i2c_lock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);

	ret = regmap_write_bits(regmap, FL2000_RST_CTRL_REG, FL2000_RST_CTRL_SW_RESET,
				FL2000_RST_CTRL_SW_RESET);
	if (ret)
		goto unlock;

	msleep(FL2000_HW_RST_MDELAY);
	regcache_drop_region(regmap, 0, FL2000_MAX_REGISTER);

	ret = 0;
unlock:
	if (adapter)
		i2c_unlock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);
	return ret;
}

/**
 * fl2000_afe_magic() - Apply the vendor analog-front-end quirk
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_afe_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	return regmap_write_bits(regmap, FL2000_USB_LPM_REG, FL2000_USB_LPM_VENDOR_QUIRK,
				 FL2000_USB_LPM_VENDOR_QUIRK);
}

/**
 * fl2000_usb_magic() - Restart detection after enabling the PLL
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Enables EDID detection, waits 750 ms, then enables external-monitor
 * detection and applies USB power-management setup. Serializes changes to
 * the shared transaction-engine register with the adapter bus lock. As with
 * fl2000_reset(), callers must ensure complete bridge sequences are quiescent.
 *
 * Context: Process context. PLL must run; caller must not hold the adapter
 *          bus lock.
 * Return: 0 on success or a negative error code.
 */
int fl2000_usb_magic(struct usb_device *usb_dev)
{
	struct i2c_adapter *adapter = fl2000_i2c_adapter(usb_dev);
	int ret;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask;

	/* Detection setup shares the transaction engine's control register. */
	if (adapter)
		i2c_lock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);

	/* Restart detection after the PLL is running (register manual, page 25).
	 * The vendor enables EDID detection first and gives it 750 ms to settle.
	 */
	mask = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT;
	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, 0);
	if (ret)
		goto unlock;
	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, FL2000_VGA_I2C_SC_EDID_DETECT);
	if (ret)
		goto unlock;
	msleep(FL2000_MONITOR_DETECT_MDELAY);
	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, mask);
	if (ret)
		goto unlock;

	ret = regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, FL2000_VGA_CTRL3_WAKEUP_CLR_EN, 0);
	if (ret)
		goto unlock;

	mask = FL2000_USB_LPM_U1_REJECT | FL2000_USB_LPM_U2_REJECT;
	ret = regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, mask);
	if (ret)
		goto unlock;

	ret = regmap_write_bits(regmap, FL2000_USB_CTRL_REG, FL2000_USB_CTRL_WAKE_NRDY, 0);
	if (ret)
		goto unlock;

	ret = 0;
unlock:
	if (adapter)
		i2c_unlock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);
	return ret;
}

/**
 * fl2000_enable_interrupts() - Program controller event enables and polarity
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_enable_interrupts(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 value = FL2000_VGA_CTRL_ACLK_VGA_ERR_INT_EN | FL2000_VGA_CTRL_ACLK_LBUF_ERR_INT_EN |
		    FL2000_VGA_CTRL_ACLK_EXT_MON_INT_EN | FL2000_VGA_CTRL_ACLK_EDID_MON_INT_EN;
	u32 mask = value | FL2000_VGA_CTRL_ACLK_VGA_STATUS_SELF_CLEAR |
		   FL2000_VGA_CTRL_ACLK_FEEDBACK_INT_EN;
	int ret;

	/* Explicitly configure both detection IRQs and our acknowledgment mode.
	 * On tested FL2000 hardware bit 4 set inhibits read-to-clear, contrary
	 * to the register manual. Keep it clear so pending events are acknowledged.
	 */
	ret = regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, value);
	if (ret)
		return ret;

	return regmap_write_bits(regmap, FL2000_VGA_CTRL2_REG_ACLK,
				 FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN |
					 FL2000_VGA_CTRL2_ACLK_HDMI_INT_ACTIVE_HIGH,
				 FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN);
}

/**
 * fl2000_check_interrupt() - Consume controller status and acknowledge LBUF flags
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * The status read clears event bits on the tested configuration. A retry
 * may therefore need to dispatch the bridge without another event bit. This
 * does not implement recovery from LBUF halt or VGA errors.
 *
 * Context: Process context. May sleep.
 * Return: 1 for a sink event, 0 otherwise, or a negative register-access
 *         error.
 */
int fl2000_check_interrupt(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 status;
	u32 errors;
	bool sink_event;
	int ret;

	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status);
	if (ret)
		return ret;

	sink_event = status & (FL2000_VGA_STATUS_HDMI_EVENT | FL2000_VGA_STATUS_MONITOR_EVENT |
			       FL2000_VGA_STATUS_EDID_EVENT);

	/* Acknowledge recoverable LBUF errors without rereading self-clear events. */
	errors = status & (FL2000_VGA_STATUS_LBUF_OVERFLOW | FL2000_VGA_STATUS_LBUF_UNDERFLOW);
	if (errors) {
		ret = regmap_write(regmap, FL2000_VGA_STATUS_REG, errors);
		if (ret)
			return ret;
	}

	/* LBUF halt and VGA error recovery are not implemented. */
	return sink_event ? 1 : 0;
}

/**
 * fl2000_i2c_dword() - Execute one aligned controller I2C command
 * @usb_dev: USB parent whose interface resources remain alive.
 * @read: True to read, false to write.
 * @addr: Seven-bit I2C slave address.
 * @offset: Four-byte-aligned register offset.
 * @data: CPU-endian word to write or destination for a successful read.
 *
 * The controller transfers four bytes, including adjacent registers with
 * read side effects. This function does not acquire the adapter bus lock;
 * normal calls come from the serialized I2C adapter transfer callback.
 *
 * Context: Process context. Caller serializes the complete controller
 *          command.
 * Return: 0 on success or a negative error code.
 */
int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 control;
	u32 status;
	u32 mask = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT |
		   FL2000_VGA_I2C_SC_I2C_STATUS | FL2000_VGA_I2C_SC_I2C_ADDR |
		   FL2000_VGA_I2C_SC_I2C_CMD | FL2000_VGA_I2C_SC_I2C_OFFSET |
		   FL2000_VGA_I2C_SC_I2C_DONE;
	int ret;

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret)
			return ret;
	}

	/* Detection enable reads back clear, so restore it for each command. */
	control = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT |
		  FIELD_PREP(FL2000_VGA_I2C_SC_I2C_ADDR, addr) |
		  FIELD_PREP(FL2000_VGA_I2C_SC_I2C_CMD, read) |
		  FIELD_PREP(FL2000_VGA_I2C_SC_I2C_OFFSET, offset);
	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, control);
	if (ret)
		return ret;

	status = control;
	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, status,
				       status & FL2000_VGA_I2C_SC_I2C_DONE, I2C_RDWR_INTERVAL,
				       I2C_RDWR_TIMEOUT);
	if (ret || FIELD_GET(FL2000_VGA_I2C_SC_I2C_STATUS, status)) {
		dev_dbg(&usb_dev->dev, "I2C %s addr=%02x reg=%02x status=%08x poll=%d\n",
			read ? "read" : "write", addr, offset, status, ret);
		return ret ? ret : -EIO;
	}

	if (read)
		return regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
	return 0;
}

/**
 * fl2000_regmap_init() - Create the USB-backed controller regmap
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative error code.
 */
int fl2000_regmap_init(struct usb_device *usb_dev)
{
	const struct regmap *regmap;

	regmap = regmap_init(&usb_dev->dev, NULL, usb_dev, &fl2000_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR_OR_ZERO(regmap);

	return 0;
}

/**
 * fl2000_regmap_cleanup() - Release the controller regmap
 * @usb_dev: USB parent whose interface resources remain alive.
 *
 * All I2C clients, IRQ work and streaming/display register users must be
 * quiesced first. Does nothing if the regmap is absent.
 *
 * Context: Process context. Caller serializes adapter teardown.
 */
void fl2000_regmap_cleanup(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	if (regmap)
		regmap_exit(regmap);
}
