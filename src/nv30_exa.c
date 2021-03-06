/*
 * Copyright 2007 Ben Skeggs
 * Copyright 2007 Stephane Marchesin
 * Copyright 2007 Jeremy Kolb
 * Copyright 2007 Patrice Mandin
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

typedef struct nv30_exa_state {
	Bool have_mask;

	struct {
		PictTransformPtr transform;
		float width;
		float height;
	} unit[2];
} nv30_exa_state_t;
static nv30_exa_state_t exa_state;
#define NV30EXA_STATE nv30_exa_state_t *state = &exa_state

static nv_pict_surface_format_t
NV30SurfaceFormat[] = {
	{ PICT_a8r8g8b8	, 0x148 },
	{ PICT_a8b8g8r8	, 0x150 },
	{ PICT_x8r8g8b8	, 0x145 },
	{ PICT_x8b8g8r8	, 0x14f },
	{ PICT_r5g6b5	, 0x143 },
	{ PICT_a8       , 0x149 },
	{ PICT_x1r5g5b5	, 0x142 },
};

static nv_pict_surface_format_t *
NV30_GetPictSurfaceFormat(int format)
{
	int i;

	for(i=0;i<sizeof(NV30SurfaceFormat)/sizeof(NV30SurfaceFormat[0]);i++)
	{
		if (NV30SurfaceFormat[i].pict_fmt == format)
			return &NV30SurfaceFormat[i];
	}

	return NULL;
}

enum {
	NV30EXA_FPID_PASS_COL0 = 0,
	NV30EXA_FPID_PASS_TEX0 = 1,
	NV30EXA_FPID_COMPOSITE_MASK = 2,
	NV30EXA_FPID_COMPOSITE_MASK_SA_CA = 3,
	NV30EXA_FPID_COMPOSITE_MASK_CA = 4,
	NV30EXA_FPID_MAX = 5
} NV30EXA_FPID;

static nv_shader_t *nv40_fp_map[NV30EXA_FPID_MAX] = {
	&nv30_fp_pass_col0,
	&nv30_fp_pass_tex0,
	&nv30_fp_composite_mask,
	&nv30_fp_composite_mask_sa_ca,
	&nv30_fp_composite_mask_ca
};

static nv_shader_t *nv40_fp_map_a8[NV30EXA_FPID_MAX];

static void
NV30EXAHackupA8Shaders(ScrnInfoPtr pScrn)
{
	int s;

	for (s = 0; s < NV30EXA_FPID_MAX; s++) {
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

/* should be in nouveau_reg.h at some point.. */
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_X_ZERO	 0
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_X_ONE	 1
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_X_S1		 2

#define NV30_3D_TEX_SWIZZLE_UNIT_S0_X_SHIFT	14
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_Y_SHIFT	12
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_Z_SHIFT	10
#define NV30_3D_TEX_SWIZZLE_UNIT_S0_W_SHIFT	 8

#define NV30_3D_TEX_SWIZZLE_UNIT_S1_X_X		 3
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_X_Y		 2
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_X_Z		 1
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_X_W		 0

#define NV30_3D_TEX_SWIZZLE_UNIT_S1_X_SHIFT	 6
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_Y_SHIFT	 4
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_Z_SHIFT	 2
#define NV30_3D_TEX_SWIZZLE_UNIT_S1_W_SHIFT	 0

