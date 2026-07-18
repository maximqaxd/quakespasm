/*
================================================================================
pvr_clip.c -- near-plane polygon clipping for the PVR renderer (maximqad)

The PVR has no near-plane clipper of its own: a vertex behind the eye divides by
a non-positive w and smears across the screen. Convex polys that straddle the
near plane must be clipped in clip space (before the perspective divide) and the
resulting fan re-emitted. Off-screen X/Y are handled by the PVR guard band, so
only near-Z needs software clipping.

Fill status: STUB. Sutherland-Hodgman against the near plane; emit through
PVR_TransformVertex. Ported from ref_pvr pvr_clip.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

int PVR_ClipPolygon (const float *verts, int numverts, int stride)
{
	(void)verts; (void)numverts; (void)stride;
	// TODO: transform to clip space, clip against near (w > epsilon), emit fan.
	return 0;
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
