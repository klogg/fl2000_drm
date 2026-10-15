// SPDX-License-Identifier: GPL-2.0
/* Integer SIMD only. Callers must hold kernel FPU protection. */
#include <linux/types.h>

#include "ssse3.h"

typedef long long v2di __attribute__((vector_size(16)));
typedef char v16qi __attribute__((vector_size(16)));
typedef int v4si __attribute__((vector_size(16)));
typedef unsigned int v4su __attribute__((vector_size(16)));
typedef unsigned short v8hu __attribute__((vector_size(16)));

static v16qi load(const u8 *src)
{
	v16qi value;

	__builtin_memcpy(&value, src, 16);
	return value;
}

static void store(u8 *dst, v16qi value)
{
	__builtin_memcpy(dst, &value, 16);
}

void fl2000_ssse3_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bytes_pix)
{
	const v16qi rgb = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1 };
	const v16qi wire = { 4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11 };
	unsigned int i;

	for (i = 0; i < pixels; i += 8, src += 32) {
		v16qi a = load(src), b = load(src + 16), out;

		if (bytes_pix == 3) {
			a = __builtin_ia32_pshufb128(a, rgb);
			b = __builtin_ia32_pshufb128(b, rgb);
			out = a | (v16qi)__builtin_ia32_pslldqi128((v2di)b, 96);
			out = __builtin_ia32_pshufb128(out, wire);
			store(dst, out);
			out = (v16qi)__builtin_ia32_psrldqi128((v2di)b, 32);
			out = __builtin_ia32_pshufb128(out, wire);
			__builtin_memcpy(dst + 16, &out, 8);
			dst += 24;
		} else {
			v4su pa = (v4su)a, pb = (v4su)b;
			const v4su bias = { 32768, 32768, 32768, 32768 };
			const v8hu sign = { 0x8000, 0x8000, 0x8000, 0x8000,
					    0x8000, 0x8000, 0x8000, 0x8000 };

			pa = ((pa >> 8) & 0xf800) | ((pa >> 5) & 0x7e0) | ((pa >> 3) & 0x1f);
			pb = ((pb >> 8) & 0xf800) | ((pb >> 5) & 0x7e0) | ((pb >> 3) & 0x1f);
			out = (v16qi)__builtin_ia32_packssdw128((v4si)(pa - bias),
								(v4si)(pb - bias));
			out = (v16qi)((v8hu)out ^ sign);
			store(dst, __builtin_ia32_pshufb128(out, wire));
			dst += 16;
		}
	}
}
