/*
================================================================================
pvr_rsurf.c -- world/brush surface submission for the native PVR renderer (maximqad)

The agnostic half of the world pass stays in r_world.c / r_brush.c: R_MarkSurfaces
walks the PVS and builds per-texture surface chains, BuildSurfaceDisplayList bakes
each surface's glpoly_t (world-space xyz + texture s/t + lightmap s/t), and
R_TextureAnimation picks the animated frame. This module just consumes that: for
each texture chain it binds the texture and fires every surface's polygon into the
PVR opaque list.

Each glpoly_t is a convex fan (GL_POLYGON). Vertices are transformed by the MVP in
the XMTRX bank (one ftrv each) to clip space, near-plane clipped (pvr_clip -- verts
behind the eye would divide by w<=0 and hang the TA), then emitted as fan triangles
straight into the TA through the store queues. The poly header comes from the shared
context cache (pvr_context): opaque list, MODULATE, depth GEQUAL/write, so it slots
in before the 2D punch-through pass.

Fill status: STEP 5. Opaque textured world, lit per-vertex from the static
lightmap (PVR_LightVertex, single MODULATE pass -- no second geometry pass).
Lightstyle animation works via d_lightstylevalue; dynamic lights, sky, water,
alpha surfaces and brush-model entities are still TODO.
================================================================================
*/
#include "pvr_local.h"
#include <math.h>

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

// SURF_ flags for surfaces we skip in the opaque pass (sky + warped water)
#define SURF_SKIP_MASK	(SURF_DRAWSKY | SURF_DRAWTURB)

extern int	d_lightstylevalue[256];	// 8.8 lightstyle intensities (gl_rmain.c)
extern cvar_t	gl_fullbrights;		// r_world.c toggle for the glow pass
extern cvar_t	gl_overbright;		// 2x lighting boost (single-pass approximation)

// Apply per-vertex dynamic lights this pass. Only the world model has its
// surfaces marked by R_PushDlights (R_MarkLights walks cl.worldmodel->nodes) and
// keeps its verts in world space, so dlights are gated to the world -- brush-model
// entities draw model-local verts and would need the light in model space.
static qboolean	pvr_vertex_dlights;

/*
==============
PVR_LightVertex -- per-vertex Gouraud light from the static lightmap

Single-pass lighting: instead of a second modulate pass with the lightmap atlas
(which fights the PVR list order), we sample the BSP lightmap nearest the vertex
and hand the result as the vertex color -- the OP header's MODULATE env then does
texture * light. Mirrors R_BuildLightMap's DC path (grayscale, 1 byte/luxel,
accumulate styles * d_lightstylevalue, >>7). Lightstyle animation (flicker/pulse)
falls out for free via d_lightstylevalue. Dynamic lights are TODO.
==============
*/
// Accumulate every active dynamic light's contribution at a world-space vertex.
// Mirrors R_AddDynamicLights, but sampled once at the vertex (Euclidean in-plane
// distance) instead of per-luxel, and folded straight into the r/g/b light sums
// in the same 8.8 blocklights scale (color * 256, later >>7). Colored dlights
// (lordhavoc lit support) tint the vertex.
static void PVR_AddDynamicLightsVertex (msurface_t *surf, const float *v,
					unsigned *pr, unsigned *pg, unsigned *pb)
{
	mplane_t	*plane = surf->plane;
	int		lnum, i;
	float		rad, perp, minl, dist, bright;
	vec3_t		impact, delta;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		if (!(surf->dlightbits[lnum >> 5] & (1U << (lnum & 31))))
			continue;			// surface not lit by this light

		rad  = cl_dlights[lnum].radius;
		perp = DotProduct (cl_dlights[lnum].origin, plane->normal) - plane->dist;
		rad -= fabsf (perp);			// subtract perpendicular distance to plane
		minl = cl_dlights[lnum].minlight;
		if (rad < minl)
			continue;

		for (i = 0; i < 3; i++)			// closest point on the plane to the light
			impact[i] = cl_dlights[lnum].origin[i] - plane->normal[i] * perp;

		VectorSubtract (v, impact, delta);	// v lies on the plane: in-plane offset
		dist = sqrtf (DotProduct (delta, delta));
		if (dist >= rad - minl)
			continue;			// below the light's minimum contribution

		bright = rad - dist;
		*pr += (unsigned)(bright * cl_dlights[lnum].color[0] * 256.0f);
		*pg += (unsigned)(bright * cl_dlights[lnum].color[1] * 256.0f);
		*pb += (unsigned)(bright * cl_dlights[lnum].color[2] * 256.0f);
	}
}

