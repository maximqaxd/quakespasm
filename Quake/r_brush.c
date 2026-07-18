/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_brush.c: brush model rendering. renamed from r_surf.c

#include "quakedef.h"

extern cvar_t gl_fullbrights, r_drawflat, gl_overbright, r_oldwater; //johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix

int		gl_lightmap_format;
int		lightmap_bytes;

#define MAX_SANITY_LIGHTMAPS (1u<<20)
struct lightmap_s	*lightmaps;
int		lightmap_count;

static int	allocated[LMBLOCK_WIDTH];
static int	last_lightmap_allocated;

static unsigned	blocklights[LMBLOCK_WIDTH*LMBLOCK_HEIGHT*3]; //johnfitz -- was 18*18, added lit support (*3) and loosened surface extents maximum (LMBLOCK_WIDTH*LMBLOCK_HEIGHT)


/*
===============
R_TextureAnimation -- johnfitz -- added "frame" param to eliminate use of "currententity" global

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	int		relative;
	int		count;

	if (frame)
		if (base->alternate_anims)
			base = base->alternate_anims;

	if (!base->anim_total)
		return base;

	relative = (int)(cl.time*10) % base->anim_total;

	count = 0;
	while (base->anim_min > relative || base->anim_max <= relative)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}

/*
================
DrawGLPoly
================
*/
void DrawGLPoly (glpoly_t *p)
{
	float	*v;
	int		i;

	glBegin (GL_POLYGON);
	v = p->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE)
	{
		glTexCoord2f (v[3], v[4]);
		glVertex3fv (v);
	}
	glEnd ();
}

/*
================
DrawGLTriangleFan -- johnfitz -- like DrawGLPoly but for r_showtris
================
*/
void DrawGLTriangleFan (glpoly_t *p)
{
	float	*v;
	int		i;

	glBegin (GL_TRIANGLE_FAN);
	v = p->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE)
	{
		glVertex3fv (v);
	}
	glEnd ();
}

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
/*
=================
R_PVRBrushModel_Setup -- prepare a brush-model entity for PVR submission

Cull, mark dynamic lights on the submodel, load the entity MVP into XMTRX, and
(re)build the entity's chain_model surface chains. Returns false if culled.

Factored out of R_DrawBrushModel so the scene can iterate the brush entities
once per PVR list phase (OP, then TR): the PVR requires each list be opened
exactly once in hardware order, so an entity can't submit solid (OP) then glow
(TR) inline -- the next entity's solid pass would reopen the closed OP list. The
chains are transient (per model), so each phase rebuilds them via this call; on
true the caller submits the phase's list(s) then PVR_RestoreWorldMatrix.
=================
*/
qboolean R_PVRBrushModel_Setup (entity_t *e)
{
	int			i, k;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;

	if (R_CullModelForEntity(e))
		return false;

	currententity = e;
	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	// dynamic lighting for the bmodel if it's not an instanced model
	if (clmodel->firstmodelsurface != 0 && !gl_flashblend.value)
	{
		for (k=0 ; k<MAX_DLIGHTS ; k++)
		{
			if ((cl_dlights[k].die < cl.time) || (!cl_dlights[k].radius))
				continue;
			R_MarkLights (&cl_dlights[k], k,
				clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	e->angles[0] = -e->angles[0];	// stupid quake bug
	PVR_SetupEntityMatrices (e->origin, e->angles, e->scale);
	e->angles[0] = -e->angles[0];	// stupid quake bug

	R_ClearTextureChains (clmodel, chain_model);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_ChainSurface (psurf, chain_model);
			R_RenderDynamicLightmaps(psurf);
			rs_brushpolys++;
		}
	}
	return true;
}
#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (entity_t *e)
{
#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
	// Opaque-only single-entity path (kept for completeness). The scene actually
	// drives brush ents through the phased R_PVRBrushModel_Setup loops in
	// R_RenderScene so lists stay in order; glow goes in the TR phase there.
	qmodel_t *clmodel;

	if (!R_PVRBrushModel_Setup (e))
		return;
	clmodel = e->model;
	PVR_DrawBrushModel (clmodel);
	PVR_DrawBrushModel_Water (clmodel);
	PVR_RestoreWorldMatrix ();
#else
	int			i, k;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;

	if (R_CullModelForEntity(e))
		return;

	currententity = e;
	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (clmodel->firstmodelsurface != 0 && !gl_flashblend.value)
	{
		for (k=0 ; k<MAX_DLIGHTS ; k++)
		{
			if ((cl_dlights[k].die < cl.time) ||
				(!cl_dlights[k].radius))
				continue;

			R_MarkLights (&cl_dlights[k], k,
				clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	e->angles[0] = -e->angles[0];	// stupid quake bug
	glPushMatrix ();
	if (gl_zfix.value)
	{
		e->origin[0] -= DIST_EPSILON;
		e->origin[1] -= DIST_EPSILON;
		e->origin[2] -= DIST_EPSILON;
	}
	R_RotateForEntity (e->origin, e->angles, e->scale);
	if (gl_zfix.value)
	{
		e->origin[0] += DIST_EPSILON;
		e->origin[1] += DIST_EPSILON;
		e->origin[2] += DIST_EPSILON;
	}
	e->angles[0] = -e->angles[0];	// stupid quake bug

	R_ClearTextureChains (clmodel, chain_model);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_ChainSurface (psurf, chain_model);
			R_RenderDynamicLightmaps(psurf);
			rs_brushpolys++;
		}
	}

	R_DrawTextureChains (clmodel, e, chain_model);
	R_DrawTextureChains_Water (clmodel, e, chain_model);

	glPopMatrix ();
#endif
}

/*
=================
R_DrawBrushModel_ShowTris -- johnfitz
=================
*/
void R_DrawBrushModel_ShowTris (entity_t *e)
{
	int			i;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;
	glpoly_t	*p;

	if (R_CullModelForEntity(e))
		return;

	currententity = e;
	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	glPushMatrix ();
	e->angles[0] = -e->angles[0];	// stupid quake bug
	R_RotateForEntity (e->origin, e->angles, e->scale);
	e->angles[0] = -e->angles[0];	// stupid quake bug

	//
	// draw it
	//
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			if ((psurf->flags & SURF_DRAWTURB) && r_oldwater.value)
				for (p = psurf->polys->next; p; p = p->next)
					DrawGLTriangleFan (p);
			else
				DrawGLTriangleFan (psurf->polys);
		}
	}

	glPopMatrix ();
}

