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

// Split entry points so the scene can place each liquid pass in its own list
// phase: opaque liquid belongs with the OP work, translucent with the TR work.
// (The combined PVR_DrawWorld_Water is kept for callers that don't phase.)
void PVR_DrawWorld_WaterOpaque (qmodel_t *model)
{
	PVR_DrawWaterChains (model, chain_world, false);
}

void PVR_DrawWorld_WaterTrans (qmodel_t *model)
{
	PVR_DrawWaterChains (model, chain_world, true);
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

//==============================================================================
// Sky
//
// Two paths, matching gl_sky.c: r_fastsky draws the sky surfaces as a flat
// skyflatcolor fill; otherwise the classic two scrolling cloud layers -- an
// opaque solidsky layer (OP) and a transparent alphasky layer blended over it
// (TR) -- with texcoords projected per vertex from the view origin (the sphere-
// flattened Sky_GetTexCoord math). Drawn directly on the real sky surfaces (not
// a skybox), so they write depth at their world position and are occluded
// normally; the alpha layer sits at the same depth via GEQUAL.
//==============================================================================

// Per-vertex sky texcoord (gl_sky.c Sky_GetTexCoord): project the vertex
// direction from the eye, flatten the sphere, and scroll by cl.time * speed.
static void PVR_SkyTexCoord (const float *v, float speed, float *s, float *t)
{
	vec3_t	dir;
	float	length, scroll;

	VectorSubtract (v, r_origin, dir);
	dir[2] *= 3.0f;					// flatten the sphere

	length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
	length = sqrtf (length);
	length = (6.0f * 63.0f) / length;

	scroll  = (float)cl.time * speed;
	scroll -= (int)scroll & ~127;

	*s = (scroll + dir[0] * length) * (1.0f / 128.0f);
	*t = (scroll + dir[1] * length) * (1.0f / 128.0f);
}

// Emit every SURF_DRAWSKY face of a texture chain. mode: 0 = flat color, 1 =
// solidsky (speed 8), 2 = alphasky (speed 16). uu/vv unused for flat.
static void PVR_DrawSkyChains (qmodel_t *model, int mode, uint32_t flatcol)
{
	int		i, j, n;
	texture_t	*t;
	msurface_t	*s;
	glpoly_t	*p;
	float		speed = (mode == 2) ? 16.0f : 8.0f;

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain_world])
			continue;
		if (!(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
			continue;

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
		{
			if (!(s->flags & SURF_DRAWSKY) || !s->polys)
				continue;

			for (p = s->polys; p; p = p->next)
			{
				float		uu[PVR_MAX_POLY_VERTS], vv[PVR_MAX_POLY_VERTS];
				uint32_t	col[PVR_MAX_POLY_VERTS];
				float		*base = p->verts[0];

				n = p->numverts;
				if (n > PVR_MAX_POLY_VERTS)
					n = PVR_MAX_POLY_VERTS;
				for (j = 0; j < n; j++)
				{
					float *vv3 = base + j * VERTEXSIZE;
					if (mode == 0)
					{
						uu[j] = vv[j] = 0.0f;
						col[j] = flatcol;
					}
					else
					{
						PVR_SkyTexCoord (vv3, speed, &uu[j], &vv[j]);
						col[j] = 0xffffffffu;
					}
				}
				PVR_EmitPoly (p, uu, vv, col);
			}
		}
	}
}

//------------------------------------------------------------------------------
// Skybox: 6 gfx/env/ faces (when skybox_name is set), a farclip-sized cube
// centered on the eye, drawn opaque in the OP list. Vertex/texcoord math mirrors
// gl_sky.c Sky_EmitSkyBoxVertex; corners are the full [-1,1] face (the tjunction
// hack). World geometry draws over it; the sky-marked surfaces are skipped, so
// the box shows through them.
//------------------------------------------------------------------------------
extern cvar_t	gl_farclip;

static const int pvr_skytexorder[6] = { 0, 2, 1, 3, 4, 5 };
static const int pvr_st_to_vec[6][3] =
{
	{ 3, -1, 2 }, { -3, 1, 2 }, { 1, 3, 2 }, { -1, -3, 2 }, { -2, -1, 3 }, { 2, -1, -3 }
};

