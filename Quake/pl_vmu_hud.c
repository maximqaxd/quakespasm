/*
================================================================================
pl_vmu_hud.c -- Dreamcast VMU LCD mini-HUD

Mirrors the in-game HUD onto the 48x32 monochrome LCD of the VMU in controller
port A, laid out like the real HUD: on the left the health-based Quake face and
health number (the "player" half), on the right the current ammo-type icon and
its count (the "ammo" half). A glance-able status display like the memory-card
screens some Dreamcast games used.

Both the face and the ammo icons are the real gfx.wad HUD pics -- exactly the
lumps Sbar_Draw uses (face1..5 for the portrait; sb_shells/sb_nails/sb_rocket/
sb_cells for the ammo type). They're thresholded to 1bpp once at init (before
Sbar_Init uploads and clobbers their pixel data): the face uses an adaptive
mean-luminance cut so its interior features read, the ammo icons use a plain
opaque-silhouette cut so the shape reads solid on the tiny LCD.

The LCD is written over the maple bus we also poll every frame for input, so
updates are rate-capped and only pushed when something changes.
================================================================================
*/
#include "quakedef.h"

#if defined(PLATFORM_DREAMCAST)

#include <dc/maple.h>
#include <dc/vmu_fb.h>

extern unsigned	d_8to24table[256];	// palette RGBA, for luminance thresholding

cvar_t	vmu_hud = {"vmu_hud", "1", CVAR_ARCHIVE};

#define	VMU_HUD_HZ	6.0	// at most this many LCD writes per second
#define	VMU_PAIN_TIME	0.4	// seconds the pain face is shown after damage

// Face and ammo share the same 24x24 footprint, side by side.
#define	ICON_W	24
#define	ICON_H	24
#define	ICON_BYTES	(((ICON_W + 7) / 8) * ICON_H)	// 3 * 24 = 72

// 1bpp icons, tightly packed MSB-first, exactly as vmufb_paint_area reads them.
static uint8_t	ic_face[5][ICON_BYTES];		// by health bracket: [0]=~20hp .. [4]=~100hp
static uint8_t	ic_face_pain[5][ICON_BYTES];	// pain-flash variants
static uint8_t	ic_ammo[4][ICON_BYTES];		// shells, nails, rockets, cells

// Idle-screen Quake "Q" logo, rasterized below: a broken ring with the nail
// forming the tail. 24 wide x 32 tall, 3 bytes/row, centered on the 48px LCD.
#define	LOGO_W	24
#define	LOGO_H	32
static uint8_t		ic_logo[((LOGO_W + 7) / 8) * LOGO_H];	// 3 * 32 = 96

static maple_device_t	*vmu_lcd;
static double		vmu_next;			// earliest time we may write again
static double		pain_time;			// pain face shown until this time
static int		last_hp = -9999, last_ai = -9999, last_am = -9999;
static qboolean		last_pain, showed_idle, icons_ok;

