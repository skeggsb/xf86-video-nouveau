/*
 * Copyright 2007 Ben Skeggs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nv_include.h"
#include "nv30_shaders.h"

#include "hwdefs/nv_object.xml.h"
#include "hwdefs/nv30-40_3d.xml.h"
#include "nv04_accel.h"

typedef struct nv_pict_surface_format {
	int	 pict_fmt;
	uint32_t card_fmt;
} nv_pict_surface_format_t;

typedef struct nv_pict_texture_format {
	int	 pict_fmt;
	uint32_t card_fmt;
	uint32_t card_swz;
} nv_pict_texture_format_t;

typedef struct nv_pict_op {
	Bool	 src_alpha;
	Bool	 dst_alpha;
	uint32_t src_card_op;
	uint32_t dst_card_op;
} nv_pict_op_t;

typedef struct nv40_exa_state {
	Bool have_mask;

	struct {
		PictTransformPtr transform;
		float width;
		float height;
	} unit[2];
} nv40_exa_state_t;
static nv40_exa_state_t exa_state;
#define NV40EXA_STATE nv40_exa_state_t *state = &exa_state

static nv_pict_surface_format_t
NV40SurfaceFormat[] = {
	{ PICT_a8r8g8b8	, NV30_3D_RT_FORMAT_COLOR_A8R8G8B8 },
	{ PICT_x8r8g8b8	, NV30_3D_RT_FORMAT_COLOR_X8R8G8B8 },
	{ PICT_r5g6b5	, NV30_3D_RT_FORMAT_COLOR_R5G6B5   },
	{ PICT_a8       , NV30_3D_RT_FORMAT_COLOR_B8       },
	{ -1, ~0 }
};

static nv_pict_surface_format_t *
NV40_GetPictSurfaceFormat(int format)
{
	int i = 0;

	while (NV40SurfaceFormat[i].pict_fmt != -1) {
		if (NV40SurfaceFormat[i].pict_fmt == format)
			return &NV40SurfaceFormat[i];
		i++;
	}

	return NULL;
}

enum {
	NV40EXA_FPID_PASS_COL0 = 0,
	NV40EXA_FPID_PASS_TEX0 = 1,
	NV40EXA_FPID_COMPOSITE_MASK = 2,
	NV40EXA_FPID_COMPOSITE_MASK_SA_CA = 3,
	NV40EXA_FPID_COMPOSITE_MASK_CA = 4,
	NV40EXA_FPID_MAX = 5
} NV40EXA_FPID;

static nv_shader_t *nv40_fp_map[NV40EXA_FPID_MAX] = {
	&nv30_fp_pass_col0,
	&nv30_fp_pass_tex0,
	&nv30_fp_composite_mask,
	&nv30_fp_composite_mask_sa_ca,
	&nv30_fp_composite_mask_ca
};

static nv_shader_t *nv40_fp_map_a8[NV40EXA_FPID_MAX];

static void
NV40EXAHackupA8Shaders(ScrnInfoPtr pScrn)
{
	int s;

	for (s = 0; s < NV40EXA_FPID_MAX; s++) {
		nv_shader_t *def, *a8;

		def = nv40_fp_map[s];
		a8 = calloc(1, sizeof(nv_shader_t));
		a8->card_priv.NV30FP.num_regs = def->card_priv.NV30FP.num_regs;
		a8->size = def->size + 4;
		memcpy(a8->data, def->data, def->size * sizeof(uint32_t));
		nv40_fp_map_a8[s] = a8;

		a8->data[a8->size - 8 + 0] &= ~0x00000081;
		a8->data[a8->size - 4 + 0]  = 0x01401e81;
		a8->data[a8->size - 4 + 1]  = 0x1c9dfe00;
		a8->data[a8->size - 4 + 2]  = 0x0001c800;
		a8->data[a8->size - 4 + 3]  = 0x0001c800;
	}
}

#define _(r,tf,ts0x,ts0y,ts0z,ts0w,ts1x,ts1y,ts1z,ts1w)                        \
  {                                                                            \
  PICT_##r, NV40_3D_TEX_FORMAT_FORMAT_##tf,                                    \
  NV30_3D_TEX_SWIZZLE_S0_X_##ts0x | NV30_3D_TEX_SWIZZLE_S0_Y_##ts0y |          \
  NV30_3D_TEX_SWIZZLE_S0_Z_##ts0z | NV30_3D_TEX_SWIZZLE_S0_W_##ts0w |          \
  NV30_3D_TEX_SWIZZLE_S1_X_##ts1x | NV30_3D_TEX_SWIZZLE_S1_Y_##ts1y |          \
  NV30_3D_TEX_SWIZZLE_S1_Z_##ts1z | NV30_3D_TEX_SWIZZLE_S1_W_##ts1w,           \
  }
static nv_pict_texture_format_t
NV40TextureFormat[] = {
        _(a8r8g8b8, A8R8G8B8,   S1,   S1,   S1,   S1, X, Y, Z, W),
        _(x8r8g8b8, A8R8G8B8,   S1,   S1,   S1,  ONE, X, Y, Z, W),
        _(x8b8g8r8, A8R8G8B8,   S1,   S1,   S1,  ONE, Z, Y, X, W),
        _(a1r5g5b5, A1R5G5B5,   S1,   S1,   S1,   S1, X, Y, Z, W),
        _(x1r5g5b5, A1R5G5B5,   S1,   S1,   S1,  ONE, X, Y, Z, W),
        _(  r5g6b5,   R5G6B5,   S1,   S1,   S1,   S1, X, Y, Z, W),
        _(      a8,       L8, ZERO, ZERO, ZERO,   S1, X, X, X, X),
        { -1, ~0, ~0 }
};
#undef _

static nv_pict_texture_format_t *
NV40_GetPictTextureFormat(int format)
{
	int i = 0;

	while (NV40TextureFormat[i].pict_fmt != -1) {
		if (NV40TextureFormat[i].pict_fmt == format)
			return &NV40TextureFormat[i];
		i++;
	}

	return NULL;
}

#define SF(bf) (NV30_3D_BLEND_FUNC_SRC_RGB_##bf |                              \
		NV30_3D_BLEND_FUNC_SRC_ALPHA_##bf)
#define DF(bf) (NV30_3D_BLEND_FUNC_DST_RGB_##bf |                              \
		NV30_3D_BLEND_FUNC_DST_ALPHA_##bf)
static nv_pict_op_t 
NV40PictOp[] = {
/* Clear       */ { 0, 0, SF(               ZERO), DF(               ZERO) },
/* Src         */ { 0, 0, SF(                ONE), DF(               ZERO) },
/* Dst         */ { 0, 0, SF(               ZERO), DF(                ONE) },
/* Over        */ { 1, 0, SF(                ONE), DF(ONE_MINUS_SRC_ALPHA) },
/* OverReverse */ { 0, 1, SF(ONE_MINUS_DST_ALPHA), DF(                ONE) },
/* In          */ { 0, 1, SF(          DST_ALPHA), DF(               ZERO) },
/* InReverse   */ { 1, 0, SF(               ZERO), DF(          SRC_ALPHA) },
/* Out         */ { 0, 1, SF(ONE_MINUS_DST_ALPHA), DF(               ZERO) },
/* OutReverse  */ { 1, 0, SF(               ZERO), DF(ONE_MINUS_SRC_ALPHA) },
/* Atop        */ { 1, 1, SF(          DST_ALPHA), DF(ONE_MINUS_SRC_ALPHA) },
/* AtopReverse */ { 1, 1, SF(ONE_MINUS_DST_ALPHA), DF(          SRC_ALPHA) },
/* Xor         */ { 1, 1, SF(ONE_MINUS_DST_ALPHA), DF(ONE_MINUS_SRC_ALPHA) },
/* Add         */ { 0, 0, SF(                ONE), DF(                ONE) }
};