static uint32_t PVR_LightVertex (msurface_t *surf, const float *v)
{
	mtexinfo_t	*tex;
	int		smax, tmax, size, ls, lt, maps, r, g, b, sh;
	float		s, t;
	unsigned	racc, gacc, bacc;

	if (!surf || !surf->samples || !cl.worldmodel->lightdata)
		return 0xffffffffu;		// unlit -> fullbright

	tex  = surf->texinfo;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax * tmax;

	s = DotProduct (v, tex->vecs[0]) + tex->vecs[0][3] - surf->texturemins[0];
	t = DotProduct (v, tex->vecs[1]) + tex->vecs[1][3] - surf->texturemins[1];
	ls = (int)(s * (1.0f / 16.0f)); if (ls < 0) ls = 0; if (ls >= smax) ls = smax - 1;
	lt = (int)(t * (1.0f / 16.0f)); if (lt < 0) lt = 0; if (lt >= tmax) lt = tmax - 1;

	// Static lightmap: DC lightdata is grayscale (1 byte/luxel), replicated to rgb.
	racc = gacc = bacc = 0;
	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		unsigned lum = (unsigned)surf->samples[maps * size + lt * smax + ls]
			     * (unsigned)d_lightstylevalue[surf->styles[maps]];
		racc += lum; gacc += lum; bacc += lum;
	}

	// Dynamic lights (rockets, explosions, muzzle flashes) -- world surfaces only.
	if (pvr_vertex_dlights && surf->dlightframe == r_framecount)
		PVR_AddDynamicLightsVertex (surf, v, &racc, &gacc, &bacc);

	// gl_overbright doubles the lighting (>>6 instead of >>7). The PVR has no
	// hardware 2x-modulate, so this is a single-pass boost with clamp rather than
	// true per-texel overbright (bright areas saturate instead of exceeding the
	// texture), but it gives the brighter overbright look for free.
	sh = gl_overbright.value ? 6 : 7;
	r = (int)(racc >> sh); if (r > 255) r = 255;
	g = (int)(gacc >> sh); if (g > 255) g = 255;
	b = (int)(bacc >> sh); if (b > 255) b = 255;

	return 0xff000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

