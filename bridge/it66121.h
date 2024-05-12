/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef __IT66121_H__
#define __IT66121_H__

#include <linux/version.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/regmap.h>
#include <drm/drm_atomic.h>
#include <drm/drm_bridge.h>
#include <drm/drm_print.h>
#include <drm/drm_edid.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_probe_helper.h>

#include "it66121_registers.h"

#define UNUSED(x) ((void)(x))

/* HW defect leading to losing 3 first bytes during EDID read operation */
#define EDID_LOSS_LEN 3

#endif /* __IT66121_H__ */