/*
=============================================================

	LIGHTMAPS

=============================================================
*/

/*
================
R_RenderDynamicLightmaps
called during rendering
================
*/
void R_RenderDynamicLightmaps (msurface_t *fa)
{
#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
	// The native PVR renderer lights the world per-vertex (pvr_rsurf.c) and never
	// samples the lightmap atlas, so there's nothing to bake or upload here --
	// and calling glTexSubImage2D on the uninitialized GLdc would just spam.
	(void)fa;
	return;
#else
#if !defined(PLATFORM_DREAMCAST)
	byte		*base;
	int			maps;
	glRect_t    *theRect;
	int smax, tmax;
#endif

	if (fa->flags & SURF_DRAWTILED) //johnfitz -- not a lightmapped surface
		return;

	// add to lightmap chain
	fa->polys->chain = lightmaps[fa->lightmaptexturenum].polys;
	lightmaps[fa->lightmaptexturenum].polys = fa->polys;

#if defined(PLATFORM_DREAMCAST)
	{
		static byte	dc_lmtemp[LMBLOCK_WIDTH * LMBLOCK_HEIGHT * 2];	/* RGB565 */
		qboolean	needsupdate = false;
		int		m, smax, tmax;

		if (!r_dynamic.value)
			return;

		for (m = 0 ; m < MAXLIGHTMAPS && fa->styles[m] != 255 ; m++)
			if (d_lightstylevalue[fa->styles[m]] != fa->cached_light[m])
			{ needsupdate = true; break; }

		if (fa->dlightframe == r_framecount || fa->cached_dlight)
			needsupdate = true;

		if (!needsupdate)
			return;

		smax = (fa->extents[0] >> 4) + 1;
		tmax = (fa->extents[1] >> 4) + 1;

		// R_BuildLightMap bakes styles + this frame's dlights and updates
		// fa->cached_light / cached_dlight so we self-heal when the light leaves.
		R_BuildLightMap (fa, dc_lmtemp, smax * 2 /* bytes per row, tight */);

		GL_Bind (lightmaps[fa->lightmaptexturenum].texture);
		glTexSubImage2D (GL_TEXTURE_2D, 0, fa->light_s, fa->light_t, smax, tmax,
				 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dc_lmtemp);
		rs_dynamiclightmaps++;
	}
	return;
#else
	// check for lightmap modification
	for (maps=0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
dynamic:
		if (r_dynamic.value)
		{
			struct lightmap_s *lm = &lightmaps[fa->lightmaptexturenum];
			lm->modified = true;
			theRect = &lm->rectchange;
			if (fa->light_t < theRect->t) {
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l) {
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0]>>4)+1;
			tmax = (fa->extents[1]>>4)+1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s-theRect->l)+smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t-theRect->t)+tmax;
			base = lm->data;
			base += fa->light_t * LMBLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, LMBLOCK_WIDTH*lightmap_bytes);
		}
	}