static nv_pict_op_t *
NV40_GetPictOpRec(int op)
{
	if (op >= PictOpSaturate)
		return NULL;
	return &NV40PictOp[op];
}

static void
NV40_SetupBlend(ScrnInfoPtr pScrn, nv_pict_op_t *blend,
		PictFormatShort dest_format, Bool component_alpha)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	uint32_t sblend, dblend;

	sblend = blend->src_card_op;
	dblend = blend->dst_card_op;

	if (blend->dst_alpha) {
		if (!PICT_FORMAT_A(dest_format)) {
			if (sblend == SF(DST_ALPHA)) {
				sblend = SF(ONE);
			} else if (sblend == SF(ONE_MINUS_DST_ALPHA)) {
				sblend = SF(ZERO);
			}
		} else if (dest_format == PICT_a8) {
			if (sblend == SF(DST_ALPHA)) {
				sblend = SF(DST_COLOR);
			} else if (sblend == SF(ONE_MINUS_DST_ALPHA)) {
				sblend = SF(ONE_MINUS_DST_COLOR);
			}
		}
	}

	if (blend->src_alpha && (component_alpha || dest_format == PICT_a8)) {
		if (dblend == DF(SRC_ALPHA)) {
			dblend = DF(SRC_COLOR);
		} else if (dblend == DF(ONE_MINUS_SRC_ALPHA)) {
			dblend = DF(ONE_MINUS_SRC_COLOR);
		}
	}

	if (sblend == SF(ONE) && dblend == DF(ZERO)) {
		BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 1);
		PUSH_DATA (push, 0);
	} else {
		BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 5);
		PUSH_DATA (push, 1);
		PUSH_DATA (push, sblend);
		PUSH_DATA (push, dblend);
		PUSH_DATA (push, 0x00000000);
		PUSH_DATA (push, NV40_3D_BLEND_EQUATION_ALPHA_FUNC_ADD |
				 NV40_3D_BLEND_EQUATION_RGB_FUNC_ADD);
	}
}

