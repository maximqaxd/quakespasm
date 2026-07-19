/*
================================================================================
pvr_texmgr.c -- PVR-native texture manager for the native PVR renderer

Replaces gl_texmgr.c when USE_PVR_RENDER is set (the Makefile swaps the object).

It keeps all of QuakeSpasm's renderer-agnostic image work verbatim -- palette
build, indexed->RGBA expansion, resample/mipmap/pad, colormap (shirt/pants)
reloading, the active/free gltexture_t bookkeeping -- and swaps only the low-level
upload/bind/delete for the PVR: uploads go to VRAM via PVR_UploadTexture
(pvr_image.c) and GL_Bind records the bound texture for the context cache instead
of calling glBindTexture.

A gltexture_t on DC carries a VRAM pointer (pvr_vram) + PVR_TXRFMT_* (pvr_fmt); the
GL "texnum" is kept only as a cheap identity for GL_Bind's redundant-bind filter.
================================================================================
*/
#include "pvr_local.h"		// dc/pvr.h before quakedef.h (HZ clash) + gltexture_t

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

static cvar_t	gl_texturemode = {"gl_texturemode", "", CVAR_ARCHIVE};
static cvar_t	gl_texture_anisotropy = {"gl_texture_anisotropy", "1", CVAR_ARCHIVE};
static cvar_t	gl_max_size = {"gl_max_size", "0", CVAR_NONE};
static cvar_t	gl_picmip = {"gl_picmip", "0", CVAR_NONE};	// full-res textures on PVR
static cvar_t	gl_picmip_models = {"gl_picmip_models", "1", CVAR_NONE};	// halve alias skins (VRAM)
static GLint	gl_hardware_maxsize = 1024;	// PVR max texture dimension

#define	MAX_GLTEXTURES	1024
static int numgltextures;
static gltexture_t	*active_gltextures, *free_gltextures;
gltexture_t		*notexture, *nulltexture;

static GLuint		texnum_seq = 1;		// running identity for GL_Bind cache (0 = none)

unsigned int d_8to24table[256];
unsigned int d_8to24table_fbright[256];
unsigned int d_8to24table_fbright_fence[256];
unsigned int d_8to24table_nobright[256];
unsigned int d_8to24table_nobright_fence[256];
unsigned int d_8to24table_conchars[256];

// filter-mode table kept only so gl_texturemode cvar parsing still works (the PVR
// samples with a filter chosen at context-compile time from the texture flags).
typedef struct { const char *name; } glmode_t;
static glmode_t glmodes[] = {
	{"GL_NEAREST"}, {"GL_NEAREST_MIPMAP_NEAREST"}, {"GL_NEAREST_MIPMAP_LINEAR"},
	{"GL_LINEAR"},  {"GL_LINEAR_MIPMAP_NEAREST"},  {"GL_LINEAR_MIPMAP_LINEAR"},
};
#define NUM_GLMODES (int)Q_COUNTOF(glmodes)
static int glmode_idx = NUM_GLMODES - 1;

/*
================================================================================
	COMMANDS
================================================================================
*/
static void TexMgr_DescribeTextureModes_f (void)
{
	int i;
	for (i = 0; i < NUM_GLMODES; i++)
		Con_SafePrintf ("   %2i: %s\n", i + 1, glmodes[i].name);
	Con_Printf ("%i modes\n", i);
}

static void TexMgr_TextureMode_f (cvar_t *var)
{
	int i;
	(void)var;

	for (i = 0; i < NUM_GLMODES; i++)
	{
		if (!q_strcasecmp (glmodes[i].name, gl_texturemode.string))
		{
			glmode_idx = i;
			Cvar_SetQuick (&gl_texturemode, glmodes[i].name);
			Sbar_Changed ();
			return;
		}
	}

	i = atoi (gl_texturemode.string);
	if (i >= 1 && i <= NUM_GLMODES)
	{
		glmode_idx = i - 1;
		Cvar_SetQuick (&gl_texturemode, glmodes[i-1].name);
		return;
	}

	Con_Printf ("\"%s\" is not a valid texturemode\n", gl_texturemode.string);
	Cvar_SetQuick (&gl_texturemode, glmodes[glmode_idx].name);
}

static void TexMgr_Anisotropy_f (cvar_t *var)
{
	if (gl_texture_anisotropy.value < 1)
		Cvar_SetQuick (&gl_texture_anisotropy, "1");
	else if (gl_texture_anisotropy.value > gl_max_anisotropy)
		Cvar_SetValueQuick (&gl_texture_anisotropy, gl_max_anisotropy);
}

static void TexMgr_Imagelist_f (void)
{
	float texels = 0;
	gltexture_t	*glt;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		Con_SafePrintf ("   %4i x%4i %s\n", glt->width, glt->height, glt->name);
		texels += (glt->width * glt->height);
	}

	Con_Printf ("%i textures %i pixels %1.1f megabytes\n",
		    numgltextures, (int)texels, texels * 2.0f / 0x100000);
}

/*
===============
TexMgr_FrameUsage
===============
*/
float TexMgr_FrameUsage (void)
{
	float texels = 0;
	gltexture_t	*glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->visframe == r_framecount)
			texels += (glt->width * glt->height);

	return texels * 2.0f / 0x100000;	// 16bpp on PVR
}

