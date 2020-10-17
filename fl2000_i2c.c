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
#define I2C_CMESSAGES_NUM	2
#define I2C_REG_ADDR_SIZE	(sizeof(u8))
#define I2C_REG_DATA_SIZE	(sizeof(u8))
#define I2C_XFER_ADDR_MASK	(~0x3ul)

/* Timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL	(200)
#define I2C_RDWR_TIMEOUT	(256 * 1000)

struct fl2000_i2c_data {
	struct usb_device *usb_dev;
#if defined(CONFIG_DEBUG_FS)
	u8 address, offset;
	struct dentry *root_dir, *address_file, *offset_file, *data_file;
#endif
};

static int fl2000_i2c_xfer_dword(struct i2c_adapter *adapter, bool read, u16 addr, u8 offset,
				 u32 *data);

static inline int fl2000_i2c_read_dword(struct i2c_adapter *adapter, u16 addr, u8 offset,
					u32 *data)
{
	return fl2000_i2c_xfer_dword(adapter, true, addr, offset, data);
}

static inline int fl2000_i2c_write_dword(struct i2c_adapter *adapter, u16 addr, u8 offset,
					 u32 *data)
{
	return fl2000_i2c_xfer_dword(adapter, false, addr, offset, data);
}

#if defined(CONFIG_DEBUG_FS)

static int fl2000_debugfs_i2c_read(void *data, u64 *value)
{
	int ret;
	u32 u32_value;
	struct i2c_adapter *adapter = data;
	struct fl2000_i2c_data *i2c_data = adapter->algo_data;

	ret = fl2000_i2c_read_dword(adapter, i2c_data->address,
			i2c_data->offset, &u32_value);
	*value = u32_value;
	return ret;
}

static int fl2000_debugfs_i2c_write(void *data, u64 value)
{
	u32 u32_value = value;
	struct i2c_adapter *adapter = data;
	struct fl2000_i2c_data *i2c_data = adapter->algo_data;

	return fl2000_i2c_write_dword(adapter, i2c_data->address,
			i2c_data->offset, &u32_value);
}

DEFINE_SIMPLE_ATTRIBUTE(i2c_ops, fl2000_debugfs_i2c_read, fl2000_debugfs_i2c_write, "%08llx\n");

static int fl2000_debugfs_i2c_init(struct i2c_adapter *adapter)
{
	struct fl2000_i2c_data *i2c_data = adapter->algo_data;

	i2c_data->root_dir = debugfs_create_dir("fl2000_i2c", NULL);
	if (IS_ERR(i2c_data->root_dir))
		return PTR_ERR(i2c_data->root_dir);

	i2c_data->address_file = debugfs_create_x8("i2c_address", fl2000_debug_umode,
			i2c_data->root_dir, &i2c_data->address);
	if (IS_ERR(i2c_data->address_file))
		return PTR_ERR(i2c_data->address_file);

	i2c_data->offset_file = debugfs_create_x8("i2c_offset", fl2000_debug_umode,
			i2c_data->root_dir, &i2c_data->offset);
	if (IS_ERR(i2c_data->offset_file))
		return PTR_ERR(i2c_data->offset_file);

	i2c_data->data_file = debugfs_create_file("i2c_data", fl2000_debug_umode,
			i2c_data->root_dir, adapter, &i2c_ops);
	if (IS_ERR(i2c_data->data_file))
		return PTR_ERR(i2c_data->data_file);

	return 0;
}

static void fl2000_debugfs_i2c_remove(struct i2c_adapter *adapter)
{
	struct fl2000_i2c_data *i2c_data = adapter->algo_data;

	debugfs_remove(i2c_data->data_file);
	debugfs_remove(i2c_data->offset_file);
	debugfs_remove(i2c_data->address_file);
	debugfs_remove(i2c_data->root_dir);
}

#else /* CONFIG_DEBUG_FS */

#define fl2000_debugfs_i2c_init(adapter)
#define fl2000_debugfs_i2c_remove(adapter)

#endif /* CONFIG_DEBUG_FS */

