// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

/* The controller transfers aligned four-byte register groups. Byte writes
 * require read-modify-write; full groups avoid destructive side-effect reads.
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

static bool fl2000_i2c_xfer_valid(const struct i2c_msg *msgs, int num)
{
	bool read;

	/* DMA safety describes the buffer and does not change the wire protocol. */
	if (!msgs || (num != 1 && num != 2) || !msgs[0].buf || (msgs[0].flags & ~I2C_M_DMA_SAFE) ||
	    msgs[0].addr > 0x7f)
		return false;
	read = num == 2;
	if (read) {
		if (msgs[0].len != 1 || !msgs[1].buf || (msgs[1].len != 1 && msgs[1].len != 4) ||
		    (msgs[1].flags & ~I2C_M_DMA_SAFE) != I2C_M_RD || msgs[1].addr != msgs[0].addr)
			return false;
	} else if (msgs[0].len != 2 && msgs[0].len != 5) {
		return false;
	}
	return true;
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

	if (!fl2000_i2c_xfer_valid(msgs, num))
		return -EOPNOTSUPP;
	read = num == 2;
	addr = msgs[0].addr;
	offset = msgs[0].buf[0] & I2C_XFER_ADDR_MASK;
	idx = msgs[0].buf[0] - offset;

	if ((read ? msgs[1].len == 4 : msgs[0].len == 5) && idx)
		return -EOPNOTSUPP;

	if (read) {
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data.w);
		if (ret)
			return ret;

		for (unsigned int i = 0; i < msgs[1].len; i++)
			msgs[1].buf[i] = (data.w >> ((idx + i) * 8)) & 0xff;
	} else {
		if (msgs[0].len == 5) {
			data.w = 0;
			for (unsigned int i = 0; i < 4; i++)
				data.w |= (u32)msgs[0].buf[i + 1] << (i * 8);
		} else {
			ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data.w);
			if (ret)
				return ret;
			data.w = (data.w & ~(0xffU << (idx * 8))) |
				 ((u32)msgs[0].buf[1] << (idx * 8));
		}

		ret = fl2000_i2c_write_dword(adapter->algo_data, addr, offset, &data.w);
		if (ret)
			return ret;
	}

	return num;
}

static u32 fl2000_i2c_func(struct i2c_adapter *adap __always_unused)
{
	return I2C_FUNC_I2C;
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
	.max_write_len = 5,
	.max_read_len = 4,
	.max_comb_1st_msg_len = I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len = 4,
};

static void fl2000_i2c_adapter_release(struct device *dev, void *res)
{
	struct i2c_adapter *adapter = res;

	dev_info(dev, "Releasing I2C adapter");
	i2c_del_adapter(adapter);
}

/* Callers must keep control-interface resources alive while using the adapter. */
struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	return device->i2c;
}

/**
 * fl2000_i2c_verify_client - Verify I2C client
 *
 * @usb_dev: USB device
 * @client_dev: Device to verify
 * @client_name: Expected I2C client name
 *
 * @return: true if client is verified, false otherwise
 */
bool fl2000_i2c_verify_client(struct usb_device *usb_dev, struct device *client_dev,
			      const char *client_name)
{
	const struct i2c_adapter *adapter;
	const struct i2c_client *client;
	const struct device_driver *driver;

	adapter = fl2000_i2c_adapter(usb_dev);
	if (!adapter)
		return false;

	client = i2c_verify_client(client_dev);
	if (!client)
		return false;

	driver = client_dev->driver;
	if (!driver || strcmp(driver->name, client_name))
		return false;

	if (client->adapter != adapter)
		return false;

	return true;
}

static int fl2000_bridge_detect(struct i2c_adapter *adapter, unsigned short addr)
{
	/* i2c_new_scanned_device expects 1 for a match, 0 otherwise. */
	u8 bytes[4];
	int ret;

	for (unsigned int i = 0; i < sizeof(bytes); i++) {
		u8 reg = (u8)i;
		struct i2c_msg msgs[] = {
			{ .addr = addr, .len = 1, .buf = &reg },
			{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &bytes[i] },
		};

		ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
		if (ret != ARRAY_SIZE(msgs))
			return 0;
	}
	return bytes[0] == 0x54 && bytes[1] == 0x49 && bytes[2] == 0x12 &&
	       (bytes[3] & 0x0f) == 0x06;
}

int fl2000_i2c_init(struct usb_device *usb_dev)
{
	static const unsigned short addresses[] = { 0x4c, 0x4d, I2C_CLIENT_END };
	struct i2c_board_info info = { I2C_BOARD_INFO("it66121", 0) };
	struct i2c_client *client;
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	int ret;
	struct i2c_adapter *adapter;

	/* Adapter must be allocated before anything else */
	adapter = devres_alloc(fl2000_i2c_adapter_release, sizeof(*adapter), GFP_KERNEL);
	if (!adapter) {
		dev_err(&usb_dev->dev, "Cannot allocate adapter!");
		return -ENOMEM;
	}

	adapter->owner = THIS_MODULE;
	adapter->class = I2C_CLASS_DEPRECATED;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;
	adapter->algo_data = usb_dev;
	adapter->dev.parent = &usb_dev->dev;
	strscpy(adapter->name, "FL2000 bridge I2C bus", sizeof(adapter->name));

	ret = i2c_add_adapter(adapter);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot add adapter! (%d)", ret);
		devres_free(adapter);
		return ret;
	}

	devres_add(&usb_dev->dev, adapter);
	device->i2c = adapter;
	client = i2c_new_scanned_device(adapter, &info, addresses, fl2000_bridge_detect);
	if (IS_ERR(client)) {
		ret = PTR_ERR_OR_ZERO(client);
		devres_release(&usb_dev->dev, fl2000_i2c_adapter_release, NULL, NULL);
		device->i2c = NULL;
		return ret;
	}
	return 0;
}

void fl2000_i2c_cleanup(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	devres_release(&usb_dev->dev, fl2000_i2c_adapter_release, NULL, NULL);
	device->i2c = NULL;
}