/*
================================================================================
	TEXTURE MANAGER
================================================================================
*/
gltexture_t *TexMgr_FindTexture (qmodel_t *owner, const char *name)
{
	gltexture_t	*glt;

	if (name)
		for (glt = active_gltextures; glt; glt = glt->next)
			if (glt->owner == owner && !strcmp (glt->name, name))
				return glt;

	return NULL;
}

gltexture_t *TexMgr_NewTexture (void)
{
	gltexture_t *glt;

	if (numgltextures == MAX_GLTEXTURES)
		Sys_Error ("numgltextures == MAX_GLTEXTURES\n");

	glt = free_gltextures;
	free_gltextures = glt->next;
	glt->next = active_gltextures;
	active_gltextures = glt;

	glt->texnum   = texnum_seq++;
	glt->pvr_vram = NULL;
	glt->pvr_fmt  = 0;
	numgltextures++;
	return glt;
}

static void GL_DeleteTexture (gltexture_t *texture);

//ericw -- workaround for preventing TexMgr_FreeTexture during TexMgr_ReloadImages
static qboolean in_reload_images;

void TexMgr_FreeTexture (gltexture_t *kill)
{
	gltexture_t *glt;

	if (in_reload_images)
		return;

	if (kill == NULL)
	{
		Con_Printf ("TexMgr_FreeTexture: NULL texture\n");
		return;
	}

	if (active_gltextures == kill)
	{
		active_gltextures = kill->next;
		kill->next = free_gltextures;
		free_gltextures = kill;
		GL_DeleteTexture (kill);
		numgltextures--;
		return;
	}

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->next == kill)
		{
			glt->next = kill->next;
			kill->next = free_gltextures;
			free_gltextures = kill;
			GL_DeleteTexture (kill);
			numgltextures--;
			return;
		}
	}

	Con_Printf ("TexMgr_FreeTexture: not found\n");
}

/*
================
TexMgr_FreeTextures -- free where (flags & mask) == (flags & mask)
================
*/
void TexMgr_FreeTextures (unsigned int flags, unsigned int mask)
{
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if ((glt->flags & mask) == (flags & mask))
			TexMgr_FreeTexture (glt);
	}
}

void TexMgr_FreeTexturesForOwner (qmodel_t *owner)
{
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if (glt && glt->owner == owner)
			TexMgr_FreeTexture (glt);
	}
}

void TexMgr_DeleteTextureObjects (void)
{
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		GL_DeleteTexture (glt);
}

/*
================================================================================
	INIT
================================================================================
*/