#define _(r,tf,ts0x,ts0y,ts0z,ts0w,ts1x,ts1y,ts1z,ts1w)                       \
  {                                                                           \
  PICT_##r,                                                                   \
  (tf),                                                                       \
  (NV30_3D_TEX_SWIZZLE_UNIT_S0_X_##ts0x << NV30_3D_TEX_SWIZZLE_UNIT_S0_X_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S0_X_##ts0y << NV30_3D_TEX_SWIZZLE_UNIT_S0_Y_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S0_X_##ts0z << NV30_3D_TEX_SWIZZLE_UNIT_S0_Z_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S0_X_##ts0w << NV30_3D_TEX_SWIZZLE_UNIT_S0_W_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S1_X_##ts1x << NV30_3D_TEX_SWIZZLE_UNIT_S1_X_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S1_X_##ts1y << NV30_3D_TEX_SWIZZLE_UNIT_S1_Y_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S1_X_##ts1z << NV30_3D_TEX_SWIZZLE_UNIT_S1_Z_SHIFT)|\
  (NV30_3D_TEX_SWIZZLE_UNIT_S1_X_##ts1w << NV30_3D_TEX_SWIZZLE_UNIT_S1_W_SHIFT)\
  }

static nv_pict_texture_format_t
NV30TextureFormat[] = {
	_(a8r8g8b8, 0x12,   S1,   S1,   S1,   S1, X, Y, Z, W),
	_(a8b8g8r8, 0x12,   S1,   S1,   S1,   S1, Z, Y, X, W),
	_(x8r8g8b8, 0x12,   S1,   S1,   S1,  ONE, X, Y, Z, W),
	_(x8b8g8r8, 0x12,   S1,   S1,   S1,  ONE, Z, Y, X, W),

	_(a1r5g5b5, 0x10,   S1,   S1,   S1,   S1, X, Y, Z, W),
	_(x1r5g5b5, 0x10,   S1,   S1,   S1,  ONE, X, Y, Z, W),
	_(a1b5g5r5, 0x10,   S1,   S1,   S1,   S1, Z, Y, X, W),
	_(x1b5g5r5, 0x10,   S1,   S1,   S1,  ONE, Z, Y, X, W),

	_(x4r4g4b4, 0x1d,   S1,   S1,   S1,  ONE, X, Y, Z, W),
	_(a4r4g4b4, 0x1d,   S1,   S1,   S1,   S1, X, Y, Z, W),
	_(x4b4g4r4, 0x1d,   S1,   S1,   S1,  ONE, Z, Y, X, W),
	_(a4b4g4r4, 0x1d,   S1,   S1,   S1,   S1, Z, Y, X, W),

	_(      a8, 0x1b, ZERO, ZERO, ZERO,   S1, X, X, X, X),

	_(  r5g6b5, 0x11,   S1,   S1,   S1,  ONE, X, Y, Z, W),
	_(  b5g6r5, 0x11,   S1,   S1,   S1,  ONE, Z, Y, X, W),
};


static nv_pict_texture_format_t *
NV30_GetPictTextureFormat(int format)
{
	int i;

	for(i=0;i<sizeof(NV30TextureFormat)/sizeof(NV30TextureFormat[0]);i++)
	{
		if (NV30TextureFormat[i].pict_fmt == format)
			return &NV30TextureFormat[i];
	}

	return NULL;
}

#define NV30_3D_BF_ZERO                                     0x0000
#define NV30_3D_BF_ONE                                      0x0001
#define NV30_3D_BF_SRC_COLOR                                0x0300
#define NV30_3D_BF_ONE_MINUS_SRC_COLOR                      0x0301
#define NV30_3D_BF_SRC_ALPHA                                0x0302
#define NV30_3D_BF_ONE_MINUS_SRC_ALPHA                      0x0303
#define NV30_3D_BF_DST_ALPHA                                0x0304
#define NV30_3D_BF_ONE_MINUS_DST_ALPHA                      0x0305
#define NV30_3D_BF_DST_COLOR                                0x0306
#define NV30_3D_BF_ONE_MINUS_DST_COLOR                      0x0307
#define NV30_3D_BF_ALPHA_SATURATE                           0x0308
#define BF(bf) NV30_3D_BF_##bf

static nv_pict_op_t 
NV30PictOp[] = {
/* Clear       */ { 0, 0, BF(               ZERO), BF(               ZERO) },
/* Src         */ { 0, 0, BF(                ONE), BF(               ZERO) },
/* Dst         */ { 0, 0, BF(               ZERO), BF(                ONE) },
/* Over        */ { 1, 0, BF(                ONE), BF(ONE_MINUS_SRC_ALPHA) },
/* OverReverse */ { 0, 1, BF(ONE_MINUS_DST_ALPHA), BF(                ONE) },
/* In          */ { 0, 1, BF(          DST_ALPHA), BF(               ZERO) },
/* InReverse   */ { 1, 0, BF(               ZERO), BF(          SRC_ALPHA) },
/* Out         */ { 0, 1, BF(ONE_MINUS_DST_ALPHA), BF(               ZERO) },
/* OutReverse  */ { 1, 0, BF(               ZERO), BF(ONE_MINUS_SRC_ALPHA) },
/* Atop        */ { 1, 1, BF(          DST_ALPHA), BF(ONE_MINUS_SRC_ALPHA) },
/* AtopReverse */ { 1, 1, BF(ONE_MINUS_DST_ALPHA), BF(          SRC_ALPHA) },
/* Xor         */ { 1, 1, BF(ONE_MINUS_DST_ALPHA), BF(ONE_MINUS_SRC_ALPHA) },
/* Add         */ { 0, 0, BF(                ONE), BF(                ONE) }
};

static nv_pict_op_t *
NV30_GetPictOpRec(int op)
{
	if (op >= PictOpSaturate)
		return NULL;
#if 0
	switch(op)
	{
		case 0:ErrorF("Op Clear\n");break;
		case 1:ErrorF("Op Src\n");break;
		case 2:ErrorF("Op Dst\n");break;
		case 3:ErrorF("Op Over\n");break;
		case 4:ErrorF("Op OverReverse\n");break;
		case 5:ErrorF("Op In\n");break;
		case 6:ErrorF("Op InReverse\n");break;
		case 7:ErrorF("Op Out\n");break;
		case 8:ErrorF("Op OutReverse\n");break;
		case 9:ErrorF("Op Atop\n");break;
		case 10:ErrorF("Op AtopReverse\n");break;
		case 11:ErrorF("Op Xor\n");break;
		case 12:ErrorF("Op Add\n");break;
	}
#endif
	return &NV30PictOp[op];
}

static void
NV30_SetupBlend(ScrnInfoPtr pScrn, nv_pict_op_t *blend,
		PictFormatShort dest_format, Bool component_alpha)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	uint32_t sblend, dblend;

	sblend = blend->src_card_op;
	dblend = blend->dst_card_op;

	if (blend->dst_alpha) {
		if (!PICT_FORMAT_A(dest_format)) {
			if (sblend == BF(DST_ALPHA)) {
				sblend = BF(ONE);
			} else if (sblend == BF(ONE_MINUS_DST_ALPHA)) {
				sblend = BF(ZERO);
			}
		} else if (dest_format == PICT_a8) {
			if (sblend == BF(DST_ALPHA)) {
				sblend = BF(DST_COLOR);
			} else if (sblend == BF(ONE_MINUS_DST_ALPHA)) {
				sblend = BF(ONE_MINUS_DST_COLOR);
			}
		}
	}

	if (blend->src_alpha && (component_alpha || dest_format == PICT_a8)) {
		if (dblend == BF(SRC_ALPHA)) {
			dblend = BF(SRC_COLOR);
		} else if (dblend == BF(ONE_MINUS_SRC_ALPHA)) {
			dblend = BF(ONE_MINUS_SRC_COLOR);
		}
	}

	if (sblend == BF(ONE) && dblend == BF(ZERO)) {
		BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 1);
		PUSH_DATA (push, 0);
	} else {
		BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 3);
		PUSH_DATA (push, 1);
		PUSH_DATA (push, (sblend << 16) | sblend);
		PUSH_DATA (push, (dblend << 16) | dblend);
	}
}

