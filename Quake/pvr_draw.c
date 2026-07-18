/*
================================================================================
pvr_draw.c -- 2D drawing (HUD / menu / console) for the native PVR renderer (maximqad)

REPLACES gl_draw.c when USE_PVR_RENDER is set (the Makefile swaps the object).

The pic caching / scrap / init half of gl_draw.c is renderer-agnostic (it all goes
through TexMgr) and is kept verbatim. Only the actual submission changes: instead
of GLdc immediate mode + glOrtho canvases, 2D quads are mapped to screen pixels by
a per-canvas affine transform (derived from the same glOrtho+glViewport math) and
fired into the PVR translucent list as triangle strips.

All 2D lives in the TR list (drawn last, over the 3D scene). Cutouts (conchars,
HUD pics) rely on the palette's alpha (index 255/0 -> alpha 0) blended with
SRC_ALPHA/INV_SRC_ALPHA; solid fills/fades use the vertex color's alpha. A tiny
per-quad depth increment preserves submission (painter) order under TR autosort.
================================================================================
*/
#include "pvr_local.h"		// dc/pvr.h before quakedef.h (HZ), pulls quakedef too

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

//extern unsigned char d_15to8table[65536]; //johnfitz -- never used

cvar_t		scr_conalpha = {"scr_conalpha", "0.5", CVAR_ARCHIVE}; //johnfitz

qpic_t		*draw_disc;
qpic_t		*draw_backtile;

gltexture_t *char_texture; //johnfitz
qpic_t		*pic_ovr, *pic_ins; //johnfitz -- new cursor handling
qpic_t		*pic_nul; //johnfitz -- for missing gfx, don't crash

//johnfitz -- new pics
byte pic_ovr_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255, 15, 15, 15, 15, 15, 15,255},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255,255,  2,  2,  2,  2,  2,  2},
};

byte pic_ins_data[9][8] =
{
	{ 15, 15,255,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{255,  2,  2,255,255,255,255,255},
};

byte pic_nul_data[8][8] =
{
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
};

byte pic_stipple_data[8][8] =
{
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
};

byte pic_crosshair_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255,255,255,  8,  9,255,255,255},
	{255,255,255,  6,  8,  2,255,255},
	{255,  6,  8,  8,  6,  8,  8,255},
	{255,255,  2,  8,  8,  2,  2,  2},
	{255,255,255,  7,  8,  2,255,255},
	{255,255,255,255,  2,  2,255,255},
	{255,255,255,255,255,255,255,255},
};
//johnfitz

typedef struct
{
	gltexture_t *gltexture;
	float		sl, tl, sh, th;
} glpic_t;

canvastype currentcanvas = CANVAS_NONE; //johnfitz -- for GL_SetCanvas

//==============================================================================
//
//  PVR 2D SUBMISSION
//
//==============================================================================

// Per-canvas screen-space affine: screen_x = cx*c_sx + c_tx, screen_y = cy*c_sy + c_ty
static float	c_sx = 1.0f, c_tx = 0.0f, c_sy = 1.0f, c_ty = 0.0f;

// Painter-order depth: bumped a hair per quad so later 2D draws sort in front under
// the TR autosort (z is 1/w, larger == nearer). Reset each frame in GL_Set2D.
static float	draw2d_z = 1.0f;

static void PVR_SetCanvasXform (float l, float r, float b, float t,
				float vx, float vy, float vw, float vh)
{
	// Reproduce GL's glOrtho(l,r,b,t)+glViewport(vx,vy,vw,vh) mapping, folding in the
	// bottom-origin -> top-origin flip so the result is direct PVR screen pixels.
	c_sx = vw / (r - l);
	c_tx = vx - l * c_sx;
	c_sy = -vh / (t - b);
	c_ty = ((float)glheight - vy) - b * c_sy;
}