/*
=================
TexMgr_LoadPalette -- build d_8to24table variants from gfx/palette.lmp
=================
*/
void TexMgr_LoadPalette (void)
{
	byte *pal, *src, *dst;
	int i, mark;
	FILE *f;

	COM_FOpenFile ("gfx/palette.lmp", &f, NULL);
	if (!f)
		Sys_Error ("Couldn't load gfx/palette.lmp");

	mark = Hunk_LowMark ();
	pal = (byte *) Hunk_Alloc (768);
	if (!fread (pal, 768, 1, f))
		Sys_Error ("Failed reading gfx/palette.lmp");
	fclose (f);

	//standard palette, 255 is transparent
	dst = (byte *)d_8to24table;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	((byte *) &d_8to24table[255]) [3] = 0;

	//fullbright palette, 0-223 are black (for additive blending)
	src = pal + 224*3;
	dst = (byte *) &d_8to24table_fbright[224];
	for (i = 224; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 0; i < 224; i++)
	{
		dst = (byte *) &d_8to24table_fbright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

	//nobright palette, 224-255 are black (for additive blending)
	dst = (byte *)d_8to24table_nobright;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 224; i < 256; i++)
	{
		dst = (byte *) &d_8to24table_nobright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

	//fullbright palette, for fence textures
	memcpy (d_8to24table_fbright_fence, d_8to24table_fbright, 256*4);
	d_8to24table_fbright_fence[255] = 0;

	//nobright palette, for fence textures
	memcpy (d_8to24table_nobright_fence, d_8to24table_nobright, 256*4);
	d_8to24table_nobright_fence[255] = 0;

	//conchars palette, 0 and 255 are transparent
	memcpy (d_8to24table_conchars, d_8to24table, 256*4);
	((byte *) &d_8to24table_conchars[0]) [3] = 0;

	Hunk_FreeToLowMark (mark);
}

/*
=================
TexMgr_LoadMiptexPalette -- convert a 24bit color palette to 32bit
=================
*/
static void TexMgr_LoadMiptexPalette (byte *in, byte *out, int numcolors, unsigned flags)
{
	extern cvar_t gl_fullbrights;
	int i, numnobright;

	if (numcolors == 0)
		return;

	if (!(flags & (TEXPREF_FULLBRIGHT | TEXPREF_NOBRIGHT)))
	{
		for (i = 0; i < numcolors; i++)
		{
			*out++ = *in++;
			*out++ = *in++;
			*out++ = *in++;
			*out++ = 255;
		}
	}
	else
	{
		numnobright = q_min (224, numcolors);
		if (flags & TEXPREF_NOBRIGHT)
		{
			if (!gl_fullbrights.value)
				numnobright = numcolors;
			for (i = 0; i < numnobright; i++)
			{
				*out++ = *in++;
				*out++ = *in++;
				*out++ = *in++;
				*out++ = 255;
			}
			for (i = numnobright; i < numcolors; i++)
			{
				*out++ = 0; *out++ = 0; *out++ = 0; *out++ = 255;
			}
		}
		else
		{
			for (i = 0; i < numnobright; i++)
			{
				*out++ = 0; *out++ = 0; *out++ = 0; *out++ = 255;
			}
			in += numnobright * 3;
			for (i = numnobright; i < numcolors; i++)
			{
				*out++ = *in++;
				*out++ = *in++;
				*out++ = *in++;
				*out++ = 255;
			}
		}
	}

	if (flags & TEXPREF_ALPHA)
		out[-1] = 0;
}

/*
================
TexMgr_LoadPvrPalettes -- push Quake's palette variants into the PVR palette banks

Called after TexMgr_LoadPalette rebuilds the d_8to24table* tables. Every 8bpp
indexed texture then samples one of these four banks.
================
*/
static void TexMgr_LoadPvrPalettes (void)
{
	PVR_PaletteInit ();
	PVR_PaletteSetBank (PVR_PALBANK_STD,      d_8to24table);
	PVR_PaletteSetBank (PVR_PALBANK_FBRIGHT,  d_8to24table_fbright);
	PVR_PaletteSetBank (PVR_PALBANK_CONCHARS, d_8to24table_conchars);
	PVR_PaletteSetBank (PVR_PALBANK_NOBRIGHT, d_8to24table_nobright);
}

void TexMgr_NewGame (void)
{
	TexMgr_FreeTextures (0, TEXPREF_PERSIST);
	TexMgr_LoadPalette ();
	TexMgr_LoadPvrPalettes ();
}

/*
=============
TexMgr_RecalcWarpImageSize -- choose warp size and (re)allocate warp textures
=============
*/
void TexMgr_RecalcWarpImageSize (void)
{
	int	mark;
	gltexture_t *glt;
	byte *dummy;

	gl_warpimagesize = TexMgr_SafeTextureSize (512);
	while (gl_warpimagesize > vid.width)
		gl_warpimagesize >>= 1;
	while (gl_warpimagesize > vid.height)
		gl_warpimagesize >>= 1;

	// allocate a blank 565 buffer to seed the warp VRAM textures
	mark = Hunk_LowMark ();
	dummy = (byte *) Hunk_Alloc (gl_warpimagesize * gl_warpimagesize * 2);

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->flags & TEXPREF_WARPIMAGE)
		{
			glt->width = glt->height = gl_warpimagesize;
			PVR_UploadTexture565 (glt, dummy, glt->width, glt->height);
		}
	}

	Hunk_FreeToLowMark (mark);
}

