/*
 *  Misc Stuff for MPEG2Dec3
 *
 *  Copyright (C) 2002-2003 Marc Fauconneau <marc.fd@liberysurf.fr>
 *
 *  This file is part of MPEG2Dec3, a free MPEG-2 decoder
 *
 *  MPEG2Dec3 is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  MPEG2Dec3 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <cstdio>
#ifdef _WIN32
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <avisynth.h>
#include "win_import_min.h"
#endif
#include "misc.h"


size_t __cdecl dprintf(char* fmt, ...)
{
    char printString[1024];

    va_list argp;

    va_start(argp, fmt);
    vsprintf_s(printString, 1024, fmt, argp);
    va_end(argp);
    OutputDebugString(printString);
    return strlen(printString);
}


void __stdcall
fast_copy(const uint8_t* src, const int src_stride, uint8_t* dst,
          const int dst_stride, const int horizontal_size, int vertical_size) noexcept
{
    if (vertical_size == 0) {
        return;
    } else if (horizontal_size == src_stride && src_stride == dst_stride) {
        memcpy(dst, src, static_cast<int64_t>(horizontal_size) * vertical_size);
    } else {
        do {
            memcpy(dst, src, horizontal_size);
            dst += dst_stride;
            src += src_stride;
        } while (--vertical_size != 0);
    }
}


#ifdef _WIN32
enum {
    CPU_NO_X86_SIMD          = 0x000000,
    CPU_SSE2_SUPPORT         = 0x000001,
    CPU_SSE3_SUPPORT         = 0x000002,
    CPU_SSSE3_SUPPORT        = 0x000004,
    CPU_SSE4_1_SUPPORT       = 0x000008,
    CPU_SSE4_2_SUPPORT       = 0x000010,
    CPU_SSE4_A_SUPPORT       = 0x000020,
    CPU_FMA4_SUPPORT         = 0x000040,
    CPU_FMA3_SUPPORT         = 0x000080,
    CPU_AVX_SUPPORT          = 0x000100,
    CPU_AVX2_SUPPORT         = 0x000200,
    CPU_AVX512F_SUPPORT      = 0x000400,
    CPU_AVX512DQ_SUPPORT     = 0x000800,
    CPU_AVX512IFMA52_SUPPORT = 0x001000,
    CPU_AVX512PF_SUPPORT     = 0x002000,
    CPU_AVX512ER_SUPPORT     = 0x004000,
    CPU_AVX512CD_SUPPORT     = 0x008000,
    CPU_AVX512BW_SUPPORT     = 0x010000,
    CPU_AVX512VL_SUPPORT     = 0x020000,
    CPU_AVX512VBMI_SUPPORT   = 0x040000,
};


static __forceinline bool is_bit_set(int bitfield, int bit) noexcept
{
    return (bitfield & (1 << bit)) != 0;
}

static uint32_t get_simd_support_info(void) noexcept
{
    uint32_t ret = 0;
    int regs[4] = {0};

    __cpuid(regs, 0x00000001);
    if (is_bit_set(regs[3], 26)) {
        ret |= CPU_SSE2_SUPPORT;
    }
    if (is_bit_set(regs[2], 0)) {
        ret |= CPU_SSE3_SUPPORT;
    }
    if (is_bit_set(regs[2], 9)) {
        ret |= CPU_SSSE3_SUPPORT;
    }
    if (is_bit_set(regs[2], 19)) {
        ret |= CPU_SSE4_1_SUPPORT;
    }
    if (is_bit_set(regs[2], 26)) {
        ret |= CPU_SSE4_2_SUPPORT;
    }
    if (is_bit_set(regs[2], 27)) {
        if (is_bit_set(regs[2], 28)) {
            ret |= CPU_AVX_SUPPORT;
        }
        if (is_bit_set(regs[2], 12)) {
            ret |= CPU_FMA3_SUPPORT;
        }
    }

    regs[3] = 0;
    __cpuid(regs, 0x80000001);
    if (is_bit_set(regs[3], 6)) {
        ret |= CPU_SSE4_A_SUPPORT;
    }
    if (is_bit_set(regs[3], 16)) {
        ret |= CPU_FMA4_SUPPORT;
    }

    __cpuid(regs, 0x00000000);
    if (regs[0] < 7) {
        return ret;
    }

    __cpuidex(regs, 0x00000007, 0);
    if (is_bit_set(regs[1], 5)) {
        ret |= CPU_AVX2_SUPPORT;
    }
    if (!is_bit_set(regs[1], 16)) {
        return ret;
    }

    ret |= CPU_AVX512F_SUPPORT;
    if (is_bit_set(regs[1], 17)) {
        ret |= CPU_AVX512DQ_SUPPORT;
    }
    if (is_bit_set(regs[1], 21)) {
        ret |= CPU_AVX512IFMA52_SUPPORT;
    }
    if (is_bit_set(regs[1], 26)) {
        ret |= CPU_AVX512PF_SUPPORT;
    }
    if (is_bit_set(regs[1], 27)) {
        ret |= CPU_AVX512ER_SUPPORT;
    }
    if (is_bit_set(regs[1], 28)) {
        ret |= CPU_AVX512CD_SUPPORT;
    }
    if (is_bit_set(regs[1], 30)) {
        ret |= CPU_AVX512BW_SUPPORT;
    }
    if (is_bit_set(regs[1], 31)) {
        ret |= CPU_AVX512VL_SUPPORT;
    }
    if (is_bit_set(regs[2], 1)) {
        ret |= CPU_AVX512VBMI_SUPPORT;
    }

    return ret;
}

bool has_sse2() noexcept
{
    return (get_simd_support_info() & CPU_SSE2_SUPPORT) != 0;
}

bool has_sse3() noexcept
{
    return (get_simd_support_info() & CPU_SSE4_1_SUPPORT) != 0;
}

bool has_avx2() noexcept
{
    uint32_t flags = CPU_AVX2_SUPPORT | CPU_FMA3_SUPPORT;
    return (get_simd_support_info() & flags) == flags;
}
#endif

