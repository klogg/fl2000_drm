// SPDX-License-Identifier: GPL-2.0
#include "fl2000.h"
#include "fl2000_registers.h"
#include "compat/drm_atomic.h"
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <drm/drm_bridge.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_drv.h>

/**
 * struct fl2000_vga - Integrated DAC bridge and native DDC transport
 * @bridge: Reference-counted bridge retained by the encoder and connector.
 * @drm: Display device protecting hardware access after unplug.
 * @usb_dev: Controller whose I2C resources remain alive while bound.
 * @ddc_segment_dirty: Segment zero must be restored, protected by the bus lock.
 *
 * There is no external I2C bridge client. The adapter bus lock serializes
 * segment selection, block reads and segment restoration.
 */
struct fl2000_vga {
	struct drm_bridge bridge;
	struct drm_device *drm;
	struct usb_device *usb_dev;
	bool ddc_segment_dirty;
};

/**
 * fl2000_vga_read_edid_block() - Read native DDC through four-byte commands
 * @context: Integrated VGA bridge.
 * @buf: Destination for up to one EDID block.
 * @block: EDID block number, from zero through 511.
 * @len: Requested byte count, from one through 128.
 *
 * The command engine always sends four bytes. Repeating the segment value
 * keeps the DDC segment pointer unchanged across those bytes. Restore segment
 * zero after extended blocks so subsequent base reads also work.
 *
 * Context: Process context with live controller resources. Takes the bus lock.
 * Return: 0 on success or a negative DDC/USB error.
 */
static int fl2000_vga_read_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct fl2000_vga *vga = context;
	struct i2c_adapter *adapter = fl2000_i2c_adapter(vga->usb_dev);
	unsigned int segment = block / 2;
	u8 base = (block & 1) * 128;
	u32 data;
	int ret = 0;

	if (!buf || !len || len > 128 || segment > 255)
		return -EINVAL;
	i2c_lock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);
	if (segment || vga->ddc_segment_dirty) {
		vga->ddc_segment_dirty = true;
		data = segment * 0x01010101U;
		ret = fl2000_i2c_dword(vga->usb_dev, false, 0x30, segment, &data);
		if (ret)
			goto restore;
		vga->ddc_segment_dirty = segment != 0;
	}
	for (size_t offset = 0; offset < len; offset += sizeof(data)) {
		ret = fl2000_i2c_dword(vga->usb_dev, true, 0x50, base + offset, &data);
		if (ret)
			break;
		for (size_t i = 0; i < min_t(size_t, sizeof(data), len - offset); i++)
			buf[offset + i] = data >> (i * 8);
	}
restore:
	if (vga->ddc_segment_dirty) {
		int restore;

		data = 0;
		restore = fl2000_i2c_dword(vga->usb_dev, false, 0x30, 0, &data);
		if (restore) {
			dev_err_ratelimited(&vga->usb_dev->dev,
					    "VGA DDC segment restoration failed: %d\n", restore);
			if (!ret)
				ret = restore;
		} else {
			vga->ddc_segment_dirty = false;
		}
	}
	i2c_unlock_bus(adapter, I2C_LOCK_ROOT_ADAPTER);
	return ret;
}

/**
 * fl2000_vga_detect() - Read physical analog monitor connection status
 * @bridge: Integrated VGA bridge.
 * @connector: Requesting connector; unused.
 *
 * A register read consumes event bits but the interrupt worker also rescans
 * monitor state, so polling cannot hide a connection transition.
 *
 * Context: Process context. May sleep.
 * Return: Connected, disconnected, or unknown on a live read failure.
 */
static enum drm_connector_status fl2000_vga_detect(struct drm_bridge *bridge,
						   struct drm_connector *connector __always_unused)
{
	struct fl2000_vga *vga = container_of(bridge, struct fl2000_vga, bridge);
	struct regmap *regmap;
	unsigned int status;
	int idx, ret;

