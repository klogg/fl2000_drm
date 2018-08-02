/*
 * fl2000_drm_i2c.c
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000_drm.h"

static int fl2000_drm_pipe_create(struct usb_host_interface *host_interface)
{
	int i;
	struct usb_host_endpoint *endpoint = NULL;

	for (i = 0; i < host_interface->desc.bNumEndpoints; i++) {
		endpoint = host_interface->endpoint[i];
		if (usb_endpoint_is_int_in(&endpoint->desc))
			break;
		endpoint = NULL;
	}

	if (endpoint == NULL) {
		dbg_msg(TRACE_LEVEL_ERROR, DBG_PNP,
			"couldn't find ep_intr_in" );
		goto exit;
	}

	/*
	 * dev_ctx->ep_num_intr_in should be 3
	 */
	dev_ctx->usb_pipe_intr_in = usb_rcvintpipe(dev_ctx->usb_dev, 3);
	if (!dev_ctx->usb_pipe_intr_in) {
		ASSERT(false);
		dbg_msg(TRACE_LEVEL_ERROR, DBG_PNP,
			"ERROR Invalid interrupt pipe." );
		ret_val = -EINVAL;
		goto exit;
	}

	dev_ctx->intr_urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!dev_ctx->intr_urb) {
		dbg_msg(TRACE_LEVEL_ERROR, DBG_PNP,
			"ERROR Allocate interrupt urb failed.");
		ret_val = -ENOMEM;
		goto exit;
	}

	dev_ctx->intr_pipe_wq = create_workqueue("intr_pipe_wq");
	if (dev_ctx->intr_pipe_wq == NULL) {
		usb_free_urb(dev_ctx->intr_urb);
		dev_ctx->intr_urb = NULL;

		dbg_msg(TRACE_LEVEL_ERROR, DBG_PNP,
			"ERROR Allocate interrupt urb failed.");
		ret_val = -ENOMEM;
		goto exit;
	}

	ret_val = 0;

exit:
	return ret_val;
}

static int fl2000_drm_i2c_xfer(struct i2c_adapter *adapter,
		struct i2c_msg *msgs, int num)
{
	unsigned char *pstatus;
	struct i2c_msg *pmsg;
	int i, ret = 0;

	for (i = 0; i < num; i++) {


		if (is_read) {
			bRequest = REQUEST_I2C_COMMAND_READ;
			req_type = (USB_DIR_IN | USB_TYPE_VENDOR);
			pipe = usb_rcvctrlpipe(dev_ctx->usb_dev, 0);
		}
		else {
			bRequest = REQUEST_I2C_COMMAND_WRITE;
			req_type = (USB_DIR_OUT | USB_TYPE_VENDOR);
			pipe = usb_sndctrlpipe(dev_ctx->usb_dev, 0);
		}

		req_index = (uint16_t) offset;
		ret_val = usb_control_msg(
			dev_ctx->usb_dev,
			pipe,
			bRequest,
			req_type,
			0,
			req_index,
			&dev_ctx->ctrl_xfer_buf,
			REQUEST_I2C_RW_DATA_COMMAND_LENGTH,
			CTRL_XFER_TIMEOUT);

		if (is_read)
			*data = dev_ctx->ctrl_xfer_buf;

	}

}



static const struct i2c_algorithm fl2000_drm_i2c_algorithm = {
        .master_xfer    = fl2000_drm_i2c_xfer,
        .functionality  = fl2000_drm_i2c_func,
};


static struct i2c_adapter fl2000_drm_i2c_adapter = {
	.owner = THIS_MODULE,
	.class = I2C_CLASS_DDC,
	.algo = &fl2000_drm_i2c_algorithm,
};


static int probe()
{


	snprintf(fl2000_drm_i2c_adapter.name,
			sizeof(fl2000_drm_i2c_adapter.name),
	                "FL2000 I2C at USB bus %03d device %03d",
	                dev->usb_dev->bus->busnum, dev->usb_dev->devnum);

}
