/*
================================================================================
pvr_rmath.c -- matrices & vertex transform for the PVR renderer (maximqad)

QuakeSpasm builds a GL projection (GL_SetFrustum) and a modelview (R_SetupGL,
per-entity R_RotateForEntity). We keep both, combine to an MVP, and keep it in
the SH4 hardware matrix bank (xmtrx) via sh4zam so the per-vertex transform is a
single ftrv. Screen mapping (viewport scale/bias, Y-flip, and z -> 1/w for the
PVR's depth) is folded in here.

Fill status: STEP 1 skeleton. Matrix plumbing + a straightforward transform;
the screen/viewport constants and the exact z->1/w mapping are TODO and must be
matched to pvr_backend's framebuffer + QuakeSpasm's depth range on hardware.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

static shz_mat4x4_t	pvr_proj;	// GL projection
static shz_mat4x4_t	pvr_mview;	// modelview
static shz_mat4x4_t	pvr_mvp;	// proj * modelview (loaded into xmtrx)

static float	vp_ox, vp_oy;		// viewport origin (px)
static float	vp_hw, vp_hh;		// half width/height (px) for NDC->screen

void PVR_SetViewport (int x, int y, int w, int h)
{
	vp_ox = (float) x;
	vp_oy = (float) y;
	vp_hw = (float) w * 0.5f;
	vp_hh = (float) h * 0.5f;
}

void PVR_LoadProjection (const float m[16])
{
	// GL stores column-major; shz_mat4x4_t matches that layout (m[col][row]).
	shz_mat4x4_copy_unaligned (&pvr_proj, m);
}

void PVR_LoadModelview (const float m[16])
{
	shz_mat4x4_copy_unaligned (&pvr_mview, m);
}

void PVR_UpdateMVP (void)
{
	// mvp = proj * modelview
	shz_mat4x4_copy (&pvr_mvp, &pvr_proj);
	shz_mat4x4_apply (&pvr_mvp, &pvr_mview);
	// Load into the SH4 XMTRX bank for fast per-vertex ftrv.
	shz_xmtrx_load_4x4 (&pvr_mvp);
}

/*
==============
PVR_TransformVertex

pos (world/eye space) -> clip (via xmtrx) -> perspective divide -> screen px,
with z carrying 1/w for the PVR depth compare.

TODO(hw): confirm sign of Y flip and the exact depth term the PVR wants; ref_pvr
submits z = 1/w and lets the TA sort. Guard-band / near-plane handling is done by
pvr_clip before this for polys that cross the near plane.
==============
*/
void PVR_TransformVertex (pvr_vertex_t *out, const float pos[3])
{
	shz_vec4_t in = shz_vec4_init (pos[0], pos[1], pos[2], 1.0f);
	shz_vec4_t clip = shz_xmtrx_transform_vec4 (in);	// ftrv against loaded MVP
	float invw = 1.0f / clip.w;

	out->x = vp_ox + vp_hw * (1.0f + clip.x * invw);
	out->y = vp_oy + vp_hh * (1.0f - clip.y * invw);	// Y flip for screen space
	out->z = invw;						// PVR depth = 1/w
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
