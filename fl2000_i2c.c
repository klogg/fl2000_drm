/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_i2c.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

/* Custom class for DRM bridge autodetection when there is no DT support */
#ifndef CONFIG_OF
#define I2C_CLASS_HDMI	(1<<9)
#endif

/* I2C controller require mandatory 8-bit (1 bite) sub-address provided for any
 * read/write operation. Each read or write operate with 8-bit (1-byte) data.
 * Every exchange shall consist of 2 messages (sub-address + data) combined.
 * USB xfer always bounds address to 4-byte boundary */
#define I2C_CMESSAGES_NUM	2
#define I2C_REG_ADDR_SIZE	(sizeof(u8))
#define I2C_REG_DATA_SIZE	(sizeof(u8))
#define I2C_XFER_ADDR_MASK	(~0x3ul)
#define I2C_XFER_SIZE		(sizeof(u32))

/* I2C enable timeout */
#define I2C_ENABLE_TIMEOUT	750

/* Timeout in ms for I2C read/write operations */
#define I2C_RDWR_TIMEOUT	5
#define I2C_RDWR_RETRIES	20

struct fl2000_i2c_bus {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
	struct i2c_adapter adapter;
	struct mutex hw_mutex;
};

#define fl2000_i2c_read_dword(i2c_bus, addr, offset, data) \
		fl2000_i2c_xfer_dword(i2c_bus, true, addr, offset, data)

#define fl2000_i2c_write_dword(i2c_bus, addr, offset, data) \
		fl2000_i2c_xfer_dword(i2c_bus, false, addr, offset, data)

static int fl2000_i2c_xfer_dword(struct fl2000_i2c_bus *i2c_bus, bool read,
		u16 addr, u8 offset, u32 *data)
{
	int i, ret;
	fl2000_bus_control_reg control;

	if (!read) {
		ret = fl2000_reg_write(i2c_bus->usb_dev, &control.w,
				FL2000_REG_BUS_DATA_WR);
		if (ret != 0) goto error;
	}

	ret = fl2000_reg_read(i2c_bus->usb_dev, &control.w,
			FL2000_REG_BUS_CTRL);
	if (ret != 0) goto error;

	control.addr = addr;
	control.cmd = read ? FL2000_CTRL_CMD_READ : FL2000_CTRL_CMD_WRITE;
	control.offset = offset;
	control.bus = FL2000_CTRL_BUS_I2C;
	control.spi_erase = false;
	control.op_status = FL2000_CTRL_OP_STATUS_PROGRESS;
	control.flags |= FL2000_DETECT_MONITOR;

	ret = fl2000_reg_write(i2c_bus->usb_dev, &control.w,
			FL2000_REG_BUS_CTRL);
	if (ret != 0) goto error;

	for (i = 0; i < I2C_RDWR_RETRIES; i++) {
		msleep(I2C_RDWR_TIMEOUT);
		ret = fl2000_reg_read(i2c_bus->usb_dev, &control.w,
				FL2000_REG_BUS_CTRL);
		if (ret != 0) goto error;

		if (control.op_status == FL2000_CTRL_OP_STATUS_DONE) break;
	}

	if (control.data_status != FL2000_DATA_STATUS_PASS ||
			control.op_status != FL2000_CTRL_OP_STATUS_DONE) {
		ret = -1;
		goto error;
	}

	if (read) {
		ret = fl2000_reg_read(i2c_bus->usb_dev, data,
				FL2000_REG_BUS_DATA_RD);
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
	struct fl2000_i2c_bus *i2c_bus = adapter->algo_data;
	u8 idx, offset = addr_msg->buf[0] & I2C_XFER_ADDR_MASK;
	union {
		u8 b[I2C_XFER_SIZE];
		u32 w;
	} data;

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
		ret = fl2000_i2c_read_dword(i2c_bus, addr_msg->addr,
				offset, &data.w);
		if (ret != 0) goto error;

		data_msg->buf[0] = data.b[idx];
	} else {
		/* Since FL2000 i2c bus implementation always operates with
		 * 4-byte messages, we need to read before write in order not to
		 * corrupt unrelated registers in case if we do not write whole
		 * dword */
		if (data_msg->len < I2C_REG_DATA_SIZE) {
			ret = fl2000_i2c_read_dword(i2c_bus, addr_msg->addr,
					offset, &data.w);
			if (ret != 0) goto error;
		}

		data.b[idx] = data_msg->buf[0];

		ret = fl2000_i2c_write_dword(i2c_bus, addr_msg->addr,
				offset, &data.w);
		if (ret != 0) goto error;
	}

	return num;
error:
	dev_err(&i2c_bus->adapter.dev, "USB I2C operation failed (%d)", ret);
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

static void fl2000_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct fl2000_i2c_bus *i2c_bus = adapter->algo_data;
	mutex_lock(&i2c_bus->hw_mutex);
}

static int fl2000_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct fl2000_i2c_bus *i2c_bus = adapter->algo_data;
	return mutex_trylock(&i2c_bus->hw_mutex);
}

static void fl2000_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct fl2000_i2c_bus *i2c_bus = adapter->algo_data;
	mutex_unlock(&i2c_bus->hw_mutex);
}

struct i2c_lock_operations fl2000_i2c_lock_operations = {
	.lock_bus = fl2000_lock_bus,
	.trylock_bus = fl2000_trylock_bus,
	.unlock_bus = fl2000_unlock_bus,
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

int fl2000_i2c_create(struct usb_interface *interface)
{
	int ret = 0;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_i2c_bus *i2c_bus;

	i2c_bus = kzalloc(sizeof(*i2c_bus), GFP_KERNEL);
	if (IS_ERR(i2c_bus)) {
		dev_err(&usb_dev->dev, "Out of memory");
		ret = PTR_ERR(i2c_bus);
		goto error;
	}

	usb_set_intfdata(interface, i2c_bus);

	mutex_init(&i2c_bus->hw_mutex);

	i2c_bus->usb_dev = usb_dev;
	i2c_bus->interface = interface;

	i2c_bus->adapter.owner = THIS_MODULE;
#ifndef CONFIG_OF
	i2c_bus->adapter.class = I2C_CLASS_HDMI;
#else
	i2c_bus->adapter.class = I2C_CLASS_DEPRECATED;
#endif
	i2c_bus->adapter.algo = &fl2000_i2c_algorithm;
	i2c_bus->adapter.lock_ops = &fl2000_i2c_lock_operations;
	i2c_bus->adapter.quirks = &fl2000_i2c_quirks;
	i2c_bus->adapter.algo_data = i2c_bus;

	snprintf(i2c_bus->adapter.name, sizeof(i2c_bus->adapter.name),
		 "FL2000 I2C adapter at USB bus %03d device %03d",
		 usb_dev->bus->busnum, usb_dev->devnum);

	i2c_bus->adapter.dev.parent = &usb_dev->dev;

	ret = i2c_add_adapter(&i2c_bus->adapter);
	if (ret != 0) {
		dev_err(&usb_dev->dev, "Out of memory");
		goto error;
	}

	dev_info(&i2c_bus->adapter.dev, "Connected I2C adapter");
	return 0;

error:
	/* Enforce cleanup in case of error */
	fl2000_i2c_destroy(interface);

	return ret;
}

void fl2000_i2c_destroy(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_i2c_bus *i2c_bus = usb_get_intfdata(interface);

	if (i2c_bus == NULL) return;

	i2c_del_adapter(&i2c_bus->adapter);

	dev_set_drvdata(&usb_dev->dev, NULL);
	kfree(i2c_bus);
}