static Bool
NV30EXATexture(ScrnInfoPtr pScrn, PixmapPtr pPix, PicturePtr pPict, int unit)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pPix);
	nv_pict_texture_format_t *fmt;
	unsigned reloc = NOUVEAU_BO_VRAM | NOUVEAU_BO_GART | NOUVEAU_BO_RD;
	uint32_t pitch = exaGetPixmapPitch(pPix);
	uint32_t log2h = log2i(pPix->drawable.height);
	uint32_t log2w = log2i(pPix->drawable.width);
	uint32_t card_filter, card_repeat;
	NV30EXA_STATE;

	fmt = NV30_GetPictTextureFormat(pPict->format);
	if (!fmt)
		return FALSE;

	card_repeat = 3; /* repeatNone */

	if (pPict->filter == PictFilterBilinear)
		card_filter = 2;
	else
		card_filter = 1;

	BEGIN_NV04(push, NV30_3D(TEX_OFFSET(unit)), 8);
	PUSH_MTHDl(push, NV30_3D(TEX_OFFSET(unit)), bo, 0, reloc);
	PUSH_MTHDs(push, NV30_3D(TEX_FORMAT(unit)), bo, (1 << 16) | 8 |
			 NV30_3D_TEX_FORMAT_DIMS_2D |
			 (fmt->card_fmt << NV30_3D_TEX_FORMAT_FORMAT__SHIFT) |
			 (log2w << NV30_3D_TEX_FORMAT_BASE_SIZE_U__SHIFT) |
			 (log2h << NV30_3D_TEX_FORMAT_BASE_SIZE_V__SHIFT),
			 reloc, NV30_3D_TEX_FORMAT_DMA0,
			 NV30_3D_TEX_FORMAT_DMA1);
	PUSH_DATA (push, (card_repeat << NV30_3D_TEX_WRAP_S__SHIFT) |
			 (card_repeat << NV30_3D_TEX_WRAP_T__SHIFT) |
			 (card_repeat << NV30_3D_TEX_WRAP_R__SHIFT));
	PUSH_DATA (push, NV30_3D_TEX_ENABLE_ENABLE);
	PUSH_DATA (push, (pitch << NV30_3D_TEX_SWIZZLE_RECT_PITCH__SHIFT ) |
			 fmt->card_swz);
	PUSH_DATA (push, (card_filter << NV30_3D_TEX_FILTER_MIN__SHIFT) |
			 (card_filter << NV30_3D_TEX_FILTER_MAG__SHIFT) |
			 0x2000 /* engine lock */);
	PUSH_DATA (push, (pPix->drawable.width <<
			  NV30_3D_TEX_NPOT_SIZE_W__SHIFT) |
			 pPix->drawable.height);
	PUSH_DATA (push, 0x00000000); /* border ARGB */

	state->unit[unit].width		= (float)pPix->drawable.width;
	state->unit[unit].height	= (float)pPix->drawable.height;
	state->unit[unit].transform	= pPict->transform;
	return TRUE;
}

