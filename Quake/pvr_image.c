/*
================================================================================
pvr_image.c -- texture upload to VRAM for the PVR renderer (maximqad)

Converts QuakeSpasm's 32-bit (or indexed) source into a PVR-native texture:
twiddled RGB565 for opaque, ARGB1555/4444 for alpha, plus mip chain. Replaces
the glTexImage2D path in gl_texmgr.c. On DC, gltexture_t gains a pvr_ptr_t +
format (the GL name is meaningless here).

Fill status: STUB. Reuse the twiddle + 565 pack we already understand from the
lightmap path; wire PVR_BindTexture into the context cache.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

void PVR_UploadTexture (struct gltexture_s *glt, const void *rgba, int w, int h, unsigned flags)
{
	(void)glt; (void)rgba; (void)w; (void)h; (void)flags;
	// TODO: pvr_mem_malloc, twiddle+pack to RGB565/ARGB, pvr_txr_load, store
	// pvr_ptr_t + format on glt.
}

void PVR_FreeTexture (struct gltexture_s *glt)
{
	(void)glt;
	// TODO: pvr_mem_free(glt->pvr_ptr)
}

void PVR_BindTexture (struct gltexture_s *glt)
{
	(void)glt;
	// TODO: tell the context cache which VRAM texture/format the next batch uses.
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
