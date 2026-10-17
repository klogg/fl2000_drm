/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef FL2000_BRIDGE_H
#define FL2000_BRIDGE_H

struct drm_bridge;
struct module;

/* Component handoff from the USB-attached bridge to the FL2000 DRM master. */
struct fl2000_bridge {
	struct drm_bridge *bridge;
	struct module *owner;
	/* IRQ resource supplied by the master before component binding. */
	int irq;
};

#endif /* FL2000_BRIDGE_H */
