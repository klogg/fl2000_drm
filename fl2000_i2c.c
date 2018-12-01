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

/* Timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL	(5 * 1000)
#define I2C_RDWR_TIMEOUT	(20 * I2C_RDWR_INTERVAL)

struct fl2000_i2c_algo_data {
	struct i2c_adapter *adapter;
	struct usb_device *usb_dev;
	struct regmap *regmap;
	struct regmap_field *i2c_addr;
	struct regmap_field *i2c_cmd;
	struct regmap_field *i2c_offset;
	struct regmap_field *i2c_status;
	struct regmap_field *i2c_ready;
	struct regmap_field *i2c_done;
};

#define fl2000_i2c_read_dword(adapter, addr, offset, data) \
		fl2000_i2c_xfer_dword(adapter, true, addr, offset, data)

#define fl2000_i2c_write_dword(adapter, addr, offset, data) \
		fl2000_i2c_xfer_dword(adapter, false, addr, offset, data)

static int fl2000_i2c_xfer_dword(struct i2c_adapter *adapter, bool read,
		u16 addr, u8 offset, u32 *data)
{
	int ret;
	unsigned int val;
	struct fl2000_i2c_algo_data *i2c_algo_data = adapter->algo_data;
	struct regmap *regmap = i2c_algo_data->regmap;

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret != 0) return -EIO;
	}

	ret = regmap_field_write(i2c_algo_data->i2c_status, 0);
	if (ret != 0) return -EIO;
	ret = regmap_field_write(i2c_algo_data->i2c_addr, addr);
	if (ret != 0) return -EIO;
	ret = regmap_field_write(i2c_algo_data->i2c_cmd, read);
	if (ret != 0) return -EIO;
	ret = regmap_field_write(i2c_algo_data->i2c_offset, offset);
	if (ret != 0) return -EIO;
	ret = regmap_field_write(i2c_algo_data->i2c_done, false);
	if (ret != 0) return -EIO;

	/* TODO: force update register? */

	ret = regmap_field_read_poll_timeout(i2c_algo_data->i2c_done, val,
			(val == true), I2C_RDWR_INTERVAL, I2C_RDWR_TIMEOUT);
	if (ret != 0) return ret;

	ret = regmap_field_read(i2c_algo_data->i2c_status, &val);
	if (ret != 0) return ret;

	if (val != 0) {
		ret = -EIO;
		return ret;
	}

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret != 0) return -EIO;
	}

	return 0;
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter,
		struct i2c_msg *msgs, int num)
{
	int ret;
	struct i2c_msg *addr_msg = &msgs[0], *data_msg;
	u8 idx, offset = addr_msg->buf[0] & I2C_XFER_ADDR_MASK;
	union {
		u32 w;
		u8 b[4];
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
		ret = fl2000_i2c_read_dword(adapter, addr_msg->addr,
				offset, &data.w);
		if (ret != 0) goto error;

		data_msg->buf[0] = data.b[idx];
	} else {
		/* Since FL2000 i2c bus implementation always operates with
		 * 4-byte messages, we need to read before write in order not to
		 * corrupt unrelated registers in case if we do not write whole
		 * dword */
		if (data_msg->len < sizeof(data)) {
			ret = fl2000_i2c_read_dword(adapter, addr_msg->addr,
					offset, &data.w);
			if (ret != 0) goto error;
		}

		data.b[idx] = data_msg->buf[0];

		ret = fl2000_i2c_write_dword(adapter, addr_msg->addr,
				offset, &data.w);
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

static void fl2000_i2c_algo_data_release(struct device *dev, void *res)
{
	i2c_del_adapter(res);
}

struct i2c_adapter *fl2000_get_i2c_adapter(struct usb_device *usb_dev)
{
	return devres_find(&usb_dev->dev, &fl2000_i2c_algo_data_release, NULL, NULL);
}

struct regmap *fl2000_get_regmap(struct usb_device *usb_dev);

int fl2000_i2c_create(struct usb_device *usb_dev)
{
	int ret;
	struct i2c_adapter *adapter;
	struct fl2000_i2c_algo_data *i2c_algo_data;
	struct regmap *regmap;

	/* Adapter must be allocated before anything else */
	adapter = devres_alloc(fl2000_i2c_algo_data_release, sizeof(*adapter),
			GFP_KERNEL);
	if (IS_ERR_OR_NULL(adapter))
		return IS_ERR(adapter) ? PTR_ERR(adapter) : -ENOMEM;
	devres_add(&usb_dev->dev, adapter);

	/* On de-initialization of algo_data i2c adapter will be unregistered */
	i2c_algo_data = devm_kzalloc(&usb_dev->dev, sizeof(*i2c_algo_data),
			GFP_KERNEL);
	if (IS_ERR_OR_NULL(i2c_algo_data))
		return IS_ERR(i2c_algo_data) ? PTR_ERR(i2c_algo_data) : -ENOMEM;

	/* Regmap must exist on i2c initialization */
	regmap = fl2000_get_regmap(usb_dev);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	i2c_algo_data->usb_dev = usb_dev;
	i2c_algo_data->regmap = regmap;

	i2c_algo_data->i2c_addr = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_STATUS_REG_i2c_addr);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	i2c_algo_data->i2c_cmd = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_STATUS_REG_i2c_cmd);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	i2c_algo_data->i2c_offset = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_STATUS_REG_i2c_offset);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	i2c_algo_data->i2c_done = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_STATUS_REG_i2c_done);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	i2c_algo_data->i2c_status = devm_regmap_field_alloc(&usb_dev->dev,
			regmap, FL2000_VGA_STATUS_REG_i2c_status);
	if (IS_ERR_OR_NULL(regmap))
		return IS_ERR(regmap) ? PTR_ERR(regmap) : -ENOMEM;

	adapter->owner = THIS_MODULE;
	adapter->class = I2C_CLASS_HDMI;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;

	adapter->algo_data = i2c_algo_data;

	adapter->dev.parent = &usb_dev->dev;

	usb_make_path(usb_dev, adapter->name, sizeof(adapter->name));

	ret = i2c_add_adapter(adapter);
	if (ret != 0) return ret;

	/* Set adapter to algo data only on successful addition so that release
	 * function will not fail trying to remove it */
	i2c_algo_data->adapter = adapter;

	dev_info(&adapter->dev, "Connected FL2000 I2C adapter");
	return 0;
}
