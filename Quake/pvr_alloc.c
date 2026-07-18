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
	// Nothing to reserve for plain pvr_mem_malloc. When VRAM fragmentation bites
	// (many small brush/model textures churned on map change) this is where a
	// packing/defrag allocator would live -- see ref_pvr pvr_alloc.c.
}

/*
==============
PVR_VramAlloc / PVR_VramFree

Thin wrappers over KOS pvr_mem so the rest of the renderer never touches pvr_mem_*
directly -- swapping in a smarter allocator later stays a one-file change.
==============
*/
void *PVR_VramAlloc (unsigned bytes)
{
	if (!bytes)
		return NULL;
	return pvr_mem_malloc (bytes);
}

void PVR_VramFree (void *ptr)
{
	if (ptr)
		pvr_mem_free (ptr);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