static void PVR_EmitQuad (gltexture_t *tex, int env, int bsrc, int bdst,
			  float x0, float y0, float x1, float y1,
			  float s0, float t0, float s1, float t1, uint32_t argb)
{
	pvr_vertex_t	*vp;
	float		ax0, ay0, ax1, ay1, z;

	PVR_ListBegin (PVR_LIST_TR_POLY);

	if (tex)
		GL_Bind (tex);
	else
		PVR_BindTexture (NULL);
	PVR_SetTexEnv (env);
	PVR_SetBlend (bsrc, bdst);
	PVR_FlushState ();		// compiles + submits the poly header if state changed

	ax0 = x0 * c_sx + c_tx;  ay0 = y0 * c_sy + c_ty;
	ax1 = x1 * c_sx + c_tx;  ay1 = y1 * c_sy + c_ty;

	z = draw2d_z;
	draw2d_z += 1.0f / 65536.0f;

	// Fire the 4 strip verts (TL, TR, BL, BR) straight into the TA via the store
	// queues: pvr_dr_target hands back a 32-byte-aligned slot (== sizeof(pvr_vertex_t)),
	// write it, pvr_dr_commit flushes the SQ. No init/finish -- deprecated no-ops now.
	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX;     vp->x = ax0; vp->y = ay0; vp->z = z;
	vp->u = s0; vp->v = t0; vp->argb = argb; vp->oargb = 0;
	pvr_dr_commit (vp);

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX;     vp->x = ax1; vp->y = ay0; vp->z = z;
	vp->u = s1; vp->v = t0; vp->argb = argb; vp->oargb = 0;
	pvr_dr_commit (vp);

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX;     vp->x = ax0; vp->y = ay1; vp->z = z;
	vp->u = s0; vp->v = t1; vp->argb = argb; vp->oargb = 0;
	pvr_dr_commit (vp);

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX_EOL; vp->x = ax1; vp->y = ay1; vp->z = z;
	vp->u = s1; vp->v = t1; vp->argb = argb; vp->oargb = 0;
	pvr_dr_commit (vp);
}

//==============================================================================
//
//  PIC CACHING  (renderer-agnostic; verbatim from gl_draw.c)
//
//==============================================================================

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[32];	// for appended glpic
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

byte		menuplyr_pixels[4096];

#define	MAX_SCRAPS		2
#define	BLOCK_WIDTH		256
#define	BLOCK_HEIGHT	256

int			scrap_allocated[MAX_SCRAPS][BLOCK_WIDTH];
byte		scrap_texels[MAX_SCRAPS][BLOCK_WIDTH*BLOCK_HEIGHT];
qboolean	scrap_dirty;
gltexture_t	*scrap_textures[MAX_SCRAPS]; //johnfitz

/*
================
Scrap_AllocBlock
================
*/
int Scrap_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	for (texnum=0 ; texnum<MAX_SCRAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (scrap_allocated[texnum][i+j] >= best)
					break;
				if (scrap_allocated[texnum][i+j] > best2)
					best2 = scrap_allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			scrap_allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("Scrap_AllocBlock: full");
	return 0;
}

/*
================
Scrap_Upload -- johnfitz -- now uses TexMgr
================
*/
void Scrap_Upload (void)
{
	char name[8];
	int	i;

	for (i=0; i<MAX_SCRAPS; i++)
	{
		sprintf (name, "scrap%i", i);
		scrap_textures[i] = TexMgr_LoadImage (NULL, name, BLOCK_WIDTH, BLOCK_HEIGHT, SRC_INDEXED, scrap_texels[i],
			"", (src_offset_t)scrap_texels[i], TEXPREF_ALPHA | TEXPREF_OVERWRITE | TEXPREF_NOPICMIP);
	}

	scrap_dirty = false;
}

