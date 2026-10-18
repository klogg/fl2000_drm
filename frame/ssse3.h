/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_SSSE3_H
#define FL2000_SSSE3_H

/**
 * fl2000_ssse3_convert() - Pack pixels with SSSE3 and wire reordering
 * @dst: Destination at a global wire-word boundary; CPU alignment is
 *       arbitrary.
 * @src: Unaligned XRGB8888 source with at least @pixels * 4 bytes.
 * @pixels: Pixel count, a multiple of eight.
 * @bytes_pix: Output bytes per pixel: 2 or 3.
 *
 * Writes exactly @pixels * @bytes_pix bytes. Does not check CPU features,
 * limit work duration, or enter/leave kernel FPU protection; the wrapper must.
 *
 * Context: Caller verified SSSE3 support and holds kernel FPU protection.
 */
void fl2000_ssse3_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bytes_pix);
#endif
