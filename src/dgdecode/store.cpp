/*
 *  MPEG2Dec3 : YV12 & PostProcessing
 *
 *	Copyright (C) 2002-2003 Marc Fauconneau <marc.fd@liberysurf.fr>
 *
 *	based of the intial MPEG2Dec Copyright (C) Chia-chen Kuo - April 2001
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

//#define MPEG2DEC_EXPORTS
#include "global.h"
#include "postprocess.h"
#include "AvisynthAPI.h"

#ifdef uc  // its defined in AvisynthAPI.h, need our own def here
#undef uc
#endif

#define uc uint8_t

// Write 2-digits numbers in a 16x16 zone.
__inline void MBnum(uc* dst, int stride, int number) 
{
	int y,c,d;
	uc cc = 255;
	uc hc = 128;

	uc rien[7] = { 0,0,0,0,0,0,0 };
	uc num0[7] = { 1,4,4,4,4,4,1 };
	uc num1[7] = { 3,3,3,3,3,3,3 };
	uc num2[7] = { 1,3,3,1,2,2,1 };
	uc num3[7] = { 1,3,3,1,3,3,1 };
	uc num4[7] = { 4,4,4,1,3,3,3 };
	uc num5[7] = { 1,2,2,1,3,3,1 };
	uc num6[7] = { 1,2,2,1,4,4,1 };
	uc num7[7] = { 1,3,3,3,3,3,3 };
	uc num8[7] = { 1,4,4,1,4,4,1 };
	uc num9[7] = { 1,4,4,1,3,3,1 };
	uc* nums[10] = {num0,num1,num2,num3,num4,num5,num6,num7,num8,num9};
	uc* num;

	dst += 3*stride;
	for (y=0;y<7;y++) {
		if (y==0 || y==3 || y==6) {
				dst[2+y*stride] = hc;
				dst[3+y*stride] = hc;
				//
				dst[5+2+y*stride] = hc;
				dst[5+3+y*stride] = hc;
				//
				dst[10+2+y*stride] = hc;
				dst[10+3+y*stride] = hc;
		}
		dst[1+y*stride] = hc;
		dst[4+y*stride] = hc;
		//
		dst[5+1+y*stride] = hc;
		dst[5+4+y*stride] = hc;
		//
		dst[10+1+y*stride] = hc;
		dst[10+4+y*stride] = hc;
	}

	c = (number/100)%10;
	num = nums[c]; // x00
	if (c==0) num = rien;
	for (y=0;y<7;y++) {
		if (num[y] == 1) { // --
			dst[1+y*stride] = cc;
			dst[2+y*stride] = cc;
			dst[3+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
		if (num[y] == 2) { // |x
			dst[1+y*stride] = cc;
		}
		if (num[y] == 3) { // x|
			dst[4+y*stride] = cc;
		}
		if (num[y] == 4) { // ||
			dst[1+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
	}
	dst += 5;
	d = (number/10)%10;
	num = nums[d]; // 0x0
	if (c==0 && d==0) num = rien;
	for (y=0;y<7;y++) {
		if (num[y] == 1) { // --
			dst[1+y*stride] = cc;
			dst[2+y*stride] = cc;
			dst[3+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
		if (num[y] == 2) { // |x
			dst[1+y*stride] = cc;
		}
		if (num[y] == 3) { // x|
			dst[4+y*stride] = cc;
		}
		if (num[y] == 4) { // ||
			dst[1+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
	}
	dst += 5;
	num = nums[number%10]; // 00x
	for (y=0;y<7;y++) {
		if (num[y] == 1) { // --
			dst[1+y*stride] = cc;
			dst[2+y*stride] = cc;
			dst[3+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
		if (num[y] == 2) { // |x
			dst[1+y*stride] = cc;
		}
		if (num[y] == 3) { // x|
			dst[4+y*stride] = cc;
		}
		if (num[y] == 4) { // ||
			dst[1+y*stride] = cc;
			dst[4+y*stride] = cc;
		}
	}
}

void CMPEG2Decoder::assembleFrame(unsigned char *src[], int pf, YV12PICT *dst)
{
	#ifdef PROFILING
		start_timer();
	#endif

	dst->pf = pf;

	if (Fault_Flag) Fault_Flag = 0;

	if (pp_mode != 0)
	{
		uc* ppptr[3];
		if (!(upConv && chroma_format == 1))
		{
			ppptr[0] = dst->y;
			ppptr[1] = dst->u;
			ppptr[2] = dst->v;
		}
		else
		{
			ppptr[0] = dst->y;
			ppptr[1] = u422;
			ppptr[2] = v422;
		}
		if (iPP == 1 || (iPP == -1 && pf == 0)) 
		{	// Field Based PP
			postprocess(src, this->Coded_Picture_Width*2, ppptr, this->Coded_Picture_Width*2, this->Coded_Picture_Width*2,
				this->Coded_Picture_Height/2, this->QP, this->mb_width, pp_mode, moderate_h, moderate_v, chroma_format == 1 ? false : true);
		}
		else 
		{	// Image Based PP
			postprocess(src, this->Coded_Picture_Width, ppptr, this->Coded_Picture_Width, this->Coded_Picture_Width,
				this->Coded_Picture_Height, this->QP, this->mb_width, pp_mode, moderate_h, moderate_v, chroma_format == 1 ? false : true);
		}
		if (upConv && chroma_format == 1)
		{
			conv420to422(ppptr[1],dst->u,pf,dst->ypitch,Coded_Picture_Height);
			conv420to422(ppptr[2],dst->v,pf,dst->ypitch,Coded_Picture_Height);
		}
	} 
	else 
	{
		YV12PICT psrc;
		psrc.y = src[0]; psrc.u = src[1]; psrc.v = src[2];
		psrc.ypitch = Coded_Picture_Width; 
		psrc.uvpitch = Chroma_Width;
		if (upConv && chroma_format == 1)
		{
			AVSenv->BitBlt(dst->y,dst->ypitch,psrc.y,psrc.ypitch,dst->ypitch,Coded_Picture_Height);
			conv420to422(psrc.u,dst->u,pf,dst->ypitch,Coded_Picture_Height);
			conv420to422(psrc.v,dst->v,pf,dst->ypitch,Coded_Picture_Height);
		}
		else Copyall(&psrc,dst);
	}

	if (showQ)
	{
		int x, y;
		for(y=0; y<this->mb_height; y++) 
		{
			for(x=0;x<this->mb_width; x++) 
			{
				MBnum(&dst->y[x*16+y*16*this->Coded_Picture_Width],this->Coded_Picture_Width,QP[x+y*this->mb_width]);
			}
		}
	}

	#ifdef PROFILING
		stop_timer(&tim.post);
		start_timer();
	#endif
}