/* Copyright (C) 1996, MPEG Software Simulation Group. All Rights Reserved. */

/*
 * Disclaimer of Warranty
 *
 * These software programs are available to the user without any license fee or
 * royalty on an "as is" basis.  The MPEG Software Simulation Group disclaims
 * any and all warranties, whether express, implied, or statuary, including any
 * implied warranties or merchantability or of fitness for a particular
 * purpose.  In no event shall the copyright-holder be liable for any
 * incidental, punitive, or consequential damages of any kind whatsoever
 * arising from the use of these programs.
 *
 * This disclaimer of warranty extends to the user of these programs and user's
 * customers, employees, agents, transferees, successors, and assigns.
 *
 * The MPEG Software Simulation Group does not represent or warrant that the
 * programs furnished hereunder are free of infringement of any third-party
 * patents.
 *
 * Commercial implementations of MPEG-1 and MPEG-2 video, including shareware,
 * are subject to royalty fees to patent holders.  Many of these patents are
 * general enough such that they are unavoidable regardless of implementation
 * design.
 *
 */

/* SSE2 optimized by Dmitry Rozhdestvensky */

#include "global.h"
#include "getbit.h"

static int cc_table[12] = {
	0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 1, 2
};

/* private prototypes*/
__forceinline static void Update_Picture_Buffers(void);
__forceinline static void picture_data(void);
__forceinline static int slice(int MBAmax);
__forceinline static void macroblock_modes(int *pmacroblock_type, int *pmotion_type,
	int *pmotion_vector_count, int *pmv_format, int *pdmv, int *pmvscale, int *pdct_type);
__forceinline static void Clear_Block(int count);
__forceinline static void Add_Block(int count, int bx, int by, int dct_type, int addflag);
__forceinline static void motion_compensation(int MBA, int macroblock_type, int motion_type,
	int PMV[2][2][2], int motion_vertical_field_select[2][2], int dmvector[2], int dct_type);
__forceinline static void skipped_macroblock(int dc_dct_pred[3], int PMV[2][2][2], 
	int *motion_type, int motion_vertical_field_select[2][2], int *macroblock_type);
__forceinline static int start_of_slice(int *MBA, int *MBAinc, int dc_dct_pred[3], int PMV[2][2][2]);
__forceinline static int decode_macroblock(int *macroblock_type, int *motion_type, int *dct_type,
	int PMV[2][2][2], int dc_dct_pred[3], int motion_vertical_field_select[2][2], int dmvector[2]);
__forceinline static void Decode_MPEG2_Intra_Block(int comp, int dc_dct_pred[]);
__forceinline static void Decode_MPEG2_Non_Intra_Block(int comp);

__forceinline static int Get_macroblock_type(void);
__forceinline static int Get_I_macroblock_type(void);
__forceinline static int Get_P_macroblock_type(void);
__forceinline static int Get_B_macroblock_type(void);
__forceinline static int Get_D_macroblock_type(void);
__forceinline static int Get_coded_block_pattern(void);
__forceinline static int Get_macroblock_address_increment(void);
__forceinline static int Get_Luma_DC_dct_diff(void);
__forceinline static int Get_Chroma_DC_dct_diff(void);

__forceinline static void form_predictions(int bx, int by, int macroblock_type, int motion_type, int PMV[2][2][2], 
	int motion_vertical_field_select[2][2], int dmvector[2]);
static void form_prediction(unsigned char *src[], int sfield, unsigned char *dst[], int dfield, 
	int lx, int lx2, int w, int h, int x, int y, int dx, int dy, int average_flag);
__forceinline static void form_component_prediction(unsigned char *src, unsigned char *dst,
	int lx, int lx2, int w, int h, int x, int y, int dx, int dy, int average_flag);
	
/* decode one frame */
struct ENTRY
{
	int lba;
	__int64 position;
	int pct;
	int trf;
	int closed;
	int vob_id;
	int cell_id;
} gop_entries[MAX_PICTURES_PER_GOP];
int gop_entries_ndx = 0;

void WriteGopLine(int finish)
{
	char buf[1024], temp[255], position[255];
	int m, r, ref;
	int had_P = 0;
	int mark = 0x10;

	// Reorder trf's for display order.
	for (m = 0, r = 0; m < gop_entries_ndx; m++)
	{
		// To speed up random access navigation, we mark frames that can
		// be accessed by decoding just the GOP that they
		// belong to. The mark is done by setting bit 0x10
		// on the trf value. This is read by MPEG2DEC3dg
		// and used to avoid having to decode the previous
		// GOP when doing random access. B frames before or
		// P frame in a non-closed GOP cannot be marked because
		// they reference a frame in the previous GOP.
		if (gop_entries[m].pct == I_TYPE)
		{
			ref = gop_entries[m].trf | mark;
		}
		else if (gop_entries[m].pct == P_TYPE)
		{
			gop_entries[r++].trf = ref;
			ref = gop_entries[m].trf | mark;
			had_P = 1;
		}
		else if (gop_entries[m].pct == B_TYPE)
		{
			if (had_P || gop_entries[m].closed)
				gop_entries[r++].trf = gop_entries[m].trf | mark;
			else
				gop_entries[r++].trf = gop_entries[m].trf;
		}
	}
	gop_entries[r++].trf = ref;

	// Write the GOP line to the D2V file.
	_i64toa(gop_entries[0].position, position, 10);
	sprintf(buf,"7 %d %s %d %d", d2v_forward.file, position, gop_entries[0].vob_id, gop_entries[0].cell_id);
	for (m = 0; m < gop_entries_ndx; m++)
	{
		sprintf(temp," %x", gop_entries[m].trf);
		strcat(buf, temp);
	}
	if (finish) strcat(buf, " 9\n");
	else strcat(buf, "\n");
	fprintf(D2VFile, "%s", buf);
	gop_entries_ndx = 0;
}

void Decode_Picture()
{
	extern int closed_gop;

	if (picture_structure==FRAME_PICTURE && Second_Field)
		Second_Field = 0;

	if (picture_coding_type!=B_TYPE)
	{
		d2v_forward = d2v_backward;
		d2v_backward = d2v_current;
	}

	// D2V file generation rewritten by Donald Graft to support IBBPBBP...
	if (D2V_Flag  && (picture_structure==FRAME_PICTURE  || !Second_Field))
	{	
		if (picture_coding_type == I_TYPE && gop_entries_ndx > 0)
		{
			WriteGopLine(0);
		}
		gop_entries[gop_entries_ndx].lba = (int) d2v_current.lba;
		gop_entries[gop_entries_ndx].position = d2v_current.position;
		gop_entries[gop_entries_ndx].pct = picture_coding_type;
		gop_entries[gop_entries_ndx].trf = d2v_current.trf;
		gop_entries[gop_entries_ndx].closed = closed_gop;
		gop_entries[gop_entries_ndx].vob_id = VOB_ID;
		gop_entries[gop_entries_ndx].cell_id = CELL_ID;
		if (gop_entries_ndx < MAX_PICTURES_PER_GOP - 1)
			gop_entries_ndx++;
		else
		{
			MessageBox(hWnd, "Too many pictures per GOP (>= 500).\nDGIndex will terminate.", NULL, MB_OK | MB_ICONERROR);
			exit(1);
		}
	}
	if (D2V_Flag)
	{
		if (Frame_Number && picture_structure==FRAME_PICTURE  || Second_Field)
		{
			if (d2v_current.type==B_TYPE)
			{
				DetectVideoType(Frame_Number-1, d2v_current.trf);
			}
			else
				switch (d2v_forward.type)
				{
					case P_TYPE:
						DetectVideoType(Frame_Number-1, d2v_forward.trf);
						break;

					case I_TYPE:
						DetectVideoType(Frame_Number-1, d2v_forward.trf);
						break;

					default:
						SetDlgItemText(hDlg, IDC_INFO, "P.E.!");
						break;
				}
		}
	}
	else if (!Decision_Flag)
	{
		/* update picture buffer pointers */
		Update_Picture_Buffers();

		/* decode picture data ISO/IEC 13818-2 section 6.2.3.7 */
		picture_data();

		/* write or display current or previously decoded reference frame */
		/* ISO/IEC 13818-2 section 6.1.1.11: Frame reordering */
		if (picture_structure == FRAME_PICTURE || Second_Field)
		{
			if (process.locate != LOCATE_RIP)
			{
				Write_Frame(backward_reference_frame, d2v_backward, 0);
				ThreadKill();
			}
			else if (Frame_Number > 0)
			{
				if (picture_coding_type==B_TYPE)
					Write_Frame(auxframe, d2v_current, Frame_Number-1);
				else
					Write_Frame(forward_reference_frame, d2v_forward, Frame_Number-1);
			}
		}
	}

	if (picture_structure!=FRAME_PICTURE)
		Second_Field = !Second_Field;

	if (!Second_Field)
		Frame_Number ++;
}

