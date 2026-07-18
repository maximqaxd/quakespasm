/*
================================================================================
pvr_warp.c -- warped liquid (and later sky) surfaces for the PVR renderer (maximqad)

Water/lava/slime faces are flagged SURF_DRAWTURB and subdivided at load into a
chain of small polys (GL_SubdivideSurface). They're drawn with an animated sine
warp of the texture coordinates (the classic Quake turb effect) -- the geometry is
static, only the u/v scroll. This mirrors gl_warp.c's r_oldwater DrawWaterPoly
path, but emits through pvr_rsurf's shared transform/clip/strip core (PVR_EmitPoly)
instead of glBegin.

Fill status: STEP 6. Opaque fullbright liquid in the OP list. Water alpha
(translucent, TR list) and sky are still TODO.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include <math.h>

// turb warp sine table (256 entries), shared with gl_warp.c
static const float	turbsin[] =
{
	#include "gl_warp_sin.h"
};

// old-water warp (matches gl_warp.c WARPCALC2; load_subdivide_size default > 48):
// scroll the base texcoord by a time-animated sine, then normalize by the 64-wide
// turb texture.
#define WARPCALC2(s,t)	((s + turbsin[(int)((t*0.125f + cl.time) * (128.0f/(float)M_PI)) & 255]) * (1.0f/64.0f))

/*
==============
PVR_DrawWorld_Water -- submit the world's liquid (SURF_DRAWTURB) chains

Called from r_world.c R_DrawWorld_Water under USE_PVR_RENDER. Opaque + fullbright
for now, in the OP list right after the solid world.
==============
*/
void PVR_DrawWorld_Water (qmodel_t *model)
{
	int		i, j, n;
	texture_t	*t;
	msurface_t	*s;
	glpoly_t	*p;

	PVR_ListBegin (PVR_LIST_OP_POLY);
	PVR_SetBlend (GL_ONE, GL_ZERO);		// opaque
	PVR_SetTexEnv (GL_MODULATE);		// fullbright (white vertex color)

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain_world])
			continue;
		if (!(t->texturechains[chain_world]->flags & SURF_DRAWTURB))
			continue;

		GL_Bind (t->gltexture);
		PVR_FlushState ();

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
		{
			if (!(s->flags & SURF_DRAWTURB) || !s->polys)
				continue;

			// s->polys is the whole face; the subdivided pieces are the ->next chain
			for (p = s->polys->next; p; p = p->next)
			{
				float		uu[PVR_MAX_POLY_VERTS], vv[PVR_MAX_POLY_VERTS];
				uint32_t	col[PVR_MAX_POLY_VERTS];
				float		*base = p->verts[0];

				n = p->numverts;
				if (n > PVR_MAX_POLY_VERTS)
					n = PVR_MAX_POLY_VERTS;
				for (j = 0; j < n; j++)
				{
					float *v = base + j * VERTEXSIZE;
					uu[j]  = WARPCALC2 (v[3], v[4]);
					vv[j]  = WARPCALC2 (v[4], v[3]);
					col[j] = 0xffffffffu;	// fullbright liquid
				}
				PVR_EmitPoly (p, uu, vv, col);
			}
		}
	}
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
