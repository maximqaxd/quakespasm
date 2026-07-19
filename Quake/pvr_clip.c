/*
================================================================================
pvr_clip.c -- near-plane clipping for PVR direct rendering

The PVR has no near-plane clipper; vertices behind the eye (w <= 0) blow up under
the 1/w perspective divide and hang the TA (hard reboot). With
shz_xmtrx_apply_perspective the projection gives clip.w = -z_eye
(distance) and clip.z = near, so a vertex is in front of the near plane when
w >= z. We test each triangle's 3 verts, fast-path fully-visible ones, drop fully-
behind ones, and clip the straddling ones in clip space (interpolating position,
uv and color) before the divide -- then submit as 1 or 2 triangles.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include <math.h>

typedef struct
{
	shz_vec4_t	pos;	// clip-space (x,y,z,w)
	float		u, v;
	uint32_t	argb;
} clipvert_t;

// bit0/1/2 set = v0/v1/v2 in front of the near plane
static inline unsigned VisMaskTri (const clipvert_t *v)
{
	unsigned mask = 0;
	if (v[0].pos.w >= v[0].pos.z + PVR_NEAR_CLIP_EPSILON) mask |= 1;
	if (v[1].pos.w >= v[1].pos.z + PVR_NEAR_CLIP_EPSILON) mask |= 2;
	if (v[2].pos.w >= v[2].pos.z + PVR_NEAR_CLIP_EPSILON) mask |= 4;
	return mask;
}

static inline uint32_t LerpARGB (uint32_t c1, uint32_t c2, int ti)
{
	uint32_t rb = ((((c2 & 0x00FF00FF) - (c1 & 0x00FF00FF)) * ti) >> 8) + (c1 & 0x00FF00FF);
	uint32_t g  = ((((c2 & 0x0000FF00) - (c1 & 0x0000FF00)) * ti) >> 8) + (c1 & 0x0000FF00);
	uint32_t a  = ((((c2 >> 24) - (c1 >> 24)) * ti) >> 8) + (c1 >> 24);
	return (a << 24) | (rb & 0x00FF00FF) | (g & 0x0000FF00);
}

static inline float lerpf (float a, float b, float t) { return a + (b - a) * t; }

// clip edge v1->v2 against the plane (w - z - eps = 0); result stays in front
static inline void ClipEdge (const clipvert_t *v1, const clipvert_t *v2, clipvert_t *out)
{
	const float d0 = v1->pos.w - v1->pos.z - PVR_NEAR_CLIP_EPSILON;
	const float d1 = v2->pos.w - v2->pos.z - PVR_NEAR_CLIP_EPSILON;
	const float denom = d0 - d1;
	float t;
	int ti;

	// precise division here: near-plane clipping hits negative denominators where
	// the FSRRA reciprocal is too rough and warps geometry.
	t = (fabsf (denom) < 1e-8f) ? 0.0f : (d0 / denom);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;

	out->pos.x = lerpf (v1->pos.x, v2->pos.x, t);
	out->pos.y = lerpf (v1->pos.y, v2->pos.y, t);
	out->pos.z = lerpf (v1->pos.z, v2->pos.z, t);
	out->pos.w = lerpf (v1->pos.w, v2->pos.w, t);
	out->u = lerpf (v1->u, v2->u, t);
	out->v = lerpf (v1->v, v2->v, t);

	ti = (int)(t * 255.0f);
	if (ti < 0) ti = 0;
	if (ti > 255) ti = 255;
	out->argb = LerpARGB (v1->argb, v2->argb, ti);
}

static inline void SubmitVert (const clipvert_t *cv, uint32_t flags)
{
	const float	invw = 1.0f / cv->pos.w;
	pvr_vertex_t	*vert = (pvr_vertex_t *) pvr_dr_target (NULL);

	vert->flags = flags;
	vert->x = cv->pos.x * invw;
	vert->y = cv->pos.y * invw;
	vert->z = invw + pvr_depth_bias;	// view weapon lifts itself above world depth
	vert->u = cv->u;
	vert->v = cv->v;
	vert->argb = cv->argb;
	vert->oargb = 0;
	pvr_dr_commit (vert);
}

void PVR_ClipAndSubmitTriangle (shz_vec4_t p0, shz_vec4_t p1, shz_vec4_t p2,
				float u0, float v0, float u1, float v1, float u2, float v2,
				uint32_t c0, uint32_t c1, uint32_t c2)
{
	clipvert_t	verts[4];
	unsigned	n_verts = 3;
	unsigned	vismask;

	verts[0].pos = p0; verts[0].u = u0; verts[0].v = v0; verts[0].argb = c0;
	verts[1].pos = p1; verts[1].u = u1; verts[1].v = v1; verts[1].argb = c1;
	verts[2].pos = p2; verts[2].u = u2; verts[2].v = v2; verts[2].argb = c2;

	vismask = VisMaskTri (verts);

	if (vismask == 0)
		return;				// entirely behind the near plane

	if (vismask == 7)			// fully visible
	{
		SubmitVert (&verts[0], PVR_CMD_VERTEX);
		SubmitVert (&verts[1], PVR_CMD_VERTEX);
		SubmitVert (&verts[2], PVR_CMD_VERTEX_EOL);
		return;
	}

	// straddling: clip the outside edges, forming 1 tri (3 verts) or a quad (4)
	switch (vismask)
	{
	case 1:	// v0 only
		ClipEdge (&verts[0], &verts[1], &verts[1]);
		ClipEdge (&verts[0], &verts[2], &verts[2]);
		break;
	case 2:	// v1 only
		ClipEdge (&verts[1], &verts[0], &verts[0]);
		ClipEdge (&verts[1], &verts[2], &verts[2]);
		break;
	case 3:	// v0 + v1
		n_verts = 4;
		ClipEdge (&verts[1], &verts[2], &verts[3]);
		ClipEdge (&verts[0], &verts[2], &verts[2]);
		break;
	case 4:	// v2 only
		ClipEdge (&verts[2], &verts[0], &verts[0]);
		ClipEdge (&verts[2], &verts[1], &verts[1]);
		break;
	case 5:	// v0 + v2
		n_verts = 4;
		ClipEdge (&verts[1], &verts[2], &verts[3]);
		ClipEdge (&verts[0], &verts[1], &verts[1]);
		break;
	case 6:	// v1 + v2
		n_verts = 4;
		verts[3] = verts[2];
		ClipEdge (&verts[0], &verts[2], &verts[2]);
		ClipEdge (&verts[0], &verts[1], &verts[0]);
		break;
	}

	if (n_verts == 3)
	{
		SubmitVert (&verts[0], PVR_CMD_VERTEX);
		SubmitVert (&verts[1], PVR_CMD_VERTEX);
		SubmitVert (&verts[2], PVR_CMD_VERTEX_EOL);
	}
	else
	{
		SubmitVert (&verts[0], PVR_CMD_VERTEX);
		SubmitVert (&verts[1], PVR_CMD_VERTEX);
		SubmitVert (&verts[2], PVR_CMD_VERTEX);
		SubmitVert (&verts[3], PVR_CMD_VERTEX_EOL);
	}
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
