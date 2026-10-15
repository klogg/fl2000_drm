/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_FRAME_H
#define FL2000_FRAME_H

/*
 * dst starts at an eight-byte wire boundary; src may be unaligned.
 * Convert consumes XRGB8888 pixels and produces RGB888/RGB565 wire bytes.
 * Reorder consumes native-format bytes. Backends return the number consumed,
 * in complete eight-byte wire words, or zero to request scalar processing.
 * Each call must release SIMD protection before returning; callers can sleep.
 */
#ifdef FL2000_FRAME_ACCELERATED
unsigned int fl2000_frame_convert(u8 *dst, const u8 *src, unsigned int pixels,
				  unsigned int bytes_pix);
unsigned int fl2000_frame_reorder(u8 *dst, const u8 *src, unsigned int bytes);
#else
static inline unsigned int fl2000_frame_convert(u8 *dst, const u8 *src, unsigned int pixels,
						unsigned int bytes_pix)
{
	return 0;
}
static inline unsigned int fl2000_frame_reorder(u8 *dst, const u8 *src, unsigned int bytes)
{
	return 0;
}
#endif
#endif