/* reuse old picture buffers as soon as they are no longer needed */
static void Update_Picture_Buffers()
{
	int cc;
	unsigned char *tmp;

	for (cc=0; cc<3; cc++)
	{
		/* B pictures do not need to be save for future reference */
		if (picture_coding_type==B_TYPE)
			current_frame[cc] = auxframe[cc];
		else
		{
			if (!Second_Field)
			{
				/* only update at the beginning of the coded frame */
				tmp = forward_reference_frame[cc];

				/* the previously decoded reference frame is stored coincident with the 
				   location where the backward reference frame is stored (backwards 
				   prediction is not needed in P pictures) */
				forward_reference_frame[cc] = backward_reference_frame[cc];

				/* update pointer for potential future B pictures */
				backward_reference_frame[cc] = tmp;
			}

			/* can erase over old backward reference frame since it is not used
			   in a P picture, and since any subsequent B pictures will use the 
			   previously decoded I or P frame as the backward_reference_frame */
			current_frame[cc] = backward_reference_frame[cc];
		}

	    if (picture_structure==BOTTOM_FIELD)
			current_frame[cc] += (cc==0) ? Coded_Picture_Width : Chroma_Width;
	}
}

/* decode all macroblocks of the current picture */
/* stages described in ISO/IEC 13818-2 section 7 */
static void picture_data()
{
	int MBAmax;

	/* number of macroblocks per picture */
	MBAmax = mb_width*mb_height;

	if (picture_structure!=FRAME_PICTURE)
		MBAmax>>=1;

	for (;;)
	{		
		if (slice(MBAmax)<0)
			return;
	}
}

/* decode all macroblocks of the current picture */
/* ISO/IEC 13818-2 section 6.3.16 */
/* return 0 : go to next slice */
/* return -1: go to next picture */
static int slice(int MBAmax)
{
	int MBA = 0, MBAinc =0, macroblock_type, motion_type, dct_type, ret;
	int dc_dct_pred[3], PMV[2][2][2], motion_vertical_field_select[2][2], dmvector[2];

	if ((ret=start_of_slice(&MBA, &MBAinc, dc_dct_pred, PMV))!=1)
		return ret;

	for (;;)
	{
		/* this is how we properly exit out of picture */
		if (MBA>=MBAmax) return -1;		// all macroblocks decoded

		if (MBAinc==0)
		{
			if (!Show_Bits(23) || Fault_Flag)	// next_start_code or fault
			{
resync:
				if (Fault_Flag)
					SetDlgItemText(hDlg, IDC_INFO, "V.E.!");

				Fault_Flag = 0;
				return 0;	// trigger: go to next slice
			}
			else /* neither next_start_code nor Fault_Flag */
			{
				/* decode macroblock address increment */
				MBAinc = Get_macroblock_address_increment();
				if (Fault_Flag) goto resync;
			}
		}

		if (MBAinc==1) /* not skipped */
		{
			if (!decode_macroblock(&macroblock_type, &motion_type, &dct_type, PMV,
				dc_dct_pred, motion_vertical_field_select, dmvector))
				goto resync;
		}
		else /* MBAinc!=1: skipped macroblock */
			/* ISO/IEC 13818-2 section 7.6.6 */
			skipped_macroblock(dc_dct_pred, PMV, &motion_type, motion_vertical_field_select, &macroblock_type);

		/* ISO/IEC 13818-2 section 7.6 */
		motion_compensation(MBA, macroblock_type, motion_type, PMV,
							motion_vertical_field_select, dmvector, dct_type);

		/* advance to next macroblock */
		MBA++; MBAinc--;

		if (MBA>=MBAmax) return -1;		// all macroblocks decoded
	}
}

/* ISO/IEC 13818-2 section 6.3.17.1: Macroblock modes */
static void macroblock_modes(int *pmacroblock_type, int *pmotion_type,
							 int *pmotion_vector_count, int *pmv_format,
							 int *pdmv, int *pmvscale, int *pdct_type)
{
	int macroblock_type, motion_type, motion_vector_count;
	int mv_format, dmv, mvscale, dct_type;

	/* get macroblock_type */
	macroblock_type = Get_macroblock_type();
	if (Fault_Flag) return;

	/* get frame/field motion type */
	if (macroblock_type & (MACROBLOCK_MOTION_FORWARD|MACROBLOCK_MOTION_BACKWARD))
	{
		if (picture_structure==FRAME_PICTURE)
			motion_type = frame_pred_frame_dct ? MC_FRAME : Get_Bits(2);
		else
			motion_type = Get_Bits(2);
    }
	else if ((macroblock_type & MACROBLOCK_INTRA) && concealment_motion_vectors)
		motion_type = (picture_structure==FRAME_PICTURE) ? MC_FRAME : MC_FIELD;
	else
	{
		// I don't know if this is right, but we can't leave it uninitialised. [DAG]
		motion_type = MC_FRAME;
	}

	/* derive motion_vector_count, mv_format and dmv, (table 6-17, 6-18) */
	if (picture_structure==FRAME_PICTURE)
	{
		motion_vector_count = (motion_type==MC_FIELD) ? 2 : 1;
		mv_format = (motion_type==MC_FRAME) ? MV_FRAME : MV_FIELD;
	}
	else
	{
		motion_vector_count = (motion_type==MC_16X8) ? 2 : 1;
		mv_format = MV_FIELD;
	}
	
	dmv = (motion_type==MC_DMV); /* dual prime */

	/*
	   field mv predictions in frame pictures have to be scaled
	   ISO/IEC 13818-2 section 7.6.3.1 Decoding the motion vectors
	*/
	mvscale = (mv_format==MV_FIELD && picture_structure==FRAME_PICTURE);

	/* get dct_type (frame DCT / field DCT) */
	dct_type = picture_structure==FRAME_PICTURE && !frame_pred_frame_dct
				&& (macroblock_type & (MACROBLOCK_PATTERN|MACROBLOCK_INTRA)) ? Get_Bits(1) : 0;

	/* return values */
	*pmacroblock_type = macroblock_type;
	*pmotion_type = motion_type;
	*pmotion_vector_count = motion_vector_count;
	*pmv_format = mv_format;
	*pdmv = dmv;
	*pmvscale = mvscale;
	*pdct_type = dct_type;
}