#endif	/* !PLATFORM_DREAMCAST */
#endif	/* !(PLATFORM_DREAMCAST && USE_PVR_RENDER) */
}

/*
========================
AllocBlock -- returns a texture number and the position inside it
========================
*/
int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	// ericw -- rather than searching starting at lightmap 0 every time,
	// start at the last lightmap we allocated a surface in.
	// This makes AllocBlock much faster on large levels (can shave off 3+ seconds
	// of load time on a level with 180 lightmaps), at a cost of not quite packing
	// lightmaps as tightly vs. not doing this (uses ~5% more lightmaps)
	for (texnum=last_lightmap_allocated ; texnum<MAX_SANITY_LIGHTMAPS ; texnum++)
	{
		if (texnum == lightmap_count)
		{
			lightmap_count++;
			lightmaps = (struct lightmap_s *) realloc(lightmaps, sizeof(*lightmaps)*lightmap_count);
			memset(&lightmaps[texnum], 0, sizeof(lightmaps[texnum]));
			lightmaps[texnum].data = (byte *) calloc(1, lightmap_bytes*LMBLOCK_WIDTH*LMBLOCK_HEIGHT);
			//as we're only tracking one texture, we don't need multiple copies of allocated any more.
			memset(allocated, 0, sizeof(allocated));
		}
		best = LMBLOCK_HEIGHT;

		for (i=0 ; i<LMBLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (allocated[i+j] >= best)
					break;
				if (allocated[i+j] > best2)
					best2 = allocated[i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > LMBLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			allocated[*x + i] = best + h;

		last_lightmap_allocated = texnum;
		return texnum;
	}

	Sys_Error ("AllocBlock: full");
	return 0; //johnfitz -- shut up compiler
}


static mvertex_t	*r_pcurrentvertbase;
static  qmodel_t	*currentmodel;

/*
========================
GL_CreateSurfaceLightmap
========================
*/
void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int		smax, tmax;
	byte	*base;

	if (surf->flags & SURF_DRAWTILED)
	{
		surf->lightmaptexturenum = -1;
		return;
	}

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;

	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	base = lightmaps[surf->lightmaptexturenum].data;
	base += (surf->light_t * LMBLOCK_WIDTH + surf->light_s) * lightmap_bytes;
	R_BuildLightMap (surf, base, LMBLOCK_WIDTH*lightmap_bytes);
}