//------------------------------------------------------------------------------
// Emit one convex-fan polygon with near-plane clipping (shared by world + warp)
//
// Transform each vertex to clip space against the XMTRX MVP, flag which are in
// front of the near plane (w >= z), then: skip if none visible; fast-path the
// common all-visible case as a triangle strip; otherwise clip each fan triangle.
// Per-vertex texcoords (uu/vv) and colors (col) are supplied by the caller so the
// world (lit, straight texcoords) and pvr_warp (fullbright, warped texcoords) can
// share this transform/clip/emit core. Arrays are sized to p->numverts.
//------------------------------------------------------------------------------
void PVR_EmitPoly (glpoly_t *p, const float *uu, const float *vv, const uint32_t *col)
{
	shz_vec4_t	clip[PVR_MAX_POLY_VERTS];
	int		n = p->numverts;
	float		*base = p->verts[0];
	unsigned	vismask = 0, allvis;
	int		i;

	if (n < 3)
		return;
	if (n > PVR_MAX_POLY_VERTS)
		n = PVR_MAX_POLY_VERTS;

	for (i = 0; i < n; i++)
	{
		float *v = base + i * VERTEXSIZE;
		clip[i] = shz_xmtrx_transform_vec4 (shz_vec4_init (v[0], v[1], v[2], 1.0f));
		if (clip[i].w >= clip[i].z + PVR_NEAR_CLIP_EPSILON)
			vismask |= (1u << i);
	}

	if (vismask == 0)
		return;				// whole poly behind the near plane

	allvis = (1u << n) - 1;
	if (vismask == allvis)
	{
		// fast path: one triangle STRIP for the whole convex fan (n verts, not
		// 3*(n-2) separate-triangle verts) -- 3x less TA traffic, which is what
		// keeps heavy maps from overflowing the tile bins and dropping surfaces.
		// zig-zag strip order over the fan: 0, 1, n-1, 2, n-2, 3, ...
		int lo = 2, hi = n - 1, emitted = 0;

		#define EMITV(idx) do {                                                  \
			float iw = 1.0f / clip[idx].w;                                   \
			pvr_vertex_t *vp = (pvr_vertex_t *) pvr_dr_target (NULL);         \
			vp->flags = (++emitted == n) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;\
			vp->x = clip[idx].x * iw; vp->y = clip[idx].y * iw; vp->z = iw;   \
			vp->u = uu[idx]; vp->v = vv[idx]; vp->argb = col[idx]; vp->oargb = 0;\
			pvr_dr_commit (vp);                                              \
		} while (0)

		EMITV (0);
		EMITV (1);
		while (lo <= hi)
		{
			EMITV (hi); hi--;
			if (lo <= hi) { EMITV (lo); lo++; }
		}
		#undef EMITV
	}
	else
	{
		// slow path: clip each fan triangle against the near plane
		for (i = 1; i < n - 1; i++)
			PVR_ClipAndSubmitTriangle (clip[0], clip[i], clip[i+1],
						   uu[0], vv[0], uu[i], vv[i], uu[i+1], vv[i+1],
						   col[0], col[i], col[i+1]);
	}
}

// Fill a surface poly's per-vertex texcoords (straight) + lit colors, then emit.
static void PVR_SubmitLitPoly (glpoly_t *p, msurface_t *surf)
{
	float		uu[PVR_MAX_POLY_VERTS], vv[PVR_MAX_POLY_VERTS];
	uint32_t	col[PVR_MAX_POLY_VERTS];
	float		*base = p->verts[0];
	int		n = p->numverts, i;

	if (n > PVR_MAX_POLY_VERTS)
		n = PVR_MAX_POLY_VERTS;
	for (i = 0; i < n; i++)
	{
		float *v = base + i * VERTEXSIZE;
		uu[i]  = v[3];
		vv[i]  = v[4];
		col[i] = PVR_LightVertex (surf, v);
	}
	PVR_EmitPoly (p, uu, vv, col);
}

/*
==============
PVR_DrawChains -- submit a model's opaque solid texture chains to the PVR OP list

Shared by the world (chain_world) and brush-model entities (chain_model). For
entities the caller has already post-multiplied the object transform into XMTRX,
so the model-local verts map correctly. Mirrors R_DrawTextureChains_TextureOnly
but emits through the PVR instead of glBegin.
==============
*/
static void PVR_DrawChains (qmodel_t *model, texchain_t chain)
{
	int		i;
	texture_t	*t, *ta;
	msurface_t	*s;

	PVR_ListBegin (PVR_LIST_OP_POLY);
	PVR_SetBlend (GL_ONE, GL_ZERO);		// opaque
	PVR_SetTexEnv (GL_MODULATE);		// texture * per-vertex light

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain])
			continue;
		if (t->texturechains[chain]->flags & SURF_SKIP_MASK)
			continue;			// sky / water chains handled elsewhere

		ta = R_TextureAnimation (t, 0);
		if (ta->gltexture && (ta->gltexture->flags & TEXPREF_ALPHA))
			continue;			// fence ('{') textures -> punch-through pass
		GL_Bind (ta->gltexture);		// records the bound texture for the header
		PVR_FlushState ();			// compile + submit the OP poly header if changed

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (s->flags & SURF_SKIP_MASK)
				continue;
			if (s->polys)
				PVR_SubmitLitPoly (s->polys, s);
		}
	}
}

void PVR_DrawWorld (qmodel_t *model)
{
	pvr_vertex_dlights = true;		// world verts are world-space + dlight-marked
	PVR_DrawChains (model, chain_world);
}