/* move/add 8x8-Block from block[comp] to backward_reference_frame */
/* copy reconstructed 8x8 block from block[comp] to current_frame[]
   ISO/IEC 13818-2 section 7.6.8: Adding prediction and coefficient data
   This stage also embodies some of the operations implied by:
   - ISO/IEC 13818-2 section 7.6.7: Combining predictions
   - ISO/IEC 13818-2 section 6.1.3: Macroblock
*/
static void Add_Block(int count, int bx, int by, int dct_type, int addflag)
{
	static const __int64 mmmask_128 = 0x0080008000800080;

	int comp, cc, iincr, bxh, byh;
	unsigned char *rfp;
	short *Block_Ptr;

	for (comp=0; comp<count; comp++)
	{
		Block_Ptr = block[comp];
		cc = cc_table[comp];

		bxh = bx; byh = by;

		if (cc==0)
		{
			if (picture_structure==FRAME_PICTURE)
			{
				if (dct_type)
				{
					rfp = current_frame[0] + Coded_Picture_Width*(by+((comp&2)>>1)) + bx + ((comp&1)<<3);
					iincr = Coded_Picture_Width<<1;
				}
				else
				{
					rfp = current_frame[0] + Coded_Picture_Width*(by+((comp&2)<<2)) + bx + ((comp&1)<<3);
					iincr = Coded_Picture_Width;
				}
			}
			else
			{
				rfp = current_frame[0] + (Coded_Picture_Width<<1)*(by+((comp&2)<<2)) + bx + ((comp&1)<<3);
				iincr = Coded_Picture_Width<<1;
			}
		}
		else
		{
			if (chroma_format!=CHROMA444)
				bxh >>= 1;
			if (chroma_format==CHROMA420)
				byh >>= 1;

			if (picture_structure==FRAME_PICTURE)
			{
				if (dct_type && chroma_format!=CHROMA420)
				{
					// field DCT coding 
					rfp = current_frame[cc] + Chroma_Width*(byh+((comp&2)>>1)) + bxh + (comp&8);
					iincr = Chroma_Width<<1;
				}
				else
				{
					// frame DCT coding 
					rfp = current_frame[cc] + Chroma_Width*(byh+((comp&2)<<2)) + bxh + (comp&8);
					iincr = Chroma_Width;
				}
			}
			else
			{
				// field picture 
				rfp = current_frame[cc] + (Chroma_Width<<1)*(byh+((comp&2)<<2)) + bxh + (comp&8);
				iincr = Chroma_Width<<1;
			}
		}

		if (cpu.sse2)	// SSE2
		{
			if (addflag)
			{
				__asm
				{
					pxor		xmm0, xmm0
					mov			eax, [rfp]
					mov			ebx, [Block_Ptr]
					mov			ecx, [iincr]
					mov			edx, ecx
					add			edx, edx
					mov 		edi, edx
					add			edx, ecx
					add			edi, edi
					add			edi, ecx
					mov			esi, edx
					add 		esi, esi
					add			esi, ecx

					movq		xmm1, qword ptr[eax]
					punpcklbw	xmm1, xmm0
					paddsw		xmm1, [ebx+16*0]
					packuswb	xmm1, xmm0
					movq		qword ptr[eax], xmm1

					movq		xmm2, qword ptr[eax+ecx]
					punpcklbw	xmm2, xmm0
					paddsw		xmm2, [ebx+16*1]
					packuswb	xmm2, xmm0
					movq		qword ptr[eax+ecx], xmm2

					movq		xmm3, qword ptr[eax+ecx*2]
					punpcklbw	xmm3, xmm0
					paddsw		xmm3, [ebx+16*2]
					packuswb	xmm3, xmm0
					movq		qword ptr[eax+ecx*2], xmm3

					movq		xmm4, qword ptr[eax+edx]
					punpcklbw	xmm4, xmm0
					paddsw		xmm4, [ebx+16*3]
					packuswb	xmm4, xmm0
					movq		qword ptr[eax+edx], xmm4

					movq		xmm5, qword ptr[eax+ecx*4]
					punpcklbw	xmm5, xmm0
					paddsw		xmm5, [ebx+16*4]
					packuswb	xmm5, xmm0
					movq		qword ptr[eax+ecx*4], xmm5

					movq		xmm6, qword ptr[eax+edi]
					punpcklbw	xmm6, xmm0
					paddsw		xmm6, [ebx+16*5]
					packuswb	xmm6, xmm0
					movq		qword ptr[eax+edi], xmm6

					movq		xmm7, qword ptr[eax+edx*2]
					punpcklbw	xmm7, xmm0
					paddsw		xmm7, [ebx+16*6]
					packuswb	xmm7, xmm0
					movq		qword ptr[eax+edx*2], xmm7

					movq		xmm1, qword ptr[eax+esi]
					punpcklbw	xmm1, xmm0
					paddsw		xmm1, [ebx+16*7]
					packuswb	xmm1, xmm0
					movq		qword ptr[eax+esi], xmm1
				}
			}
			else
			{
				__asm
				{
					mov			eax, 0x00800080
					movd		xmm7, eax
					pshufd		xmm7, xmm7, 0

					mov			eax, [rfp]
					mov			ebx, [Block_Ptr]
					mov			ecx, [iincr]
					mov			edx, ecx
					add			edx, edx
					mov 		edi, edx
					add			edx, ecx
					add			edi, edi
					add			edi, ecx
					mov			esi, edx
					add 		esi, esi
					add			esi, ecx

					movdqa		xmm0, [ebx+16*0]
					paddsw		xmm0, xmm7
					packuswb	xmm0, xmm0
					movq		qword ptr[eax], xmm0

					movdqa		xmm1, [ebx+16*1]
					paddsw		xmm1, xmm7
					packuswb	xmm1, xmm1
					movq		qword ptr[eax+ecx],xmm1

					movdqa		xmm2, [ebx+16*2]
					paddsw		xmm2, xmm7
					packuswb	xmm2, xmm2
					movq		qword ptr [eax+ecx*2], xmm2

					movdqa		xmm3, [ebx+16*3]
					paddsw		xmm3, xmm7
					packuswb	xmm3, xmm3
					movq		qword ptr [eax+edx], xmm3

					movdqa		xmm4, [ebx+16*4]
					paddsw		xmm4, xmm7
					packuswb	xmm4, xmm4
					movq		qword ptr[eax+ecx*4], xmm4

					movdqa		xmm5, [ebx+16*5]
					paddsw		xmm5, xmm7
					packuswb	xmm5, xmm5
					movq		qword ptr [eax+edi], xmm5

					movdqa		xmm6, [ebx+16*6]
					paddsw		xmm6, xmm7
					packuswb	xmm6, xmm6
					movq		qword ptr [eax+edx*2], xmm6

					paddsw		xmm7, [ebx+16*7]
					packuswb	xmm7, xmm7
					movq		qword ptr[eax+esi], xmm7
				} 
			}
		}
		else	// MMX
		{
			if (addflag)
			{
				__asm
				{
					pxor		mm0, mm0
					mov			eax, [rfp]
					mov			ebx, [Block_Ptr]
					mov			edi, 8
addon:
					movq		mm2, [ebx+8]

					movq		mm3, [eax]
					movq		mm4, mm3

					movq		mm1, [ebx]
					punpckhbw	mm3, mm0

					paddsw		mm3, mm2
					packuswb	mm3, mm0

					punpcklbw	mm4, mm0
					psllq		mm3, 32

					paddsw		mm4, mm1
					packuswb	mm4, mm0

					por			mm3, mm4			
					add			ebx, 16

					dec			edi
					movq		[eax], mm3

					add			eax, [iincr]
					cmp			edi, 0x00
					jg			addon
				}
			}
			else
			{
				__asm
				{
					mov			eax, [rfp]
					mov			ebx, [Block_Ptr]
					mov			edi, 8

					pxor		mm0, mm0
					movq		mm1, [mmmask_128]
addoff:
					movq		mm3, [ebx+8]
					movq		mm4, [ebx]

					paddsw		mm3, mm1
					paddsw		mm4, mm1

					packuswb	mm3, mm0
					packuswb	mm4, mm0

					psllq		mm3, 32
					por			mm3, mm4
				
					add			ebx, 16
					dec			edi

					movq		[eax], mm3

					add			eax, [iincr]
					cmp			edi, 0x00
					jg			addoff
				}
			}
		}
	}
	__asm emms;
}