/*
================
BuildSurfaceDisplayList -- called at level load time
================
*/
void BuildSurfaceDisplayList (msurface_t *fa)
{
	int			i, lindex, lnumverts;
	medge_t		*pedges, *r_pedge;
	float		*vec;
	float		s, t, s0, t0, sdiv, tdiv;
	glpoly_t	*poly;

// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	//
	// draw texture
	//
	poly = (glpoly_t *) Hunk_Alloc (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	fa->polys = poly;
	poly->numverts = lnumverts;

	if (fa->flags & SURF_DRAWTURB)
	{
		// match Mod_PolyForUnlitSurface
		s0 = t0 = 0.f;
		sdiv = tdiv = 128.f;
	}
	else
	{
		s0 = fa->texinfo->vecs[0][3];
		t0 = fa->texinfo->vecs[1][3];
		sdiv = fa->texinfo->texture->width;
		tdiv = fa->texinfo->texture->height;
	}

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + s0;
		s /= sdiv;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + t0;
		t /= tdiv;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		// Q64 RERELEASE texture shift
		if (fa->texinfo->texture->shift > 0)
		{
			poly->verts[i][3] /= ( 2 * fa->texinfo->texture->shift);
			poly->verts[i][4] /= ( 2 * fa->texinfo->texture->shift);
		}

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= LMBLOCK_WIDTH*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= LMBLOCK_HEIGHT*16; //fa->texinfo->texture->height;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	//johnfitz -- removed gl_keeptjunctions code

	poly->numverts = lnumverts;

	// support r_oldwater 1 on lit water
	if (fa->flags & SURF_DRAWTURB)
		GL_SubdivideSurface (fa);
}

/*
==================
GL_BuildLightmaps -- called at level load time

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
#if !(defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER))
	char	name[24];	// lightmap texture names -- PVR path uploads no lightmaps
#endif
	int		i, j;
	struct lightmap_s *lm;
	qmodel_t	*m;

	r_framecount = 1; // no dlightcache

	//Spike -- wipe out all the lightmap data (johnfitz -- the gltexture objects were already freed by Mod_ClearAll)
	for (i=0; i < lightmap_count; i++)
		free(lightmaps[i].data);
	free(lightmaps);
	lightmaps = NULL;
	last_lightmap_allocated = 0;
	lightmap_count = 0;

#if defined(PLATFORM_DREAMCAST)
	gl_lightmap_format = GL_RGB565_KOS;	/* 2 bytes/luxel: halves lightmap RAM + VRAM */
#else
	gl_lightmap_format = GL_RGBA;//FIXME: hardcoded for now!
#endif

	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		lightmap_bytes = 4;
		break;
	case GL_BGRA:
		lightmap_bytes = 4;
		break;
#if defined(PLATFORM_DREAMCAST)
	case GL_RGB565_KOS:
		lightmap_bytes = 2;
		break;
#endif
	default:
		Sys_Error ("GL_BuildLightmaps: bad lightmap format");
	}

	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			//johnfitz -- rewritten to use SURF_DRAWTILED instead of the sky/water flags
			if (m->surfaces[i].flags & SURF_DRAWTILED)
				continue;
			GL_CreateSurfaceLightmap (m->surfaces + i);
			BuildSurfaceDisplayList (m->surfaces + i);
			//johnfitz
		}
	}

	//
	// upload all lightmaps that were filled
	//
	for (i=0; i<lightmap_count; i++)
	{
		lm = &lightmaps[i];
		lm->modified = false;
		lm->rectchange.l = LMBLOCK_WIDTH;
		lm->rectchange.t = LMBLOCK_HEIGHT;
		lm->rectchange.w = 0;
		lm->rectchange.h = 0;

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
		// The native PVR renderer lights surfaces per-vertex (PVR_LightVertex reads
		// surf->samples directly), so the lightmap atlas is never sampled. Uploading
		// it would waste VRAM (RGB565, 32KB per 128x128 block -- 1-2MB on big maps)
		// that we need for full-res skins. Keep the atlas unbuilt in VRAM; free the
		// RAM copy too.
		lm->texture = NULL;
		free (lm->data);
		lm->data = NULL;
#else
		//johnfitz -- use texture manager
		sprintf(name, "lightmap%07i",i);
		lm->texture = TexMgr_LoadImage (cl.worldmodel, name, LMBLOCK_WIDTH, LMBLOCK_HEIGHT,
						SRC_LIGHTMAP, lm->data, "", (src_offset_t)lm->data, TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		//johnfitz
#if defined(PLATFORM_DREAMCAST)
		lm->texture->source_offset = 0;
		free (lm->data);
		lm->data = NULL;
#endif
#endif
	}

	//johnfitz -- warn about exceeding old limits
	//GLQuake limit was 64 textures of 128x128. Estimate how many 128x128 textures we would need
	//given that we are using lightmap_count of LMBLOCK_WIDTH x LMBLOCK_HEIGHT
	i = lightmap_count * ((LMBLOCK_WIDTH / 128) * (LMBLOCK_HEIGHT / 128));
	if (i > 64)
		Con_DWarning("%i lightmaps exceeds standard limit of 64.\n",i);
	//johnfitz
}

