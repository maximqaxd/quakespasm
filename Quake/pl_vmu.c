/*
================================================================================
pl_vmu.c -- Dreamcast VMU save-game layer

The CD (/cd/id1) is read-only, so QuakeSpasm's savegames go to the VMU instead.
A save is written by the normal code to a temp file on the KOS ramdisk (/ram),
then this layer zlib-compresses it (a Quake .sav is bulky text -- a VMU only has
~100KB free, so compression is essential) and writes it to /vmu/a1/<NAME> with a
BIOS icon + description. Loading reverses that.

fs_vmu owns the VMU file header: fs_vmu_set_header() attaches the icon/desc and
fs_vmu builds the package on close; fs_read() returns just our payload (the
header is stripped). So we do NOT call vmu_pkg_build/parse ourselves.

Compression is zlib (kos-ports); the bundled miniz has deflate disabled. The
payload carries an uncompressed prefix (magic + size + the savegame comment) so
the load/save menu can list slots without inflating each file. The BIOS long-
description is the map name; the icon is a 32x32 Quake logo.
================================================================================
*/
#include <kos/fs.h>
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>
#include <zlib.h>

#include "quakedef.h"

#if defined(PLATFORM_DREAMCAST)

// Payload prefix before the compressed bytes: magic + uncompressed length + the
// savegame comment (uncompressed, for the menu slot list).
#define VMU_SAVE_MAGIC	"QSAV"
#define VMU_HDR_LEN	8		// 4 magic + 4 uncompressed length
#define VMU_COMMENT_LEN	40		// SAVEGAME_COMMENT_LENGTH(39) + 1, null-padded
#define VMU_PREFIX_LEN	(VMU_HDR_LEN + VMU_COMMENT_LEN)

// Menu save-slot comment cache. Reading a comment means pulling the whole
// compressed save off the VMU (fs_vmu loads the entire file on open), and the
// menu asks for all 20 slots every time it opens. Doing that over the slow
// maple bus while a menu sound plays starves the audio DMA and the sound loops.
// So we scan the VMU exactly once at boot (no sound yet) and keep the comments
// in RAM; the menu reads the cache instantly and saves patch it in place.
#define DC_VMU_MAXSLOTS	20
static char	s_vmu_comment[DC_VMU_MAXSLOTS][VMU_COMMENT_LEN];
static qboolean	s_vmu_loadable[DC_VMU_MAXSLOTS];
static qboolean	s_vmu_scanned;

// "s0".."s19" -> slot index, or -1 for a non-numbered name.
static int DC_VMU_SlotIndex (const char *save)
{
	int	n = 0;
	const char *p;

	if ((save[0] != 's' && save[0] != 'S') || save[1] < '0' || save[1] > '9')
		return -1;
	for (p = save + 1; *p >= '0' && *p <= '9'; p++)
		n = n * 10 + (*p - '0');
	if (*p || n < 0 || n >= DC_VMU_MAXSLOTS)
		return -1;
	return n;
}