/* set scratch pad macroblock to zero */
static void Clear_Block(int count)
{
	int comp;
	short *Block_Ptr;
	
	for (comp=0; comp<count; comp++)
	{
		Block_Ptr = block[comp];

		if (cpu.sse2)	// SSE2
		{
			__asm
			{
				mov			eax, [Block_Ptr];
				pxor		xmm0, xmm0;
				movdqa		[eax+0 ], xmm0;
				movdqa		[eax+16 ], xmm0;
				movdqa		[eax+32 ], xmm0;
				movdqa		[eax+48 ], xmm0;
				movdqa		[eax+64 ], xmm0;
				movdqa		[eax+80 ], xmm0;
				movdqa		[eax+96 ], xmm0;
				movdqa		[eax+112 ], xmm0;
			}
		}
		else	// MMX
		{
			__asm
			{
				mov			eax, [Block_Ptr];
				pxor		mm0, mm0;
				movq		[eax+0 ], mm0;
				movq		[eax+8 ], mm0;
				movq		[eax+16], mm0;
				movq		[eax+24], mm0;
				movq		[eax+32], mm0;
				movq		[eax+40], mm0;
				movq		[eax+48], mm0;
				movq		[eax+56], mm0;
				movq		[eax+64], mm0;
				movq		[eax+72], mm0;
				movq		[eax+80], mm0;
				movq		[eax+88], mm0;
				movq		[eax+96], mm0;
				movq		[eax+104],mm0;
				movq		[eax+112],mm0;
				movq		[eax+120],mm0;
			}
		}
	}

	__asm emms;

}

/* ISO/IEC 13818-2 section 7.6 */
static void motion_compensation(int MBA, int macroblock_type, int motion_type, 
								int PMV[2][2][2], int motion_vertical_field_select[2][2],
								int dmvector[2], int dct_type)
{
	int bx, by;
	int comp;

	/* derive current macroblock position within picture */
	/* ISO/IEC 13818-2 section 6.3.1.6 and 6.3.1.7 */
	bx = 16*(MBA%mb_width);
	by = 16*(MBA/mb_width);

	/* motion compensation */
	if (!(macroblock_type & MACROBLOCK_INTRA))
		form_predictions(bx, by, macroblock_type, motion_type, PMV,
						 motion_vertical_field_select, dmvector);

	switch (iDCT_Flag)
	{
		case IDCT_MMX:
			for (comp=0; comp<block_count; comp++)
				MMX_IDCT(block[comp]);
			break;

		case IDCT_SSEMMX:
			for (comp=0; comp<block_count; comp++)
				SSEMMX_IDCT(block[comp]);
			break;

		case IDCT_FPU:
			for (comp=0; comp<block_count; comp++)
				FPU_IDCT(block[comp]);
			break;

		case IDCT_REF:
			for (comp=0; comp<block_count; comp++)
				REF_IDCT(block[comp]);
			break;

		case IDCT_SSE2MMX:
			for (comp=0; comp<block_count; comp++)
				SSE2MMX_IDCT(block[comp]);
			break;
	}

	Add_Block(block_count, bx, by, dct_type, (macroblock_type & MACROBLOCK_INTRA)==0);
}

/* ISO/IEC 13818-2 section 7.6.6 */
static void skipped_macroblock(int dc_dct_pred[3], int PMV[2][2][2], int *motion_type, 
							   int motion_vertical_field_select[2][2], int *macroblock_type)
{
	Clear_Block(block_count);

	/* reset intra_dc predictors */
	/* ISO/IEC 13818-2 section 7.2.1: DC coefficients in intra blocks */
	dc_dct_pred[0]=dc_dct_pred[1]=dc_dct_pred[2]=0;

	/* reset motion vector predictors */
	/* ISO/IEC 13818-2 section 7.6.3.4: Resetting motion vector predictors */
	if (picture_coding_type==P_TYPE)
		PMV[0][0][0]=PMV[0][0][1]=PMV[1][0][0]=PMV[1][0][1]=0;

	/* derive motion_type */
	if (picture_structure==FRAME_PICTURE)
		*motion_type = MC_FRAME;
	else
	{
		*motion_type = MC_FIELD;
		motion_vertical_field_select[0][0] = motion_vertical_field_select[0][1] = 
			(picture_structure==BOTTOM_FIELD);
	}

	/* clear MACROBLOCK_INTRA */
	*macroblock_type&= ~MACROBLOCK_INTRA;
}

/* return==-1 means go to next picture */
/* the expression "start of slice" is used throughout the normative
   body of the MPEG specification */
static int start_of_slice(int *MBA, int *MBAinc,
						  int dc_dct_pred[3], int PMV[2][2][2])
{
	unsigned int code;
	int slice_vert_pos_ext;

	next_start_code();
	code = Get_Bits(32);

	if (code<SLICE_START_CODE_MIN || code>SLICE_START_CODE_MAX)
	{
		// only slice headers are allowed in picture_data
		Fault_Flag = 10;
		return -1;
	}

	/* decode slice header (may change quantizer_scale) */
	slice_vert_pos_ext = slice_header();

	/* decode macroblock address increment */
	*MBAinc = Get_macroblock_address_increment();
	if (Fault_Flag)
		return -1;

	/* set current location */
	/* NOTE: the arithmetic used to derive macroblock_address below is
	   equivalent to ISO/IEC 13818-2 section 6.3.17: Macroblock */
	*MBA = ((slice_vert_pos_ext<<7) + (code&255) - 1)*mb_width + *MBAinc - 1;
	*MBAinc = 1;	// first macroblock in slice: not skipped

	/* reset all DC coefficient and motion vector predictors */
	/* ISO/IEC 13818-2 section 7.2.1: DC coefficients in intra blocks */
	dc_dct_pred[0]=dc_dct_pred[1]=dc_dct_pred[2]=0;
  
	/* ISO/IEC 13818-2 section 7.6.3.4: Resetting motion vector predictors */
	PMV[0][0][0]=PMV[0][0][1]=PMV[1][0][0]=PMV[1][0][1]=0;
	PMV[0][1][0]=PMV[0][1][1]=PMV[1][1][0]=PMV[1][1][1]=0;

	/* successfull: trigger decode macroblocks in slice */
	return 1;
}

