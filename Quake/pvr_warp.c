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
PVR_DrawWaterChains -- submit a model's liquid (SURF_DRAWTURB) chains, warped

Two passes selected by `translucent`:
  false: opaque liquid (alpha >= 1) in the OP list, fullbright.
  true:  translucent liquid (alpha < 1) in the TR list -- MODULATEALPHA env +
         SRC_ALPHA/INV_SRC_ALPHA blend, per-surface alpha (GL_WaterAlphaForSurface)
         in the vertex color so you can see through it.
Per-surface alpha routes each face to its matching pass. For entities XMTRX
already holds the object transform.
==============
*/
static void PVR_DrawWaterChains (qmodel_t *model, texchain_t chain, qboolean translucent)
{
	int		i, j, n;
	texture_t	*t;
	msurface_t	*s;
	glpoly_t	*p;

	if (translucent)
	{
		PVR_ListBegin (PVR_LIST_TR_POLY);
		PVR_SetBlend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		PVR_SetTexEnv (PVR_TEXENV_MODULATEALPHA);	// vertex alpha controls see-through
	}
	else
	{
		PVR_ListBegin (PVR_LIST_OP_POLY);
		PVR_SetBlend (GL_ONE, GL_ZERO);
		PVR_SetTexEnv (GL_MODULATE);			// fullbright
	}

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain])
			continue;
		if (!(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;

		GL_Bind (t->gltexture);
		PVR_FlushState ();

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			float		a;
			uint32_t	argb;

			if (!(s->flags & SURF_DRAWTURB) || !s->polys)
				continue;

			a = GL_WaterAlphaForSurface (s);
			if ((a < 1.0f) != translucent)
				continue;			// belongs to the other pass

			{
				uint32_t alpha8 = (uint32_t)(a * 255.0f);
				if (alpha8 > 255) alpha8 = 255;
				argb = (alpha8 << 24) | 0x00ffffffu;
			}

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
					col[j] = argb;
				}
				PVR_EmitPoly (p, uu, vv, col);
			}
		}
	}
}

// Called from r_world.c R_DrawWorld_Water under USE_PVR_RENDER: opaque liquid in
// the OP list, then translucent liquid in the TR list (must follow all OP work).
void PVR_DrawWorld_Water (qmodel_t *model)
{
	PVR_DrawWaterChains (model, chain_world, false);
	PVR_DrawWaterChains (model, chain_world, true);
}

// Brush-model entity liquid (opaque only for now; XMTRX holds the entity transform).
void PVR_DrawBrushModel_Water (qmodel_t *model)
{
	PVR_DrawWaterChains (model, chain_model, false);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