/*
==============
PVR_DrawWorld_Fence -- alpha-tested ('{') world surfaces into the punch-through list

Fence/grate textures load with TEXPREF_ALPHA (palette index 255 -> alpha 0). The
PT list alpha-tests those texels away. PT renders last (with the 2D HUD), so this
runs after the OP solid world and TR translucent water.
==============
*/
void PVR_DrawWorld_Fence (qmodel_t *model)
{
	int		i;
	texture_t	*t, *ta;
	msurface_t	*s;

	pvr_vertex_dlights = true;		// fence surfaces belong to the world model
	PVR_ListBegin (PVR_LIST_PT_POLY);
	PVR_SetBlend (GL_ONE, GL_ZERO);		// no blend; the PT list does the alpha test
	PVR_SetTexEnv (GL_MODULATE);		// lit

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain_world])
			continue;
		if (t->texturechains[chain_world]->flags & SURF_SKIP_MASK)
			continue;

		ta = R_TextureAnimation (t, 0);
		if (!ta->gltexture || !(ta->gltexture->flags & TEXPREF_ALPHA))
			continue;			// only fence textures here

		GL_Bind (ta->gltexture);
		PVR_FlushState ();

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
		{
			if (s->flags & SURF_SKIP_MASK)
				continue;
			if (s->polys)
				PVR_SubmitLitPoly (s->polys, s);
		}
	}
}

// Brush-model entity solid surfaces (door/plat/button); XMTRX already holds the
// entity's world*object MVP (PVR_SetupEntityMatrices).
void PVR_DrawBrushModel (qmodel_t *model)
{
	pvr_vertex_dlights = false;		// model-local verts: skip world-space dlights
	PVR_DrawChains (model, chain_model);
}

//------------------------------------------------------------------------------
// Fullbright / glow pass (mirrors R_DrawTextureChains_Glow)
//
// Textures with a luma/glow map (t->fullbright: lava, slime, tech panels, torch
// wall lights) get a second additive pass so their bright texels ignore the
// lightmap and glow. The glow map is black except on the glowing texels, so
// additive blend (ONE,ONE) with a white vertex color adds only the glow. Emitted
// into the TR list (blended, depth-tested against the opaque world but no depth
// write) between the translucent water and the PT fence pass.
//------------------------------------------------------------------------------
static void PVR_SubmitFlatPoly (glpoly_t *p, uint32_t color)
{
	float		uu[PVR_MAX_POLY_VERTS], vv[PVR_MAX_POLY_VERTS];
	uint32_t	col[PVR_MAX_POLY_VERTS];
	float		*base = p->verts[0];
	int		n = p->numverts, i;

	if (n > PVR_MAX_POLY_VERTS)
		n = PVR_MAX_POLY_VERTS;
	for (i = 0; i < n; i++)
	{
		float *v = base + i * VERTEXSIZE;
		uu[i]  = v[3];
		vv[i]  = v[4];
		col[i] = color;
	}
	PVR_EmitPoly (p, uu, vv, col);
}

static void PVR_DrawGlowChains (qmodel_t *model, texchain_t chain)
{
	int		i;
	texture_t	*t;
	gltexture_t	*glt;
	msurface_t	*s;

	if (!gl_fullbrights.value)
		return;

	PVR_ListBegin (PVR_LIST_TR_POLY);
	PVR_SetBlend (GL_ONE, GL_ONE);		// additive glow
	PVR_SetTexEnv (GL_MODULATE);		// texel * white == texel

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain])
			continue;
		if (t->texturechains[chain]->flags & SURF_SKIP_MASK)
			continue;
		glt = R_TextureAnimation (t, 0)->fullbright;
		if (!glt)
			continue;			// no glow map for this texture

		GL_Bind (glt);
		PVR_FlushState ();

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (s->flags & SURF_SKIP_MASK)
				continue;
			if (s->polys)
				PVR_SubmitFlatPoly (s->polys, 0xffffffffu);
		}
	}
}

void PVR_DrawWorld_Fullbright (qmodel_t *model)
{
	PVR_DrawGlowChains (model, chain_world);
}

// Brush-model entity glow surfaces; XMTRX already holds the entity MVP.
void PVR_DrawBrushModel_Fullbright (qmodel_t *model)
{
	PVR_DrawGlowChains (model, chain_model);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
