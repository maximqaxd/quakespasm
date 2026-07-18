/*
================================================================================
pvr_context.c -- poly-context cache for the PVR renderer (maximqad)

The PVR wants a compiled poly header (pvr_poly_compile) whenever render state
changes -- list, blend mode, texture, filter, tex-env. Recompiling/re-submitting
a header per surface would stall the DR stream, so the render modules set the
desired GL-style state (blend, tex-env, bound texture via GL_Bind) and this module
tracks it, marks dirty on change, and compiles + submits a header lazily right
before the next vertex batch via PVR_FlushState.

The bound texture comes from pvr_bound_texture (set by GL_Bind -> PVR_BindTexture
in pvr_texmgr): its pvr_vram + pvr_fmt + flags feed the txr side of the context.

Fill status: STEP 3. Real GL->PVR state mapping + header compile/submit. Depth,
culling and list-dependent alpha use sane defaults the render modules can refine.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

//------------------------------------------------------------------------------
// Desired state (GL-style), plus a dirty flag
//------------------------------------------------------------------------------
static int	st_blendsrc = GL_ONE;
static int	st_blenddst = GL_ZERO;
static int	st_texenv   = GL_MODULATE;
static struct gltexture_s *st_lasttex;		// last texture we compiled a header for
static int	st_lastlist = -1;		// list the cached header targets
static qboolean	st_dirty = true;

static pvr_poly_hdr_t	st_hdr;			// last compiled header

//------------------------------------------------------------------------------
// GL blend factor -> PVR blend mode
//------------------------------------------------------------------------------
static pvr_blend_mode_t PVR_MapBlend (int glfactor)
{
	switch (glfactor)
	{
	case GL_ZERO:			return PVR_BLEND_ZERO;
	case GL_ONE:			return PVR_BLEND_ONE;
	case GL_DST_COLOR:		return PVR_BLEND_DESTCOLOR;
	case GL_ONE_MINUS_DST_COLOR:	return PVR_BLEND_INVDESTCOLOR;
	case GL_SRC_ALPHA:		return PVR_BLEND_SRCALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:	return PVR_BLEND_INVSRCALPHA;
	case GL_DST_ALPHA:		return PVR_BLEND_DESTALPHA;
	case GL_ONE_MINUS_DST_ALPHA:	return PVR_BLEND_INVDESTALPHA;
	// GL_SRC_COLOR as a dest factor (modulate: dst *= src) has no direct PVR dest
	// factor, but is equivalent to src*DESTCOLOR + dst*ZERO -- handled specially in
	// PVR_FlushState. Fall back to ONE here.
	default:			return PVR_BLEND_ONE;
	}
}

void PVR_SetBlend (int src, int dst)
{
	if (src != st_blendsrc || dst != st_blenddst)
	{
		st_blendsrc = src;
		st_blenddst = dst;
		st_dirty = true;
	}
}

void PVR_SetTexEnv (int env)
{
	if (env != st_texenv)
	{
		st_texenv = env;
		st_dirty = true;
	}
}

//------------------------------------------------------------------------------
// Compile + submit the header for the current state into the open list
//------------------------------------------------------------------------------
void PVR_FlushState (void)
{
	pvr_poly_cxt_t	cxt;
	struct gltexture_s *tex = pvr_bound_texture;
	int		list = pvr_frame_list;
	pvr_blend_mode_t bsrc, bdst;

	if (list < 0)
		return;		// no list open; nothing to submit into

	// Re-compile when any tracked input changed since the last header.
	if (!st_dirty && tex == st_lasttex && list == st_lastlist)
		return;

	// Modulate special case: glBlendFunc(GL_ZERO, GL_SRC_COLOR) == dst *= src.
	// PVR expresses that as src*DESTCOLOR + dst*ZERO.
	if (st_blendsrc == GL_ZERO && st_blenddst == GL_SRC_COLOR)
	{
		bsrc = PVR_BLEND_DESTCOLOR;
		bdst = PVR_BLEND_ZERO;
	}
	else
	{
		bsrc = PVR_MapBlend (st_blendsrc);
		bdst = PVR_MapBlend (st_blenddst);
	}

	if (tex && tex->pvr_vram)
	{
		pvr_filter_mode_t filt = (tex->flags & TEXPREF_NEAREST)
			? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR;

		pvr_poly_cxt_txr (&cxt, list, (int)tex->pvr_fmt,
				  (int)tex->width, (int)tex->height,
				  (pvr_ptr_t)tex->pvr_vram, filt);

		// REPLACE shows the texel straight (2D pics at full brightness); MODULATE
		// multiplies by vertex color for lit world/model geometry.
		cxt.txr.env = (st_texenv == GL_REPLACE)
			? PVR_TXRENV_REPLACE : PVR_TXRENV_MODULATE;
		// In the opaque list texel/palette alpha is irrelevant; disable it so index
		// 255 shows its color. Punch-through / translucent keep alpha for cutouts.
		cxt.txr.alpha = (list == PVR_LIST_OP_POLY);
	}
	else
	{
		pvr_poly_cxt_col (&cxt, list);
	}

	// Blending: the OP list never blends (hardware ignores it); PT/TR use ours.
	if (list != PVR_LIST_OP_POLY)
	{
		cxt.blend.src = bsrc;
		cxt.blend.dst = bdst;
	}

	// Depth: PVR depth is 1/w (nearer == larger), so compare GEQUAL. Write depth in
	// the opaque/punch-through lists; translucent overlays test but don't write.
	cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
	cxt.depth.write = (list != PVR_LIST_TR_POLY);

	// Winding is not yet normalized between Quake and the PVR; leave culling off
	// until the surface/model submit paths settle their vertex order.
	cxt.gen.culling = PVR_CULLING_NONE;

	pvr_poly_compile (&st_hdr, &cxt);
	pvr_prim (&st_hdr, sizeof(st_hdr));

	st_lasttex  = tex;
	st_lastlist = list;
	st_dirty = false;
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
