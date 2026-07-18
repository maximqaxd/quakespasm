/*
================================================================================
pvr_alloc.c -- VRAM texture allocator for the PVR renderer (maximqad)

The PVR samples textures from VRAM (pvr_mem). This owns a simple allocator over
pvr_mem_malloc/free so gltexture_t can hold a pvr_ptr_t. 8MB VRAM is the budget;
paletted 8bpp (revisit) would halve it. Ported concept from ref_pvr pvr_alloc.

Fill status: STUB. Start with plain pvr_mem_malloc/free; add packing/eviction
only if VRAM fragmentation bites.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

void PVR_TexAlloc_Init (void)
{
	// TODO: nothing needed for plain pvr_mem_malloc; reserve/track here if we
	// add a packing allocator later.
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