static Bool
NV30_SetupSurface(ScrnInfoPtr pScrn, PixmapPtr pPix, PicturePtr pPict)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nouveau_bo *bo = nouveau_pixmap_bo(pPix);
	uint32_t pitch = exaGetPixmapPitch(pPix);
	nv_pict_surface_format_t *fmt;

	fmt = NV30_GetPictSurfaceFormat(pPict->format);
	if (!fmt) {
		ErrorF("AIII no format\n");
		return FALSE;
	}

	BEGIN_NV04(push, NV30_3D(RT_FORMAT), 3);
	PUSH_DATA (push, fmt->card_fmt); /* format */
	PUSH_DATA (push, pitch << 16 | pitch);
	PUSH_MTHDl(push, NV30_3D(COLOR0_OFFSET), bo, 0,
			 NOUVEAU_BO_VRAM | NOUVEAU_BO_RDWR);
	return TRUE;
}

static Bool
NV30EXACheckCompositeTexture(PicturePtr pPict, PicturePtr pdPict, int op)
{
	nv_pict_texture_format_t *fmt;
	int w, h;

	if (!pPict->pDrawable)
		NOUVEAU_FALLBACK("Solid and gradient pictures unsupported\n");

	w = pPict->pDrawable->width;
	h = pPict->pDrawable->height;

	if ((w > 4096) || (h > 4096))
		NOUVEAU_FALLBACK("picture too large, %dx%d\n", w, h);

	fmt = NV30_GetPictTextureFormat(pPict->format);
	if (!fmt)
		NOUVEAU_FALLBACK("picture format 0x%08x not supported\n",
				pPict->format);

	if (pPict->filter != PictFilterNearest &&
			pPict->filter != PictFilterBilinear)
		NOUVEAU_FALLBACK("filter 0x%x not supported\n", pPict->filter);

	if (!(w==1 && h==1) && pPict->repeat && pPict->repeatType != RepeatNone)
		NOUVEAU_FALLBACK("repeat 0x%x not supported (surface %dx%d)\n",
				 pPict->repeatType,w,h);

	/* Opengl and Render disagree on what should be sampled outside an XRGB 
	 * texture (with no repeating). Opengl has a hardcoded alpha value of 
	 * 1.0, while render expects 0.0. We assume that clipping is done for 
	 * untranformed sources.
	 */
	if (NV30PictOp[op].src_alpha && !pPict->repeat &&
		pPict->transform && (PICT_FORMAT_A(pPict->format) == 0)
		&& (PICT_FORMAT_A(pdPict->format) != 0))
		NOUVEAU_FALLBACK("REPEAT_NONE unsupported for XRGB source\n");

	return TRUE;
}

