/* adler32_simd.c
 *
 * (C) 1995-2013 Jean-loup Gailly and Mark Adler
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Jean-loup Gailly        Mark Adler
 * jloup@gzip.org          madler@alumni.caltech.edu
 *
 * Copyright 2017 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 *
 * Per http://en.wikipedia.org/wiki/Adler-32 the adler32 A value (aka s1) is
 * the sum of N input data bytes D1 ... DN,
 *
 *   A = A0 + D1 + D2 + ... + DN
 *
 * where A0 is the initial value.
 *
 * SSE2 _mm_sad_epu8() can be used for byte sums (see http://bit.ly/2wpUOeD,
 * for example) and accumulating the byte sums can use SSE shuffle-adds (see
 * the "Integer" section of http://bit.ly/2erPT8t for details). Arm NEON has
 * similar instructions.
 *
 * The adler32 B value (aka s2) sums the A values from each step:
 *
 *   B0 + (A0 + D1) + (A0 + D1 + D2) + ... + (A0 + D1 + D2 + ... + DN) or
 *
 *       B0 + N.A0 + N.D1 + (N-1).D2 + (N-2).D3 + ... + (N-(N-1)).DN
 *
 * B0 being the initial value. For 32 bytes (ideal for garden-variety SIMD):
 *
 *   B = B0 + 32.A0 + [D1 D2 D3 ... D32] x [32 31 30 ... 1].
 *
 * Adjacent blocks of 32 input bytes can be iterated with the expressions to
 * compute the adler32 s1 s2 of M >> 32 input bytes [1].
 *
 * As M grows, the s1 s2 sums grow. If left unchecked, they would eventually
 * overflow the precision of their integer representation (bad). However, s1
 * and s2 also need to be computed modulo the adler BASE value (reduced). If
 * at most NMAX bytes are processed before a reduce, s1 s2 _cannot_ overflow
 * a uint32_t type (the NMAX constraint) [2].
 *
 * [1] the iterative equations for s2 contain constant factors; these can be
 * hoisted from the n-blocks do loop of the SIMD code.
 *
 * [2] zlib adler32_z() uses this fact to implement NMAX-block-based updates
 * of the adler s1 s2 of uint32_t type (see adler32.c).
 */

#include "adler32_simd.h"

/* Definitions from adler32.c: largest prime smaller than 65536 */
#define BASE 65521U
/* NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 */
#define NMAX 5552

#if defined(ADLER32_SIMD_SSSE3)

#include <tmmintrin.h>

