/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_COMPAT_DRM_ATOMIC_H
#define FL2000_COMPAT_DRM_ATOMIC_H

#include <linux/version.h>

/* Linux 7.3 renamed the transaction container, not the per-object states. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0)
#define FL2000_DRM_LEGACY_ATOMIC
#define drm_atomic_commit drm_atomic_state
#endif

#endif