/* ISO/IEC 13818-2 sections 7.2 through 7.5 */
static int decode_macroblock(int *macroblock_type, int *motion_type, int *dct_type,
							 int PMV[2][2][2], int dc_dct_pred[3], 
							 int motion_vertical_field_select[2][2], int dmvector[2])
{
	int quantizer_scale_code, comp, motion_vector_count, mv_format; 
	int dmv, mvscale, coded_block_pattern;

	/* ISO/IEC 13818-2 section 6.3.17.1: Macroblock modes */
	macroblock_modes(macroblock_type, motion_type, &motion_vector_count, &mv_format,
					 &dmv, &mvscale, dct_type);
	if (Fault_Flag) return 0;	// trigger: go to next slice

	if (*macroblock_type & MACROBLOCK_QUANT)
	{
		quantizer_scale_code = Get_Bits(5);

		/* ISO/IEC 13818-2 section 7.4.2.2: Quantizer scale factor */
		quantizer_scale = q_scale_type ?
		Non_Linear_quantizer_scale[quantizer_scale_code] : (quantizer_scale_code << 1);
	}

	/* ISO/IEC 13818-2 section 6.3.17.2: Motion vectors */
	/* decode forward motion vectors */
	if ((*macroblock_type & MACROBLOCK_MOTION_FORWARD) 
		|| ((*macroblock_type & MACROBLOCK_INTRA) && concealment_motion_vectors))
		motion_vectors(PMV, dmvector, motion_vertical_field_select, 0,
		motion_vector_count, mv_format, f_code[0][0]-1, f_code[0][1]-1, dmv, mvscale);
	if (Fault_Flag)
		return 0;	// trigger: go to next slice

	/* decode backward motion vectors */
	if (*macroblock_type & MACROBLOCK_MOTION_BACKWARD)
		motion_vectors(PMV, dmvector, motion_vertical_field_select, 1,
		motion_vector_count,mv_format, f_code[1][0]-1, f_code[1][1]-1, 0, mvscale);
	if (Fault_Flag) return 0;  // trigger: go to next slice

	if ((*macroblock_type & MACROBLOCK_INTRA) && concealment_motion_vectors)
		Flush_Buffer(1);	// marker bit

	/* macroblock_pattern */
	/* ISO/IEC 13818-2 section 6.3.17.4: Coded block pattern */
	if (*macroblock_type & MACROBLOCK_PATTERN)
	{
		coded_block_pattern = Get_coded_block_pattern();

		if (chroma_format==CHROMA422)
			coded_block_pattern = (coded_block_pattern<<2) | Get_Bits(2);
		else if (chroma_format==CHROMA444)
			coded_block_pattern = (coded_block_pattern<<6) | Get_Bits(6);
	}
	else
	    coded_block_pattern = (*macroblock_type & MACROBLOCK_INTRA) ? (1<<block_count)-1 : 0;

	if (Fault_Flag) return 0;	// trigger: go to next slice

	Clear_Block(block_count);

	/* decode blocks */
	for (comp=0; comp<block_count; comp++)
	{
		if (coded_block_pattern & (1<<(block_count-1-comp)))
		{
			if (*macroblock_type & MACROBLOCK_INTRA)
				Decode_MPEG2_Intra_Block(comp, dc_dct_pred);
			else
				Decode_MPEG2_Non_Intra_Block(comp);

			if (Fault_Flag) return 0;	// trigger: go to next slice
		}
	}

	/* reset intra_dc predictors */
	/* ISO/IEC 13818-2 section 7.2.1: DC coefficients in intra blocks */
	if (!(*macroblock_type & MACROBLOCK_INTRA))
		dc_dct_pred[0]=dc_dct_pred[1]=dc_dct_pred[2]=0;

	/* reset motion vector predictors */
	if ((*macroblock_type & MACROBLOCK_INTRA) && !concealment_motion_vectors)
	{
		/* intra mb without concealment motion vectors */
		/* ISO/IEC 13818-2 section 7.6.3.4: Resetting motion vector predictors */
		PMV[0][0][0]=PMV[0][0][1]=PMV[1][0][0]=PMV[1][0][1]=0;
		PMV[0][1][0]=PMV[0][1][1]=PMV[1][1][0]=PMV[1][1][1]=0;
	}

	/* special "No_MC" macroblock_type case */
	/* ISO/IEC 13818-2 section 7.6.3.5: Prediction in P pictures */
	if (picture_coding_type==P_TYPE && !(*macroblock_type & (MACROBLOCK_MOTION_FORWARD|MACROBLOCK_INTRA)))
	{
		/* non-intra mb without forward mv in a P picture */
		/* ISO/IEC 13818-2 section 7.6.3.4: Resetting motion vector predictors */
		PMV[0][0][0]=PMV[0][0][1]=PMV[1][0][0]=PMV[1][0][1]=0;

		/* derive motion_type */
		/* ISO/IEC 13818-2 section 6.3.17.1: Macroblock modes, frame_motion_type */
		if (picture_structure==FRAME_PICTURE)
			*motion_type = MC_FRAME;
		else
		{
			*motion_type = MC_FIELD;
			motion_vertical_field_select[0][0] = (picture_structure==BOTTOM_FIELD);
		}
	}
	/* successfully decoded macroblock */
	return 1 ;
}

/* decode one intra coded MPEG-2 block */
static void Decode_MPEG2_Intra_Block(int comp, int dc_dct_pred[])
{
	int val, i, j, sign, *qmat;
	unsigned int code;
	DCTtab *tab;
	short *bp;

	bp = block[comp];
	qmat = (comp<4 || chroma_format==CHROMA420) 
		? intra_quantizer_matrix : chroma_intra_quantizer_matrix;

	/* ISO/IEC 13818-2 section 7.2.1: decode DC coefficients */
	switch (cc_table[comp])
	{
		case 0:
			val = (dc_dct_pred[0]+= Get_Luma_DC_dct_diff());
			break;

		case 1:
			val = (dc_dct_pred[1]+= Get_Chroma_DC_dct_diff());
			break;

		case 2:
			val = (dc_dct_pred[2]+= Get_Chroma_DC_dct_diff());
			break;
	}

	bp[0] = val << (3-intra_dc_precision);

	/* decode AC coefficients */
	for (i=1; ; i++)
	{
		code = Show_Bits(16);

		if (code>=16384 && !intra_vlc_format)
			tab = &DCTtabnext[(code>>12)-4];
		else if (code>=1024)
		{
			if (intra_vlc_format)
				tab = &DCTtab0a[(code>>8)-4];
			else
				tab = &DCTtab0[(code>>8)-4];
		}
		else if (code>=512)
		{
			if (intra_vlc_format)
				tab = &DCTtab1a[(code>>6)-8];
			else
				tab = &DCTtab1[(code>>6)-8];
		}
		else if (code>=256)
			tab = &DCTtab2[(code>>4)-16];
		else if (code>=128)
			tab = &DCTtab3[(code>>3)-16];
		else if (code>=64)
			tab = &DCTtab4[(code>>2)-16];
		else if (code>=32)
			tab = &DCTtab5[(code>>1)-16];
		else if (code>=16)
			tab = &DCTtab6[code-16];
		else
		{
			Fault_Flag = 1;
			return;
		}

		Flush_Buffer(tab->len);

		if (tab->run<64)
		{
			i+= tab->run;
			val = tab->level;
			sign = Get_Bits(1);
		}
		else if (tab->run==64) /* end_of_block */
			return;
		else /* escape */
		{
			i+= Get_Bits(6);
			val = Get_Bits(12);

			if (sign = (val>=2048))
				val = 4096 - val;
		}

		j = scan[alternate_scan][i];

		val = (val * quantizer_scale * qmat[j]) >> 4;
		bp[j] = sign ? -val : val;
	}
}

