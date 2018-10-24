/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_drm_i2c.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000_drm.h"

#define I2C_CMD_WRITE		0
#define I2C_CMD_READ		1
#define I2C_DATA_STATUS_PASS	0
#define I2C_DATA_STATUS_FAIL	1
#define I2C_OP_STATUS_PROGRESS	0
#define I2C_OP_STATUS_DONE	1

/* I2C controller require mandatory 16-bit (2 bite) sub-address provided for any
 * read/write operation. Each read or write operate with 32-bit (4-byte) data.
 * Every exchange shall consist of 2 messages (sub-address + data) combined */
#define I2C_CMESSAGE_SIZE	2
#define I2C_REG_ADDR_SIZE	2
#define I2C_REG_DATA_SIZE	4

/* Timeout in ms for I2C read/write operations */
#define I2C_RDWR_TIMEOUT	5
#define I2C_RDWR_RETRIES	10

struct fl2000_drm_i2c_bus {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
	struct i2c_adapter adapter;
};

typedef union {
	struct {
		u32 addr	:7; /* I2C address */
		u32 rw		:1; /* 1 read, 0 write */
		u32 offset	:8; /* I2C offset, 9:8 shall be always 0 */
		u32 is_spi	:1; /* 1 SPI, 0 EEPROM */
		u32 spi_erase	:1; /* 1 SPI, 0 EEPROM */
		u32 res_1	:6;
		u32 data_status	:4; /* mask for failed bytes */
		u32 res_2	:3;
		u32 op_status	:1; /* I2C operation status, 0 in progress,
				       1 done. Write 0 to reset */
	} s;
	u32 w;
} fl2000_i2c_control_reg;

static u32 fl2000_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter,
		struct i2c_msg *msgs, int num)
{
	int i, ret = 0;
	struct i2c_msg *addr_msg = &msgs[0], *data_msg = &msgs[1];
	struct fl2000_drm_i2c_bus *i2c_bus = adapter->algo_data;
	fl2000_i2c_control_reg control = {.w = 0};
	bool read = !!(data_msg->flags & I2C_M_RD);

#if DEBUG
	/* xfer validation (actually i2c stack shall do it):
	 *  - there is only 2 messages for an xfer
	 *  - 1st message size is 2 bytes
	 *  - 2nd message size is 4 bytes
	 *  - 1st message is always "write"
	 *  - both messages have same addresses */
	WARN_ON((num != I2C_CMESSAGE_SIZE) ||
		(addr_msg->len != I2C_REG_ADDR_SIZE) ||
		(data_msg->len != I2C_REG_DATA_SIZE) ||
		(addr_msg->flags & I2C_M_RD) ||
		(addr_msg->addr != data_msg->addr));
#endif

	if (!read) {
		ret = fl2000_reg_write(i2c_bus->usb_dev, (u32 *)data_msg->buf,
				FL2000_REG_I2C_DATA_WR);
		if (ret != 0) goto error;
	}

	/* TODO: Not quite sure if control register has to be just 0-ed. In the
	 * original implementation its "reserved" fields are set with reading
	 * actual register contents - useless? Also, bit 28 is always being set
	 * to 1 nevertheless it always reads 0 */

	control.s.addr = data_msg->addr;
	control.s.rw = read ? I2C_CMD_READ : I2C_CMD_WRITE;
	control.s.offset = *(u16 *)addr_msg->buf;

	fl2000_reg_write(i2c_bus->usb_dev, &control.w, FL2000_REG_I2C_CTRL);
	if (ret != 0) goto error;

	for (i = 0; i < I2C_RDWR_RETRIES; i++) {
		msleep(I2C_RDWR_TIMEOUT);
		ret = fl2000_reg_read(i2c_bus->usb_dev, &control.w,
				FL2000_REG_I2C_CTRL);
		if (ret != 0) goto error;
		if (control.s.op_status == I2C_OP_STATUS_DONE) break;
	}

	if (control.s.data_status != I2C_DATA_STATUS_PASS ||
			control.s.op_status != I2C_OP_STATUS_DONE)
		goto error;

	if (read) {
		ret = fl2000_reg_read(i2c_bus->usb_dev, (u32 *)data_msg->buf,
				FL2000_REG_I2C_DATA_RD);
		if (ret != 0) goto error;
	}

	return 0;

error:
	dev_err(&i2c_bus->adapter.dev, "USB I2C operation failed (%d)", ret);
	return ret;
}

static const struct i2c_adapter_quirks fl2000_i2c_quirks = {
	.flags = I2C_AQ_COMB |		   /* enforce "combined" mode */
		 I2C_AQ_COMB_WRITE_FIRST | /* address write goes first */
		 I2C_AQ_COMB_SAME_ADDR,    /* both are on the same address */
	.max_num_msgs		= I2C_CMESSAGE_SIZE,
	.max_write_len		= I2C_REG_DATA_SIZE,
	.max_read_len		= I2C_REG_DATA_SIZE,
	.max_comb_1st_msg_len	= I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len	= I2C_REG_DATA_SIZE,
};

static const struct i2c_algorithm fl2000_i2c_algorithm = {
        .master_xfer    = fl2000_i2c_xfer,
        .functionality  = fl2000_i2c_func,
};

int fl2000_i2c_connect(struct usb_device *usb_dev)
{
	int ret = 0;
	struct fl2000_drm_i2c_bus *i2c_bus;

	i2c_bus = kzalloc(sizeof(*i2c_bus), GFP_KERNEL);
	if (i2c_bus == NULL) {
		dev_err(&usb_dev->dev, "Out of memory");
		ret = -ENOMEM;
		goto error;
	}

	dev_set_drvdata(&usb_dev->dev, i2c_bus);
	i2c_bus->usb_dev = usb_dev;

	i2c_bus->adapter.owner = THIS_MODULE;
	i2c_bus->adapter.class = I2C_CLASS_DEPRECATED;
	i2c_bus->adapter.algo = &fl2000_i2c_algorithm;
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
	fl2000_i2c_disconnect(usb_dev);

	return ret;
}

void fl2000_i2c_disconnect(struct usb_device *usb_dev)
{
	struct fl2000_drm_i2c_bus *i2c_bus = dev_get_drvdata(&usb_dev->dev);

	if (i2c_bus == NULL) return;

	dev_info(&i2c_bus->adapter.dev, "Disconnected I2C adapter");
	i2c_del_adapter(&i2c_bus->adapter);

	dev_set_drvdata(&usb_dev->dev, NULL);
	kfree(i2c_bus);
}