/*
================
Draw_PicFromWad
================
*/
qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t	*p;
	glpic_t	gl;
	src_offset_t offset; //johnfitz

	p = (qpic_t *) W_GetLumpName (name);
	if (!p) return pic_nul; //johnfitz

	// load little ones into the scrap
	if (p->width < 64 && p->height < 64)
	{
		int		x, y;
		int		i, j, k;
		int		texnum;

		texnum = Scrap_AllocBlock (p->width, p->height, &x, &y);
		scrap_dirty = true;
		k = 0;
		for (i=0 ; i<p->height ; i++)
		{
			for (j=0 ; j<p->width ; j++, k++)
				scrap_texels[texnum][(y+i)*BLOCK_WIDTH + x + j] = p->data[k];
		}
		gl.gltexture = scrap_textures[texnum]; //johnfitz -- changed to an array
		gl.sl = x/(float)BLOCK_WIDTH;
		gl.sh = (x+p->width)/(float)BLOCK_WIDTH;
		gl.tl = y/(float)BLOCK_WIDTH;
		gl.th = (y+p->height)/(float)BLOCK_WIDTH;
	}
	else
	{
		char texturename[64]; //johnfitz
		q_snprintf (texturename, sizeof(texturename), "%s:%s", WADFILENAME, name); //johnfitz

		offset = (src_offset_t)p - (src_offset_t)wad_base + sizeof(int)*2; //johnfitz

		gl.gltexture = TexMgr_LoadImage (NULL, texturename, p->width, p->height, SRC_INDEXED, p->data, WADFILENAME,
										  offset, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP); //johnfitz -- TexMgr
		gl.sl = 0;
		gl.sh = (float)p->width/(float)TexMgr_PadConditional(p->width); //johnfitz
		gl.tl = 0;
		gl.th = (float)p->height/(float)TexMgr_PadConditional(p->height); //johnfitz
	}

	memcpy (p->data, &gl, sizeof(glpic_t));

	return p;
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (const char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	glpic_t		gl;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
	{
		if (!strcmp (path, pic->name))
			return &pic->pic;
	}
	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	menu_numcachepics++;
	strcpy (pic->name, path);

	dat = (qpic_t *)COM_LoadTempFile (path, NULL);
	if (!dat)
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	SwapPic (dat);

	// HACK -- keep the bytes for the translatable player picture (menu color config)
	if (!strcmp (path, "gfx/menuplyr.lmp"))
		memcpy (menuplyr_pixels, dat->data, dat->width*dat->height);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl.gltexture = TexMgr_LoadImage (NULL, path, dat->width, dat->height, SRC_INDEXED, dat->data, path,
									  sizeof(int)*2, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP); //johnfitz -- TexMgr
	gl.sl = 0;
	gl.sh = (float)dat->width/(float)TexMgr_PadConditional(dat->width); //johnfitz
	gl.tl = 0;
	gl.th = (float)dat->height/(float)TexMgr_PadConditional(dat->height); //johnfitz
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

/*
================
Draw_MakePic -- johnfitz -- generate pics from internal data
================
*/
qpic_t *Draw_MakePic (const char *name, int width, int height, byte *data)
{
	int flags = TEXPREF_NEAREST | TEXPREF_ALPHA | TEXPREF_PERSIST | TEXPREF_NOPICMIP | TEXPREF_PAD;
	qpic_t		*pic;
	glpic_t		gl;

	pic = (qpic_t *) Hunk_Alloc (sizeof(qpic_t) - 4 + sizeof (glpic_t));
	pic->width = width;
	pic->height = height;

	gl.gltexture = TexMgr_LoadImage (NULL, name, width, height, SRC_INDEXED, data, "", (src_offset_t)data, flags);
	gl.sl = 0;
	gl.sh = (float)width/(float)TexMgr_PadConditional(width);
	gl.tl = 0;
	gl.th = (float)height/(float)TexMgr_PadConditional(height);
	memcpy (pic->data, &gl, sizeof(glpic_t));

	return pic;
}

//==============================================================================
//
//  INIT  (renderer-agnostic; verbatim)
//
//==============================================================================

/*
===============
Draw_LoadPics -- johnfitz
===============
*/
void Draw_LoadPics (void)
{
	byte		*data;
	src_offset_t	offset;

	data = (byte *) W_GetLumpName ("conchars");
	if (!data) Sys_Error ("Draw_LoadPics: couldn't load conchars");
	offset = (src_offset_t)data - (src_offset_t)wad_base;
	char_texture = TexMgr_LoadImage (NULL, WADFILENAME":conchars", 128, 128, SRC_INDEXED, data,
		WADFILENAME, offset, TEXPREF_ALPHA | TEXPREF_NEAREST | TEXPREF_NOPICMIP | TEXPREF_CONCHARS);

	draw_disc = Draw_PicFromWad ("disc");
	draw_backtile = Draw_PicFromWad ("backtile");
}

/*
===============
Draw_NewGame -- johnfitz
===============
*/
void Draw_NewGame (void)
{
	cachepic_t	*pic;
	int			i;

	// empty scrap and reallocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty gltextures

	// reload wad pics
	W_LoadWadFile ();
	Draw_LoadPics ();
	SCR_LoadPics ();
	Sbar_LoadPics ();

	// empty lmp cache
	for (pic = menu_cachepics, i = 0; i < menu_numcachepics; pic++, i++)
		pic->name[0] = 0;
	menu_numcachepics = 0;
}

/*
===============
Draw_Init -- johnfitz -- rewritten
===============
*/
void Draw_Init (void)
{
	Cvar_RegisterVariable (&scr_conalpha);

	// clear scrap and allocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty textures

	// create internal pics
	pic_ins = Draw_MakePic ("ins", 8, 9, &pic_ins_data[0][0]);
	pic_ovr = Draw_MakePic ("ovr", 8, 8, &pic_ovr_data[0][0]);
	pic_nul = Draw_MakePic ("nul", 8, 8, &pic_nul_data[0][0]);

	// load game pics
	Draw_LoadPics ();
}

//==============================================================================
//
//  2D DRAWING  (PVR-native)
//
//==============================================================================

#define ARGB_WHITE	0xffffffffu

/*
================
Draw_CharacterQuad -- emit one 8x8 conchars glyph
================
*/
static void Draw_CharacterQuad (int x, int y, char num)
{
	int	row = (num >> 4) & 15, col = num & 15;
	float	frow = row * 0.0625f, fcol = col * 0.0625f, size = 0.0625f;

	PVR_EmitQuad (char_texture, GL_MODULATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		      x, y, x + 8, y + 8,
		      fcol, frow, fcol + size, frow + size, ARGB_WHITE);
}

/*
================
Draw_Character
================
*/
void Draw_Character (int x, int y, int num)
{
	if (y <= -8)
		return;			// totally off screen

	num &= 255;
	if (num == 32)
		return;			// space

	Draw_CharacterQuad (x, y, (char) num);
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, const char *str)
{
	if (y <= -8)
		return;			// totally off screen

	while (*str)
	{
		if (*str != 32)
			Draw_CharacterQuad (x, y, *str);
		str++;
		x += 8;
	}
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	glpic_t	*gl;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;

	PVR_EmitQuad (gl->gltexture, GL_MODULATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		      x, y, x + pic->width, y + pic->height,
		      gl->sl, gl->tl, gl->sh, gl->th, ARGB_WHITE);
}

/*
=============
Draw_TransPicTranslate -- player color selection menu
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom)
{
	static int oldtop = -2;
	static int oldbottom = -2;

	if (top != oldtop || bottom != oldbottom)
	{
		glpic_t *p = (glpic_t *)pic->data;
		gltexture_t *glt = p->gltexture;
		oldtop = top;
		oldbottom = bottom;
		TexMgr_ReloadImage (glt, top, bottom);
	}
	Draw_Pic (x, y, pic);
}

/*
================
Draw_ConsoleBackground
================
*/
void Draw_ConsoleBackground (void)
{
	qpic_t *pic;
	float alpha;
	glpic_t *gl;
	uint32_t a;

	pic = Draw_CachePic ("gfx/conback.lmp");
	pic->width = vid.conwidth;
	pic->height = vid.conheight;

	alpha = (con_forcedup) ? 1.0f : scr_conalpha.value;

	GL_SetCanvas (CANVAS_CONSOLE);

	if (alpha <= 0.0f)
		return;

	a = (uint32_t)(CLAMP(0.0f, alpha, 1.0f) * 255.0f);
	gl = (glpic_t *)pic->data;

	PVR_EmitQuad (gl->gltexture, GL_MODULATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		      0, 0, pic->width, pic->height,
		      gl->sl, gl->tl, gl->sh, gl->th, (a << 24) | 0x00ffffff);
}

/*
=============
Draw_TileClear -- repeat a 64x64 tile to fill around a sized-down refresh window
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	glpic_t	*gl = (glpic_t *)draw_backtile->data;

	PVR_EmitQuad (gl->gltexture, GL_REPLACE, GL_ONE, GL_ZERO,
		      x, y, x + w, y + h,
		      x / 64.0f, y / 64.0f, (x + w) / 64.0f, (y + h) / 64.0f, ARGB_WHITE);
}

/*
=============
Draw_Fill -- fill a box with a single palette color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c, float alpha)
{
	byte *pal = (byte *)d_8to24table;
	uint32_t a = (uint32_t)(CLAMP(0.0f, alpha, 1.0f) * 255.0f);
	uint32_t argb = (a << 24) | (pal[c*4] << 16) | (pal[c*4+1] << 8) | pal[c*4+2];

	PVR_EmitQuad (NULL, GL_MODULATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		      x, y, x + w, y + h, 0, 0, 0, 0, argb);
}

/*
================
Draw_FadeScreen
================
*/
void Draw_FadeScreen (void)
{
	GL_SetCanvas (CANVAS_DEFAULT);

	PVR_EmitQuad (NULL, GL_MODULATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		      0, 0, glwidth, glheight, 0, 0, 0, 0, 0x80000000u /* black, 50% */);

	Sbar_Changed ();
}

/*
================
GL_SetCanvas -- johnfitz -- support various canvas types (PVR: affine, no glOrtho)
================
*/
void GL_SetCanvas (canvastype newcanvas)
{
	extern vrect_t scr_vrect;
	float s;
	int lines;

	if (newcanvas == currentcanvas)
		return;

	currentcanvas = newcanvas;

	switch(newcanvas)
	{
	case CANVAS_DEFAULT:
		PVR_SetCanvasXform (0, glwidth, glheight, 0, glx, gly, glwidth, glheight);
		break;
	case CANVAS_CONSOLE:
		lines = vid.conheight - (scr_con_current * vid.conheight / glheight);
		PVR_SetCanvasXform (0, vid.conwidth, vid.conheight + lines, lines, glx, gly, glwidth, glheight);
		break;
	case CANVAS_MENU:
		s = q_min((float)glwidth / 320.0f, (float)glheight / 200.0f);
		s = CLAMP (1.0f, scr_menuscale.value, s);
		PVR_SetCanvasXform (0, 640, 200, 0,
			glx + (glwidth - 320*s) / 2, gly + (glheight - 200*s) / 2, 640*s, 200*s);
		break;
	case CANVAS_SBAR:
		s = CLAMP (1.0f, scr_sbarscale.value, (float)glwidth / 320.0f);
		if (cl.gametype == GAME_DEATHMATCH)
			PVR_SetCanvasXform (0, glwidth / s, 48, 0, glx, gly, glwidth, 48*s);
		else
			PVR_SetCanvasXform (0, 320, 48, 0, glx + (glwidth - 320*s) / 2, gly, 320*s, 48*s);
		break;
	case CANVAS_WARPIMAGE:
		PVR_SetCanvasXform (0, 128, 0, 128, glx, gly+glheight-gl_warpimagesize, gl_warpimagesize, gl_warpimagesize);
		break;
	case CANVAS_CROSSHAIR: //0,0 is center of viewport
		s = CLAMP (1.0f, scr_crosshairscale.value, 10.0f);
		PVR_SetCanvasXform (scr_vrect.width/-2/s, scr_vrect.width/2/s, scr_vrect.height/2/s, scr_vrect.height/-2/s,
			scr_vrect.x, glheight - scr_vrect.y - scr_vrect.height, scr_vrect.width & ~1, scr_vrect.height & ~1);
		break;
	case CANVAS_BOTTOMLEFT: //used by devstats
		s = (float)glwidth/vid.conwidth; //use console scale
		PVR_SetCanvasXform (0, 320, 200, 0, glx, gly, 320*s, 200*s);
		break;
	case CANVAS_BOTTOMRIGHT: //used by fps/clock
		s = (float)glwidth/vid.conwidth; //use console scale
		PVR_SetCanvasXform (0, 320, 200, 0, glx+glwidth-320*s, gly, 320*s, 200*s);
		break;
	case CANVAS_TOPRIGHT: //used by disc
		s = 1;
		PVR_SetCanvasXform (0, 320, 200, 0, glx+glwidth-320*s, gly+glheight-200*s, 320*s, 200*s);
		break;
	default:
		Sys_Error ("GL_SetCanvas: bad canvas type");
	}
}

/*
================
GL_Set2D -- begin the 2D pass
================
*/
void GL_Set2D (void)
{
	currentcanvas = CANVAS_INVALID;
	GL_SetCanvas (CANVAS_DEFAULT);
	draw2d_z = 1.0f;		// restart painter-order depth for this frame's 2D
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