/* decode one non-intra coded MPEG-2 block */
static void Decode_MPEG2_Non_Intra_Block(int comp)
{
	int val, i, j, sign, *qmat;
	unsigned int code;
	DCTtab *tab;
	short *bp;

	bp = block[comp];
	qmat = (comp<4 || chroma_format==CHROMA420) 
		? non_intra_quantizer_matrix : chroma_non_intra_quantizer_matrix;

	/* decode AC coefficients */
	for (i=0; ; i++)
	{
		code = Show_Bits(16);

		if (code>=16384)
		{
			if (i==0)
				tab = &DCTtabfirst[(code>>12)-4];
			else
				tab = &DCTtabnext[(code>>12)-4];
		}
		else if (code>=1024)
			tab = &DCTtab0[(code>>8)-4];
		else if (code>=512)
			tab = &DCTtab1[(code>>6)-8];
		else if (code>=256)
			tab = &DCTtab2[(code>>4)-16];
		else if (code>=128)
			tab = &DCTtab3[(code>>3)-16];
		else if (code>=64)
			tab = &DCTtab4[(code>>2)-16];
		else if (code>=32)
			tab = &DCTtab5[(code>>1)-16];
		else if (code>=16)
			tab = &DCTtab6[code-16];
		else
		{
			Fault_Flag = 1;
			return;
		}

		Flush_Buffer(tab->len);

		if (tab->run<64)
		{
			i+= tab->run;
			val = tab->level;
			sign = Get_Bits(1);
		}
		else if (tab->run==64) /* end_of_block */
			return;
		else /* escape */
		{
			i+= Get_Bits(6);
			val = Get_Bits(12);

			if (sign = (val>=2048))
				val = 4096 - val;
		}

		j = scan[alternate_scan][i];

		val = (((val<<1)+1) * quantizer_scale * qmat[j]) >> 5;
		bp[j] = sign ? -val : val;
	}
}

static int Get_macroblock_type()
{
	int macroblock_type;

	switch (picture_coding_type)
	{
		case I_TYPE:
			macroblock_type = Get_I_macroblock_type();
			break;

		case P_TYPE:
			macroblock_type = Get_P_macroblock_type();
			break;

		case B_TYPE:
			macroblock_type = Get_B_macroblock_type();
			break;
	}

	return macroblock_type;
}

static int Get_I_macroblock_type()
{
	if (Get_Bits(1))
		return 1;

	if (!Get_Bits(1))
		Fault_Flag = 2;

	return 17;
}

static int Get_P_macroblock_type()
{
	int code;

	if ((code = Show_Bits(6))>=8)
	{
		code >>= 3;
		Flush_Buffer(PMBtab0[code].len);

		return PMBtab0[code].val;
	}

	if (code==0)
	{
		Fault_Flag = 2;
		return 0;
	}

	Flush_Buffer(PMBtab1[code].len);

	return PMBtab1[code].val;
}

static int Get_B_macroblock_type()
{
	int code;

	if ((code = Show_Bits(6))>=8)
	{
		code >>= 2;
		Flush_Buffer(BMBtab0[code].len);

		return BMBtab0[code].val;
	}

	if (code==0)
	{
		Fault_Flag = 2;
		return 0;
	}

	Flush_Buffer(BMBtab1[code].len);

	return BMBtab1[code].val;
}

static int Get_coded_block_pattern()
{
	int code;

	if ((code = Show_Bits(9))>=128)
	{
		code >>= 4;
		Flush_Buffer(CBPtab0[code].len);

		return CBPtab0[code].val;
	}

	if (code>=8)
	{
		code >>= 1;
		Flush_Buffer(CBPtab1[code].len);

		return CBPtab1[code].val;
	}

	if (code<1)
	{
		Fault_Flag = 3;
		return 0;
	}

	Flush_Buffer(CBPtab2[code].len);

	return CBPtab2[code].val;
}

static int Get_macroblock_address_increment()
{
	int code, val;

	val = 0;

	while ((code = Show_Bits(11))<24)
	{
		if (code!=15) /* if not macroblock_stuffing */
		{
			if (code==8) /* if macroblock_escape */
				val+= 33;
			else
			{
				Fault_Flag = 4;
				return 1;
			}
		}
		Flush_Buffer(11);
	}

	/* macroblock_address_increment == 1 */
	/* ('1' is in the MSB position of the lookahead) */
	if (code>=1024)
	{
		Flush_Buffer(1);
		return val + 1;
	}

	/* codes 00010 ... 011xx */
	if (code>=128)
	{
		/* remove leading zeros */
		code >>= 6;
		Flush_Buffer(MBAtab1[code].len);
    
		return val + MBAtab1[code].val;
	}
  
	/* codes 00000011000 ... 0000111xxxx */
	code -= 24; /* remove common base */
	Flush_Buffer(MBAtab2[code].len);

	return val + MBAtab2[code].val;
}

/*
   parse VLC and perform dct_diff arithmetic.
   MPEG-2:  ISO/IEC 13818-2 section 7.2.1 

   Note: the arithmetic here is presented more elegantly than
   the spec, yet the results, dct_diff, are the same.
*/
static int Get_Luma_DC_dct_diff()
{
	int code, size, dct_diff;

	/* decode length */
	code = Show_Bits(5);

	if (code<31)
	{
		size = DClumtab0[code].val;
		Flush_Buffer(DClumtab0[code].len);
	}
	else
	{
		code = Show_Bits(9) - 0x1f0;
		size = DClumtab1[code].val;
		Flush_Buffer(DClumtab1[code].len);
	}

	if (size==0)
		dct_diff = 0;
	else
	{
		dct_diff = Get_Bits(size);

		if ((dct_diff & (1<<(size-1)))==0)
			dct_diff-= (1<<size) - 1;
	}

	return dct_diff;
}

static int Get_Chroma_DC_dct_diff()
{
	int code, size, dct_diff;

	/* decode length */
	code = Show_Bits(5);

	if (code<31)
	{
		size = DCchromtab0[code].val;
		Flush_Buffer(DCchromtab0[code].len);
	}
	else
	{
		code = Show_Bits(10) - 0x3e0;
		size = DCchromtab1[code].val;
		Flush_Buffer(DCchromtab1[code].len);
	}

	if (size==0)
		dct_diff = 0;
	else
	{
		dct_diff = Get_Bits(size);

		if ((dct_diff & (1<<(size-1)))==0)
			dct_diff-= (1<<size) - 1;
	}

	return dct_diff;
}