Bool
NV30EXACheckComposite(int op, PicturePtr psPict,
		PicturePtr pmPict,
		PicturePtr pdPict)
{
	nv_pict_surface_format_t *fmt;
	nv_pict_op_t *opr;

	opr = NV30_GetPictOpRec(op);
	if (!opr)
		NOUVEAU_FALLBACK("unsupported blend op 0x%x\n", op);

	fmt = NV30_GetPictSurfaceFormat(pdPict->format);
	if (!fmt)
		NOUVEAU_FALLBACK("dst picture format 0x%08x not supported\n",
				pdPict->format);

	if (!NV30EXACheckCompositeTexture(psPict, pdPict, op))
		NOUVEAU_FALLBACK("src picture\n");
	if (pmPict) {
		if (pmPict->componentAlpha &&
				PICT_FORMAT_RGB(pmPict->format) &&
				opr->src_alpha && opr->src_card_op != BF(ZERO))
			NOUVEAU_FALLBACK("mask CA + SA\n");
		if (!NV30EXACheckCompositeTexture(pmPict, pdPict, op))
			NOUVEAU_FALLBACK("mask picture\n");
	}

	return TRUE;
}

Bool
NV30EXAPrepareComposite(int op, PicturePtr psPict,
		PicturePtr pmPict,
		PicturePtr pdPict,
		PixmapPtr  psPix,
		PixmapPtr  pmPix,
		PixmapPtr  pdPix)
{
	ScrnInfoPtr pScrn = xf86Screens[psPix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	nv_pict_op_t *blend = NV30_GetPictOpRec(op);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	int fpid = NV30EXA_FPID_PASS_COL0;
	NV30EXA_STATE;

	if (!PUSH_SPACE(push, 128))
		return FALSE;
	PUSH_RESET(push);

	NV30_SetupBlend(pScrn, blend, pdPict->format,
			(pmPict && pmPict->componentAlpha &&
			 PICT_FORMAT_RGB(pmPict->format)));

	if (!NV30_SetupSurface(pScrn, pdPix, pdPict) ||
	    !NV30EXATexture(pScrn, psPix, psPict, 0))
		return FALSE;

	if (pmPict) {
		if (!NV30EXATexture(pScrn, pmPix, pmPict, 1))
			return FALSE;

		if (pmPict->componentAlpha && PICT_FORMAT_RGB(pmPict->format)) {
			if (blend->src_alpha)
				fpid = NV30EXA_FPID_COMPOSITE_MASK_SA_CA;
			else
				fpid = NV30EXA_FPID_COMPOSITE_MASK_CA;
		} else {
			fpid = NV30EXA_FPID_COMPOSITE_MASK;
		}

		state->have_mask = TRUE;
	} else {
		fpid = NV30EXA_FPID_PASS_TEX0;

		state->have_mask = FALSE;
	}

	if (!NV30_LoadFragProg(pScrn, (pdPict->format == PICT_a8) ?
			       nv40_fp_map_a8[fpid] : nv40_fp_map[fpid]))
		return FALSE;

	BEGIN_NV04(push, NV30_3D(TEX_UNITS_ENABLE), 1);
	PUSH_DATA (push, pmPict ? 3 : 1);

	nouveau_pushbuf_bufctx(push, pNv->bufctx);
	if (nouveau_pushbuf_validate(push)) {
		nouveau_pushbuf_bufctx(push, NULL);
		return FALSE;
	}

	return TRUE;
}

#define xFixedToFloat(v) \
	((float)xFixedToInt((v)) + ((float)xFixedFrac(v) / 65536.0))

static void
NV30EXATransformCoord(PictTransformPtr t, int x, int y, float sx, float sy,
					  float *x_ret, float *y_ret)
{
	PictVector v;

	if (t) {
		v.vector[0] = IntToxFixed(x);
		v.vector[1] = IntToxFixed(y);
		v.vector[2] = xFixed1;
		PictureTransformPoint(t, &v);
		*x_ret = xFixedToFloat(v.vector[0]);
		*y_ret = xFixedToFloat(v.vector[1]);
	} else {
		*x_ret = (float)x;
		*y_ret = (float)y;
	}
}

#define CV_OUTm(sx,sy,mx,my,dx,dy) do {                                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2F_X(8)), 4);                        \
	PUSH_DATAf(push, (sx)); PUSH_DATAf(push, (sy));                        \
	PUSH_DATAf(push, (mx)); PUSH_DATAf(push, (my));                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2I(0)), 1);                          \
	PUSH_DATA (push, ((dy)<<16)|(dx));                                     \
} while(0)
#define CV_OUT(sx,sy,dx,dy) do {                                               \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2F_X(8)), 2);                        \
	PUSH_DATAf(push, (sx)); PUSH_DATAf(push, (sy));                        \
	BEGIN_NV04(push, NV30_3D(VTX_ATTR_2I(0)), 1);                          \
	PUSH_DATA (push, ((dy)<<16)|(dx));                                     \
} while(0)