static Bool
NV40EXATexture(ScrnInfoPtr pScrn, PixmapPtr pPix, PicturePtr pPict, int unit)
{
	NVPtr pNv = NVPTR(pScrn);
	unsigned reloc = NOUVEAU_BO_VRAM | NOUVEAU_BO_GART | NOUVEAU_BO_WR;
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pPix);
	nv_pict_texture_format_t *fmt;
	NV40EXA_STATE;

	fmt = NV40_GetPictTextureFormat(pPict->format);
	if (!fmt)
		return FALSE;

	BEGIN_NV04(push, NV30_3D(TEX_OFFSET(unit)), 8);
	PUSH_MTHDl(push, NV30_3D(TEX_OFFSET(unit)), bo, 0, reloc);
	PUSH_MTHDs(push, NV30_3D(TEX_FORMAT(unit)), bo, fmt->card_fmt |
			 NV40_3D_TEX_FORMAT_LINEAR |
			 NV30_3D_TEX_FORMAT_DIMS_2D | 0x8000 |
			 NV30_3D_TEX_FORMAT_NO_BORDER |
			 (1 << NV40_3D_TEX_FORMAT_MIPMAP_COUNT__SHIFT),
			 reloc | NOUVEAU_BO_OR,
			 NV30_3D_TEX_FORMAT_DMA0, NV30_3D_TEX_FORMAT_DMA1);
	if (pPict->repeat) {
		switch(pPict->repeatType) {
		case RepeatPad:
			PUSH_DATA (push, NV30_3D_TEX_WRAP_S_CLAMP | 
					 NV30_3D_TEX_WRAP_T_CLAMP |
					 NV30_3D_TEX_WRAP_R_CLAMP);
			break;
		case RepeatReflect:
			PUSH_DATA (push, NV30_3D_TEX_WRAP_S_MIRRORED_REPEAT |
					 NV30_3D_TEX_WRAP_T_MIRRORED_REPEAT |
					 NV30_3D_TEX_WRAP_R_MIRRORED_REPEAT);
			break;
		case RepeatNormal:
		default:
			PUSH_DATA (push, NV30_3D_TEX_WRAP_S_REPEAT |
					 NV30_3D_TEX_WRAP_T_REPEAT |
					 NV30_3D_TEX_WRAP_R_REPEAT);
			break;
		}
	} else {
		PUSH_DATA (push, NV30_3D_TEX_WRAP_S_CLAMP_TO_BORDER |
				 NV30_3D_TEX_WRAP_T_CLAMP_TO_BORDER |
				 NV30_3D_TEX_WRAP_R_CLAMP_TO_BORDER);
	}
	PUSH_DATA (push, NV40_3D_TEX_ENABLE_ENABLE);
	PUSH_DATA (push, fmt->card_swz);
	if (pPict->filter == PictFilterBilinear) {
		PUSH_DATA (push, NV30_3D_TEX_FILTER_MIN_LINEAR |
				 NV30_3D_TEX_FILTER_MAG_LINEAR | 0x3fd6);
	} else {
		PUSH_DATA (push, NV30_3D_TEX_FILTER_MIN_NEAREST |
				 NV30_3D_TEX_FILTER_MAG_NEAREST | 0x3fd6);
	}
	PUSH_DATA (push, (pPix->drawable.width << 16) | pPix->drawable.height);
	PUSH_DATA (push, 0); /* border ARGB */
	BEGIN_NV04(push, NV40_3D(TEX_SIZE1(unit)), 1);
	PUSH_DATA (push, (1 << NV40_3D_TEX_SIZE1_DEPTH__SHIFT) |
			 (uint32_t)exaGetPixmapPitch(pPix));

	state->unit[unit].width		= (float)pPix->drawable.width;
	state->unit[unit].height	= (float)pPix->drawable.height;
	state->unit[unit].transform	= pPict->transform;
	return TRUE;
}