/*
================
TexMgr_Init -- must be called before any texture loading
================
*/
void TexMgr_Init (void)
{
	int i;
	static byte notexture_data[16] = {159,91,83,255,0,0,0,255,0,0,0,255,159,91,83,255};
	static byte nulltexture_data[16] = {127,191,255,255,0,0,0,255,0,0,0,255,127,191,255,255};

	free_gltextures = (gltexture_t *) Hunk_AllocName (MAX_GLTEXTURES * sizeof(gltexture_t), "gltextures");
	active_gltextures = NULL;
	for (i = 0; i < MAX_GLTEXTURES - 1; i++)
		free_gltextures[i].next = &free_gltextures[i+1];
	free_gltextures[i].next = NULL;
	numgltextures = 0;

	TexMgr_LoadPalette ();
	TexMgr_LoadPvrPalettes ();

	Cvar_RegisterVariable (&gl_max_size);
	Cvar_RegisterVariable (&gl_picmip);
	Cvar_RegisterVariable (&gl_picmip_models);
	Cvar_RegisterVariable (&gl_texture_anisotropy);
	Cvar_SetCallback (&gl_texture_anisotropy, &TexMgr_Anisotropy_f);
	gl_texturemode.string = glmodes[glmode_idx].name;
	Cvar_RegisterVariable (&gl_texturemode);
	Cvar_SetCallback (&gl_texturemode, &TexMgr_TextureMode_f);
	Cmd_AddCommand ("gl_describetexturemodes", &TexMgr_DescribeTextureModes_f);
	Cmd_AddCommand ("imagelist", &TexMgr_Imagelist_f);

	gl_hardware_maxsize = 1024;

	notexture  = TexMgr_LoadImage (NULL, "notexture",  2, 2, SRC_RGBA, notexture_data,  "", (src_offset_t)notexture_data,  TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	nulltexture = TexMgr_LoadImage (NULL, "nulltexture", 2, 2, SRC_RGBA, nulltexture_data, "", (src_offset_t)nulltexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);

	r_notexture_mip->gltexture = r_notexture_mip2->gltexture = notexture;

	gl_warpimagesize = 0;
	TexMgr_RecalcWarpImageSize ();
}

/*
================================================================================
	IMAGE LOADING
================================================================================
*/
int TexMgr_Pad (int s)
{
	int i;
	for (i = 1; i < s; i<<=1)
		;
	if (i < 8)
		i = 8;
	return i;
}

int TexMgr_SafeTextureSize (int s)
{
	int p = (int)gl_max_size.value;
	if (!gl_texture_NPOT)
		s = TexMgr_Pad (s);
	if (p > 0)
	{
		p = TexMgr_Pad (p);
		if (p < s) s = p;
	}
	if (s > gl_hardware_maxsize)
		s = gl_hardware_maxsize;
	return s;
}

int TexMgr_PadConditional (int s)
{
	if (s < TexMgr_SafeTextureSize (s))
		return TexMgr_Pad (s);
	return s;
}

static unsigned *TexMgr_MipMapW (unsigned *data, int width, int height)
{
	int	i, size;
	byte	*out, *in;

	out = in = (byte *)data;
	size = (width*height)>>1;

	for (i = 0; i < size; i++, out += 4, in += 8)
	{
		out[0] = (in[0] + in[4])>>1;
		out[1] = (in[1] + in[5])>>1;
		out[2] = (in[2] + in[6])>>1;
		out[3] = (in[3] + in[7])>>1;
	}
	return data;
}

static unsigned *TexMgr_MipMapH (unsigned *data, int width, int height)
{
	int	i, j;
	byte	*out, *in;

	out = in = (byte *)data;
	height>>=1;
	width<<=2;

	for (i = 0; i < height; i++, in += width)
		for (j = 0; j < width; j += 4, out += 4, in += 4)
		{
			out[0] = (in[0] + in[width+0])>>1;
			out[1] = (in[1] + in[width+1])>>1;
			out[2] = (in[2] + in[width+2])>>1;
			out[3] = (in[3] + in[width+3])>>1;
		}
	return data;
}

static unsigned *TexMgr_ResampleTexture (unsigned *in, int inwidth, int inheight, qboolean alpha)
{
	byte *nwpx, *nepx, *swpx, *sepx, *dest;
	unsigned xfrac, yfrac, x, y, modx, mody, imodx, imody, injump, outjump;
	unsigned *out;
	int i, j, outwidth, outheight;

	if (inwidth == TexMgr_Pad(inwidth) && inheight == TexMgr_Pad(inheight))
		return in;

	outwidth = TexMgr_Pad (inwidth);
	outheight = TexMgr_Pad (inheight);
	out = (unsigned *) Hunk_Alloc (outwidth*outheight*4);

	xfrac = ((inwidth-1) << 16) / (outwidth-1);
	yfrac = ((inheight-1) << 16) / (outheight-1);
	y = outjump = 0;

	for (i = 0; i < outheight; i++)
	{
		mody = (y>>8) & 0xFF;
		imody = 256 - mody;
		injump = (y>>16) * inwidth;
		x = 0;

		for (j = 0; j < outwidth; j++)
		{
			modx = (x>>8) & 0xFF;
			imodx = 256 - modx;

			nwpx = (byte *)(in + (x>>16) + injump);
			nepx = nwpx + 4;
			swpx = nwpx + inwidth*4;
			sepx = swpx + 4;

			dest = (byte *)(out + outjump + j);

			dest[0] = (nwpx[0]*imodx*imody + nepx[0]*modx*imody + swpx[0]*imodx*mody + sepx[0]*modx*mody)>>16;
			dest[1] = (nwpx[1]*imodx*imody + nepx[1]*modx*imody + swpx[1]*imodx*mody + sepx[1]*modx*mody)>>16;
			dest[2] = (nwpx[2]*imodx*imody + nepx[2]*modx*imody + swpx[2]*imodx*mody + sepx[2]*modx*mody)>>16;
			if (alpha)
				dest[3] = (nwpx[3]*imodx*imody + nepx[3]*modx*imody + swpx[3]*imodx*mody + sepx[3]*modx*mody)>>16;
			else
				dest[3] = 255;

			x += xfrac;
		}
		outjump += outwidth;
		y += yfrac;
	}

	return out;
}

/*
===============
TexMgr_AlphaEdgeFix -- eliminate pink edges on sprites, etc.
===============
*/
static void TexMgr_AlphaEdgeFix (byte *data, int width, int height)
{
	int	i, j, n = 0, b, c[3] = {0,0,0},
		lastrow, thisrow, nextrow, lastpix, thispix, nextpix;
	byte	*dest = data;

	for (i = 0; i < height; i++)
	{
		lastrow = width * 4 * ((i == 0) ? height-1 : i-1);
		thisrow = width * 4 * i;
		nextrow = width * 4 * ((i == height-1) ? 0 : i+1);

		for (j = 0; j < width; j++, dest += 4)
		{
			if (dest[3])
				continue;

			lastpix = 4 * ((j == 0) ? width-1 : j-1);
			thispix = 4 * j;
			nextpix = 4 * ((j == width-1) ? 0 : j+1);

			b = lastrow + lastpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = thisrow + lastpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = nextrow + lastpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = lastrow + thispix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = nextrow + thispix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = lastrow + nextpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = thisrow + nextpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}
			b = nextrow + nextpix; if (data[b+3]) {c[0] += data[b]; c[1] += data[b+1]; c[2] += data[b+2]; n++;}

			if (n)
			{
				dest[0] = (byte)(c[0]/n);
				dest[1] = (byte)(c[1]/n);
				dest[2] = (byte)(c[2]/n);
				n = c[0] = c[1] = c[2] = 0;
			}
		}
	}
}

