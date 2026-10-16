/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef __IT66121_H__
#define __IT66121_H__

struct drm_bridge;
struct drm_connector;
struct module;

/* Component handoff from the USB-attached bridge to the FL2000 DRM master. */
struct fl2000_bridge {
	struct drm_bridge *bridge;
	struct module *owner;
	struct drm_connector **connector;
	/* Return zero after status processing, or a negative errno for retry. */
	int (*interrupt)(struct drm_bridge *bridge);
};

#endif /* __IT66121_H__ */
