/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_BMI2_H
#define FL2000_BMI2_H

/**
 * fl2000_bmi2_convert() - Pack pixels with PEXT and wire reordering
 * @dst: Destination at a global wire-word boundary; CPU alignment is
 *       arbitrary.
 * @src: Unaligned XRGB8888 source with at least @pixels * 4 bytes.
 * @pixels: Multiple of eight for RGB888 or four for RGB565.
 * @bytes_pix: Output bytes per pixel: 2 or 3.
 *
 * Writes exactly @pixels * @bytes_pix bytes. The wrapper checks CPU support
 * and bounds work duration. PEXT uses general-purpose registers, so no FPU
 * protection is needed.
 *
 * Context: Caller verified BMI2 support.
 */
void fl2000_bmi2_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bytes_pix);
#endif