/*
=============================================================

	VBO support

=============================================================
*/

GLuint gl_bmodel_vbo = 0;

void GL_DeleteBModelVertexBuffer (void)
{
	if (!(gl_vbo_able && gl_mtexable && gl_max_texture_units >= 3))
		return;

	GL_DeleteBuffersFunc (1, &gl_bmodel_vbo);
	gl_bmodel_vbo = 0;

	GL_ClearBufferBindings ();
}

/*
==================
GL_BuildBModelVertexBuffer

Deletes gl_bmodel_vbo if it already exists, then rebuilds it with all
surfaces from world + all brush models
==================
*/
void GL_BuildBModelVertexBuffer (void)
{
	unsigned int	numverts, varray_bytes, varray_index;
	int		i, j;
	qmodel_t	*m;
	float		*varray;

	if (!(gl_vbo_able && gl_mtexable && gl_max_texture_units >= 3))
		return;

// ask GL for a name for our VBO
	GL_DeleteBuffersFunc (1, &gl_bmodel_vbo);
	GL_GenBuffersFunc (1, &gl_bmodel_vbo);
	
// count all verts in all models
	numverts = 0;
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i=0 ; i<m->numsurfaces ; i++)
		{
			numverts += m->surfaces[i].numedges;
		}
	}
	
// build vertex array
	varray_bytes = VERTEXSIZE * sizeof(float) * numverts;
	varray = (float *) malloc (varray_bytes);
	varray_index = 0;
	
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i=0 ; i<m->numsurfaces ; i++)
		{
			msurface_t *s = &m->surfaces[i];
			s->vbo_firstvert = varray_index;
			memcpy (&varray[VERTEXSIZE * varray_index], s->polys->verts, VERTEXSIZE * sizeof(float) * s->numedges);
			varray_index += s->numedges;
		}
	}

// upload to GPU
	GL_BindBufferFunc (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BufferDataFunc (GL_ARRAY_BUFFER, varray_bytes, varray, GL_STATIC_DRAW);
	free (varray);
	
// invalidate the cached bindings
	GL_ClearBufferBindings ();
}

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;
	//johnfitz -- lit support via lordhavoc
	float		cred, cgreen, cblue, brightness;
	unsigned	*bl;
	//johnfitz

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if (! (surf->dlightbits[lnum >> 5] & (1U << (lnum & 31))))
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		//johnfitz -- lit support via lordhavoc
		bl = blocklights;
		cred = cl_dlights[lnum].color[0] * 256.0f;
		cgreen = cl_dlights[lnum].color[1] * 256.0f;
		cblue = cl_dlights[lnum].color[2] * 256.0f;
		//johnfitz
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				//johnfitz -- lit support via lordhavoc
				{
					brightness = rad - dist;
					bl[0] += (int) (brightness * cred);
					bl[1] += (int) (brightness * cgreen);
					bl[2] += (int) (brightness * cblue);
				}
				bl += 3;
				//johnfitz
			}
		}
	}
}


