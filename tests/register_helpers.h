/* SPDX-License-Identifier: GPL-2.0 */
/* Userspace equivalents of the kernel's unsigned register field operations. */
#ifndef FL2000_TEST_REGISTER_HELPERS_H
#define FL2000_TEST_REGISTER_HELPERS_H

#include <stdint.h>

#ifndef BIT
#define BIT(n) (UINT32_C(1) << (n))
#endif
#define GENMASK(h, l)		((UINT32_MAX << (l)) & (UINT32_MAX >> (31 - (h))))
#define BIT_U32(n) BIT(n)
#define GENMASK_U32(h, l) GENMASK(h, l)
#define FIELD_PREP(mask, value) (((uint32_t)(value) << __builtin_ctz(mask)) & (uint32_t)(mask))
#define FIELD_MAX(mask) ((uint32_t)(mask) >> __builtin_ctz(mask))
#define FIELD_GET(mask, value)	(((uint32_t)(value) & (uint32_t)(mask)) >> __builtin_ctz(mask))

#endif