static void form_predictions(int bx, int by, int macroblock_type, int motion_type,
					  int PMV[2][2][2], int motion_vertical_field_select[2][2],
					  int dmvector[2])
{
	static int currentfield;
	static unsigned char **predframe;
	static int DMV[2][2];
	static int stw;
	
	stw = 0;

	if ((macroblock_type & MACROBLOCK_MOTION_FORWARD) || picture_coding_type==P_TYPE)
	{
		if (picture_structure==FRAME_PICTURE)
		{
			if (motion_type==MC_FRAME || !(macroblock_type & MACROBLOCK_MOTION_FORWARD))
			{
				/* frame-based prediction (broken into top and bottom halves
				   for spatial scalability prediction purposes) */
				form_prediction(forward_reference_frame, 0, current_frame, 0, Coded_Picture_Width, 
					Coded_Picture_Width<<1, 16, 8, bx, by, PMV[0][0][0], PMV[0][0][1], stw);

				form_prediction(forward_reference_frame, 1, current_frame, 1, Coded_Picture_Width, 
					Coded_Picture_Width<<1, 16, 8, bx, by, PMV[0][0][0], PMV[0][0][1], stw);
			}
			else if (motion_type==MC_FIELD) /* field-based prediction */
			{
				/* top field prediction */
				form_prediction(forward_reference_frame, motion_vertical_field_select[0][0], 
					current_frame, 0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by>>1, PMV[0][0][0], PMV[0][0][1]>>1, stw);

				/* bottom field prediction */
				form_prediction(forward_reference_frame, motion_vertical_field_select[1][0], 
					current_frame, 1, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by>>1, PMV[1][0][0], PMV[1][0][1]>>1, stw);
			}
			else if (motion_type==MC_DMV) /* dual prime prediction */
			{
				/* calculate derived motion vectors */
				Dual_Prime_Arithmetic(DMV, dmvector, PMV[0][0][0], PMV[0][0][1]>>1);

				/* predict top field from top field */
				form_prediction(forward_reference_frame, 0, current_frame, 0, 
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by>>1,
					PMV[0][0][0], PMV[0][0][1]>>1, 0);

				/* predict and add to top field from bottom field */
				form_prediction(forward_reference_frame, 1, current_frame, 0,
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by>>1,
					DMV[0][0], DMV[0][1], 1);

				/* predict bottom field from bottom field */
				form_prediction(forward_reference_frame, 1, current_frame, 1,
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by>>1,
					PMV[0][0][0], PMV[0][0][1]>>1, 0);

				/* predict and add to bottom field from top field */
				form_prediction(forward_reference_frame, 0, current_frame, 1,
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by>>1,
					DMV[1][0], DMV[1][1], 1);
			}
			else
				Fault_Flag = 5;
		}
		else
		{
			/* field picture */
			currentfield = (picture_structure==BOTTOM_FIELD);

			/* determine which frame to use for prediction */
			if (picture_coding_type==P_TYPE && Second_Field && currentfield!=motion_vertical_field_select[0][0])
				predframe = backward_reference_frame;
			else
				predframe = forward_reference_frame;

			if (motion_type==MC_FIELD || !(macroblock_type & MACROBLOCK_MOTION_FORWARD))
			{
				form_prediction(predframe, motion_vertical_field_select[0][0], current_frame, 0, 
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 16, bx, by,
					PMV[0][0][0], PMV[0][0][1], stw);
			}
			else if (motion_type==MC_16X8)
			{
				form_prediction(predframe, motion_vertical_field_select[0][0], current_frame, 0, 
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by,
					PMV[0][0][0], PMV[0][0][1], stw);

				if (picture_coding_type==P_TYPE && Second_Field && currentfield!=motion_vertical_field_select[1][0])
					predframe = backward_reference_frame;
				else
					predframe = forward_reference_frame;

				form_prediction(predframe, motion_vertical_field_select[1][0], current_frame, 
					0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8, bx, by+8,
					PMV[1][0][0], PMV[1][0][1], stw);
			}
			else if (motion_type==MC_DMV)
			{
				if (Second_Field)
					predframe = backward_reference_frame;
				else
					predframe = forward_reference_frame;

				/* calculate derived motion vectors */
				Dual_Prime_Arithmetic(DMV, dmvector, PMV[0][0][0], PMV[0][0][1]);

				/* predict from field of same parity */
				form_prediction(forward_reference_frame, currentfield, current_frame, 0, 
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 16, bx, by,
					PMV[0][0][0], PMV[0][0][1], 0);

				/* predict from field of opposite parity */
				form_prediction(predframe, !currentfield, current_frame, 0,
					Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 16, bx, by,
					DMV[0][0], DMV[0][1], 1);
			}
			else
				Fault_Flag = 5;
		}

		stw = 1;
	}

	if (macroblock_type & MACROBLOCK_MOTION_BACKWARD)
	{
		if (picture_structure==FRAME_PICTURE)
		{
			if (motion_type==MC_FRAME)
			{
				/* frame-based prediction */
				form_prediction(backward_reference_frame, 0, current_frame, 0,
					Coded_Picture_Width, Coded_Picture_Width<<1, 16, 8, bx, by,
					PMV[0][1][0], PMV[0][1][1], stw);

				form_prediction(backward_reference_frame, 1, current_frame, 1,
					Coded_Picture_Width, Coded_Picture_Width<<1, 16, 8, bx, by,
					PMV[0][1][0], PMV[0][1][1], stw);
			}
			else /* field-based prediction */
			{
				/* top field prediction */
				form_prediction(backward_reference_frame, motion_vertical_field_select[0][1], 
					current_frame, 0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by>>1, PMV[0][1][0], PMV[0][1][1]>>1, stw);

				/* bottom field prediction */
				form_prediction(backward_reference_frame, motion_vertical_field_select[1][1], 
					current_frame, 1, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by>>1, PMV[1][1][0], PMV[1][1][1]>>1, stw);
			}
		}
		else
		{
			/* field picture */
			if (motion_type==MC_FIELD)
			{
				/* field-based prediction */
				form_prediction(backward_reference_frame, motion_vertical_field_select[0][1], 
					current_frame, 0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 16,
					bx, by, PMV[0][1][0], PMV[0][1][1], stw);
			}
			else if (motion_type==MC_16X8)
			{
				form_prediction(backward_reference_frame, motion_vertical_field_select[0][1],
					current_frame, 0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by, PMV[0][1][0], PMV[0][1][1], stw);

				form_prediction(backward_reference_frame, motion_vertical_field_select[1][1],
					current_frame, 0, Coded_Picture_Width<<1, Coded_Picture_Width<<1, 16, 8,
					bx, by+8, PMV[1][1][0], PMV[1][1][1], stw);
			}
			else
				Fault_Flag = 5;
		}
	}

	__asm emms;
}

static void form_prediction(unsigned char *src[], int sfield, unsigned char *dst[],
							int dfield, int lx, int lx2, int w, int h, int x, int y,
							int dx, int dy, int average_flag)
{
	form_component_prediction(src[0]+(sfield?lx2>>1:0), dst[0]+(dfield?lx2>>1:0),
		lx, lx2, w, h, x, y, dx, dy, average_flag);

	if (chroma_format!=CHROMA444)
	{
		lx>>=1; lx2>>=1; w>>=1; x>>=1; dx/=2;
	}

	if (chroma_format==CHROMA420)
	{
		h>>=1; y>>=1; dy/=2;
	}

	/* Cb */
	form_component_prediction(src[1]+(sfield?lx2>>1:0), dst[1]+(dfield?lx2>>1:0),
		lx, lx2, w, h, x, y, dx, dy, average_flag);

	/* Cr */
	form_component_prediction(src[2]+(sfield?lx2>>1:0), dst[2]+(dfield?lx2>>1:0),
		lx, lx2, w, h, x, y, dx, dy, average_flag);
}

