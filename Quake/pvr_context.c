/*
================================================================================
pvr_context.c -- poly-context cache for the PVR renderer (maximqad)

The PVR wants a compiled poly header (pvr_poly_compile) whenever render state
changes -- list, blend mode, texture, filter, shading, tex-env. Recompiling and
re-submitting a header per surface would stall the DR stream, so we cache the
current desired state, mark it dirty on change, and only compile+submit a header
lazily right before the next vertex batch (PVR_FlushState).

Fill status: STUB. Holds the GL-style state QuakeSpasm sets (glBlendFunc,
glTexEnv, bound texture) and maps it to a pvr_poly_cxt_t. Ported from ref_pvr
pvr_context.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

void PVR_SetBlend (int src, int dst)
{
	(void)src; (void)dst;
	// TODO: map GL blend factors -> PVR_BLEND_*, mark state dirty
}

void PVR_SetTexEnv (int env)
{
	(void)env;
	// TODO: GL_REPLACE/GL_MODULATE -> PVR_TXRENV_*, mark dirty
}

void PVR_FlushState (void)
{
	// TODO: if dirty: pvr_poly_cxt_txr/col + pvr_poly_compile, submit header,
	// clear dirty. Called by the render modules before each vertex batch.
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
