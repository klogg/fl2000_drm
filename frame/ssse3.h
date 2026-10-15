/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FL2000_SSSE3_H
#define FL2000_SSSE3_H

/* Eight-pixel / sixteen-byte multiples, with destination wire-word aligned. */
void fl2000_ssse3_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bytes_pix);
#endif