static Bool
NV40_SetupSurface(ScrnInfoPtr pScrn, PixmapPtr pPix, PictFormatShort format)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pPix);
	nv_pict_surface_format_t *fmt;

	fmt = NV40_GetPictSurfaceFormat(format);
	if (!fmt) {
		ErrorF("AIII no format\n");
		return FALSE;
	}

	BEGIN_NV04(push, NV30_3D(RT_FORMAT), 3);
	PUSH_DATA (push, NV30_3D_RT_FORMAT_TYPE_LINEAR |
			 NV30_3D_RT_FORMAT_ZETA_Z24S8 | fmt->card_fmt);
	PUSH_DATA (push, exaGetPixmapPitch(pPix));
	PUSH_MTHDl(push, NV30_3D(COLOR0_OFFSET), bo, 0,
			 NOUVEAU_BO_VRAM | NOUVEAU_BO_RDWR);
	return TRUE;
}

static Bool
NV40EXACheckCompositeTexture(PicturePtr pPict, PicturePtr pdPict, int op)
{
	nv_pict_texture_format_t *fmt;
	int w, h;

	if (!pPict->pDrawable)
		NOUVEAU_FALLBACK("Solid and gradient pictures unsupported\n");

	w = pPict->pDrawable->width;
	h = pPict->pDrawable->height;

	if ((w > 4096) || (h > 4096))
		NOUVEAU_FALLBACK("picture too large, %dx%d\n", w, h);

	fmt = NV40_GetPictTextureFormat(pPict->format);
	if (!fmt)
		NOUVEAU_FALLBACK("picture format 0x%08x not supported\n",
				pPict->format);

	if (pPict->filter != PictFilterNearest &&
	    pPict->filter != PictFilterBilinear)
		NOUVEAU_FALLBACK("filter 0x%x not supported\n", pPict->filter);

	/* Opengl and Render disagree on what should be sampled outside an XRGB 
	 * texture (with no repeating). Opengl has a hardcoded alpha value of 
	 * 1.0, while render expects 0.0. We assume that clipping is done for 
	 * untranformed sources.
	 */
	if (NV40PictOp[op].src_alpha && !pPict->repeat &&
		pPict->transform && (PICT_FORMAT_A(pPict->format) == 0)
		&& (PICT_FORMAT_A(pdPict->format) != 0))
		NOUVEAU_FALLBACK("REPEAT_NONE unsupported for XRGB source\n");

	return TRUE;
}