static void TexMgr_PadEdgeFixW (byte *data, int width, int height)
{
	byte *src, *dst;
	int i, padw, padh;

	padw = TexMgr_PadConditional (width);
	padh = TexMgr_PadConditional (height);

	src = data + (width - 1) * 4;
	for (i = 0; i < padh; i++)
	{
		src[4] = src[0];
		src[5] = src[1];
		src[6] = src[2];
		src += padw * 4;
	}

	src = data;
	dst = data + (padw - 1) * 4;
	for (i = 0; i < padh; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += padw * 4;
		dst += padw * 4;
	}
}

static void TexMgr_PadEdgeFixH (byte *data, int width, int height)
{
	byte *src, *dst;
	int i, padw, padh;

	padw = TexMgr_PadConditional (width);
	padh = TexMgr_PadConditional (height);

	dst = data + height * padw * 4;
	src = dst - padw * 4;
	for (i = 0; i < padw; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += 4;
		dst += 4;
	}

	dst = data + (padh - 1) * padw * 4;
	src = data;
	for (i = 0; i < padw; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += 4;
		dst += 4;
	}
}

static unsigned *TexMgr_8to32 (byte *in, int pixels, unsigned int *usepal)
{
	int i;
	unsigned *out, *data;

	out = data = (unsigned *) Hunk_Alloc (pixels*4);
	for (i = 0; i < pixels; i++)
		*out++ = usepal[*in++];

	return data;
}

static byte *TexMgr_PadImageW (byte *in, int width, int height, byte padbyte)
{
	int i, j, outwidth;
	byte *out, *data;

	if (width == TexMgr_Pad(width))
		return in;

	outwidth = TexMgr_Pad (width);
	out = data = (byte *) Hunk_Alloc (outwidth*height);

	for (i = 0; i < height; i++)
	{
		for (j = 0; j < width; j++)
			*out++ = *in++;
		for (  ; j < outwidth; j++)
			*out++ = padbyte;
	}
	return data;
}

static byte *TexMgr_PadImageH (byte *in, int width, int height, byte padbyte)
{
	int i, srcpix, dstpix;
	byte *data, *out;

	if (height == TexMgr_Pad(height))
		return in;

	srcpix = width * height;
	dstpix = width * TexMgr_Pad (height);
	out = data = (byte *) Hunk_Alloc (dstpix);

	for (i = 0; i < srcpix; i++)
		*out++ = *in++;
	for (     ; i < dstpix; i++)
		*out++ = padbyte;

	return data;
}

/*
================
TexMgr_PicmipFor -- picmip level for a texture

gl_picmip applies to everything (0 by default on DC -- full res). Alias model
skins additionally honor gl_picmip_models so the many large monster/item skins
can be halved for VRAM without touching world/HUD resolution. NOPICMIP always
wins (full size).
================
*/
static int TexMgr_PicmipFor (gltexture_t *glt)
{
	int	p;

	if (glt->flags & TEXPREF_NOPICMIP)
		return 0;
	p = q_max ((int)gl_picmip.value, 0);
	if (glt->flags & TEXPREF_MDLSKIN)
		p = q_max (p, q_max ((int)gl_picmip_models.value, 0));
	return p;
}

/*
================
TexMgr_WantMipmap -- eligible for a PVR hardware mip chain?

PVR mipmaps need a SQUARE power-of-two texture; we only mipmap those (non-square
world textures stay single-level to keep VRAM in budget). Such textures take the
twiddled 565/4444 mip path instead of the 8bpp paletted / non-twiddled upload.
================
*/
static qboolean TexMgr_WantMipmap (gltexture_t *glt)
{
	return (glt->flags & TEXPREF_MIPMAP)
	    && glt->width == glt->height
	    && (glt->width & (glt->width - 1)) == 0
	    && glt->width >= 8;
}

/*
================
TexMgr_LoadImage32 -- process 32bit RGBA then upload to VRAM
================
*/
static void TexMgr_LoadImage32 (gltexture_t *glt, unsigned *data)
{
	int mipwidth, mipheight, picmip;

	if (!gl_texture_NPOT)
	{
		data = TexMgr_ResampleTexture (data, glt->width, glt->height, glt->flags & TEXPREF_ALPHA);
		glt->width = TexMgr_Pad (glt->width);
		glt->height = TexMgr_Pad (glt->height);
	}

	// mipmap down to the picmip'd / hardware-safe size
	picmip = TexMgr_PicmipFor (glt);
	mipwidth = TexMgr_SafeTextureSize (glt->width >> picmip);
	mipheight = TexMgr_SafeTextureSize (glt->height >> picmip);
	while ((int) glt->width > mipwidth)
	{
		TexMgr_MipMapW (data, glt->width, glt->height);
		glt->width >>= 1;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix ((byte *)data, glt->width, glt->height);
	}
	while ((int) glt->height > mipheight)
	{
		TexMgr_MipMapH (data, glt->width, glt->height);
		glt->height >>= 1;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix ((byte *)data, glt->width, glt->height);
	}

	// upload to VRAM: square-POT mipmapped -> twiddled 565/4444 mip chain;
	// otherwise a single non-twiddled level (RGB565 opaque / ARGB4444 alpha)
	if (TexMgr_WantMipmap (glt))
		PVR_UploadTextureMipmap (glt, data, glt->width, glt->height, glt->flags);
	else
		PVR_UploadTexture (glt, data, glt->width, glt->height, glt->flags);
}