void
NV30EXAComposite(PixmapPtr pdPix, int srcX , int srcY,
				  int maskX, int maskY,
				  int dstX , int dstY,
				  int width, int height)
{
	ScrnInfoPtr pScrn = xf86Screens[pdPix->drawable.pScreen->myNum];
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	float sX0, sX1, sX2, sY0, sY1, sY2;
	float mX0, mX1, mX2, mY0, mY1, mY2;
	NV30EXA_STATE;

	if (!PUSH_SPACE(push, 64))
		return;

	/* We're drawing a triangle, we need to scissor it to a quad. */
	/* The scissors are here for a good reason, we don't get the full
	 * image, but just a part. */
	/* Handling the cliprects is done for us already. */
	BEGIN_NV04(push, NV30_3D(SCISSOR_HORIZ), 2);
	PUSH_DATA (push, (width << 16) | dstX);
	PUSH_DATA (push, (height << 16) | dstY);
	BEGIN_NV04(push, NV30_3D(VERTEX_BEGIN_END), 1);
	PUSH_DATA (push, NV30_3D_VERTEX_BEGIN_END_TRIANGLES);

	NV30EXATransformCoord(state->unit[0].transform, 
				srcX, srcY - height,
				state->unit[0].width,
				state->unit[0].height, &sX0, &sY0);
	NV30EXATransformCoord(state->unit[0].transform,
				srcX, srcY + height,
				state->unit[0].width,
				state->unit[0].height, &sX1, &sY1);
	NV30EXATransformCoord(state->unit[0].transform,
				srcX + 2*width, srcY + height,
				state->unit[0].width,
				state->unit[0].height, &sX2, &sY2);

	if (state->have_mask) {
		NV30EXATransformCoord(state->unit[1].transform, 
					maskX, maskY - height,
					state->unit[1].width,
					state->unit[1].height, &mX0, &mY0);
		NV30EXATransformCoord(state->unit[1].transform,
					maskX, maskY + height,
					state->unit[1].width,
					state->unit[1].height, &mX1, &mY1);
		NV30EXATransformCoord(state->unit[1].transform,
					maskX + 2*width, maskY + height,
					state->unit[1].width,
					state->unit[1].height, &mX2, &mY2);

		CV_OUTm(sX0, sY0, mX0, mY0, dstX,             dstY - height);
		CV_OUTm(sX1, sY1, mX1, mY1, dstX,             dstY + height);
		CV_OUTm(sX2, sY2, mX2, mY2, dstX + 2 * width, dstY + height);
	} else {
		CV_OUT(sX0, sY0, dstX            , dstY - height);
		CV_OUT(sX1, sY1, dstX            , dstY + height);
		CV_OUT(sX2, sY2, dstX + 2 * width, dstY + height);
	}

	BEGIN_NV04(push, NV30_3D(VERTEX_BEGIN_END), 1);
	PUSH_DATA (push, 0);
}

void
NV30EXADoneComposite(PixmapPtr pdPix)
{
	ScrnInfoPtr pScrn = xf86Screens[pdPix->drawable.pScreen->myNum];
	nouveau_pushbuf_bufctx(NVPTR(pScrn)->pushbuf, NULL);
}

