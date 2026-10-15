// SPDX-License-Identifier: GPL-2.0
/* PEXT uses general-purpose registers; no kernel FPU section is needed. */
#include <linux/types.h>
#include <linux/unaligned.h>
#include "bmi2.h"

static u64 fl2000_extract(u64 value, u64 mask)
{
	return __builtin_ia32_pext_di(value, mask);
}

static void fl2000_wire_word(u8 *dst, u64 value)
{
	put_unaligned_le64((value << 32) | (value >> 32), dst);
}

void fl2000_bmi2_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bytes_pix)
{
	unsigned int i;

	if (bytes_pix == 3) {
		const u64 mask = 0x00ffffff00ffffffULL;

		for (i = 0; i < pixels; i += 8, src += 32, dst += 24) {
			u64 a = fl2000_extract(get_unaligned_le64(src), mask);
			u64 b = fl2000_extract(get_unaligned_le64(src + 8), mask);
			u64 c = fl2000_extract(get_unaligned_le64(src + 16), mask);
			u64 d = fl2000_extract(get_unaligned_le64(src + 24), mask);

			fl2000_wire_word(dst, a | (b << 48));
			fl2000_wire_word(dst + 8, (b >> 16) | (c << 32));
			fl2000_wire_word(dst + 16, (c >> 32) | (d << 16));
		}
	} else {
		const u64 mask = 0x00f8fcf800f8fcf8ULL;

		for (i = 0; i < pixels; i += 4, src += 16, dst += 8) {
			u64 a = fl2000_extract(get_unaligned_le64(src), mask);
			u64 b = fl2000_extract(get_unaligned_le64(src + 8), mask);

			fl2000_wire_word(dst, a | (b << 32));
		}
	}
}