uint32_t ZLIB_INTERNAL adler32_simd_(  /* SSSE3 */
    uint32_t adler,
    const unsigned char *buf,
    unsigned long len)
{
    /*
     * Split Adler-32 into component sums.
     */
    uint32_t s1 = adler & 0xffff;
    uint32_t s2 = adler >> 16;

    /*
     * Process the data in blocks.
     */
    const unsigned BLOCK_SIZE = 1 << 5;

    unsigned long blocks = len / BLOCK_SIZE;
    len -= blocks * BLOCK_SIZE;

    while (blocks)
    {
        unsigned n = NMAX / BLOCK_SIZE;  /* The NMAX constraint. */
        if (n > blocks)
            n = (unsigned) blocks;
        blocks -= n;

        const __m128i tap1 =
            _mm_setr_epi8(32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17);
        const __m128i tap2 =
            _mm_setr_epi8(16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
        const __m128i zero =
            _mm_setr_epi8( 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        const __m128i ones =
            _mm_set_epi16( 1, 1, 1, 1, 1, 1, 1, 1);

        /*
         * Process n blocks of data. At most NMAX data bytes can be
         * processed before s2 must be reduced modulo BASE.
         */
        __m128i v_ps = _mm_set_epi32(0, 0, 0, s1 * n);
        __m128i v_s2 = _mm_set_epi32(0, 0, 0, s2);
        __m128i v_s1 = _mm_set_epi32(0, 0, 0, 0);

        do {
            /*
             * Load 32 input bytes.
             */
            const __m128i bytes1 = _mm_loadu_si128((__m128i*)(buf));
            const __m128i bytes2 = _mm_loadu_si128((__m128i*)(buf + 16));

            /*
             * Add previous block byte sum to v_ps.
             */
            v_ps = _mm_add_epi32(v_ps, v_s1);

            /*
             * Horizontally add the bytes for s1, multiply-adds the
             * bytes by [ 32, 31, 30, ... ] for s2.
             */
            v_s1 = _mm_add_epi32(v_s1, _mm_sad_epu8(bytes1, zero));
            const __m128i mad1 = _mm_maddubs_epi16(bytes1, tap1);
            v_s2 = _mm_add_epi32(v_s2, _mm_madd_epi16(mad1, ones));

            v_s1 = _mm_add_epi32(v_s1, _mm_sad_epu8(bytes2, zero));
            const __m128i mad2 = _mm_maddubs_epi16(bytes2, tap2);
            v_s2 = _mm_add_epi32(v_s2, _mm_madd_epi16(mad2, ones));

            buf += BLOCK_SIZE;

        } while (--n);

        v_s2 = _mm_add_epi32(v_s2, _mm_slli_epi32(v_ps, 5));

        /*
         * Sum epi32 ints v_s1(s2) and accumulate in s1(s2).
         */

#define S23O1 _MM_SHUFFLE(2,3,0,1)  /* A B C D -> B A D C */
#define S1O32 _MM_SHUFFLE(1,0,3,2)  /* A B C D -> C D A B */

        v_s1 = _mm_add_epi32(v_s1, _mm_shuffle_epi32(v_s1, S23O1));
        v_s1 = _mm_add_epi32(v_s1, _mm_shuffle_epi32(v_s1, S1O32));

        s1 += _mm_cvtsi128_si32(v_s1);

        v_s2 = _mm_add_epi32(v_s2, _mm_shuffle_epi32(v_s2, S23O1));
        v_s2 = _mm_add_epi32(v_s2, _mm_shuffle_epi32(v_s2, S1O32));

        s2 = _mm_cvtsi128_si32(v_s2);

#undef S23O1
#undef S1O32

        /*
         * Reduce.
         */
        s1 %= BASE;
        s2 %= BASE;
    }

    /*
     * Handle leftover data.
     */
    if (len) {
        if (len >= 16) {
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            len -= 16;
        }

        while (len--) {
            s2 += (s1 += *buf++);
        }

        if (s1 >= BASE)
            s1 -= BASE;
        s2 %= BASE;
    }

    /*
     * Return the recombined sums.
     */
    return s1 | (s2 << 16);
}

#elif defined(ADLER32_SIMD_NEON)

/* __APPLE__ is insufficent, as older iOS devices will not support UDOT,
   however all NEON-supporting macOS devices will. */
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_MAC
#define ADLER32_SIMD_NEON_UDOT
#define __ARM_FEATURE_DOTPROD
#endif
#endif

#include <arm_neon.h>

#ifdef ADLER32_SIMD_NEON_UDOT

uint32_t ZLIB_INTERNAL adler32_simd_(  /* NEON */
    uint32_t adler,
    const unsigned char *buf,
    unsigned long len)
{
    /*
     * Split Adler-32 into component sums.
     */
    uint64_t s1 = adler & 0xffff;
    uint64_t s2 = adler >> 16;

    /*
     * Serially compute s1 & s2, until the data is 16-byte aligned.
     */
    if ((uintptr_t)buf & 15) {
        while ((uintptr_t)buf & 15) {
            s2 += (s1 += *buf++);
            --len;
        }

        if (s1 >= BASE)
            s1 -= BASE;
        s2 %= BASE;
    }

    /*
     * Process the data in blocks.
     */
    const unsigned BLOCK_SIZE = 1 << 6;

    unsigned long blocks = len / BLOCK_SIZE;
    len -= blocks * BLOCK_SIZE;

    while (blocks)
    {
        unsigned n = 2902; /* Maximum blocks. */
        if (n > blocks)
            n = blocks;
        blocks -= n;

        /*
         * Process n blocks of data. At most 2902 blocks can be
         * processed before s2 must be reduced modulo BASE. This
         * is greater than NMAX bytes as we're using 64-bit
         * integers.
         */
        const unsigned char MULTIPLIERS[0x40] = {
            0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39,
            0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
            0x30, 0x2f, 0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x29,
            0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
            0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19,
            0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
            0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09,
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        };
        uint8x16x4_t mul = vld1q_u8_x4(MULTIPLIERS);

        uint32x4_t accs[4] = { 0 };
        uint32x4_t sums[4] = { 0 };
        uint32x4_t extras[4] = { 0 };

        const unsigned char *end = buf + n * BLOCK_SIZE;
        do {
            /*
             * Load 64 input bytes.
             */
            uint8x16x4_t raw = vld1q_u8_x4(buf);
            buf += BLOCK_SIZE;

            for (int i = 0; i < 4; i++) {
                accs[i] = vaddq_u32(accs[i], sums[i]);
                sums[i] = vdotq_u32(sums[i], raw.val[i], vdupq_n_u8(1));
                extras[i] = vdotq_u32(extras[i], raw.val[i], mul.val[i]);
            }
        } while (buf != end);

        for (int i = 1; i < 4; i++) {
            extras[0] = vaddq_u32(extras[0], extras[i]);
            sums[0] = vaddq_u32(sums[0], sums[i]);
        }

        uint64_t acc = 0;
        for (int i = 0; i < 4; i++) {
            acc += vaddlvq_u32(accs[i]);
        }
        uint64_t extra = vaddlvq_u32(extras[0]);
        uint64_t sum = vaddlvq_u32(sums[0]);

        s2 += s1 * n * BLOCK_SIZE + acc * BLOCK_SIZE + extra;
        s1 += sum;

        /*
         * Reduce.
         */
        s1 %= BASE;
        s2 %= BASE;
    }

    /*
     * Handle leftover data.
     */
    if (len) {
        while (len >= 16) {
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            len -= 16;
        }

        while (len--) {
            s2 += (s1 += *buf++);
        }

        if (s1 >= BASE)
            s1 -= BASE;
        s2 %= BASE;
    }

    /*
     * Return the recombined sums.
     */
    return s1 | (s2 << 16);
}

#else  /* ADLER32_SIMD_NEON_UDOT */


uint32_t ZLIB_INTERNAL adler32_simd_(  /* NEON */
    uint32_t adler,
    const unsigned char *buf,
    unsigned long len)
{
    /*
     * Split Adler-32 into component sums.
     */
    uint32_t s1 = adler & 0xffff;
    uint32_t s2 = adler >> 16;

    /*
     * Serially compute s1 & s2, until the data is 16-byte aligned.
     */
    if ((uintptr_t)buf & 15) {
        while ((uintptr_t)buf & 15) {
            s2 += (s1 += *buf++);
            --len;
        }

        if (s1 >= BASE)
            s1 -= BASE;
        s2 %= BASE;
    }

    /*
     * Process the data in blocks.
     */
    const unsigned BLOCK_SIZE = 1 << 5;

    unsigned long blocks = len / BLOCK_SIZE;
    len -= blocks * BLOCK_SIZE;

    while (blocks)
    {
        unsigned n = NMAX / BLOCK_SIZE;  /* The NMAX constraint. */
        if (n > blocks)
            n = blocks;
        blocks -= n;

        /*
         * Process n blocks of data. At most NMAX data bytes can be
         * processed before s2 must be reduced modulo BASE.
         */
        uint32x4_t v_s2 = (uint32x4_t) { 0, 0, 0, s1 * n };
        uint32x4_t v_s1 = (uint32x4_t) { 0, 0, 0, 0 };

        uint16x8_t v_column_sum_1 = vdupq_n_u16(0);
        uint16x8_t v_column_sum_2 = vdupq_n_u16(0);
        uint16x8_t v_column_sum_3 = vdupq_n_u16(0);
        uint16x8_t v_column_sum_4 = vdupq_n_u16(0);

        do {
            /*
             * Load 32 input bytes.
             */
            const uint8x16_t bytes1 = vld1q_u8((uint8_t*)(buf));
            const uint8x16_t bytes2 = vld1q_u8((uint8_t*)(buf + 16));

            /*
             * Add previous block byte sum to v_s2.
             */
            v_s2 = vaddq_u32(v_s2, v_s1);

            /*
             * Horizontally add the bytes for s1.
             */
            v_s1 = vpadalq_u16(v_s1, vpadalq_u8(vpaddlq_u8(bytes1), bytes2));

            /*
             * Vertically add the bytes for s2.
             */
            v_column_sum_1 = vaddw_u8(v_column_sum_1, vget_low_u8 (bytes1));
            v_column_sum_2 = vaddw_u8(v_column_sum_2, vget_high_u8(bytes1));
            v_column_sum_3 = vaddw_u8(v_column_sum_3, vget_low_u8 (bytes2));
            v_column_sum_4 = vaddw_u8(v_column_sum_4, vget_high_u8(bytes2));

            buf += BLOCK_SIZE;

        } while (--n);

        v_s2 = vshlq_n_u32(v_s2, 5);

        /*
         * Multiply-add bytes by [ 32, 31, 30, ... ] for s2.
         */
        v_s2 = vmlal_u16(v_s2, vget_low_u16 (v_column_sum_1),
            (uint16x4_t) { 32, 31, 30, 29 });
        v_s2 = vmlal_u16(v_s2, vget_high_u16(v_column_sum_1),
            (uint16x4_t) { 28, 27, 26, 25 });
        v_s2 = vmlal_u16(v_s2, vget_low_u16 (v_column_sum_2),
            (uint16x4_t) { 24, 23, 22, 21 });
        v_s2 = vmlal_u16(v_s2, vget_high_u16(v_column_sum_2),
            (uint16x4_t) { 20, 19, 18, 17 });
        v_s2 = vmlal_u16(v_s2, vget_low_u16 (v_column_sum_3),
            (uint16x4_t) { 16, 15, 14, 13 });
        v_s2 = vmlal_u16(v_s2, vget_high_u16(v_column_sum_3),
            (uint16x4_t) { 12, 11, 10,  9 });
        v_s2 = vmlal_u16(v_s2, vget_low_u16 (v_column_sum_4),
            (uint16x4_t) {  8,  7,  6,  5 });
        v_s2 = vmlal_u16(v_s2, vget_high_u16(v_column_sum_4),
            (uint16x4_t) {  4,  3,  2,  1 });

        /*
         * Sum epi32 ints v_s1(s2) and accumulate in s1(s2).
         */
        uint32x2_t sum1 = vpadd_u32(vget_low_u32(v_s1), vget_high_u32(v_s1));
        uint32x2_t sum2 = vpadd_u32(vget_low_u32(v_s2), vget_high_u32(v_s2));
        uint32x2_t s1s2 = vpadd_u32(sum1, sum2);

        s1 += vget_lane_u32(s1s2, 0);
        s2 += vget_lane_u32(s1s2, 1);

        /*
         * Reduce.
         */
        s1 %= BASE;
        s2 %= BASE;
    }

    /*
     * Handle leftover data.
     */
    if (len) {
        if (len >= 16) {
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);
            s2 += (s1 += *buf++);

            len -= 16;
        }

        while (len--) {
            s2 += (s1 += *buf++);
        }

        if (s1 >= BASE)
            s1 -= BASE;
        s2 %= BASE;
    }

    /*
     * Return the recombined sums.
     */
    return s1 | (s2 << 16);
}

#endif  /* ADLER32_SIMD_NEON_UDOT */

#endif  /* ADLER32_SIMD_SSSE3 */