/*
================
TexMgr_ShrinkW8 / TexMgr_ShrinkH8 -- nearest 2:1 decimation of 8bpp index data

The 32-bit pipeline downsamples with bilinear averaging; index data can't be
averaged (the result wouldn't be a valid palette index), so the paletted path
decimates by point sampling -- which matches classic Quake's point-sampled look
anyway. In place; each output row stays behind the input it reads.
================
*/
static void TexMgr_ShrinkW8 (byte *data, int w, int h)
{
	int x, y, hw = w >> 1;
	byte *out = data;
	for (y = 0; y < h; y++)
	{
		const byte *in = data + y * w;
		for (x = 0; x < hw; x++)
			*out++ = in[x << 1];
	}
}

static void TexMgr_ShrinkH8 (byte *data, int w, int h)
{
	int y, hh = h >> 1;
	byte *out = data;
	for (y = 0; y < hh; y++, out += w)
		memcpy (out, data + (y << 1) * w, w);
}

/*
================
TexMgr_LoadImageIndexed -- upload 8bpp indices straight to VRAM (half the RAM)

For power-of-two indexed textures that use one of the shared palette banks. Picmip
is applied by nearest decimation in index space; no 32-bit expansion, no resample.
================
*/
static void TexMgr_LoadImageIndexed (gltexture_t *glt, byte *data, int palbank)
{
	int picmip, mipwidth, mipheight;

	picmip = TexMgr_PicmipFor (glt);
	mipwidth  = TexMgr_SafeTextureSize (glt->width >> picmip);
	mipheight = TexMgr_SafeTextureSize (glt->height >> picmip);

	while ((int) glt->width > mipwidth)
	{
		TexMgr_ShrinkW8 (data, glt->width, glt->height);
		glt->width >>= 1;
	}
	while ((int) glt->height > mipheight)
	{
		TexMgr_ShrinkH8 (data, glt->width, glt->height);
		glt->height >>= 1;
	}

	PVR_UploadTextureIndexed (glt, data, glt->width, glt->height, palbank);
}

/*
================
TexMgr_LoadImage8 -- expand indexed source to RGBA, then LoadImage32
================
*/
static void TexMgr_LoadImage8 (gltexture_t *glt, byte *data, unsigned int *usepal)
{
	extern cvar_t gl_fullbrights;
	qboolean global_pal = (usepal == NULL);	// NULL => Quake's shared palette (bankable)
	qboolean padw = false, padh = false;
	byte padbyte = 0;
	int i;

	// HACK -- b_shell1.bsp: white pixels in "shot1sid" look ugly in non-software
	if (strstr(glt->name, "shot1sid") &&
	    glt->width == 32 && glt->height == 32 &&
	    CRC_Block(data, 1024) == 65393)
		memcpy (data, data + 32*31, 32);

	// detect false alpha cases
	if (glt->flags & TEXPREF_ALPHA && !(glt->flags & TEXPREF_CONCHARS))
	{
		for (i = 0; i < (int) (glt->width * glt->height); i++)
			if (data[i] == 255)
				break;
		if (i == (int) (glt->width * glt->height))
			glt->flags -= TEXPREF_ALPHA;
	}

	// Shared-palette + already power-of-two => upload as 8bpp paletted (half the
	// VRAM of 16bpp). Transparency comes from the palette (index 255 -> alpha 0);
	// the render side still routes TEXPREF_ALPHA surfaces to the punch-through list.
	// Non-POT / padded pics fall through to the 32-bit path below (they're small).
	// Square-POT mipmapped textures also fall through -- their mip chain is 565/4444
	// twiddled (paletted mips can't be box-filtered), handled in TexMgr_LoadImage32.
	if (global_pal && !TexMgr_WantMipmap (glt) &&
	    (int) glt->width  == TexMgr_Pad ((int) glt->width) &&
	    (int) glt->height == TexMgr_Pad ((int) glt->height))
	{
		int bank;
		if (glt->flags & TEXPREF_FULLBRIGHT)
			bank = PVR_PALBANK_FBRIGHT;
		else if ((glt->flags & TEXPREF_NOBRIGHT) && gl_fullbrights.value)
			bank = PVR_PALBANK_NOBRIGHT;
		else if (glt->flags & TEXPREF_CONCHARS)
			bank = PVR_PALBANK_CONCHARS;
		else
			bank = PVR_PALBANK_STD;

		TexMgr_LoadImageIndexed (glt, data, bank);
		return;
	}

	// choose palette and padbyte
	if (!usepal)
	{
		if (glt->flags & TEXPREF_FULLBRIGHT)
		{
			usepal = (glt->flags & TEXPREF_ALPHA) ? d_8to24table_fbright_fence : d_8to24table_fbright;
			padbyte = 0;
		}
		else if (glt->flags & TEXPREF_NOBRIGHT && gl_fullbrights.value)
		{
			usepal = (glt->flags & TEXPREF_ALPHA) ? d_8to24table_nobright_fence : d_8to24table_nobright;
			padbyte = 0;
		}
		else if (glt->flags & TEXPREF_CONCHARS)
		{
			usepal = d_8to24table_conchars;
			padbyte = 0;
		}
		else
		{
			usepal = d_8to24table;
			padbyte = 255;
		}
	}

	// pad each dimension, but only if it's not going to be downsampled later
	if (glt->flags & TEXPREF_PAD)
	{
		if ((int) glt->width < TexMgr_SafeTextureSize(glt->width))
		{
			data = TexMgr_PadImageW (data, glt->width, glt->height, padbyte);
			glt->width = TexMgr_Pad (glt->width);
			padw = true;
		}
		if ((int) glt->height < TexMgr_SafeTextureSize(glt->height))
		{
			data = TexMgr_PadImageH (data, glt->width, glt->height, padbyte);
			glt->height = TexMgr_Pad (glt->height);
			padh = true;
		}
	}

	// convert to 32bit
	data = (byte *)TexMgr_8to32 (data, glt->width * glt->height, usepal);

	// fix edges
	if (glt->flags & TEXPREF_ALPHA)
		TexMgr_AlphaEdgeFix (data, glt->width, glt->height);
	else
	{
		if (padw)
			TexMgr_PadEdgeFixW (data, glt->source_width, glt->source_height);
		if (padh)
			TexMgr_PadEdgeFixH (data, glt->source_width, glt->source_height);
	}

	TexMgr_LoadImage32 (glt, (unsigned *)data);
}

