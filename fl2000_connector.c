
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "fl2000.h"

static int fl2000_read_edid(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct i2c_adapter *adapter = data;
	struct usb_device *usb_dev = adapter->algo_data;
	unsigned char start = block * EDID_LENGTH;
	//unsigned char segment = block >> 1;
	int ret;
	int i;

	for (i = 0; i < len; i += 4) {
		ret = fl2000_i2c_dword(usb_dev, true, DDC_ADDR, start + i,
					    (u32 *)&buf[start + i]);
		if (ret)
			return ret;
	}

	return 0;
}
static int fl2000_get_modes(struct drm_connector *connector)
{
	const struct drm_edid *edid;
	int ret;

	edid = drm_edid_read_custom(connector, fl2000_read_edid, connector->ddc);
	drm_edid_connector_update(connector, edid);
	ret = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);

	//ret = drm_add_modes_noedid(connector, 1920, 1200);
	//drm_set_preferred_mode(connector, 1024, 768);
	return ret;
}

static enum drm_mode_status
fl2000_connector_mode_valid(struct drm_connector *connector,
			    const struct drm_display_mode *mode)
{
	UNUSED(connector);
	UNUSED(mode);

	return MODE_OK;
}

static enum drm_connector_status fl2000_detect(struct drm_connector *connector, bool force)
{
	struct usb_device *usb_dev = connector->ddc->algo_data;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_status_reg status;
	int ret;

	UNUSED(force);

	if (!regmap)
		return connector_status_unknown;

	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret)
		return connector_status_unknown;

	return status.monitor_status ? connector_status_connected :
				       connector_status_disconnected;
}

static void fl2000_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

static const struct drm_connector_helper_funcs fl2000_connector_helper_funcs = {
	.get_modes = fl2000_get_modes,
	.mode_valid = fl2000_connector_mode_valid,
};

static const struct drm_connector_funcs fl2000_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = fl2000_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = fl2000_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int fl2000_connector_init(struct drm_device *drm, struct drm_connector *connector,
			  struct i2c_adapter *adapter)
{
	int ret;

	ret = drm_connector_init_with_ddc(drm, connector,
					  &fl2000_connector_funcs,
					  DRM_MODE_CONNECTOR_VGA,
					  adapter);
	if (ret)
		return ret;

	drm_connector_helper_add(connector, &fl2000_connector_helper_funcs);

	connector->polled = DRM_CONNECTOR_POLL_HPD |
			    DRM_CONNECTOR_POLL_CONNECT |
			    DRM_CONNECTOR_POLL_DISCONNECT;

	return 0;
}