Bool
NV40EXACheckComposite(int op, PicturePtr psPict,
			      PicturePtr pmPict,
			      PicturePtr pdPict)
{
	nv_pict_surface_format_t *fmt;
	nv_pict_op_t *opr;

	opr = NV40_GetPictOpRec(op);
	if (!opr)
		NOUVEAU_FALLBACK("unsupported blend op 0x%x\n", op);

	fmt = NV40_GetPictSurfaceFormat(pdPict->format);
	if (!fmt)
		NOUVEAU_FALLBACK("dst picture format 0x%08x not supported\n",
				pdPict->format);

	if (!NV40EXACheckCompositeTexture(psPict, pdPict, op))
		NOUVEAU_FALLBACK("src picture\n");
	if (pmPict) {
		if (pmPict->componentAlpha && 
		    PICT_FORMAT_RGB(pmPict->format) &&
		    opr->src_alpha && opr->src_card_op != SF(ZERO))
			NOUVEAU_FALLBACK("mask CA + SA\n");
		if (!NV40EXACheckCompositeTexture(pmPict, pdPict, op))
			NOUVEAU_FALLBACK("mask picture\n");
	}

	return TRUE;
}

Bool
NV40EXAPrepareComposite(int op, PicturePtr psPict,
				PicturePtr pmPict,
				PicturePtr pdPict,
				PixmapPtr  psPix,
				PixmapPtr  pmPix,
				PixmapPtr  pdPix)
{
	ScrnInfoPtr pScrn = xf86Screens[psPix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	nv_pict_op_t *blend = NV40_GetPictOpRec(op);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	int fpid = NV40EXA_FPID_PASS_COL0;
	NV40EXA_STATE;

	if (!PUSH_SPACE(push, 128))
		NOUVEAU_FALLBACK("space\n");
	PUSH_RESET(push);

	NV40_SetupBlend(pScrn, blend, pdPict->format,
			(pmPict && pmPict->componentAlpha &&
			 PICT_FORMAT_RGB(pmPict->format)));

	if (!NV40_SetupSurface(pScrn, pdPix, pdPict->format) ||
	    !NV40EXATexture(pScrn, psPix, psPict, 0))
		return FALSE;

	NV40_LoadVtxProg(pScrn, &nv40_vp_exa_render);
	if (pmPict) {
		if (!NV40EXATexture(pScrn, pmPix, pmPict, 1))
			return FALSE;

		if (pmPict->componentAlpha && PICT_FORMAT_RGB(pmPict->format)) {
			if (blend->src_alpha)
				fpid = NV40EXA_FPID_COMPOSITE_MASK_SA_CA;
			else
				fpid = NV40EXA_FPID_COMPOSITE_MASK_CA;
		} else {
			fpid = NV40EXA_FPID_COMPOSITE_MASK;
		}

		state->have_mask = TRUE;
	} else {
		fpid = NV40EXA_FPID_PASS_TEX0;

		state->have_mask = FALSE;
	}


	if (!NV40_LoadFragProg(pScrn, (pdPict->format == PICT_a8) ?
			       nv40_fp_map_a8[fpid] : nv40_fp_map[fpid]))
		return FALSE;

	/* Appears to be some kind of cache flush, needed here at least
	 * sometimes.. funky text rendering otherwise :)
	 */
	BEGIN_NV04(push, NV40_3D(TEX_CACHE_CTL), 1);
	PUSH_DATA (push, 2);
	BEGIN_NV04(push, NV40_3D(TEX_CACHE_CTL), 1);
	PUSH_DATA (push, 1);

	nouveau_pushbuf_bufctx(push, pNv->bufctx);
	if (nouveau_pushbuf_validate(push)) {
		nouveau_pushbuf_bufctx(push, NULL);
		return FALSE;
	}

	return TRUE;
}

#define xFixedToFloat(v) \
	((float)xFixedToInt((v)) + ((float)xFixedFrac(v) / 65536.0))

static inline void
NV40EXATransformCoord(PictTransformPtr t, int x, int y, float sx, float sy,
					  float *x_ret, float *y_ret)
{
	if (t) {
		PictVector v;
		v.vector[0] = IntToxFixed(x);
		v.vector[1] = IntToxFixed(y);
		v.vector[2] = xFixed1;
		PictureTransformPoint(t, &v);
		*x_ret = xFixedToFloat(v.vector[0]) / sx;
		*y_ret = xFixedToFloat(v.vector[1]) / sy;
	} else {
		*x_ret = (float)x / sx;
		*y_ret = (float)y / sy;
	}
}

#define CV_OUTm(sx,sy,mx,my,dx,dy) do {                                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2F_X(8)), 4);                  \
	PUSH_DATAf(push, (sx)); PUSH_DATAf(push, (sy));                        \
	PUSH_DATAf(push, (mx)); PUSH_DATAf(push, (my));                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2I(0)), 1);                    \
	PUSH_DATA (push, ((dy)<<16)|(dx));                                     \
} while(0)
#define CV_OUT(sx,sy,dx,dy) do {                                               \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2F_X(8)), 2);                  \
	PUSH_DATAf(push, (sx)); PUSH_DATAf(push, (sy));                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2I(0)), 1);                    \
	PUSH_DATA (push, ((dy)<<16)|(dx));                                     \
} while(0)