/*
================
TexMgr_LoadLightmap -- lightmap is already RGB565 on DC; upload straight to VRAM
================
*/
static void TexMgr_LoadLightmap (gltexture_t *glt, byte *data)
{
	PVR_UploadTexture565 (glt, data, glt->width, glt->height);
}

/*
================
TexMgr_LoadImage8Valve -- indexed source with an embedded palette (Half-Life wad)
================
*/
static void TexMgr_LoadImage8Valve (gltexture_t *glt, byte *data)
{
	byte *in, *usepal;
	unsigned short colors;

	in = data + glt->source_width * glt->source_height;
	memcpy (&colors, in, 2);
	colors = LittleShort (colors);

	usepal = (byte *)Hunk_Alloc (colors * 4);
	TexMgr_LoadMiptexPalette (in + 2, usepal, colors, glt->flags);
	TexMgr_LoadImage8 (glt, data, (unsigned *)usepal);
}

/*
================
TexMgr_LoadImage -- the one entry point for loading all textures
================
*/
gltexture_t *TexMgr_LoadImage (qmodel_t *owner, const char *name, int width, int height, enum srcformat format,
			       byte *data, const char *source_file, src_offset_t source_offset, unsigned flags)
{
	unsigned short crc;
	gltexture_t *glt;
	int mark;

	if (isDedicated)
		return NULL;

	// cache check
	switch (format)
	{
	case SRC_INDEXED:
	case SRC_INDEXED_PALETTE:
		crc = CRC_Block (data, width * height);
		break;
	case SRC_LIGHTMAP:
		crc = CRC_Block (data, width * height * lightmap_bytes);
		break;
	case SRC_RGBA:
		crc = CRC_Block (data, width * height * 4);
		break;
	default:
		crc = 0;
	}
	if ((flags & TEXPREF_OVERWRITE) && (glt = TexMgr_FindTexture (owner, name)))
	{
		if (glt->source_crc == crc)
			return glt;
	}
	else
		glt = TexMgr_NewTexture ();

	// copy data
	glt->owner = owner;
	q_strlcpy (glt->name, name, sizeof(glt->name));
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->flags &= ~TEXPREF_MIPMAP;		// no mip chain on the PVR path
	glt->shirt = -1;
	glt->pants = -1;
	q_strlcpy (glt->source_file, source_file, sizeof(glt->source_file));
	glt->source_offset = source_offset;
	glt->source_format = format;
	glt->source_width = width;
	glt->source_height = height;
	glt->source_crc = crc;

	// upload it
	mark = Hunk_LowMark ();

	switch (glt->source_format)
	{
	case SRC_INDEXED:		TexMgr_LoadImage8 (glt, data, NULL); break;
	case SRC_LIGHTMAP:		TexMgr_LoadLightmap (glt, data); break;
	case SRC_RGBA:			TexMgr_LoadImage32 (glt, (unsigned *)data); break;
	case SRC_INDEXED_PALETTE:	TexMgr_LoadImage8Valve (glt, data); break;
	}

	Hunk_FreeToLowMark (mark);

	return glt;
}

