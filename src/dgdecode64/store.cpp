/*
 *  MPEG2Dec3 : YV12 & PostProcessing
 *
 *  Copyright (C) 2002-2003 Marc Fauconneau <marc.fd@liberysurf.fr>
 *
 *  based of the intial MPEG2Dec Copyright (C) Chia-chen Kuo - April 2001
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


#include "MPEG2Decoder.h"
//#include "postprocess.h"
#include "color_convert.h"
#include "misc.h"


// Write 2-digits numbers in a 16x16 zone.
static void write_quants(uint8_t* dst, int stride, int mb_width, int mb_height,
                         const int* qp)
{
    const uint8_t rien[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t nums[10][8] = {
        { 1, 4, 4, 4, 4, 4, 1, 0 },
        { 3, 3, 3, 3, 3, 3, 3, 0 },
        { 1, 3, 3, 1, 2, 2, 1, 0 },
        { 1, 3, 3, 1, 3, 3, 1, 0 },
        { 4, 4, 4, 1, 3, 3, 3, 0 },
        { 1, 2, 2, 1, 3, 3, 1, 0 },
        { 1, 2, 2, 1, 4, 4, 1, 0 },
        { 1, 3, 3, 3, 3, 3, 3, 0 },
        { 1, 4, 4, 1, 4, 4, 1, 0 },
        { 1, 4, 4, 1, 3, 3, 1, 0 },
    };

    auto write = [](const uint8_t* num, uint8_t* dst, const int stride) {
        for (int y = 0; y < 7; ++y) {
            if (num[y] == 1) {
                dst[1 + y * stride] = 0xFF;
                dst[2 + y * stride] = 0xFF;
                dst[3 + y * stride] = 0xFF;
                dst[4 + y * stride] = 0xFF;
            }
            if (num[y] == 2) {
                dst[1 + y * stride] = 0xFF;
            }
            if (num[y] == 3) {
                dst[4 + y * stride] = 0xFF;
            }
            if (num[y] == 4) {
                dst[1 + y * stride] = 0xFF;
                dst[4 + y * stride] = 0xFF;
            }
        }
    };

    for (int y = 0; y < mb_height; ++y) {
        for (int x = 0; x < mb_width; ++x) {
            int number = qp[x + y * mb_width];
            uint8_t* dstp = dst + static_cast<int64_t>(x) * 16 + static_cast<int64_t>(3) * stride;

            int c = (number / 100) % 10;
            const uint8_t* num = nums[c]; // x00
            if (c == 0) num = rien;
            write(num, dstp, stride);

            dstp += 5;
            int d = (number / 10) % 10;
            num = nums[d]; // 0x0
            if (c==0 && d==0) num = rien;
            write(num, dstp, stride);

            dstp += 5;
            num = nums[number % 10]; // 00x
            write(num, dstp, stride);
        }
        dst += static_cast<int64_t>(16) * stride;
    }
}


static void set_qparams(const int* qp, size_t mb_size, int& minquant,
                        int& maxquant, int& avgquant)
{
    int minq = qp[0], maxq = qp[0], sum = qp[0];
    for (size_t i = 1; i < mb_size; ++i) {
        int q = qp[i];
        if (q < minq) minq = q;
        if (q > maxq) maxq = q;
        sum += q;
    }
    minquant = minq;
    maxquant = maxq;
    avgquant = static_cast<int>(static_cast<float>(sum) / mb_size + 0.5f);
}


void CMPEG2Decoder::assembleFrame(uint8_t* src[], int pf, YV12PICT& dst)
{
    dst.pf = pf;
    fast_copy(src[0], Coded_Picture_Width, dst.y, dst.ypitch, Coded_Picture_Width, Coded_Picture_Height);
    fast_copy(src[1], Chroma_Width, dst.u, dst.uvpitch, Chroma_Width, Chroma_Height);
    fast_copy(src[2], Chroma_Width, dst.v, dst.uvpitch, Chroma_Width, Chroma_Height);

    if (info == 1 || info == 2 || showQ) {
        // Re-order quant data for display order.
        const int* qp = (picture_coding_type == B_TYPE) ? auxQP : backwardQP;
        if (info == 1 || info == 2) {
            set_qparams(qp, static_cast<int64_t>(mb_width) * mb_height, minquant, maxquant, avgquant);
        }
        if (showQ) {
            write_quants(dst.y, dst.ypitch, mb_width, mb_height, qp);
        }
    }
}