// Quake logo icon (32x32 4bpp) + ARGB4444 palette for the VMU BIOS listing.
static const uint16_t quake_icon_pal[16] =
{
	0xF000,0xFE91,0xF850,0xF999,0xF555,0xF530,0xFFFD,0xFFA8,
	0xFF74,0xFF86,0xF777,0xFB70,0xFF88,0xFFB4,0xFCCC,0xF222
};
static const uint8_t quake_icon_data[512] =
{
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0xE0,0x00,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x0E,0x00,0x00,0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x0E,0x00,0x00,0x0E,0x00,0x00,0x00,0x0F,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x0E,0x30,0xE0,0x3E,0x00,0x0F,0x44,0x4F,0xA4,0x44,0x4F,0x00,0x00,0x00,0x00,0x00,
	0x00,0xEE,0xEE,0xE0,0x04,0x4A,0x44,0xA3,0x73,0xA4,0x44,0x4F,0x00,0x00,0x00,0x00,
	0x00,0x00,0xE0,0x00,0x0F,0xA4,0xF5,0x44,0xA4,0x45,0x54,0x45,0x00,0x00,0x00,0x00,
	0x00,0x00,0x30,0x00,0x04,0x40,0xF4,0x44,0x44,0x44,0x55,0xF4,0x00,0x00,0x00,0x00,
	0x00,0x00,0x30,0x00,0x05,0x0F,0x4F,0x44,0xA4,0x4F,0xF0,0xF5,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0xFF,0xFF,0xFF,0xFF,0x05,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x05,0xF0,0x28,0x99,0x89,0x78,0x20,0xF5,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x0F,0x50,0xB5,0x59,0x99,0x25,0x20,0xF5,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x05,0xF5,0x22,0x52,0x92,0x52,0x25,0xFF,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x0F,0x02,0x97,0x97,0x67,0x97,0x92,0x05,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x05,0xF5,0x29,0xB2,0x92,0x29,0x82,0x0F,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x05,0xF5,0x42,0x4A,0x44,0x42,0x52,0xF5,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x05,0x43,0x4A,0x42,0x88,0x42,0x24,0xA5,0x00,0x00,0x00,0x00,
	0x04,0x00,0x00,0x00,0x0F,0x54,0x48,0x5A,0x93,0x52,0xA4,0xFF,0x00,0x00,0x00,0x40,
	0xF9,0xFF,0x0B,0x1B,0xFF,0x5F,0xFA,0x28,0x98,0x2A,0x4F,0x5F,0x0B,0xBB,0xF0,0xAA,
	0x4C,0xFF,0x0B,0x11,0x2F,0x0F,0x04,0xAA,0x4A,0xA4,0x0F,0xFF,0x5B,0x1B,0xFF,0x9A,
	0x47,0xF4,0x02,0xB1,0xB2,0xFF,0x0F,0x05,0x0F,0x05,0x0F,0x05,0xBD,0x1B,0x0F,0x7A,
	0x43,0xFF,0x51,0x11,0xB1,0xBB,0x22,0x25,0xFF,0x52,0x22,0xB1,0xBB,0x1B,0x0F,0xA4,
	0x47,0xFF,0x21,0x1D,0x11,0xD1,0xD1,0x12,0x55,0x11,0x11,0x11,0x11,0x1B,0x5F,0xA4,
	0x43,0xFF,0x2B,0xBB,0x1D,0x11,0xDD,0xD2,0x22,0x1D,0xD1,0x1D,0x1B,0xBB,0x0F,0x34,
	0x4A,0xA0,0x2B,0x1B,0xBB,0xB2,0x22,0x88,0x88,0x82,0x2B,0xBB,0xBB,0xBB,0xFF,0x8A,
	0xA4,0xAF,0x2B,0x2B,0xB2,0x22,0x25,0x22,0x25,0xB2,0x2B,0x2B,0x1B,0x2B,0x04,0xA4,
	0x44,0x4F,0x5B,0xBB,0xB1,0x11,0x11,0x25,0x05,0x51,0x1B,0x12,0x2B,0xBB,0xF4,0xAF,
	0x4F,0x44,0x2B,0x2B,0xBB,0x21,0xBB,0xB5,0x55,0x21,0xB1,0x1B,0xBB,0xBB,0xFA,0x4F,
	0x4F,0x4F,0x22,0xBB,0xBB,0x22,0x25,0x88,0x88,0x85,0x52,0x2B,0x1B,0xBB,0xF2,0xFF,
	0xFF,0xAF,0x2B,0xBB,0xB1,0x11,0x12,0x52,0x22,0x22,0xB1,0x1B,0xBB,0x2B,0xF4,0xBF
};

// Fill a vmu_pkg_t with the icon + the given descriptions. Payload is written
// separately via fs_write; fs_vmu overrides pkg.data on close.
static void DC_VMU_InitPkg (vmu_pkg_t *pkg, const char *shortdesc, const char *longdesc)
{
	memset (pkg, 0, sizeof(*pkg));
	q_strlcpy (pkg->desc_short, shortdesc, sizeof(pkg->desc_short));
	q_strlcpy (pkg->desc_long, longdesc, sizeof(pkg->desc_long));
	strcpy (pkg->app_id, "QUAKE");
	pkg->icon_cnt = 1;
	pkg->icon_anim_speed = 0;
	pkg->eyecatch_type = VMUPKG_EC_NONE;
	pkg->icon_data = (uint8_t *)quake_icon_data;
	memcpy (pkg->icon_pal, quake_icon_pal, sizeof(quake_icon_pal));
}

// VMU filenames are <= 12 chars, uppercase alphanumeric. The menu slots
// "s0".."s19" map to "QUAKE001".."QUAKE020" for a tidy VMU file listing.
static void DC_VMU_Filename (const char *save, char *out /* [13] */)
{
	int	i, j = 0;

	if ((save[0] == 's' || save[0] == 'S') && save[1] >= '0' && save[1] <= '9')
	{
		int		n = 0;
		const char	*p = save + 1;
		while (*p >= '0' && *p <= '9')
			n = n * 10 + (*p++ - '0');
		q_snprintf (out, 13, "QUAKE%03d", n + 1);
		return;
	}

	for (i = 0; save[i] && j < 12; i++)
	{
		char c = save[i];
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			out[j++] = c;
	}
	if (j == 0)
	{
		strcpy (out, "SAVE");
		return;
	}
	out[j] = '\0';
}

