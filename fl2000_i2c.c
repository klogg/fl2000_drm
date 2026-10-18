// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/property.h>

/* The controller transfers aligned four-byte register groups. Byte writes
 * require read-modify-write; full groups avoid destructive side-effect reads.
 */
#define I2C_CMESSAGES_NUM  2
#define I2C_REG_ADDR_SIZE  (sizeof(u8))
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

static int fl2000_i2c_write_value(const struct i2c_adapter *adapter, const struct i2c_msg *msg,
				  u16 addr, u8 offset, u8 idx, u32 *data)
{
	int ret;

	if (msg->len == 5) {
		*data = 0;
		for (unsigned int i = 0; i < 4; i++)
			*data |= (u32)msg->buf[i + 1] << (i * 8);
		return 0;
	}
	ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, data);
	if (ret)
		return ret;
	*data = (*data & ~(0xffU << (idx * 8))) | ((u32)msg->buf[1] << (idx * 8));
	return 0;
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	const struct usb_device *usb_dev = adapter->algo_data;
	struct fl2000_device *device;
	int ret;
	bool read;
	u16 addr;
	u8 idx;
	u8 offset;
	u32 data;

	if (!fl2000_i2c_xfer_valid(msgs, num)) {
		ret = -EOPNOTSUPP;
		goto error;
	}
	read = num == 2;
	addr = msgs[0].addr;
	offset = msgs[0].buf[0] & I2C_XFER_ADDR_MASK;
	idx = msgs[0].buf[0] - offset;

	if ((read ? msgs[1].len == 4 : msgs[0].len == 5) && idx) {
		ret = -EOPNOTSUPP;
		goto error;
	}

	if (read) {
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data);
		if (ret)
			goto error;

		for (unsigned int i = 0; i < msgs[1].len; i++)
			msgs[1].buf[i] = (data >> ((idx + i) * 8)) & 0xff;
	} else {
		ret = fl2000_i2c_write_value(adapter, &msgs[0], addr, offset, idx, &data);
		if (ret)
			goto error;

		ret = fl2000_i2c_write_dword(adapter->algo_data, addr, offset, &data);
		if (ret)
			goto error;
	}

	return num;
error:
	device = dev_get_drvdata(&usb_dev->dev);
	atomic_inc(&device->i2c_errors);
	return ret;
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

static const struct property_entry fl2000_i2c_properties[] = {
	PROPERTY_ENTRY_U32("i2c-register-access-alignment", 4),
	PROPERTY_ENTRY_BOOL("display-controller-reset"),
	PROPERTY_ENTRY_BOOL("drm-component-attachment"),
	PROPERTY_ENTRY_BOOL("interrupt-ack-retry"),
	PROPERTY_ENTRY_U32("display-input-clock-delay", 1),
	PROPERTY_ENTRY_U32("display-input-bus-width", 24),
	{}
};

static void fl2000_i2c_adapter_release(struct device *dev, void *res)
{
	struct i2c_adapter *adapter = res;
	struct fwnode_handle *fwnode = dev_fwnode(&adapter->dev);

	dev_info(dev, "Releasing I2C adapter");
	i2c_del_adapter(adapter);
	fwnode_remove_software_node(fwnode);
}

/* Callers must keep control-interface resources alive while using the adapter. */
struct i2c_adapter *fl2000_i2c_adapter(const struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	return device->i2c;
}

bool fl2000_i2c_verify_client(const struct usb_device *usb_dev, struct device *client_dev)
{
	const struct i2c_client *client = i2c_verify_client(client_dev);

	return client && client->adapter == fl2000_i2c_adapter(usb_dev);
}

int fl2000_i2c_init(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);
	int ret;
	struct i2c_adapter *adapter;
	struct fwnode_handle *fwnode;

	adapter = devres_alloc(fl2000_i2c_adapter_release, sizeof(*adapter), GFP_KERNEL);
	if (!adapter) {
		dev_err(&usb_dev->dev, "Cannot allocate adapter!");
		return -ENOMEM;
	}

	adapter->owner = THIS_MODULE;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;
	adapter->algo_data = usb_dev;
	adapter->dev.parent = &usb_dev->dev;
	strscpy(adapter->name, "FL2000 bridge I2C bus", sizeof(adapter->name));

	fwnode = fwnode_create_software_node(fl2000_i2c_properties, NULL);
	if (IS_ERR(fwnode)) {
		devres_free(adapter);
		return PTR_ERR_OR_ZERO(fwnode);
	}
	device_set_node(&adapter->dev, fwnode);
	ret = i2c_add_adapter(adapter);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot add adapter! (%d)", ret);
		fwnode_remove_software_node(fwnode);
		devres_free(adapter);
		return ret;
	}

	devres_add(&usb_dev->dev, adapter);
	device->i2c = adapter;
	return 0;
}

void fl2000_i2c_cleanup(struct usb_device *usb_dev)
{
	struct fl2000_device *device = dev_get_drvdata(&usb_dev->dev);

	devres_release(&usb_dev->dev, fl2000_i2c_adapter_release, NULL, NULL);
	device->i2c = NULL;
}
