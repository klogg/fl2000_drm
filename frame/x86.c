// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include "../fl2000_frame.h"
#include "ssse3.h"

unsigned int fl2000_frame_convert(u8 *dst, const u8 *src,
				 unsigned int pixels, unsigned int bytes_pix)
{
	unsigned int count = min(pixels & ~7U, 4096U);

	if (count < 32 || !boot_cpu_has(X86_FEATURE_SSSE3) || !irq_fpu_usable())
		return 0;
	kernel_fpu_begin();
	fl2000_ssse3_convert(dst, src, count, bytes_pix);
	kernel_fpu_end();
	return count;
}

unsigned int fl2000_frame_reorder(u8 *dst, const u8 *src, unsigned int bytes)
{
	/* Native reordering showed no repeatable benefit in the complete workload. */
	(void)dst;
	(void)src;
	(void)bytes;
	return 0;
}