void
NV40EXAComposite(PixmapPtr pdPix, int srcX , int srcY,
				  int maskX, int maskY,
				  int dstX , int dstY,
				  int width, int height)
{
	ScrnInfoPtr pScrn = xf86Screens[pdPix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	float sX0, sX1, sX2, sY0, sY1, sY2;
	float mX0, mX1, mX2, mY0, mY1, mY2;
	NV40EXA_STATE;

	if (!PUSH_SPACE(push, 64))
		return;

	/* We're drawing a triangle, we need to scissor it to a quad. */
	/* The scissors are here for a good reason, we don't get the full
	 * image, but just a part.
	 */
	/* Handling the cliprects is done for us already. */
	BEGIN_NV04(push, NV30_3D(SCISSOR_HORIZ), 2);
	PUSH_DATA (push, (width << 16) | dstX);
	PUSH_DATA (push, (height << 16) | dstY);
	BEGIN_NV04(push, NV30_3D(VERTEX_BEGIN_END), 1);
	PUSH_DATA (push, NV30_3D_VERTEX_BEGIN_END_TRIANGLES);

	NV40EXATransformCoord(state->unit[0].transform, srcX, srcY - height,
			      state->unit[0].width, state->unit[0].height,
			      &sX0, &sY0);
	NV40EXATransformCoord(state->unit[0].transform, srcX, srcY + height,
			      state->unit[0].width, state->unit[0].height,
			      &sX1, &sY1);
	NV40EXATransformCoord(state->unit[0].transform,
			      srcX + 2*width, srcY + height,
			      state->unit[0].width,
			      state->unit[0].height, &sX2, &sY2);

	if (state->have_mask) {
		NV40EXATransformCoord(state->unit[1].transform,
				      maskX, maskY - height,
				      state->unit[1].width,
				      state->unit[1].height, &mX0, &mY0);
		NV40EXATransformCoord(state->unit[1].transform,
				      maskX, maskY + height,
				      state->unit[1].width,
				      state->unit[1].height, &mX1, &mY1);
		NV40EXATransformCoord(state->unit[1].transform,
				      maskX + 2*width, maskY + height,
				      state->unit[1].width,
				      state->unit[1].height, &mX2, &mY2);

		CV_OUTm(sX0, sY0, mX0, mY0, dstX, dstY - height);
		CV_OUTm(sX1, sY1, mX1, mY1, dstX, dstY + height);
		CV_OUTm(sX2, sY2, mX2, mY2, dstX + 2*width, dstY + height);
	} else {
		CV_OUT(sX0, sY0, dstX, dstY - height);
		CV_OUT(sX1, sY1, dstX, dstY + height);
		CV_OUT(sX2, sY2, dstX + 2*width, dstY + height);
	}

	BEGIN_NV04(push, NV30_3D(VERTEX_BEGIN_END), 1);
	PUSH_DATA (push, NV30_3D_VERTEX_BEGIN_END_STOP);
}

void
NV40EXADoneComposite(PixmapPtr pdPix)
{
	ScrnInfoPtr pScrn = xf86Screens[pdPix->drawable.pScreen->myNum];
	nouveau_pushbuf_bufctx(NVPTR(pScrn)->pushbuf, NULL);
}

#define NV30_3D_CHIPSET_4X_MASK 0x00000baf
#define NV44TCL_CHIPSET_4X_MASK 0x00005450
Bool
NVAccelInitNV40TCL(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nv04_fifo *fifo = pNv->channel->data;
	uint32_t class = 0, chipset;
	int next_hw_id = 0, next_hw_offset = 0, i;

	if (!nv40_fp_map_a8[0])
		NV40EXAHackupA8Shaders(pScrn);

	chipset = pNv->dev->chipset;
	if ((chipset & 0xf0) == NV_ARCH_40) {
		chipset &= 0xf;
		if (NV30_3D_CHIPSET_4X_MASK & (1<<chipset))
			class = NV40_3D_CLASS;
		else if (NV44TCL_CHIPSET_4X_MASK & (1<<chipset))
			class = NV44_3D_CLASS;
		else {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"NV40EXA: Unknown chipset NV4%1x\n", chipset);
			return FALSE;
		}
	} else if ( (chipset & 0xf0) == 0x60) {
		class = NV44_3D_CLASS;
	} else
		return TRUE;

	if (nouveau_object_new(pNv->channel, Nv3D, class, NULL, 0, &pNv->Nv3D))
		return FALSE;

	if (nouveau_bo_new(pNv->dev, NOUVEAU_BO_VRAM | NOUVEAU_BO_GART |
			   NOUVEAU_BO_MAP, 0, 0x1000, NULL,
			   &pNv->shader_mem)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Couldn't alloc fragprog buffer!\n");
		nouveau_object_del(&pNv->Nv3D);
		return FALSE;
	}

	if (!PUSH_SPACE(push, 256))
		return FALSE;

	BEGIN_NV04(push, NV01_SUBC(3D, OBJECT), 1);
	PUSH_DATA (push, pNv->Nv3D->handle);
	BEGIN_NV04(push, NV30_3D(DMA_NOTIFY), 1);
	PUSH_DATA (push, pNv->notify0->handle);
	BEGIN_NV04(push, NV30_3D(DMA_TEXTURE0), 2);
	PUSH_DATA (push, fifo->vram);
	PUSH_DATA (push, fifo->gart);
	BEGIN_NV04(push, NV30_3D(DMA_COLOR0), 2);
	PUSH_DATA (push, fifo->vram);
	PUSH_DATA (push, fifo->vram);

	/* voodoo */
	BEGIN_NV04(push, SUBC_3D(0x1ea4), 3);
	PUSH_DATA (push, 0x00000010);
	PUSH_DATA (push, 0x01000100);
	PUSH_DATA (push, 0xff800006);
	BEGIN_NV04(push, SUBC_3D(0x1fc4), 1);
	PUSH_DATA (push, 0x06144321);
	BEGIN_NV04(push, SUBC_3D(0x1fc8), 2);
	PUSH_DATA (push, 0xedcba987);
	PUSH_DATA (push, 0x00000021);
	BEGIN_NV04(push, SUBC_3D(0x1fd0), 1);
	PUSH_DATA (push, 0x00171615);
	BEGIN_NV04(push, SUBC_3D(0x1fd4), 1);
	PUSH_DATA (push, 0x001b1a19);
	BEGIN_NV04(push, SUBC_3D(0x1ef8), 1);
	PUSH_DATA (push, 0x0020ffff);
	BEGIN_NV04(push, SUBC_3D(0x1d64), 1);
	PUSH_DATA (push, 0x00d30000);
	BEGIN_NV04(push, SUBC_3D(0x1e94), 1);
	PUSH_DATA (push, 0x00000001);

	/* This removes the the stair shaped tearing that i get. */
	/* Verified on one G70 card that it doesn't cause regressions for people without the problem. */
	/* The blob sets this up by default for NV43. */
	BEGIN_NV04(push, SUBC_3D(0x1450), 1);
	PUSH_DATA (push, 0x0000000F);

	BEGIN_NV04(push, NV30_3D(VIEWPORT_TRANSLATE_X), 8);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);

	/* default 3D state */
	/*XXX: replace with the same state that the DRI emits on startup */
	BEGIN_NV04(push, NV30_3D(STENCIL_ENABLE(0)), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(STENCIL_ENABLE(1)), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(ALPHA_FUNC_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(DEPTH_WRITE_ENABLE), 2);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 0); 
	BEGIN_NV04(push, NV30_3D(COLOR_MASK), 1);
	PUSH_DATA (push, 0x01010101); /* TR,TR,TR,TR */
	BEGIN_NV04(push, NV30_3D(CULL_FACE_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(COLOR_LOGIC_OP_ENABLE), 2);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, NV30_3D_COLOR_LOGIC_OP_OP_COPY);
	BEGIN_NV04(push, NV30_3D(DITHER_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(SHADE_MODEL), 1);
	PUSH_DATA (push, NV30_3D_SHADE_MODEL_SMOOTH);
	BEGIN_NV04(push, NV30_3D(POLYGON_OFFSET_FACTOR),2);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	BEGIN_NV04(push, NV30_3D(POLYGON_MODE_FRONT), 2);
	PUSH_DATA (push, NV30_3D_POLYGON_MODE_FRONT_FILL);
	PUSH_DATA (push, NV30_3D_POLYGON_MODE_BACK_FILL);
	BEGIN_NV04(push, NV30_3D(POLYGON_STIPPLE_PATTERN(0)), 0x20);
	for (i=0;i<0x20;i++)
		PUSH_DATA (push, 0xFFFFFFFF);
	for (i=0;i<16;i++) {
		BEGIN_NV04(push, NV30_3D(TEX_ENABLE(i)), 1);
		PUSH_DATA (push, 0);
	}

	BEGIN_NV04(push, SUBC_3D(0x1d78), 1);
	PUSH_DATA (push, 0x110);

	BEGIN_NV04(push, NV30_3D(RT_ENABLE), 1);
	PUSH_DATA (push, NV30_3D_RT_ENABLE_COLOR0);

	BEGIN_NV04(push, NV30_3D(RT_HORIZ), 2);
	PUSH_DATA (push, (4096 << 16));
	PUSH_DATA (push, (4096 << 16));
	BEGIN_NV04(push, NV30_3D(SCISSOR_HORIZ), 2);
	PUSH_DATA (push, (4096 << 16));
	PUSH_DATA (push, (4096 << 16));
	BEGIN_NV04(push, NV30_3D(VIEWPORT_HORIZ), 2);
	PUSH_DATA (push, (4096 << 16));
	PUSH_DATA (push, (4096 << 16));
	BEGIN_NV04(push, NV30_3D(VIEWPORT_CLIP_HORIZ(0)), 2);
	PUSH_DATA (push, (4095 << 16));
	PUSH_DATA (push, (4095 << 16));

	NV40_UploadVtxProg(pNv, &nv40_vp_exa_render, &next_hw_id);
	for (i = 0; i < NV40EXA_FPID_MAX; i++) {
		NV30_UploadFragProg(pNv, nv40_fp_map[i], &next_hw_offset);
		NV30_UploadFragProg(pNv, nv40_fp_map_a8[i], &next_hw_offset);
	}

	NV40_UploadVtxProg(pNv, &nv40_vp_video, &next_hw_id);
	NV30_UploadFragProg(pNv, &nv40_fp_yv12_bicubic, &next_hw_offset);
	NV30_UploadFragProg(pNv, &nv30_fp_yv12_bilinear, &next_hw_offset);

	return TRUE;
}
