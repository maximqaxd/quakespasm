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
	glt->pvr_mipmap = 0;
}

//------------------------------------------------------------------------------
// Square-POT mipmaps (twiddled). PVR hardware mipmaps require a square texture
// whose levels sit at fixed byte offsets in a twiddled blob: level 0 = full NxN
// at offset[N], smaller mips packed before it down to 1x1 at offset 6. We build
// the whole chain in RAM -- box-downsampling in RGBA so mips are averaged, then
// packing each level to 565/4444 and twiddling it -- and upload it in one load.
// The header (pvr_context.c) sets txr.mipmap + BILINEAR when pvr_mipmap is set.
//------------------------------------------------------------------------------

// 16bpp mip-chain byte offset to the level of the given side length (GLdc table).
static unsigned PVR_MipOffset16 (int size)
{
	switch (size)
	{
	case 1024: return 0xAAAB0;
	case 512:  return 0x2AAB0;
	case 256:  return 0x0AAB0;
	case 128:  return 0x02AB0;
	case 64:   return 0x00AB0;
	case 32:   return 0x002B0;
	case 16:   return 0x000B0;
	case 8:    return 0x00030;
	case 4:    return 0x00010;
	case 2:    return 0x00008;
	case 1:    return 0x00006;
	default:   return 0;
	}
}

// spread the low 16 bits so bit i lands at bit 2i (Morton part1by1)
static inline unsigned PVR_TwidSpread (unsigned v)
{
	v &= 0x0000FFFFu;
	v = (v ^ (v << 8)) & 0x00FF00FFu;
	v = (v ^ (v << 4)) & 0x0F0F0F0Fu;
	v = (v ^ (v << 2)) & 0x33333333u;
	v = (v ^ (v << 1)) & 0x55555555u;
	return v;
}
// PVR twiddled linear index for (x,y): y in even bits, x in odd bits
static inline unsigned PVR_TwidIndex (unsigned x, unsigned y)
{
	return PVR_TwidSpread (y) | (PVR_TwidSpread (x) << 1);
}

// box-downsample a square RGBA image to half size
static void PVR_HalveRGBA (const byte *src, byte *dst, int size)
{
	int	hs = size >> 1, x, y, c;

	for (y = 0; y < hs; y++)
	{
		const byte *r0 = src + (size_t)(y * 2)     * size * 4;
		const byte *r1 = src + (size_t)(y * 2 + 1) * size * 4;
		byte	   *o  = dst + (size_t)y * hs * 4;
		for (x = 0; x < hs; x++)
		{
			const byte *a = r0 + (x * 2) * 4, *b = r0 + (x * 2 + 1) * 4;
			const byte *e = r1 + (x * 2) * 4, *f = r1 + (x * 2 + 1) * 4;
			for (c = 0; c < 4; c++)
				o[x * 4 + c] = (byte)((a[c] + b[c] + e[c] + f[c] + 2) >> 2);
		}
	}
}

void PVR_UploadTextureMipmap (struct gltexture_s *glt, const void *rgba, int w, int h, unsigned flags)
{
	int		N = w;			// square: w == h, power of two
	qboolean	alpha = (flags & TEXPREF_ALPHA) != 0;
	unsigned	chain_size = PVR_MipOffset16 (N) + (unsigned)N * N * 2;
	unsigned	chain_pad = (chain_size + 31u) & ~31u;
	byte		*chain, *lvl, *nxt;
	int		size;
	void		*vram;

	chain = (byte *) calloc (1, chain_pad);
	lvl   = (byte *) malloc ((size_t)N * N * 4);	// current level, RGBA
	nxt   = (byte *) malloc ((size_t)N * N);	// next (quarter-area) level, RGBA
	if (!chain || !lvl || !nxt)
	{
		Con_Printf ("PVR_UploadTextureMipmap: no RAM for %s (%dx%d)\n", glt->name, w, h);
		free (chain); free (lvl); free (nxt);
		return;
	}

	memcpy (lvl, rgba, (size_t)N * N * 4);

	for (size = N; size >= 1; size >>= 1)
	{
		uint16_t	*dst = (uint16_t *)(chain + PVR_MipOffset16 (size));
		int		x, y;

		for (y = 0; y < size; y++)
			for (x = 0; x < size; x++)
			{
				const byte *p = lvl + ((size_t)y * size + x) * 4;
				uint16_t    px;
				if (alpha)
					px = (uint16_t)(((p[3] >> 4) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
				else
					px = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
				dst[PVR_TwidIndex (x, y)] = px;
			}

		if (size > 1)
		{
			PVR_HalveRGBA (lvl, nxt, size);
			memcpy (lvl, nxt, (size_t)(size >> 1) * (size >> 1) * 4);
		}
	}

	free (lvl);
	free (nxt);

	if (glt->pvr_vram)
	{
		PVR_VramFree (glt->pvr_vram);
		glt->pvr_vram = NULL;
	}
	vram = PVR_VramAlloc (chain_pad);
	if (!vram)
	{
		Con_Printf ("PVR_UploadTextureMipmap: out of VRAM for %s (%dx%d)\n", glt->name, w, h);
		glt->pvr_fmt = 0;
		free (chain);
		return;
	}

	pvr_txr_load (chain, (pvr_ptr_t)vram, chain_pad);
	free (chain);

	glt->pvr_vram   = vram;
	glt->pvr_fmt    = (alpha ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565) | PVR_TXRFMT_TWIDDLED;
	glt->pvr_mipmap = 1;
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
	glt->pvr_mipmap = 0;
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
