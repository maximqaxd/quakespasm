/*
================================================================================
pvr_alloc.c -- VRAM texture allocator for the PVR renderer

The PVR samples textures from VRAM (pvr_mem). This is a thin allocator over
pvr_mem_malloc/free so gltexture_t can hold a pvr_ptr_t. The budget is 8MB of
VRAM. A packing/eviction allocator can be added here if fragmentation ever
becomes a problem.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

void PVR_TexAlloc_Init (void)
{
	// Nothing to reserve for plain pvr_mem_malloc. If VRAM fragmentation ever
	// becomes an issue (many small brush/model textures churned on map change),
	// a packing/defrag allocator would live here.
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