static void PVR_SkyBoxVert (float s, float t, int axis, float *out)
{
	vec3_t	b;
	int	j, k;
	float	fc = gl_farclip.value / 1.7320508f;	// / sqrt(3)

	b[0] = s * fc;
	b[1] = t * fc;
	b[2] = fc;
	for (j = 0; j < 3; j++)
	{
		k = pvr_st_to_vec[axis][j];
		out[j] = (k < 0) ? -b[-k - 1] : b[k - 1];
		out[j] += r_origin[j];
	}
}

static void PVR_DrawSkybox (void)
{
	int	i;

	PVR_ListBegin (PVR_LIST_OP_POLY);
	PVR_SetBlend (GL_ONE, GL_ZERO);
	PVR_SetTexEnv (GL_MODULATE);		// white vertex color -> texel straight
	PVR_RestoreWorldMatrix ();		// faces are world-space

	for (i = 0; i < 6; i++)
	{
		gltexture_t	*tex = skybox_textures[pvr_skytexorder[i]];
		float		v0[3], v1[3], v2[3], v3[3], w, h, slo, shi, tlo, thi;

		if (!tex)
			continue;
		w = (float)tex->width;
		h = (float)tex->height;

		PVR_SkyBoxVert (-1, -1, i, v0);
		PVR_SkyBoxVert (-1,  1, i, v1);
		PVR_SkyBoxVert ( 1,  1, i, v2);
		PVR_SkyBoxVert ( 1, -1, i, v3);

		slo = 0.5f / w;			// s=-1 -> 0, with half-texel border
		shi = (w - 0.5f) / w;		// s=+1 -> 1
		tlo = 1.0f - 0.5f / h;		// t=-1 (flipped)
		thi = 0.5f / h;			// t=+1 (flipped)

		PVR_BillboardQuad (tex, v0, v1, v2, v3,
				   slo, tlo, slo, thi, shi, thi, shi, tlo, 0xffffffffu);
	}
}

// OP pass: skybox if set; else fast -> flat skyflatcolor / slow -> solidsky layer.
void PVR_DrawWorld_Sky (qmodel_t *model)
{
	qboolean	fast;

	if (skybox_name[0])
	{
		PVR_DrawSkybox ();
		return;
	}

	fast = (r_fastsky.value != 0.0f) || !solidskytexture;

	PVR_ListBegin (PVR_LIST_OP_POLY);
	PVR_SetBlend (GL_ONE, GL_ZERO);
	PVR_SetTexEnv (GL_MODULATE);

	if (fast)
	{
		int r = (int)(skyflatcolor[0] * 255.0f);
		int g = (int)(skyflatcolor[1] * 255.0f);
		int b = (int)(skyflatcolor[2] * 255.0f);
		uint32_t argb;
		if (r > 255) r = 255; else if (r < 0) r = 0;
		if (g > 255) g = 255; else if (g < 0) g = 0;
		if (b > 255) b = 255; else if (b < 0) b = 0;
		argb = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

		GL_Bind (NULL);			// untextured -> flat vertex color
		PVR_FlushState ();
		PVR_DrawSkyChains (model, 0, argb);
	}
	else
	{
		GL_Bind (solidskytexture);
		PVR_FlushState ();
		PVR_DrawSkyChains (model, 1, 0);
	}
}

// TR pass: slow only -- the transparent alphasky layer scrolled over the solid.
void PVR_DrawWorld_SkyAlpha (qmodel_t *model)
{
	if (skybox_name[0])			// skybox has no scrolling alpha layer
		return;
	if (r_fastsky.value != 0.0f || !solidskytexture || !alphaskytexture)
		return;

	PVR_ListBegin (PVR_LIST_TR_POLY);
	PVR_SetBlend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	PVR_SetTexEnv (GL_MODULATE);		// keep the layer's texel alpha (holes)
	GL_Bind (alphaskytexture);
	PVR_FlushState ();

	PVR_DrawSkyChains (model, 2, 0);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