	if (!drm_dev_enter(vga->drm, &idx))
		return connector_status_disconnected;
	regmap = dev_get_regmap(&vga->usb_dev->dev, NULL);
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status);
	if (ret)
		dev_err_ratelimited(&vga->usb_dev->dev, "VGA monitor detection failed: %d\n", ret);
	drm_dev_exit(idx);
	if (ret)
		return connector_status_unknown;
	return status & FL2000_VGA_STATUS_MONITOR_STATUS ? connector_status_connected :
							   connector_status_disconnected;
}

/**
 * fl2000_vga_edid_read() - Read and validate the analog monitor's EDID
 * @bridge: Integrated VGA bridge.
 * @connector: Connector receiving EDID validation and diagnostics.
 *
 * Context: Process context. May sleep during DDC transfers.
 * Return: Caller-owned EDID, or NULL on removal or an invalid/unreadable EDID.
 */
static const struct drm_edid *fl2000_vga_edid_read(struct drm_bridge *bridge,
						   struct drm_connector *connector)
{
	struct fl2000_vga *vga = container_of(bridge, struct fl2000_vga, bridge);
	const struct drm_edid *edid;
	int idx;

	if (!drm_dev_enter(vga->drm, &idx))
		return NULL;
	edid = drm_edid_read_custom(connector, fl2000_vga_read_edid_block, vga);
	if (!edid)
		dev_warn_ratelimited(&vga->usb_dev->dev, "VGA EDID unavailable\n");
	drm_dev_exit(idx);
	return edid;
}

/**
 * fl2000_vga_disable() - Stop analog output without resetting monitor detection
 * @bridge: Integrated VGA bridge.
 * @state: Atomic transaction; unused.
 *
 * CRTC enable restores DAC output through the normal pixel-format sequence.
 *
 * Context: Process context. May sleep.
 */
static void fl2000_vga_disable(struct drm_bridge *bridge,
			       struct drm_atomic_commit *state __always_unused)
{
	struct fl2000_vga *vga = container_of(bridge, struct fl2000_vga, bridge);
	struct regmap *regmap;
	int idx, ret;

	if (!drm_dev_enter(vga->drm, &idx))
		return;
	regmap = dev_get_regmap(&vga->usb_dev->dev, NULL);
	ret = regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK,
				FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN, 0);
	if (ret)
		dev_err_ratelimited(&vga->usb_dev->dev, "VGA output disable failed: %d\n", ret);
	drm_dev_exit(idx);
}

/**
 * var fl2000_vga_bridge_funcs - Native detection and EDID bridge callbacks
 */
static const struct drm_bridge_funcs fl2000_vga_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
#ifdef FL2000_DRM_LEGACY_ATOMIC
	.atomic_reset = drm_atomic_helper_bridge_reset,
#else
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
#endif
	.atomic_disable = fl2000_vga_disable,
	.detect = fl2000_vga_detect,
	.edid_read = fl2000_vga_edid_read,
};

/**
 * fl2000_vga_attach() - Attach the integrated DAC to the common display pipeline
 * @master: USB parent owning bind-group resources.
 * @drm: Display device used to guard live controller operations.
 * @encoder: Encoder whose cleanup owns bridge detachment.
 *
 * Context: Process context. May sleep.
 * Return: 0 on success or a negative allocation/registration/attachment error.
 */
int fl2000_vga_attach(struct device *master, struct drm_device *drm, struct drm_encoder *encoder)
{
	struct fl2000_vga *vga;
	int ret;

	vga = devm_drm_bridge_alloc(master, struct fl2000_vga, bridge, &fl2000_vga_bridge_funcs);
	if (IS_ERR(vga))
		return PTR_ERR(vga);
	vga->drm = drm;
	vga->usb_dev = to_usb_device(master);
	vga->bridge.type = DRM_MODE_CONNECTOR_VGA;
	vga->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	ret = devm_drm_bridge_add(master, &vga->bridge);
	if (ret)
		return ret;
	return drm_bridge_attach(encoder, &vga->bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
}