/* TODO: Move this function to registers.c */
static int fl2000_i2c_xfer_dword(struct i2c_adapter *adapter, bool read, u16 addr, u8 offset,
				 u32 *data)
{
	int ret;
	union fl2000_vga_i2c_sc_reg reg = {.val = 0};
	struct fl2000_i2c_data *i2c_data = adapter->algo_data;
	struct usb_device *usb_dev = i2c_data->usb_dev;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask = 0;

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret) {
			dev_err(&adapter->dev, "FL2000_VGA_I2C_WR_REG write failed!");
			return ret;
		}
	}

	/* XXX: This bit always reads back as 0, so we need to restore it back. Though not quite
	 * sure if we need to enable monitor detection circuit for HDMI use case
	 */
	reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);

	reg.i2c_status = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_status);
	reg.i2c_addr = addr;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_addr);
	reg.i2c_cmd = read;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_cmd);
	reg.i2c_offset = offset;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_offset);
	reg.i2c_done = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_done);

	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
	if (ret) {
		dev_err(&adapter->dev, "FL2000_VGA_I2C_SC_REG write failed!");
		return ret;
	}

	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, reg.val, reg.i2c_done,
				       I2C_RDWR_INTERVAL, I2C_RDWR_TIMEOUT);
	/* This shouldn't normally happen: there's internal 256ms HW timeout on I2C operations and
	 * USB must be always available so no I/O errors. But if it happens we are probably in
	 * irreversible HW issue
	 */
	if (ret) {
		dev_err(&adapter->dev, "FL2000_VGA_I2C_SC_REG poll failed!");
		return ret;
	}

	/* XXX: Weirdly enough we cannot rely on internal HW 256ms I2C timeout indicated in bit 29.
	 * Somehow it always read back as 0
	 */
	if (reg.i2c_status != 0) {
		dev_err(&adapter->dev, "I2C error detected: status %d",	reg.i2c_status);
		return -EIO;
	}

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret) {
			dev_err(&adapter->dev, "FL2000_VGA_I2C_RD_REG read failed!");
			return ret;
		}
	}

	return 0;
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	int ret;
	bool read;
	u16 addr;
	u8 idx, offset;
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
	 * - 1 message, 1 byte, read
	 */
	if (num == 2) {
		read = true;
	} else if (num == 1) {
		if (msgs[0].len == 2 && !(msgs[0].flags & I2C_M_RD)) {
			read = false;
		} else if (msgs[0].len == 1 && (msgs[0].flags & I2C_M_RD)) {
			msgs[0].buf[0] = 0xAA; /* poison buffer */
			return num;
		} else {
			return -ENOTSUPP;
		}
	} else {
		return -ENOTSUPP;
	}

	/* Somehow the original FL2000 driver forces offset to be bound to 4-byte margin. This is
	 * really strange because i2c operation shall not depend on i2c margin, unless the HW design
	 * is completely crippled. Oh, yes, it is crippled :(
	 */
	if (read) {
		ret = fl2000_i2c_read_dword(adapter, addr, offset, &data.w);
		if (ret)
			return ret;

		msgs[1].buf[0] = data.b[idx];
	} else {
		/* Since FL2000 i2c bus implementation always operates with 4-byte messages, we need
		 * to read before write in order not to corrupt unrelated registers in case if we do
		 * not write whole dword
		 */
		ret = fl2000_i2c_read_dword(adapter, addr, offset, &data.w);
		if (ret)
			return ret;

		data.b[idx] = msgs[0].buf[1];

		ret = fl2000_i2c_write_dword(adapter, addr, offset, &data.w);
		if (ret)
			return ret;
	}

	return num;
}

static u32 fl2000_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_READ_BYTE;
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
	.max_write_len		= 2 * I2C_REG_DATA_SIZE,
	.max_read_len		= I2C_REG_DATA_SIZE,
	.max_comb_1st_msg_len	= I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len	= I2C_REG_DATA_SIZE,
};

static void fl2000_i2c_adapter_release(struct device *dev, void *res)
{
	/* Noop */
}

int fl2000_i2c_init(struct usb_device *usb_dev)
{
	int ret;
	struct i2c_adapter *adapter;
	struct fl2000_i2c_data *i2c_data;

	/* Adapter must be allocated before anything else */
	adapter = devres_alloc(fl2000_i2c_adapter_release, sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return -ENOMEM;
	devres_add(&usb_dev->dev, adapter);

	/* On de-initialization of algo_data i2c adapter will be unregistered */
	i2c_data = devm_kzalloc(&usb_dev->dev, sizeof(*i2c_data), GFP_KERNEL);
	if (!i2c_data)
		return -ENOMEM;

	i2c_data->usb_dev = usb_dev;

	adapter->owner = THIS_MODULE;
	adapter->class = I2C_CLASS_HDMI;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;

	adapter->algo_data = i2c_data;

	adapter->dev.parent = &usb_dev->dev;

	usb_make_path(usb_dev, adapter->name, sizeof(adapter->name));

	ret = i2c_add_adapter(adapter);
	if (ret)
		return ret;

	ret = fl2000_debugfs_i2c_init(adapter);
	if (ret)
		return ret;

	dev_info(&adapter->dev, "Connected FL2000 I2C adapter");
	return 0;
}

void fl2000_i2c_cleanup(struct usb_device *usb_dev)
{
	struct i2c_adapter *adapter = devres_find(&usb_dev->dev, fl2000_i2c_adapter_release, NULL,
			NULL);

	if (!adapter)
		return;

	fl2000_debugfs_i2c_remove(adapter);
	i2c_del_adapter(adapter);
	devm_kfree(&usb_dev->dev, adapter->algo_data);
	devres_release(&usb_dev->dev, fl2000_i2c_adapter_release, NULL, NULL);
}