static int VMU_Lum (int idx)
{
	byte	*c = (byte *) &d_8to24table[idx];
	return (c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8;	// Rec.601 luma
}

// Euclidean RGB distance (0..441) between a palette index and the icon's
// background colour. Integer sqrt, no float.
static int VMU_ColorDist (int idx, int bgidx)
{
	byte	*c = (byte *) &d_8to24table[idx];
	byte	*b = (byte *) &d_8to24table[bgidx];
	int	dr = (int)c[0] - b[0], dg = (int)c[1] - b[1], db = (int)c[2] - b[2];
	int	d2 = dr * dr + dg * dg + db * db, r = 0;
	while ((r + 1) * (r + 1) <= d2)
		r++;
	return r;
}

/*
==============
VMU_Icon -- rasterize a gfx.wad pic to a tw x th 1bpp icon (box-sampled, MSB-first).

Two thresholding modes, both derived from the pic's own pixels so they work
whatever the palette:

  - lit_if_dark (the FACE): split on the mean LUMINANCE of the opaque pixels and
    light the cells DARKER than the mean -- the portrait's drawn features
    (eyes/mouth/outline) are its dark parts. Cell decided by its average.

  - !lit_if_dark (the AMMO icons): the shape is often no brighter than the fill
    (e.g. shotgun shells), so luminance can't separate them. Instead measure each
    pixel's colour DISTANCE from the icon's background colour (its most common
    index) and split that with Otsu's method. The shape -- whatever differs in
    colour from the flat fill -- lights; the fill doesn't. Cell decided by its
    MAX distance so thin outlines survive the downsample.

fixed_thresh, if >= 0, overrides with an absolute luminance cut.
==============
*/
static void VMU_Icon (const char *lump, uint8_t *out, int tw, int th, qboolean lit_if_dark, int fixed_thresh)
{
	qpic_t	*p;
	byte	*src;
	int	sw, sh, sx, sy, tx, ty, rowbytes, cut, bgidx = 0;
	qboolean use_dist = (!lit_if_dark && fixed_thresh < 0);

	rowbytes = (tw + 7) / 8;
	memset (out, 0, rowbytes * th);

	p = (qpic_t *) W_GetLumpName (lump);
	if (!p || p->width <= 0 || p->height <= 0)
		return;
	sw = p->width;
	sh = p->height;
	src = p->data;

	if (!use_dist)
	{
		// --- luminance path (face / fixed cut) ---
		long	tot = 0;
		int	cnt = 0, mean;
		for (sy = 0; sy < sh; sy++)
			for (sx = 0; sx < sw; sx++)
			{
				byte idx = src[sy * sw + sx];
				if (idx == 255) continue;
				tot += VMU_Lum (idx);
				cnt++;
			}
		mean = cnt ? (int) (tot / cnt) : 128;
		cut = (fixed_thresh >= 0) ? fixed_thresh : mean;
	}
	else
	{
		// --- colour-distance path (ammo) ---
		int	idxcount[256], hist[442];
		int	t, best = -1, wB = 0, cntd = 0;
		long	totd = 0, sumB = 0;
		double	maxvar = -1.0;

		memset (idxcount, 0, sizeof(idxcount));
		for (sy = 0; sy < sh; sy++)
			for (sx = 0; sx < sw; sx++)
			{
				byte idx = src[sy * sw + sx];
				if (idx == 255) continue;
				idxcount[idx]++;
			}
		for (t = 0; t < 256; t++)		// background = most common opaque index
			if (idxcount[t] > best) { best = idxcount[t]; bgidx = t; }

		memset (hist, 0, sizeof(hist));
		for (sy = 0; sy < sh; sy++)
			for (sx = 0; sx < sw; sx++)
			{
				byte idx = src[sy * sw + sx];
				int  dd;
				if (idx == 255) continue;
				dd = VMU_ColorDist (idx, bgidx);
				hist[dd]++;
				totd += dd;
				cntd++;
			}
		cut = 32;				// sane fallback
		for (t = 0; t < 442; t++)		// Otsu over the distance histogram
		{
			int	wF;
			double	mB, mF, var;
			wB += hist[t];
			if (wB == 0) continue;
			wF = cntd - wB;
			if (wF == 0) break;
			sumB += (long) t * hist[t];
			mB = (double) sumB / wB;
			mF = (double) (totd - sumB) / wF;
			var = (double) wB * wF * (mB - mF) * (mB - mF);
			if (var > maxvar) { maxvar = var; cut = t + 1; }
		}
	}

	for (ty = 0; ty < th; ty++)
	{
		int	sy0 = ty * sh / th, sy1 = (ty + 1) * sh / th;
		if (sy1 <= sy0) sy1 = sy0 + 1;

		for (tx = 0; tx < tw; tx++)
		{
			int	sx0 = tx * sw / tw, sx1 = (tx + 1) * sw / tw;
			int	osum = 0, oc = 0, cell = 0, dmax = 0, avg;
			qboolean lit;
			if (sx1 <= sx0) sx1 = sx0 + 1;

			for (sy = sy0; sy < sy1 && sy < sh; sy++)
				for (sx = sx0; sx < sx1 && sx < sw; sx++)
				{
					byte idx = src[sy * sw + sx];
					cell++;
					if (idx == 255) continue;
					oc++;
					if (use_dist)
					{
						int d = VMU_ColorDist (idx, bgidx);
						if (d > dmax) dmax = d;
					}
					else
						osum += VMU_Lum (idx);
				}

			if (oc * 2 < cell)		// mostly transparent -> leave dark
				continue;
			if (use_dist)
				lit = (dmax >= cut);	// ammo: light on max colour distance
			else
			{
				avg = osum / oc;
				lit = (avg < cut);	// face: light darker-than-mean cells
			}
			if (lit)
				out[ty * rowbytes + (tx >> 3)] |= (0x80 >> (tx & 7));
		}
	}
}

/*
==============
VMU_AmmoIcon -- index into ic_ammo[] for the active ammo type, or -1 (none).
Mirrors Sbar_Draw's selection: the ammo icon is chosen by the STAT_ITEMS ammo
flags (IT_SHELLS/IT_NAILS/IT_ROCKETS/IT_CELLS), not by the weapon.
==============
*/
static int VMU_AmmoIcon (int items)
{
	if (items & IT_SHELLS)		return 0;
	if (items & IT_NAILS)		return 1;
	if (items & IT_ROCKETS)		return 2;
	if (items & IT_CELLS)		return 3;
	return -1;
}

static void VMU_LogoPix (uint8_t *b, int x, int y)
{
	if (x < 0 || x >= LOGO_W || y < 0 || y >= LOGO_H)
		return;
	b[y * 3 + (x >> 3)] |= (0x80 >> (x & 7));
}

/*
==============
VMU_BuildQuakeLogo -- rasterize the Quake "Q": a broken ring with the nail
forming the tail, into a 24x32 1bpp buffer. Integer math (distance squared in
doubled coords) so it stays symmetric with no float/sqrt.
==============
*/
static void VMU_BuildQuakeLogo (uint8_t *out)
{
	int	x, y;

	memset (out, 0, sizeof (ic_logo));

	// broken ring: annulus centered on (11.5, 9), outer r=11 / inner r=8, with a
	// gap opened at the very top (the two curling ends of the Q).
	for (y = 0; y < LOGO_H; y++)
		for (x = 0; x < LOGO_W; x++)
		{
			int	dx = 2 * x - 23;		// 2 * (x - 11.5)
			int	dy = 2 * (y - 9);
			int	d2 = dx * dx + dy * dy;
			if (d2 < 256 || d2 > 484)		// (2*8)^2 .. (2*11)^2
				continue;
			if (y <= 2 && dx > -6 && dx < 6)	// open the top of the ring
				continue;
			VMU_LogoPix (out, x, y);
		}

	// nail head: a horizontal crossbar across the middle of the ring
	for (y = 11; y <= 12; y++)
		for (x = 6; x <= 17; x++)
			VMU_LogoPix (out, x, y);

	// nail shaft: taper from the head down to a point below the ring (the tail)
	for (y = 13; y < LOGO_H; y++)
	{
		int	half = (LOGO_H - 1 - y) / 8;	// ~2 near the head, 0 at the tip
		for (x = 11 - half; x <= 12 + half; x++)
			VMU_LogoPix (out, x, y);
	}
}

void VMU_HUD_Init (void)
{
	Cvar_RegisterVariable (&vmu_hud);
	vmu_lcd = maple_enum_type (0, MAPLE_FUNC_LCD);

	// faces: sbar maps [4]=face1(~100hp) down to [0]=face5(~20hp). Light the dark
	// features so the little portrait reads as a face.
	VMU_Icon ("face1", ic_face[4], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face2", ic_face[3], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face3", ic_face[2], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face4", ic_face[1], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face5", ic_face[0], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face_p1", ic_face_pain[4], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face_p2", ic_face_pain[3], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face_p3", ic_face_pain[2], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face_p4", ic_face_pain[1], ICON_W, ICON_H, true, -1);
	VMU_Icon ("face_p5", ic_face_pain[0], ICON_W, ICON_H, true, -1);

	// ammo: the same lumps Sbar_Draw uses. These are a bright shape on a dark
	// (opaque, non-transparent) background, so split on the mean luminance and
	// light the BRIGHT half -- the shape reads against the dark, like the face
	// but inverted.
	VMU_Icon ("sb_shells", ic_ammo[0], ICON_W, ICON_H, false, -1);
	VMU_Icon ("sb_nails",  ic_ammo[1], ICON_W, ICON_H, false, -1);
	VMU_Icon ("sb_rocket", ic_ammo[2], ICON_W, ICON_H, false, -1);
	VMU_Icon ("sb_cells",  ic_ammo[3], ICON_W, ICON_H, false, -1);

	VMU_BuildQuakeLogo (ic_logo);	// idle-screen Quake "Q" logo
	icons_ok = true;
}

/*
==============
VMU_HUD_Update -- called each frame; rate-capped + dirty-checked internally.

Layout (48x32) -- two 24x24 icons side by side, numbers in the 8px strip below:
    left  (player): face + health
    right (ammo):   ammo-type icon + count
==============
*/
void VMU_HUD_Update (void)
{
	vmufb_t	fb;
	int	hp, ai, am, f, painnow;
	char	num[8];

	if (!vmu_hud.value || !icons_ok)
		return;
	if (!vmu_lcd)
	{
		vmu_lcd = maple_enum_type (0, MAPLE_FUNC_LCD);	// allow hot-plug
		if (!vmu_lcd)
			return;
	}
	if (realtime < vmu_next)
		return;

	// Not in a live level: show a static idle screen once, then leave the bus be.
	if (cls.state != ca_connected || key_dest != key_game || cl.intermission)
	{
		if (!showed_idle)
		{
			vmufb_clear (&fb);
			vmufb_paint_area (&fb, 12, 0, LOGO_W, LOGO_H, ic_logo);	// centered Quake logo
			vmufb_present (&fb, vmu_lcd);
			showed_idle = true;
			vmu_next = realtime + 1.0 / VMU_HUD_HZ;
		}
		return;
	}
	showed_idle = false;

	hp = cl.stats[STAT_HEALTH];
	ai = VMU_AmmoIcon (cl.items);			// active ammo type (shells/nails/...)
	am = cl.stats[STAT_AMMO];

	if (hp < last_hp && last_hp > -9999)		// took damage -> flash the pain face
		pain_time = realtime + VMU_PAIN_TIME;
	painnow = (realtime < pain_time);

	if (hp == last_hp && ai == last_ai && am == last_am && painnow == last_pain)
		return;					// unchanged -> don't touch the maple bus
	last_hp = hp; last_ai = ai; last_am = am; last_pain = painnow;
	vmu_next = realtime + 1.0 / VMU_HUD_HZ;

	vmufb_clear (&fb);

	// --- player half (left, 0..23): health-based face + health number ---
	f = hp / 20;
	if (f < 0) f = 0; else if (f > 4) f = 4;
	if (hp > 0)
		vmufb_paint_area (&fb, 0, 0, ICON_W, ICON_H, painnow ? ic_face_pain[f] : ic_face[f]);
	q_snprintf (num, sizeof(num), "%i", hp);
	vmufb_print_string_into (&fb, NULL, 4, 25, 20, 7, 0, num);	// health

	// --- ammo half (right, 24..47): ammo-type icon + count ---
	if (ai >= 0)
		vmufb_paint_area (&fb, 24, 0, ICON_W, ICON_H, ic_ammo[ai]);
	q_snprintf (num, sizeof(num), "%i", am);
	vmufb_print_string_into (&fb, NULL, 26, 25, 22, 7, 0, num);	// ammo count

	vmufb_present (&fb, vmu_lcd);
}

#endif	/* PLATFORM_DREAMCAST */