/* ISO/IEC 13818-2 section 7.6.4: Forming predictions */
static void form_component_prediction(unsigned char *src, unsigned char *dst,
										  int lx, int lx2, int w, int h, int x, int y,
										  int dx, int dy, int average_flag)
{
	static const __int64 mmmask_0001 = 0x0001000100010001;
	static const __int64 mmmask_0002 = 0x0002000200020002;
	static const __int64 mmmask_0003 = 0x0003000300030003;
	static const __int64 mmmask_0006 = 0x0006000600060006;

	unsigned char *s = src + lx * (y + (dy>>1)) + x + (dx>>1);
	unsigned char *d = dst + lx * y + x;
	int flag = (average_flag<<2) + ((dx & 1)<<1) + (dy & 1);

	switch (flag)
	{
		case 0:
			// d[i] = s[i];
			__asm
			{
				mov			eax, [s]
				mov			ebx, [d]
				mov			esi, 0x00
				mov			edi, [h]
mc0:
				movq		mm1, [eax+esi]
				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc0

				add			eax, [lx2]
				add			ebx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc0
			}
			break;

		case 1:
			// d[i] = (s[i]+s[i+lx]+1)>>1;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0001]
				mov			eax, [s]
				mov			ebx, [d]
				mov			ecx, eax
				add			ecx, [lx]
				mov			esi, 0x00
				mov			edi, [h]
mc1:
				movq		mm1, [eax+esi]
				movq		mm2, [ecx+esi]

				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0
				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				paddsw		mm1, mm7
				paddsw		mm3, mm7

				psrlw		mm1, 1
				psrlw		mm3, 1

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc1

				add			eax, [lx2]
				add			ebx, [lx2]
				add			ecx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc1
			}
			break;

		case 2:
			// d[i] = (s[i]+s[i+1]+1)>>1;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0001]
				mov			eax, [s]
				mov			ebx, [d]
				mov			esi, 0x00
				mov			edi, [h]
mc2:
				movq		mm1, [eax+esi]
				movq		mm2, [eax+esi+1]

				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				paddsw		mm1, mm7
				paddsw		mm3, mm7

				psrlw		mm1, 1
				psrlw		mm3, 1

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc2

				add			eax, [lx2]
				add			ebx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc2
			}
			break;

		case 3:
			// d[i] = (s[i]+s[i+1]+s[i+lx]+s[i+lx+1]+2)>>2;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0002]
				mov			eax, [s]
				mov			ebx, [d]
				mov			ecx, eax
				add			ecx, [lx]
				mov			esi, 0x00
				mov			edi, [h]
mc3:
				movq		mm1, [eax+esi]
				movq		mm2, [eax+esi+1]
				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				movq		mm5, [ecx+esi]
				paddsw		mm1, mm7

				movq		mm6, [ecx+esi+1]
				paddsw		mm3, mm7

				movq		mm2, mm5
				movq		mm4, mm6

				punpcklbw	mm2, mm0
				punpckhbw	mm5, mm0

				punpcklbw	mm4, mm0
				punpckhbw	mm6, mm0
				
				paddsw		mm2, mm4
				paddsw		mm5, mm6

				paddsw		mm1, mm2
				paddsw		mm3, mm5

				psrlw		mm1, 2
				psrlw		mm3, 2

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc3

				add			eax, [lx2]
				add			ebx, [lx2]
				add			ecx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc3
			}
			break;

		case 4:
			// d[i] = (s[i]+d[i]+1)>>1;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0001]
				mov			eax, [s]
				mov			ebx, [d]
				mov			esi, 0x00
				mov			edi, [h]
mc4:
				movq		mm1, [eax+esi]
				movq		mm2, [ebx+esi]
				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				paddsw		mm1, mm7
				paddsw		mm3, mm7

				psrlw		mm1, 1
				psrlw		mm3, 1

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc4

				add			eax, [lx2]
				add			ebx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc4
			}
			break;

		case 5:
			// d[i] = ((d[i]<<1) + s[i]+s[i+lx] + 3)>>2;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0003]
				mov			eax, [s]
				mov			ebx, [d]
				mov			ecx, eax
				add			ecx, [lx]
				mov			esi, 0x00
				mov			edi, [h]
mc5:
				movq		mm1, [eax+esi]
				movq		mm2, [ecx+esi]
				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				movq		mm5, [ebx+esi]

				paddsw		mm1, mm7
				paddsw		mm3, mm7

				movq		mm6, mm5
				punpcklbw	mm5, mm0
				punpckhbw	mm6, mm0

				psllw		mm5, 1
				psllw		mm6, 1

				paddsw		mm1, mm5
				paddsw		mm3, mm6

				psrlw		mm1, 2
				psrlw		mm3, 2

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc5

				add			eax, [lx2]
				add			ebx, [lx2]
				add			ecx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc5
			}
			break;

		case 6:
			// d[i] = ((d[i]<<1) + s[i]+s[i+1] + 3) >> 2;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0003]
				mov			eax, [s]
				mov			ebx, [d]
				mov			esi, 0x00
				mov			edi, [h]
mc6:
				movq		mm1, [eax+esi]
				movq		mm2, [eax+esi+1]
				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				movq		mm5, [ebx+esi]

				paddsw		mm1, mm7
				paddsw		mm3, mm7

				movq		mm6, mm5
				punpcklbw	mm5, mm0
				punpckhbw	mm6, mm0

				psllw		mm5, 1
				psllw		mm6, 1

				paddsw		mm1, mm5
				paddsw		mm3, mm6

				psrlw		mm1, 2
				psrlw		mm3, 2

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc6

				add			eax, [lx2]
				add			ebx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc6
			}
			break;

		case 7:
			// d[i] = ((d[i]<<2) + s[i]+s[i+1]+s[i+lx]+s[i+lx+1] + 6)>>3;
			__asm
			{
				pxor		mm0, mm0
				movq		mm7, [mmmask_0006]
				mov			eax, [s]
				mov			ebx, [d]
				mov			ecx, eax
				add			ecx, [lx]
				mov			esi, 0x00
				mov			edi, [h]
mc7:
				movq		mm1, [eax+esi]
				movq		mm2, [eax+esi+1]
				movq		mm3, mm1
				movq		mm4, mm2

				punpcklbw	mm1, mm0
				punpckhbw	mm3, mm0

				punpcklbw	mm2, mm0
				punpckhbw	mm4, mm0

				paddsw		mm1, mm2
				paddsw		mm3, mm4

				movq		mm5, [ecx+esi]
				paddsw		mm1, mm7

				movq		mm6, [ecx+esi+1]
				paddsw		mm3, mm7

				movq		mm2, mm5
				movq		mm4, mm6

				punpcklbw	mm2, mm0
				punpckhbw	mm5, mm0

				punpcklbw	mm4, mm0
				punpckhbw	mm6, mm0
				
				paddsw		mm2, mm4
				paddsw		mm5, mm6

				paddsw		mm1, mm2
				paddsw		mm3, mm5

				movq		mm6, [ebx+esi]

				movq		mm4, mm6
				punpcklbw	mm4, mm0
				punpckhbw	mm6, mm0

				psllw		mm4, 2
				psllw		mm6, 2

				paddsw		mm1, mm4
				paddsw		mm3, mm6

				psrlw		mm1, 3
				psrlw		mm3, 3

				packuswb	mm1, mm0
				packuswb	mm3, mm0

				psllq		mm3, 32
				por			mm1, mm3

				add			esi, 0x08
				cmp			esi, [w]
				movq		[ebx+esi-8], mm1
				jl			mc7

				add			eax, [lx2]
				add			ebx, [lx2]
				add			ecx, [lx2]
				dec			edi
				mov			esi, 0x00
				cmp			edi, 0x00
				jg			mc7
			}
			break;
	}
}