static byte *DC_ReadWholeFile (const char *path, long *out_len)
{
	FILE	*f = fopen (path, "rb");
	long	len;
	byte	*buf;

	if (!f)
		return NULL;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (len <= 0) { fclose (f); return NULL; }
	buf = (byte *) malloc (len);
	if (!buf) { fclose (f); return NULL; }
	if (fread (buf, 1, len, f) != (size_t)len) { free (buf); fclose (f); return NULL; }
	fclose (f);
	*out_len = len;
	return buf;
}

// Read the payload of a VMU file (fs_vmu strips its header). Caller frees; NULL
// on failure. *out_len gets the number of bytes read.
static byte *DC_VMU_ReadPayload (const char *vmuname, int *out_len)
{
	char	vmupath[32];
	file_t	f;
	int	size;
	byte	*buf;
	ssize_t	got;

	q_snprintf (vmupath, sizeof(vmupath), "/vmu/a1/%s", vmuname);
	f = fs_open (vmupath, O_RDONLY);
	if (!f)
		return NULL;
	size = (int) fs_total (f);
	if (size <= 0) { fs_close (f); return NULL; }
	buf = (byte *) malloc (size);
	if (!buf) { fs_close (f); return NULL; }
	got = fs_read (f, buf, size);
	fs_close (f);
	if (got <= 0) { free (buf); return NULL; }
	*out_len = (int)got;
	return buf;
}

/*
==============
DC_VMU_SaveGame -- compress /ram/<name>.sav and write it to the VMU
==============
*/
qboolean DC_VMU_SaveGame (const char *savename, const char *mapname, const char *comment)
{
	char		rampath[64], vmupath[32], vmuname[16];
	byte		*raw = NULL, *payload = NULL;
	long		rawlen = 0;
	uLongf		complen;
	vmu_pkg_t	pkg;
	int		paylen;
	file_t		f;
	qboolean	ok = false;

	q_snprintf (rampath, sizeof(rampath), "/ram/%s.sav", savename);
	raw = DC_ReadWholeFile (rampath, &rawlen);
	if (!raw)
	{
		Con_Printf ("VMU save: couldn't read temp file\n");
		return false;
	}

	// prefix (magic + uncompressed length + comment) then the deflate stream
	complen = compressBound ((uLong)rawlen);
	payload = (byte *) malloc (VMU_PREFIX_LEN + complen);
	if (!payload) { free (raw); return false; }
	memcpy (payload, VMU_SAVE_MAGIC, 4);
	payload[4] = (byte)(rawlen        & 0xff);
	payload[5] = (byte)((rawlen >>  8) & 0xff);
	payload[6] = (byte)((rawlen >> 16) & 0xff);
	payload[7] = (byte)((rawlen >> 24) & 0xff);
	memset (payload + VMU_HDR_LEN, 0, VMU_COMMENT_LEN);
	q_strlcpy ((char *)(payload + VMU_HDR_LEN), comment ? comment : "", VMU_COMMENT_LEN);
	if (compress2 (payload + VMU_PREFIX_LEN, &complen, raw, (uLong)rawlen, 9) != Z_OK)
	{
		Con_Printf ("VMU save: compression failed\n");
		goto done;
	}
	paylen = VMU_PREFIX_LEN + (int)complen;

	// BIOS: short = map name, long = the savegame comment (map + kills/secrets/date)
	DC_VMU_InitPkg (&pkg, mapname, (comment && comment[0]) ? comment : mapname);

	DC_VMU_Filename (savename, vmuname);
	q_snprintf (vmupath, sizeof(vmupath), "/vmu/a1/%s", vmuname);
	fs_unlink (vmupath);
	f = fs_open (vmupath, O_WRONLY);
	if (!f)
	{
		Con_Printf ("VMU save: no VMU in slot a1?\n");
		goto done;
	}
	// fs_vmu builds the package (icon + desc + our payload) on close.
	fs_vmu_set_header (f, &pkg);
	if (fs_write (f, payload, paylen) != paylen)
		Con_Printf ("VMU save: write failed (card full?)\n");
	else
		ok = true;
	fs_close (f);

	if (ok)
	{
		// Keep the menu's comment cache current without re-reading the VMU.
		int idx = DC_VMU_SlotIndex (savename);
		if (idx >= 0)
		{
			q_strlcpy (s_vmu_comment[idx], comment ? comment : "", VMU_COMMENT_LEN);
			s_vmu_loadable[idx] = true;
			s_vmu_scanned = true;
		}
	}

done:
	free (raw);
	free (payload);
	return ok;
}

