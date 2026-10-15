// SPDX-License-Identifier: GPL-2.0
/* Bridge-connector glue for kernels without CONFIG_DRM_BRIDGE_CONNECTOR. */
#include "fl2000.h"
#include <drm/drm_bridge.h>
#include <drm/drm_edid.h>

struct fl2000_connector {
	struct drm_connector connector;
	struct drm_bridge *bridge;
};

static struct drm_bridge *fl2000_connector_bridge(struct drm_connector *connector)
{
	return container_of(connector, struct fl2000_connector, connector)->bridge;
}

static enum drm_connector_status fl2000_connector_detect(struct drm_connector *connector,
							 bool force)
{
	struct drm_bridge *bridge = fl2000_connector_bridge(connector);

	return bridge->funcs->detect(bridge, connector);
}

static int fl2000_connector_get_modes(struct drm_connector *connector)
{
	struct drm_bridge *bridge = fl2000_connector_bridge(connector);
	const struct drm_edid *edid;
	int ret;

	edid = bridge->funcs->edid_read(bridge, connector);
	ret = drm_edid_connector_update(connector, edid);
	if (!ret)
		ret = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);
	return ret < 0 ? 0 : ret;
}

static enum drm_mode_status fl2000_connector_mode_valid(struct drm_connector *connector,
							const struct drm_display_mode *mode)
{
	struct drm_bridge *bridge = fl2000_connector_bridge(connector);

	return bridge->funcs->mode_valid(bridge, &connector->display_info, mode);
}

static void fl2000_connector_destroy(struct drm_connector *connector)
{
	struct fl2000_connector *output =
		container_of(connector, struct fl2000_connector, connector);

	drm_connector_cleanup(connector);
	drm_bridge_put(output->bridge);
	kfree(output);
}

static const struct drm_connector_helper_funcs fl2000_connector_helpers = {
	.get_modes = fl2000_connector_get_modes,
	.mode_valid = fl2000_connector_mode_valid,
};

static const struct drm_connector_funcs fl2000_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = fl2000_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = fl2000_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

struct drm_connector *fl2000_bridge_connector_init(struct drm_device *drm,
						   struct drm_encoder *encoder,
						   struct drm_bridge *bridge)
{
	struct fl2000_connector *output;
	int ret;

	output = kzalloc_obj(*output);
	if (!output)
		return ERR_PTR(-ENOMEM);
	output->bridge = bridge;
	drm_bridge_get(bridge);
	ret = drm_connector_init(drm, &output->connector, &fl2000_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		drm_bridge_put(bridge);
		kfree(output);
		return ERR_PTR(ret);
	}
	drm_connector_helper_add(&output->connector, &fl2000_connector_helpers);
	output->connector.polled = DRM_CONNECTOR_POLL_HPD;
	return &output->connector;
}