/*
===============
R_BuildLightMap -- johnfitz -- revised for lit support via lordhavoc

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	const int overbright = !!gl_overbright.value;
	const int wide10bits = !!r_lightmapwide.value;

	int			smax, tmax;
	unsigned		r, g, b;
	int			i, j, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	unsigned	*bl;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (cl.worldmodel->lightdata)
	{
	// clear to no light
		memset (&blocklights[0], 0, size * 3 * sizeof (unsigned int)); //johnfitz -- lit support via lordhavoc

	// add all the lightmaps
		if (lightmap)
		{
			for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
				 maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				surf->cached_light[maps] = scale;	// 8.8 fraction
				//johnfitz -- lit support via lordhavoc
				bl = blocklights;
#if defined(PLATFORM_DREAMCAST)
				// grayscale lightdata: 1 byte/luxel, replicated to r,g,b
				for (i=0 ; i<size ; i++)
				{
					unsigned lum = *lightmap++ * scale;
					*bl++ += lum;
					*bl++ += lum;
					*bl++ += lum;
				}
#else
				for (i=0 ; i<size ; i++)
				{
					*bl++ += *lightmap++ * scale;
					*bl++ += *lightmap++ * scale;
					*bl++ += *lightmap++ * scale;
				}
#endif
				//johnfitz
			}
		}

	// add all the dynamic lights
		if (surf->dlightframe == r_framecount)
			R_AddDynamicLights (surf);
	}
	else
	{
	// set to full bright if no light data
		memset (&blocklights[0], 255, size * 3 * sizeof (unsigned int)); //johnfitz -- lit support via lordhavoc
	}

// bound, invert, and shift
// store:
	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		stride -= smax * 4;
		bl = blocklights;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				if (overbright)
				{
					r = *bl++ >> 8;
					g = *bl++ >> 8;
					b = *bl++ >> 8;
				}
				else
				{
					r = *bl++ >> 7;
					g = *bl++ >> 7;
					b = *bl++ >> 7;
					if (wide10bits) {
						// artifically clamp to 255 so gl_overbright 0 renders as expected in the wide10bits case
						r = (r > 255) ? 255 : r;
						g = (g > 255) ? 255 : g;
						b = (b > 255) ? 255 : b;
						goto loc0;
					}
				}
				if (wide10bits)
				{
					r = (r > 1023)? 1023 : r;
					g = (g > 1023)? 1023 : g;
					b = (b > 1023)? 1023 : b;
					loc0:
					*(unsigned int*)dest = (r<<22) | (g<<12) | (b<<2) | 3;
					dest += 4;
				}
				else
				{
					*dest++ = (r > 255)? 255 : r;
					*dest++ = (g > 255)? 255 : g;
					*dest++ = (b > 255)? 255 : b;
					*dest++ = 255;
				}
			}
		}
		break;
	case GL_BGRA:
		stride -= smax * 4;
		bl = blocklights;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				if (overbright)
				{
					r = *bl++ >> 8;
					g = *bl++ >> 8;
					b = *bl++ >> 8;
				}
				else
				{
					r = *bl++ >> 7;
					g = *bl++ >> 7;
					b = *bl++ >> 7;
					if (wide10bits) {
						// artifically clamp to 255 so gl_overbright 0 renders as expected in the wide10bits case
						r = (r > 255) ? 255 : r;
						g = (g > 255) ? 255 : g;
						b = (b > 255) ? 255 : b;
						goto loc1;
					}
				}
				if (wide10bits)
				{
					r = (r > 1023)? 1023 : r;
					g = (g > 1023)? 1023 : g;
					b = (b > 1023)? 1023 : b;
					loc1:
					*(unsigned int*)dest = (b<<22) | (g<<12) | (r<<2) | 3;
					dest += 4;
				}
				else
				{
					*dest++ = (b > 255)? 255 : b;
					*dest++ = (g > 255)? 255 : g;
					*dest++ = (r > 255)? 255 : r;
					*dest++ = 255;
				}
			}
		}
		break;
#if defined(PLATFORM_DREAMCAST)
	case GL_RGB565_KOS:
		stride -= smax * 2;
		bl = blocklights;
		for (i=0 ; i<tmax ; i++, dest += stride)
		{
			for (j=0 ; j<smax ; j++)
			{
				if (overbright)
				{
					r = *bl++ >> 8;
					g = *bl++ >> 8;
					b = *bl++ >> 8;
				}
				else
				{
					r = *bl++ >> 7;
					g = *bl++ >> 7;
					b = *bl++ >> 7;
				}
				if (r > 255) r = 255;
				if (g > 255) g = 255;
				if (b > 255) b = 255;
				*(unsigned short *)dest = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
				dest += 2;
			}
		}
		break;
#endif
	default:
		Sys_Error ("R_BuildLightMap: bad lightmap format");
	}
}

/*
===============
R_UploadLightmap -- johnfitz -- uploads the modified lightmap to opengl if necessary

assumes lightmap texture is already bound
===============
*/
static void R_UploadLightmap(int lmap)
{
	const int wide10bits = !!r_lightmapwide.value;
	GLenum type = wide10bits ?
	    GL_UNSIGNED_INT_10_10_10_2 : GL_UNSIGNED_BYTE;
	GLenum format = gl_lightmap_format;
	struct lightmap_s *lm = &lightmaps[lmap];

	if (!lm->modified)
		return;

	lm->modified = false;

#if defined(PLATFORM_DREAMCAST)
	if (gl_lightmap_format == GL_RGB565_KOS)
	{
		format = GL_RGB;
		type = GL_UNSIGNED_SHORT_5_6_5;
	}
#endif

	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, lm->rectchange.t, LMBLOCK_WIDTH, lm->rectchange.h, format,
			type, lm->data + lm->rectchange.t*LMBLOCK_WIDTH*lightmap_bytes);
	lm->rectchange.l = LMBLOCK_WIDTH;
	lm->rectchange.t = LMBLOCK_HEIGHT;
	lm->rectchange.h = 0;
	lm->rectchange.w = 0;

	rs_dynamiclightmaps++;
}

