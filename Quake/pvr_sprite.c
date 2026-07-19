/*
================================================================================
pvr_sprite.c -- world-space billboards (particles + sprite models) for the PVR
renderer

Particles (r_part.c) and sprite-model entities (r_sprite.c) are both camera-
facing textured quads in world space. The billboard geometry is built where the
data lives (particle list / sprite frame), then handed here as four world-space
corners + texcoords + a packed color. This module binds the texture, transforms
the corners once through the XMTRX ftrv (the world MVP), near-plane clips, and
submits two triangles via the shared clipper -- no main-RAM vertex list.

Both go in blended lists (no depth write for particles): particles in TR (they
never occlude), sprites in PT so their alpha-tested edges cut cleanly and sit
with the other cutouts. PVR_BillboardBegin opens the list + state once per batch
and reloads the world matrix (entity draws leave XMTRX holding an object MVP).
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

/*
==============
PVR_BillboardBegin -- open the billboard list + blend/env, reload the world MVP
==============
*/
void PVR_BillboardBegin (int kind)
{
	int	list = (kind == PVR_BILLBOARD_PT) ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY;

	PVR_ListBegin (list);
	PVR_SetBlend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	PVR_SetTexEnv (GL_MODULATE);
	PVR_RestoreWorldMatrix ();	// particles/sprites are world-space
}

/*
==============
PVR_BillboardQuad -- transform + near-clip + submit one world-space quad

Corners v0..v3 wind around the quad; (s,t) are their texcoords; argb is the flat
color for all four. Emitted as triangles (v0,v1,v2) and (v0,v2,v3) through the
near-plane clipper. GL_Bind + PVR_FlushState here recompiles the poly header only
when the texture actually changes, so a run of same-texture particles is one
header.
==============
*/
void PVR_BillboardQuad (struct gltexture_s *tex,
			const float *v0, const float *v1, const float *v2, const float *v3,
			float s0, float t0, float s1, float t1,
			float s2, float t2, float s3, float t3, uint32_t argb)
{
	shz_vec4_t	c0, c1, c2, c3;

	GL_Bind (tex);
	PVR_FlushState ();

	c0 = shz_xmtrx_transform_vec4 (shz_vec4_init (v0[0], v0[1], v0[2], 1.0f));
	c1 = shz_xmtrx_transform_vec4 (shz_vec4_init (v1[0], v1[1], v1[2], 1.0f));
	c2 = shz_xmtrx_transform_vec4 (shz_vec4_init (v2[0], v2[1], v2[2], 1.0f));
	c3 = shz_xmtrx_transform_vec4 (shz_vec4_init (v3[0], v3[1], v3[2], 1.0f));

	PVR_ClipAndSubmitTriangle (c0, c1, c2, s0, t0, s1, t1, s2, t2, argb, argb, argb);
	PVR_ClipAndSubmitTriangle (c0, c2, c3, s0, t0, s2, t2, s3, t3, argb, argb, argb);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
