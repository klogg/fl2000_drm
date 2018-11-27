/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_i2c.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

/* I2C controller require mandatory 8-bit (1 bite) sub-address provided for any
 * read/write operation. Each read or write operate with 8-bit (1-byte) data.
 * Every exchange shall consist of 2 messages (sub-address + data) combined.
 * USB xfer always bounds address to 4-byte boundary */
#define I2C_CMESSAGES_NUM	2
#define I2C_REG_ADDR_SIZE	(sizeof(u8))
#define I2C_REG_DATA_SIZE	(sizeof(u8))
#define I2C_XFER_ADDR_MASK	(~0x3ul)

/* I2C enable timeout */
#define I2C_ENABLE_TIMEOUT	750

/* Timeout in ms for I2C read/write operations */
#define I2C_RDWR_TIMEOUT	5
#define I2C_RDWR_RETRIES	20

struct fl2000_i2c_algo_data {
	fl2000_bus_control_reg control;
	fl2000_data_reg data;
};

#define fl2000_i2c_read_dword(adapter, addr, offset, data) \
		fl2000_i2c_xfer_dword(adapter, true, addr, offset, data)

#define fl2000_i2c_write_dword(adapter, addr, offset, data) \
		fl2000_i2c_xfer_dword(adapter, false, addr, offset, data)

static int fl2000_i2c_xfer_dword(struct i2c_adapter *adapter, bool read,
		u16 addr, u8 offset, u32 *data)
{
	int i, ret;
	struct usb_device *usb_dev = container_of(adapter->dev.parent,
			struct usb_device, dev);
	struct fl2000_i2c_algo_data *algo_data = adapter->algo_data;
	fl2000_bus_control_reg *control = &algo_data->control;

	if (!read) {
		ret = fl2000_reg_write(usb_dev, &control->w,
				FL2000_REG_BUS_DATA_WR);
		if (ret != 0) goto error;
	}

	ret = fl2000_reg_read(usb_dev, &control->w, FL2000_REG_BUS_CTRL);
	if (ret != 0) goto error;

	control->addr = addr;
	control->cmd = read ? FL2000_CTRL_CMD_READ : FL2000_CTRL_CMD_WRITE;
	control->offset = offset;
	control->bus = FL2000_CTRL_BUS_I2C;
	control->spi_erase = false;
	control->op_status = FL2000_CTRL_OP_STATUS_PROGRESS;
	control->flags |= FL2000_DETECT_MONITOR;

	ret = fl2000_reg_write(usb_dev, &control->w, FL2000_REG_BUS_CTRL);
	if (ret != 0) goto error;

	for (i = 0; i < I2C_RDWR_RETRIES; i++) {
		msleep(I2C_RDWR_TIMEOUT);
		ret = fl2000_reg_read(usb_dev, &control->w, FL2000_REG_BUS_CTRL);
		if (ret != 0) goto error;

		if (control->op_status == FL2000_CTRL_OP_STATUS_DONE) break;
	}

	if (control->data_status != FL2000_DATA_STATUS_PASS ||
			control->op_status != FL2000_CTRL_OP_STATUS_DONE) {
		ret = -1;
		goto error;
	}

	if (read) {
		ret = fl2000_reg_read(usb_dev, data, FL2000_REG_BUS_DATA_RD);
		if (ret != 0) goto error;
	}

	return 0;
error:
	return ret;
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter,
		struct i2c_msg *msgs, int num)
{
	int ret;
	struct i2c_msg *addr_msg = &msgs[0], *data_msg;
	u8 idx, offset = addr_msg->buf[0] & I2C_XFER_ADDR_MASK;
	struct fl2000_i2c_algo_data *algo_data = adapter->algo_data;
	fl2000_data_reg *data = &algo_data->data;

	/* Emulate 1 byte read for detection procedure, poison buffer */
	if (num == 1) {
		msgs[0].buf[0] = 0xAA;
		return num;
	}

