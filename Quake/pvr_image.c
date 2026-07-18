/*
================================================================================
pvr_image.c -- texture upload to VRAM for the PVR renderer (maximqad)

Converts QuakeSpasm's processed 32-bit RGBA (or already-565 lightmap) source into
a PVR-native texture in VRAM and records the pvr_ptr_t + PVR_TXRFMT_* on the
gltexture_t. Replaces the glTexImage2D path that gl_texmgr.c used; here the GL
"name" is meaningless and the VRAM pointer/format is what the context cache samples.

Non-twiddled linear textures (like ref_pvr's default path): the PVR samples
rectangular non-twiddled textures directly, so we skip twiddle math entirely and
just pack + pvr_txr_load. TexMgr pads every texture to a power of two >= 8, so
w*h*2 is always a multiple of 32 (8*8*2 = 128) and needs no chunk padding.

Fill status: STEP 2. Real 565/4444 pack + VRAM upload. Twiddle/VQ/mip can be added
later if bandwidth or filtering quality needs it.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

// The texture the render path should sample next (GL_Bind -> PVR_BindTexture).
struct gltexture_s *pvr_bound_texture = NULL;

//------------------------------------------------------------------------------
// Palette RAM
//
// One global palette format (ARGB1555: 5-5-5 color + 1-bit alpha) so filtering
// stays full-speed -- ARGB8888 entries halve paletted fill rate when filtered.
// 1-bit alpha is exactly what Quake needs: index 255 is the transparent color, so
// its entry gets alpha 0 (discarded in the punch-through list for fence textures),
// every other index gets alpha 1. Four 256-entry banks hold the palette variants.
//------------------------------------------------------------------------------
void PVR_PaletteInit (void)
{
	pvr_set_pal_format (PVR_PAL_ARGB1555);
}

void PVR_PaletteSetBank (int bank, const unsigned int *rgba256)
{
	const byte	*src = (const byte *)rgba256;
	uint32_t	base = (uint32_t)bank * 256;
	int		i;

	for (i = 0; i < 256; i++, src += 4)
	{
		uint32_t a = (src[3] >= 128) ? 1u : 0u;
		uint32_t r = src[0] >> 3;
		uint32_t g = src[1] >> 3;
		uint32_t b = src[2] >> 3;
		pvr_set_pal_entry (base + i, (a << 15) | (r << 10) | (g << 5) | b);
	}
}

//------------------------------------------------------------------------------
// Pixel packing (RGBA8888 -> PVR 16bpp)
//------------------------------------------------------------------------------
static void PVR_PackRGB565 (const byte *src, uint16_t *dst, size_t pixels)
{
	size_t i;
	for (i = 0; i < pixels; i++, src += 4)
	{
		uint16_t r = src[0] >> 3;
		uint16_t g = src[1] >> 2;
		uint16_t b = src[2] >> 3;
		dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
	}
}

static void PVR_PackARGB4444 (const byte *src, uint16_t *dst, size_t pixels)
{
	size_t i;
	for (i = 0; i < pixels; i++, src += 4)
	{
		uint16_t r = src[0] >> 4;
		uint16_t g = src[1] >> 4;
		uint16_t b = src[2] >> 4;
		uint16_t a = src[3] >> 4;
		dst[i] = (uint16_t)((a << 12) | (r << 8) | (g << 4) | b);
	}
}

//------------------------------------------------------------------------------
// VRAM upload of a finished 16bpp buffer
//------------------------------------------------------------------------------
static void PVR_StoreTexture (struct gltexture_s *glt, const uint16_t *pix,
			      int w, int h, uint32_t fmt)
{
	size_t	size = (size_t)w * (size_t)h * 2;
	void	*vram;

	// re-upload: release the old VRAM first
	if (glt->pvr_vram)
	{
		PVR_VramFree (glt->pvr_vram);
		glt->pvr_vram = NULL;
	}

	vram = PVR_VramAlloc ((unsigned)size);
	if (!vram)
	{
		Con_Printf ("PVR_StoreTexture: out of VRAM for %s (%dx%d)\n", glt->name, w, h);
		glt->pvr_fmt = 0;
		return;
	}

	pvr_txr_load (pix, (pvr_ptr_t)vram, size);
	glt->pvr_vram = vram;
	glt->pvr_fmt  = fmt;
}

//------------------------------------------------------------------------------
// Public entry points
//------------------------------------------------------------------------------
void PVR_UploadTextureIndexed (struct gltexture_s *glt, const void *indices, int w, int h, int palbank)
{
	size_t	size = (size_t)w * (size_t)h;	// 8bpp: one byte per texel
	void	*vram;

	if (glt->pvr_vram)
	{
		PVR_VramFree (glt->pvr_vram);
		glt->pvr_vram = NULL;
	}

	vram = PVR_VramAlloc ((unsigned)size);
	if (!vram)
	{
		Con_Printf ("PVR_UploadTextureIndexed: out of VRAM for %s (%dx%d)\n", glt->name, w, h);
		glt->pvr_fmt = 0;
		return;
	}

	// pvr_txr_load_ex twiddles as it uploads; paletted textures are always twiddled
	// (the bank-select bits overlap the twiddle bit, so no NONTWIDDLED flag exists).
	pvr_txr_load_ex (indices, (pvr_ptr_t)vram, w, h, PVR_TXRLOAD_8BPP);

	glt->pvr_vram = vram;
	glt->pvr_fmt  = PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL (palbank);
}

void PVR_UploadTexture (struct gltexture_s *glt, const void *rgba, int w, int h, unsigned flags)
{
	size_t		pixels = (size_t)w * (size_t)h;
	uint16_t	*buf;
	uint32_t	fmt;

	buf = (uint16_t *) malloc (pixels * 2);
	if (!buf)
	{
		Con_Printf ("PVR_UploadTexture: no RAM to pack %s\n", glt->name);
		return;
	}

	if (flags & TEXPREF_ALPHA)
	{
		PVR_PackARGB4444 ((const byte *)rgba, buf, pixels);
		fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
	}
	else
	{
		PVR_PackRGB565 ((const byte *)rgba, buf, pixels);
		fmt = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED;
	}

	PVR_StoreTexture (glt, buf, w, h, fmt);
	free (buf);
}

void PVR_UploadTexture565 (struct gltexture_s *glt, const void *rgb565, int w, int h)
{
	PVR_StoreTexture (glt, (const uint16_t *)rgb565, w, h,
			  PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED);
}

void PVR_FreeTexture (struct gltexture_s *glt)
{
	if (glt->pvr_vram)
	{
		PVR_VramFree (glt->pvr_vram);
		glt->pvr_vram = NULL;
	}
	glt->pvr_fmt = 0;
	if (pvr_bound_texture == glt)
		pvr_bound_texture = NULL;
}

void PVR_BindTexture (struct gltexture_s *glt)
{
	pvr_bound_texture = glt;
	// The context cache (pvr_context.c) reads pvr_bound_texture->pvr_vram/pvr_fmt
	// when it compiles the poly header for the next batch.
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