/*
==============
DC_VMU_LoadGame -- read the VMU file, inflate it onto /ram/<name>.sav
==============
*/
qboolean DC_VMU_LoadGame (const char *savename)
{
	char		rampath[64], vmuname[16];
	byte		*payload = NULL, *out = NULL;
	int		paylen = 0;
	uLongf		outlen, dl;
	FILE		*wf;
	qboolean	ok = false;

	DC_VMU_Filename (savename, vmuname);
	payload = DC_VMU_ReadPayload (vmuname, &paylen);
	if (!payload)
		return false;

	if (paylen < VMU_PREFIX_LEN || memcmp (payload, VMU_SAVE_MAGIC, 4))
	{
		Con_Printf ("VMU load: not a Quake save\n");
		goto done;
	}
	outlen = (uLongf)payload[4] | ((uLongf)payload[5] << 8)
	       | ((uLongf)payload[6] << 16) | ((uLongf)payload[7] << 24);
	out = (byte *) malloc (outlen);
	if (!out) goto done;
	dl = outlen;
	if (uncompress (out, &dl, payload + VMU_PREFIX_LEN, paylen - VMU_PREFIX_LEN) != Z_OK)
	{
		Con_Printf ("VMU load: decompress failed\n");
		goto done;
	}

	q_snprintf (rampath, sizeof(rampath), "/ram/%s.sav", savename);
	wf = fopen (rampath, "wb");
	if (!wf) goto done;
	fwrite (out, 1, dl, wf);
	fclose (wf);
	ok = true;

done:
	free (payload);
	free (out);
	return ok;
}

/*
==============
DC_VMU_ScanSaves -- populate the comment cache from the VMU (blocking maple I/O)

Call once at boot, before any menu sound can play. Reads every numbered slot's
comment prefix; empty slots fail fast (no such file), existing saves cost one
whole-file read each.
==============
*/
void DC_VMU_ScanSaves (void)
{
	char	save[16], vmuname[16];
	byte	*payload;
	int	i, paylen;

	for (i = 0; i < DC_VMU_MAXSLOTS; i++)
	{
		s_vmu_loadable[i] = false;
		s_vmu_comment[i][0] = '\0';

		q_snprintf (save, sizeof(save), "s%i", i);
		DC_VMU_Filename (save, vmuname);
		paylen = 0;
		payload = DC_VMU_ReadPayload (vmuname, &paylen);
		if (!payload)
			continue;
		if (paylen >= VMU_PREFIX_LEN && !memcmp (payload, VMU_SAVE_MAGIC, 4))
		{
			q_strlcpy (s_vmu_comment[i], (const char *)payload + VMU_HDR_LEN, VMU_COMMENT_LEN);
			s_vmu_loadable[i] = true;
		}
		free (payload);
	}
	s_vmu_scanned = true;
}

/*
==============
DC_VMU_GetSaveComment -- a save's menu comment, served from the RAM cache

No VMU access here: the menu opens this 20 times in a row and blocking the main
thread on the maple bus would starve the sound DMA. The cache is filled once at
boot by DC_VMU_ScanSaves and kept current by DC_VMU_SaveGame.
==============
*/
qboolean DC_VMU_GetSaveComment (const char *savename, char *out, int outsize)
{
	int	idx = DC_VMU_SlotIndex (savename);

	if (idx < 0)
		return false;
	if (!s_vmu_scanned)		// safety net if boot scan was skipped
		DC_VMU_ScanSaves ();
	if (!s_vmu_loadable[idx])
		return false;
	q_strlcpy (out, s_vmu_comment[idx], outsize);
	return true;
}

/*
==============
DC_VMU_WriteConfig -- write config.cfg (uncompressed, small) to the VMU
==============
*/
qboolean DC_VMU_WriteConfig (const void *data, int len)
{
	vmu_pkg_t	pkg;
	file_t		f;
	qboolean	ok = false;

	DC_VMU_InitPkg (&pkg, "config", "Quake settings");

	fs_unlink ("/vmu/a1/QUAKECFG");
	f = fs_open ("/vmu/a1/QUAKECFG", O_WRONLY);
	if (f)
	{
		fs_vmu_set_header (f, &pkg);
		ok = (fs_write (f, data, len) == len);
		fs_close (f);
	}
	return ok;
}

// Load config.cfg from the VMU into a malloc'd buffer (caller frees). NULL if none.
byte *DC_VMU_ReadConfig (int *out_len)
{
	byte	*payload;
	int	paylen = 0;
	byte	*out;

	payload = DC_VMU_ReadPayload ("QUAKECFG", &paylen);
	if (!payload || paylen <= 0)
	{
		free (payload);
		return NULL;
	}
	out = (byte *) malloc (paylen + 1);
	if (!out) { free (payload); return NULL; }
	memcpy (out, payload, paylen);
	out[paylen] = '\0';
	*out_len = paylen;
	free (payload);
	return out;
}

#endif	/* PLATFORM_DREAMCAST */
