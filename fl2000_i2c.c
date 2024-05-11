// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

/* I2C controller require mandatory 8-bit (1 bite) sub-address provided for any read/write
 * operation. Each read or write operate with 8-bit (1-byte) data. Every exchange shall consist of 2
 * messages (sub-address + data) combined. USB xfer always bounds address to 4-byte boundary
 */
#define I2C_CMESSAGES_NUM  2
#define I2C_REG_ADDR_SIZE  (sizeof(u8))
#define I2C_REG_DATA_SIZE  (sizeof(u8))
#define I2C_XFER_ADDR_MASK (~0x3ul)

static inline int fl2000_i2c_read_dword(struct usb_device *usb_dev, u16 addr, u8 offset, u32 *data)
{
	return fl2000_i2c_dword(usb_dev, true, addr, offset, data);
}

static inline int fl2000_i2c_write_dword(struct usb_device *usb_dev, u16 addr, u8 offset, u32 *data)
{
	return fl2000_i2c_dword(usb_dev, false, addr, offset, data);
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	int ret;
	bool read;
	u16 addr;
	u8 idx;
	u8 offset;
	union {
		u32 w;
		u8 b[4];
	} data;

	addr = msgs[0].addr;
	offset = msgs[0].buf[0] & I2C_XFER_ADDR_MASK;
	idx = msgs[0].buf[0] - offset;

	/* We expect following:
	 * - 2 messages, each 1 byte, first write than read
	 * - 1 message, 2 bytes, write
	 */
	if (num == 2) {
		read = true;
	} else if (num == 1) {
		if (msgs[0].len == 2 && !(msgs[0].flags & I2C_M_RD))
			read = false;
		else
			return -ENOTSUPP;
	} else {
		return -ENOTSUPP;
	}

	/* Somehow the original FL2000 driver forces offset to be bound to 4-byte margin. This is
	 * really strange because i2c operation shall not depend on i2c margin, unless the HW design
	 * is completely crippled. Oh, yes, it is crippled :(
	 */
	if (read) {
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data.w);
		if (ret)
			return ret;

		msgs[1].buf[0] = data.b[idx];
	} else {
		/* Since FL2000 i2c bus implementation always operates with 4-byte messages, we need
		 * to read before write in order not to corrupt unrelated registers in case if we do
		 * not write whole dword
		 */
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data.w);
		if (ret)
			return ret;

		data.b[idx] = msgs[0].buf[1];

		ret = fl2000_i2c_write_dword(adapter->algo_data, addr, offset, &data.w);
		if (ret)
			return ret;
	}

	return num;
}

static u32 fl2000_i2c_func(struct i2c_adapter *adap)
{
	UNUSED(adap);
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART;
}

static const struct i2c_algorithm fl2000_i2c_algorithm = {
	.master_xfer = fl2000_i2c_xfer,
	.functionality = fl2000_i2c_func,
};

static const struct i2c_adapter_quirks fl2000_i2c_quirks = {
	.flags = I2C_AQ_COMB | /* enforce "combined" mode */
		 I2C_AQ_COMB_WRITE_FIRST | /* address write goes first */
		 I2C_AQ_COMB_SAME_ADDR, /* both are on the same address */
	.max_num_msgs = I2C_CMESSAGES_NUM,
	.max_write_len = 2 * I2C_REG_DATA_SIZE,
	.max_read_len = I2C_REG_DATA_SIZE,
	.max_comb_1st_msg_len = I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len = I2C_REG_DATA_SIZE,
};

static void fl2000_i2c_adapter_release(struct device *dev, void *res)
{
	struct i2c_adapter *adapter = res;

	dev_info(dev, "Releasing I2C adapter");
	i2c_del_adapter(adapter);
}

struct i2c_adapter *fl2000_i2c_init(struct usb_device *usb_dev)
{
	int ret;
	struct i2c_adapter *adapter;
	u8 usb_path[32];

	/* Adapter must be allocated before anything else */
	adapter = devres_alloc(fl2000_i2c_adapter_release, sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return ERR_PTR(-ENOMEM);
	devres_add(&usb_dev->dev, adapter);

	adapter->owner = THIS_MODULE;
	adapter->class = I2C_CLASS_DEPRECATED;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;
	adapter->algo_data = usb_dev;
	adapter->dev.parent = &usb_dev->dev;
	strscpy(adapter->name, "FL2000 bridge I2C bus", sizeof(adapter->name));

	ret = i2c_add_adapter(adapter);
	if (ret) {
		devres_free(adapter);
		return ERR_PTR(ret);
	}

	usb_make_path(usb_dev, usb_path, sizeof(usb_path));
	dev_dbg(&usb_dev->dev, "Created FL2000 bridge I2C bus %d at interface %s",
		i2c_adapter_id(adapter), usb_path);

	return adapter;
}
