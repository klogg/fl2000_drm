/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_FRAME_H
#define FL2000_FRAME_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>

#ifdef FL2000_FRAME_ACCELERATED
/**
 * fl2000_frame_convert() - Convert an accelerated XRGB8888 prefix
 * @dst: Destination beginning at a global eight-byte wire-word boundary.
 * @src: Source pixels in XRGB8888 memory order; may be unaligned.
 * @pixels: Maximum available source pixels.
 * @bytes_pix: Wire bytes per pixel: 2 for RGB565 or 3 for RGB888.
 *
 * Combines pixel conversion with swapping the two dwords in each wire word.
 * Backends must process complete wire words, stay within the supplied span,
 * and bound any non-preemptible or FPU-protected section. Release all SIMD/FPU
 * protection before returning so the caller can reschedule. Returning zero
 * selects the caller's portable conversion, including edges and runtime fallback.
 *
 * Context: Process context. Returns with no FPU protection held.
 * Return: Pixels consumed, at most @pixels, or 0 to request portable
 *         handling.
 */
unsigned int fl2000_frame_convert(u8 *dst, const u8 *src, unsigned int pixels,
				  unsigned int bytes_pix);
#else
static inline unsigned int fl2000_frame_convert(u8 *dst __always_unused,
						const u8 *src __always_unused,
						unsigned int pixels __always_unused,
						unsigned int bytes_pix __always_unused)
{
	return 0;
}

#endif
#endif
