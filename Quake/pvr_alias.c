/*
================================================================================
pvr_alias.c -- alias (.mdl) model vertex submission for the PVR renderer

The renderer-agnostic half stays in r_alias.c: pose/lerp selection
(R_SetupAliasFrame), entity transform lerp (R_SetupEntityTransform), lighting
(R_SetupAliasLighting -> shadedots + lightcolor), and skin selection. Its PVR
branch (PVR_DrawAliasModel) bakes the entity+decode matrix into XMTRX
(PVR_SetupAliasMatrices) and builds three flat, PVR-free arrays -- lerped
model-space byte positions, per-vertex lit ARGB, and the static normalized
texcoords -- then hands them here.

This module owns the DC-specific submission. Every unique mesh vertex is
transformed ONCE through the XMTRX ftrv (the win over per-triangle transforms),
flagged against the near plane, then the indexed triangle list is walked: fully-
visible tris fast-path straight to the TA store queue, straddling tris go through
the shared near-plane clipper (pvr_clip). One texture header per model (a single
PVR_FlushState), no main-RAM vertex list.

The whole vertex pool is transformed up front and submitted by index, which
keeps texture-header switches to a minimum.
================================================================================
*/
// pvr_local.h pulls <dc/pvr.h> before quakedef.h (HZ macro collision), so this
// TU must lead with it -- which is why the alias submission lives here and not in
// r_alias.c (that includes quakedef.h first).
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

// Scratch for the transformed vertex pool. MAXALIASVERTS (gl_model.h) is the
// engine's hard cap on poseverts; the caller also guards against overflow.
#define PVR_ALIAS_MAXVERTS	MAXALIASVERTS

static shz_vec4_t	pa_clip[PVR_ALIAS_MAXVERTS];	// clip-space position per vertex
static unsigned char	pa_vis[PVR_ALIAS_MAXVERTS];	// 1 = in front of the near plane

// Fire one already-transformed vertex into the DR store queue. The perspective
// divide uses FSRRA (1/w in ~1-2 cycles vs ~10+ for fdiv): PA_EMIT only runs for
// fully-visible tris (all corners passed the near-plane test), so w is safely > 0
// and FSRRA's reduced precision is sub-pixel. Straddling tris take the precise
// divide in pvr_clip.
#define PA_EMIT(slot, cmd)							\
	do {									\
		float iw = shz_invf_fsrra (pa_clip[slot].w);			\
		pvr_vertex_t *vp = (pvr_vertex_t *) pvr_dr_target (NULL);	\
		vp->flags = (cmd);						\
		vp->x = pa_clip[slot].x * iw;					\
		vp->y = pa_clip[slot].y * iw;					\
		vp->z = iw + pvr_depth_bias;					\
		vp->u = st[(slot) * 2 + 0];					\
		vp->v = st[(slot) * 2 + 1];					\
		vp->argb = argb[slot];						\
		vp->oargb = 0;							\
		pvr_dr_commit (vp);						\
	} while (0)

/*
==============
PVR_SubmitAliasFrame

pos: numverts * 3 floats (lerped model-space byte positions, decoded by XMTRX)
st:  numverts * 2 floats (normalized texcoords, aliashdr_t.st_dc)
argb: numverts packed colors (per-vertex light * entalpha)
idx: numtris * 3 unsigned short (triangle list into the vertex pool, idx_dc)
passkind: PVR_ALIAS_OPAQUE (OP), _HOLEY (PT, alpha-tested), _TRANS (TR, blended)

Assumes XMTRX already holds the alias MVP (PVR_SetupAliasMatrices).
==============
*/
void PVR_SubmitAliasFrame (const float *pos, const float *st, const uint32_t *argb,
			   const unsigned short *idx, int numverts, int numtris,
			   struct gltexture_s *tx, int passkind)
{
	int	i, list, sblend, dblend, env;

	if (numverts <= 0 || numverts > PVR_ALIAS_MAXVERTS)
		return;

	switch (passkind)
	{
	case PVR_ALIAS_HOLEY:	// alpha-tested cutout skin (index 255 transparent)
		list = PVR_LIST_PT_POLY; sblend = GL_ONE; dblend = GL_ZERO; env = GL_MODULATE;
		break;
	case PVR_ALIAS_TRANS:	// entity alpha < 1 (spawn shimmer, etc.)
		list = PVR_LIST_TR_POLY; sblend = GL_SRC_ALPHA; dblend = GL_ONE_MINUS_SRC_ALPHA;
		env = PVR_TEXENV_MODULATEALPHA;
		break;
	case PVR_ALIAS_GLOW:	// additive fullbright/luma overlay (glowing eyes, laser)
		list = PVR_LIST_TR_POLY; sblend = GL_ONE; dblend = GL_ONE; env = GL_MODULATE;
		break;
	case PVR_ALIAS_SHADOW:	// flattened blob shadow (untextured black, tx == NULL)
		list = PVR_LIST_TR_POLY; sblend = GL_SRC_ALPHA; dblend = GL_ONE_MINUS_SRC_ALPHA;
		env = GL_MODULATE;
		break;
	default:		// opaque
		list = PVR_LIST_OP_POLY; sblend = GL_ONE; dblend = GL_ZERO; env = GL_MODULATE;
		break;
	}

	// Transform the whole vertex pool once; flag near-plane visibility.
	for (i = 0; i < numverts; i++)
	{
		shz_vec4_t c = shz_xmtrx_transform_vec4 (
			shz_vec4_init (pos[i * 3 + 0], pos[i * 3 + 1], pos[i * 3 + 2], 1.0f));
		pa_clip[i] = c;
		pa_vis[i]  = (c.w >= c.z + PVR_NEAR_CLIP_EPSILON);
	}

	PVR_ListBegin (list);
	PVR_SetBlend (sblend, dblend);
	PVR_SetTexEnv (env);
	GL_Bind (tx);			// records the bound texture for the header
	PVR_FlushState ();		// one header for the whole model

	for (i = 0; i < numtris; i++)
	{
		int i0 = idx[i * 3 + 0];
		int i1 = idx[i * 3 + 1];
		int i2 = idx[i * 3 + 2];

		if (!pa_vis[i0] && !pa_vis[i1] && !pa_vis[i2])
			continue;		// whole triangle behind the near plane

		if (pa_vis[i0] && pa_vis[i1] && pa_vis[i2])
		{
			PA_EMIT (i0, PVR_CMD_VERTEX);
			PA_EMIT (i1, PVR_CMD_VERTEX);
			PA_EMIT (i2, PVR_CMD_VERTEX_EOL);
		}
		else
		{
			PVR_ClipAndSubmitTriangle (pa_clip[i0], pa_clip[i1], pa_clip[i2],
				st[i0 * 2], st[i0 * 2 + 1], st[i1 * 2], st[i1 * 2 + 1],
				st[i2 * 2], st[i2 * 2 + 1], argb[i0], argb[i1], argb[i2]);
		}
	}
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