void R_UploadLightmaps (void)
{
	int lmap;

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
	(void)lmap;
	return;		// PVR lights per-vertex; the lightmap atlas is never sampled
#endif

	for (lmap = 0; lmap < lightmap_count; lmap++)
	{
		if (!lightmaps[lmap].modified)
			continue;

		GL_Bind (lightmaps[lmap].texture);
		R_UploadLightmap(lmap);
	}
}

/*
================
R_RebuildAllLightmaps -- johnfitz -- called when gl_overbright gets toggled
================
*/
void R_RebuildAllLightmaps (void)
{
#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)
	return;		// PVR lights per-vertex; no lightmap atlas to rebuild/upload
#else
	const int wide10bits = !!r_lightmapwide.value;
	GLenum type = wide10bits ?
	    GL_UNSIGNED_INT_10_10_10_2 : GL_UNSIGNED_BYTE;
	GLenum format = gl_lightmap_format;
	int			i, j;
	qmodel_t	*mod;

#if defined(PLATFORM_DREAMCAST)
	if (gl_lightmap_format == GL_RGB565_KOS)
	{
		format = GL_RGB;
		type = GL_UNSIGNED_SHORT_5_6_5;
	}
#endif
	msurface_t	*fa;
	byte		*base;

	if (!cl.worldmodel) // is this the correct test?
		return;

	//for each surface in each model, rebuild lightmap with new scale
	for (i=1; i<MAX_MODELS; i++)
	{
		if (!(mod = cl.model_precache[i]))
			continue;
		fa = &mod->surfaces[mod->firstmodelsurface];
		for (j=0; j<mod->nummodelsurfaces; j++, fa++)
		{
			if (fa->flags & SURF_DRAWTILED)
				continue;
			base = lightmaps[fa->lightmaptexturenum].data;
			base += fa->light_t * LMBLOCK_WIDTH * lightmap_bytes + fa->light_s * lightmap_bytes;
			R_BuildLightMap (fa, base, LMBLOCK_WIDTH*lightmap_bytes);
		}
	}

	//for each lightmap, upload it
	for (i=0; i<lightmap_count; i++)
	{
		GL_Bind (lightmaps[i].texture);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, LMBLOCK_WIDTH, LMBLOCK_HEIGHT, format,
				 type, lightmaps[i].data);
	}
#endif	/* !(PLATFORM_DREAMCAST && USE_PVR_RENDER) */
}
