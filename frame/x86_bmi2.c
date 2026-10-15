// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/cpufeature.h>
#include "../fl2000_frame.h"
#include "bmi2.h"

unsigned int fl2000_frame_convert(u8 *dst, const u8 *src, unsigned int pixels,
				  unsigned int bytes_pix)
{
	unsigned int count = min(pixels & ~7U, 4096U);

	if (count < 32 || !boot_cpu_has(X86_FEATURE_BMI2))
		return 0;
	fl2000_bmi2_convert(dst, src, count, bytes_pix);
	return count;
}

unsigned int fl2000_frame_reorder(u8 *dst __always_unused, const u8 *src __always_unused,
				  unsigned int bytes __always_unused)
{
	return 0;
}