Bool
NVAccelInitNV30TCL(ScrnInfoPtr pScrn)
{
	NVPtr pNv = NVPTR(pScrn);
	struct nouveau_pushbuf *push = pNv->pushbuf;
	struct nv04_fifo *fifo = pNv->channel->data;
	uint32_t class = 0, chipset;
	int next_hw_offset = 0, i;

	if (!nv40_fp_map_a8[0])
		NV30EXAHackupA8Shaders(pScrn);

#define NV30TCL_CHIPSET_3X_MASK 0x00000003
#define NV35TCL_CHIPSET_3X_MASK 0x000001e0
#define NV30_3D_CHIPSET_3X_MASK 0x00000010

	chipset = pNv->dev->chipset;
	if ((chipset & 0xf0) != NV_ARCH_30)
		return TRUE;
	chipset &= 0xf;

	if (NV30TCL_CHIPSET_3X_MASK & (1<<chipset))
		class = NV30_3D_CLASS;
	else if (NV35TCL_CHIPSET_3X_MASK & (1<<chipset))
		class = NV35_3D_CLASS;
	else if (NV30_3D_CHIPSET_3X_MASK & (1<<chipset))
		class = NV34_3D_CLASS;
	else {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "NV30EXA: Unknown chipset NV3%1x\n", chipset);
		return FALSE;
	}

	if (nouveau_object_new(pNv->channel, Nv3D, class, NULL, 0, &pNv->Nv3D))
		return FALSE;

	if (nouveau_bo_new(pNv->dev, NOUVEAU_BO_VRAM | NOUVEAU_BO_MAP,
			   0, 0x1000, NULL, &pNv->shader_mem)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Couldn't alloc fragprog buffer!\n");
		nouveau_object_del(&pNv->Nv3D);
		return FALSE;
	}

	if (!PUSH_SPACE(push, 256))
		return FALSE;

	BEGIN_NV04(push, NV01_SUBC(3D, OBJECT), 1);
	PUSH_DATA (push, pNv->Nv3D->handle);
	BEGIN_NV04(push, NV30_3D(DMA_TEXTURE0), 3);
	PUSH_DATA (push, fifo->vram);
	PUSH_DATA (push, fifo->gart);
	PUSH_DATA (push, fifo->vram);
	BEGIN_NV04(push, NV30_3D(DMA_UNK1AC), 1);
	PUSH_DATA (push, fifo->vram);
	BEGIN_NV04(push, NV30_3D(DMA_COLOR0), 2);
	PUSH_DATA (push, fifo->vram);
	PUSH_DATA (push, fifo->vram);
	BEGIN_NV04(push, NV30_3D(DMA_UNK1B0), 1);
	PUSH_DATA (push, fifo->vram);

	for (i=1; i<8; i++) {
		BEGIN_NV04(push, NV30_3D(VIEWPORT_CLIP_HORIZ(i)), 2);
		PUSH_DATA (push, 0);
		PUSH_DATA (push, 0);
	}

	BEGIN_NV04(push, SUBC_3D(0x220), 1);
	PUSH_DATA (push, 1);

	BEGIN_NV04(push, SUBC_3D(0x03b0), 1);
	PUSH_DATA (push, 0x00100000);
	BEGIN_NV04(push, SUBC_3D(0x1454), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, SUBC_3D(0x1d80), 1);
	PUSH_DATA (push, 3);
	BEGIN_NV04(push, SUBC_3D(0x1450), 1);
	PUSH_DATA (push, 0x00030004);

	/* NEW */
	BEGIN_NV04(push, SUBC_3D(0x1e98), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, SUBC_3D(0x17e0), 3);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 0x3f800000);
	BEGIN_NV04(push, SUBC_3D(0x1f80), 16);
	PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); 
	PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); 
	PUSH_DATA (push, 0x0000ffff);
	PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); 
	PUSH_DATA (push, 0); PUSH_DATA (push, 0); PUSH_DATA (push, 0); 

	BEGIN_NV04(push, SUBC_3D(0x120), 3);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 1);
	PUSH_DATA (push, 2);

	BEGIN_NV04(push, SUBC_BLIT(0x120), 3);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 1);
	PUSH_DATA (push, 2);

	BEGIN_NV04(push, SUBC_3D(0x1d88), 1);
	PUSH_DATA (push, 0x00001200);

	BEGIN_NV04(push, NV30_3D(RC_ENABLE), 1);
	PUSH_DATA (push, 0);

	/* Attempt to setup a known state.. Probably missing a heap of
	 * stuff here..
	 */
	BEGIN_NV04(push, NV30_3D(STENCIL_ENABLE(0)), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(STENCIL_ENABLE(1)), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(ALPHA_FUNC_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(DEPTH_WRITE_ENABLE), 2);
	PUSH_DATA (push, 0); /* wr disable */
	PUSH_DATA (push, 0); /* test disable */
	BEGIN_NV04(push, NV30_3D(COLOR_MASK), 1);
	PUSH_DATA (push, 0x01010101); /* TR,TR,TR,TR */
	BEGIN_NV04(push, NV30_3D(CULL_FACE_ENABLE), 1);
	PUSH_DATA (push, 0);
	BEGIN_NV04(push, NV30_3D(BLEND_FUNC_ENABLE), 5);
	PUSH_DATA (push, 0);				/* Blend enable */
	PUSH_DATA (push, 0);				/* Blend src */
	PUSH_DATA (push, 0);				/* Blend dst */
	PUSH_DATA (push, 0x00000000);			/* Blend colour */
	PUSH_DATA (push, 0x8006);			/* FUNC_ADD */
	BEGIN_NV04(push, NV30_3D(COLOR_LOGIC_OP_ENABLE), 2);
	PUSH_DATA (push, 0);
	PUSH_DATA (push, 0x1503 /*GL_COPY*/);
	BEGIN_NV04(push, NV30_3D(DITHER_ENABLE), 1);
	PUSH_DATA (push, 1);
	BEGIN_NV04(push, NV30_3D(SHADE_MODEL), 1);
	PUSH_DATA (push, 0x1d01 /*GL_SMOOTH*/);
	BEGIN_NV04(push, NV30_3D(POLYGON_OFFSET_FACTOR),2);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	BEGIN_NV04(push, NV30_3D(POLYGON_MODE_FRONT), 2);
	PUSH_DATA (push, 0x1b02 /*GL_FILL*/);
	PUSH_DATA (push, 0x1b02 /*GL_FILL*/);
	/* - Disable texture units
	 * - Set fragprog to MOVR result.color, fragment.color */
	for (i=0;i<4;i++) {
		BEGIN_NV04(push, NV30_3D(TEX_ENABLE(i)), 1);
		PUSH_DATA (push, 0);
	}
	/* Polygon stipple */
	BEGIN_NV04(push, NV30_3D(POLYGON_STIPPLE_PATTERN(0)), 0x20);
	for (i=0;i<0x20;i++)
		PUSH_DATA (push, 0xFFFFFFFF);

	BEGIN_NV04(push, NV30_3D(DEPTH_RANGE_NEAR), 2);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);

	/* Ok.  If you start X with the nvidia driver, kill it, and then
	 * start X with nouveau you will get black rendering instead of
	 * what you'd expect.  This fixes the problem, and it seems that
	 * it's not needed between nouveau restarts - which suggests that
	 * the 3D context (wherever it's stored?) survives somehow.
	 */
	//BEGIN_NV04(push, SUBC_3D(0x1d60),1);
	//PUSH_DATA (push, 0x03008000);

	int w=4096;
	int h=4096;
	int pitch=4096*4;
	BEGIN_NV04(push, NV30_3D(RT_HORIZ), 5);
	PUSH_DATA (push, w<<16);
	PUSH_DATA (push, h<<16);
	PUSH_DATA (push, 0x148); /* format */
	PUSH_DATA (push, pitch << 16 | pitch);
	PUSH_DATA (push, 0x0);
	BEGIN_NV04(push, NV30_3D(VIEWPORT_TX_ORIGIN), 1);
	PUSH_DATA (push, 0);
        BEGIN_NV04(push, SUBC_3D(0x0a00), 2);
        PUSH_DATA (push, (w<<16) | 0);
        PUSH_DATA (push, (h<<16) | 0);
	BEGIN_NV04(push, NV30_3D(VIEWPORT_CLIP_HORIZ(0)), 2);
	PUSH_DATA (push, (w-1)<<16);
	PUSH_DATA (push, (h-1)<<16);
	BEGIN_NV04(push, NV30_3D(SCISSOR_HORIZ), 2);
	PUSH_DATA (push, w<<16);
	PUSH_DATA (push, h<<16);
	BEGIN_NV04(push, NV30_3D(VIEWPORT_HORIZ), 2);
	PUSH_DATA (push, w<<16);
	PUSH_DATA (push, h<<16);

	BEGIN_NV04(push, NV30_3D(VIEWPORT_TRANSLATE_X), 8);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);

	BEGIN_NV04(push, NV30_3D(MODELVIEW_MATRIX(0)), 16);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);

	BEGIN_NV04(push, NV30_3D(PROJECTION_MATRIX(0)), 16);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 0.0);
	PUSH_DATAf(push, 1.0);

	BEGIN_NV04(push, NV30_3D(SCISSOR_HORIZ), 2);
	PUSH_DATA (push, 4096<<16);
	PUSH_DATA (push, 4096<<16);

	for (i = 0; i < NV30EXA_FPID_MAX; i++) {
		NV30_UploadFragProg(pNv, nv40_fp_map[i], &next_hw_offset);
		NV30_UploadFragProg(pNv, nv40_fp_map_a8[i], &next_hw_offset);
	}
	NV30_UploadFragProg(pNv, &nv30_fp_yv12_bicubic, &next_hw_offset);
	NV30_UploadFragProg(pNv, &nv30_fp_yv12_bilinear, &next_hw_offset);

	return TRUE;
}