	data_msg = &msgs[1];

	idx = addr_msg->buf[0] - offset;

	/* Somehow the original FL2000 driver forces offset to be bound to
	 * 4-byte margin. This is really strange because i2c operation shall not
	 * depend on i2c margin, unless the HW design is completely crippled.
	 * Oh, yes, it is crippled :( */

	if (!!(data_msg->flags & I2C_M_RD)) {
		ret = fl2000_i2c_read_dword(adapter, addr_msg->addr,
				offset, &data->w);
		if (ret != 0) goto error;

		data_msg->buf[0] = data->b[idx];
	} else {
		/* Since FL2000 i2c bus implementation always operates with
		 * 4-byte messages, we need to read before write in order not to
		 * corrupt unrelated registers in case if we do not write whole
		 * dword */
		if (data_msg->len < sizeof(data)) {
			ret = fl2000_i2c_read_dword(adapter, addr_msg->addr,
					offset, &data->w);
			if (ret != 0) goto error;
		}

		data->b[idx] = data_msg->buf[0];

		ret = fl2000_i2c_write_dword(adapter, addr_msg->addr,
				offset, &data->w);
		if (ret != 0) goto error;
	}

	return num;
error:
	dev_err(&adapter->dev, "USB I2C operation failed (%d)", ret);
	return ret;
}

static u32 fl2000_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_READ_BYTE |
			I2C_FUNC_SMBUS_BYTE_DATA;
}

static const struct i2c_algorithm fl2000_i2c_algorithm = {
	.master_xfer    = fl2000_i2c_xfer,
	.functionality  = fl2000_i2c_func,
};

static const struct i2c_adapter_quirks fl2000_i2c_quirks = {
	.flags = I2C_AQ_COMB |		   /* enforce "combined" mode */
		 I2C_AQ_COMB_WRITE_FIRST | /* address write goes first */
		 I2C_AQ_COMB_SAME_ADDR,    /* both are on the same address */
	.max_num_msgs		= I2C_CMESSAGES_NUM,
	.max_write_len		= I2C_REG_DATA_SIZE,
	.max_read_len		= I2C_REG_DATA_SIZE,
	.max_comb_1st_msg_len	= I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len	= I2C_REG_DATA_SIZE,
};

void fl2000_i2c_destroy(struct i2c_adapter *adapter);

struct i2c_adapter *fl2000_i2c_create(struct usb_device *usb_dev)
{
	int ret;
	void *ret_ptr;
	struct i2c_adapter *adapter;

	adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
	if (IS_ERR_OR_NULL(adapter)) {
		dev_err(&usb_dev->dev, "Out of memory");
		ret_ptr = adapter;
		goto error;
	}

	adapter->algo_data = kzalloc(sizeof(fl2000_bus_control_reg),
			GFP_KERNEL);
	if (IS_ERR_OR_NULL(adapter->algo_data)) {
		dev_err(&usb_dev->dev, "Out of memory");
		ret_ptr = adapter->algo_data;
		goto error;
	}

	adapter->owner = THIS_MODULE;
	adapter->class = I2C_CLASS_HDMI;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;

	usb_make_path(usb_dev, adapter->name, sizeof(adapter->name));

	adapter->dev.parent = &usb_dev->dev;

	ret = i2c_add_adapter(adapter);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Out of memory");
		ret_ptr = ERR_PTR(ret);
		goto error;
	}

	dev_info(&adapter->dev, "Connected I2C adapter");
	return adapter;

error:
	/* Enforce cleanup in case of error */
	fl2000_i2c_destroy(adapter);

	return ret_ptr;
}

void fl2000_i2c_destroy(struct i2c_adapter *adapter)
{
	if (IS_ERR_OR_NULL(adapter))
		return;

	if (!IS_ERR_OR_NULL(adapter->algo_data))
		kfree(adapter->algo_data);

	i2c_del_adapter(adapter);

	kfree(adapter);
}