/*
================================================================================
	COLORMAPPING AND TEXTURE RELOADING
================================================================================
*/
void TexMgr_ReloadImage (gltexture_t *glt, int shirt, int pants)
{
	byte	translation[256];
	byte	*src, *dst, *data = NULL, *translated;
	int	mark, size, i;

	mark = Hunk_LowMark ();

	if (glt->source_file[0] && glt->source_offset)
	{
		FILE *f;
		int sz;
		COM_FOpenFile (glt->source_file, &f, NULL);
		if (!f) goto invalid;
		fseek (f, glt->source_offset, SEEK_CUR);
		size = glt->source_width * glt->source_height;
		if (glt->source_format == SRC_RGBA)
			size *= 4;
		else if (glt->source_format == SRC_LIGHTMAP)
			size *= lightmap_bytes;
		data = (byte *) Hunk_Alloc (size);
		sz = (int) fread (data, 1, size, f);
		fclose (f);
		if (sz != size)
		{
			Hunk_FreeToLowMark (mark);
			Host_Error ("Read error for %s", glt->name);
		}
	}
	else if (glt->source_file[0] && !glt->source_offset)
		data = Image_LoadImage (glt->source_file, (int *)&glt->source_width, (int *)&glt->source_height);
	else if (!glt->source_file[0] && glt->source_offset)
		data = (byte *) glt->source_offset;

	if (!data)
	{
invalid:	Con_Printf ("TexMgr_ReloadImage: invalid source for %s\n", glt->name);
		Hunk_FreeToLowMark (mark);
		return;
	}

	glt->width = glt->source_width;
	glt->height = glt->source_height;

	// apply shirt and pants colors
	if (shirt > -1 && pants > -1)
	{
		if (glt->source_format == SRC_INDEXED)
		{
			glt->shirt = shirt;
			glt->pants = pants;
		}
		else
			Con_Printf ("TexMgr_ReloadImage: can't colormap a non SRC_INDEXED texture: %s\n", glt->name);
	}
	if (glt->shirt > -1 && glt->pants > -1)
	{
		for (i = 0; i < 256; i++)
			translation[i] = i;

		shirt = glt->shirt * 16;
		if (shirt < 128)
			for (i = 0; i < 16; i++) translation[TOP_RANGE+i] = shirt + i;
		else
			for (i = 0; i < 16; i++) translation[TOP_RANGE+i] = shirt+15-i;

		pants = glt->pants * 16;
		if (pants < 128)
			for (i = 0; i < 16; i++) translation[BOTTOM_RANGE+i] = pants + i;
		else
			for (i = 0; i < 16; i++) translation[BOTTOM_RANGE+i] = pants+15-i;

		size = glt->width * glt->height;
		dst = translated = (byte *) Hunk_Alloc (size);
		src = data;
		for (i = 0; i < size; i++)
			*dst++ = translation[*src++];
		data = translated;
	}

	switch (glt->source_format)
	{
	case SRC_INDEXED:		TexMgr_LoadImage8 (glt, data, NULL); break;
	case SRC_LIGHTMAP:		TexMgr_LoadLightmap (glt, data); break;
	case SRC_RGBA:			TexMgr_LoadImage32 (glt, (unsigned *)data); break;
	case SRC_INDEXED_PALETTE:	TexMgr_LoadImage8Valve (glt, data); break;
	}

	Hunk_FreeToLowMark (mark);
}

void TexMgr_ReloadImages (void)
{
	gltexture_t *glt;

	in_reload_images = true;
	for (glt = active_gltextures; glt; glt = glt->next)
	{
		glt->texnum = texnum_seq++;
		glt->pvr_vram = NULL;		// old VRAM is gone after a device reset
		TexMgr_ReloadImage (glt, -1, -1);
	}
	in_reload_images = false;
}

void TexMgr_ReloadNobrightImages (void)
{
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->flags & TEXPREF_NOBRIGHT)
			TexMgr_ReloadImage (glt, -1, -1);
}

/*
================================================================================
	TEXTURE BINDING / TEXTURE UNIT SWITCHING
================================================================================
*/
static GLuint	currenttexture[3] = {GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE};
static GLenum	currenttarget = GL_TEXTURE0_ARB;
qboolean	mtexenabled = false;

// The PVR has no multitexture unit switching in this port; these keep the callers
// (r_alias, r_brush) compiling and track the "active unit" for GL_Bind's cache.
void GL_SelectTexture (GLenum target)
{
	currenttarget = target;
}

void GL_DisableMultitexture (void)
{
	mtexenabled = false;
	currenttarget = GL_TEXTURE0_ARB;
}

void GL_EnableMultitexture (void)
{
	// single TMU on the PVR path; two-pass lightmapping submits separate lists.
}

/*
================
GL_Bind -- record the texture the next batch should sample
================
*/
void GL_Bind (gltexture_t *texture)
{
	if (!texture)
		texture = nulltexture;

	if (texture->texnum != currenttexture[currenttarget - GL_TEXTURE0_ARB])
	{
		currenttexture[currenttarget - GL_TEXTURE0_ARB] = texture->texnum;
		texture->visframe = r_framecount;
	}

	PVR_BindTexture (texture);
}

static void GL_DeleteTexture (gltexture_t *texture)
{
	PVR_FreeTexture (texture);

	if (texture->texnum == currenttexture[0]) currenttexture[0] = GL_UNUSED_TEXTURE;
	if (texture->texnum == currenttexture[1]) currenttexture[1] = GL_UNUSED_TEXTURE;
	if (texture->texnum == currenttexture[2]) currenttexture[2] = GL_UNUSED_TEXTURE;

	texture->texnum = 0;
}

void GL_ClearBindings (void)
{
	int i;
	for (i = 0; i < 3; i++)
		currenttexture[i] = GL_UNUSED_TEXTURE;
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
