/*
================================================================================
pvr_backend.c -- PVR frame/list driver (maximqad)

Owns the KOS PVR init and the per-frame scene structure. Replaces GLdc's
init + the SDL_GL swap in gl_vidsdl.c. See pvr_local.h for the big picture.

Fill status: STEP 1 skeleton. init + clear-screen swap should light up first;
list open/close is wired so the render modules can start submitting via pvr_dr.
================================================================================
*/
#include <dc/video.h>
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

int	pvr_frame_list = -1;

static qboolean	pvr_inited;
static float	pvr_clear[3] = { 0.0f, 0.0f, 0.0f };

// Proven config lifted from xash3d_dc (engine/platform/dreamcast/vid_dc.c):
// small 8-word OPBs, a 768KB TA vertex buffer (lives in VRAM, not main RAM --
// this is the whole point vs GLdc), autosort enabled (PVR sorts the TR list),
// and 2 OPB-overflow sets to avoid tile-boundary flicker on heavy scenes.
static pvr_init_params_t pvr_params =
{
	/* opb_sizes: OP, OP-mod, TR, TR-mod, PT */
	{ PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8 },
	4096 * 256,	/* vertex_buf_size = 1MB (VRAM) -- headroom for dense world scenes */
	0,		/* dma_enabled */
	0,		/* fsaa_enabled */
	0,		/* autosort_disabled (0 = PVR autosorts translucents) */
	2		/* opb_overflow_count */
};

/*
==============
PVR_Backend_Init -- KOS-native display + PVR init (no SDL video)

Cable-aware 640x480 RGB565, then pvr_init. Mirrors xash3d_dc's VID_SetMode +
R_Init_Video. The engine no longer needs SDL_INIT_VIDEO.
==============
*/
void PVR_Backend_Init (void)
{
	int	dm;

	if (pvr_inited)
		return;

	switch (vid_check_cable ())
	{
	case CT_VGA:	dm = DM_640x480_VGA; break;
	case CT_RGB:
	case CT_COMPOSITE:
	default:	dm = DM_640x480; break;
	}
	vid_init (dm, PM_RGB565);

	if (pvr_init (&pvr_params) < 0)
		Sys_Error ("PVR_Backend_Init: pvr_init failed");

	pvr_set_bg_color (pvr_clear[0], pvr_clear[1], pvr_clear[2]);
	PVR_TexAlloc_Init ();
	pvr_inited = true;
}

/*
==============
PVR_DrawTestTriangle -- pipeline smoke test

One untextured Gouraud triangle in the OP list via direct rendering. Proves
init + scene + list + header submit + pvr_dr vertex fire before any real
geometry is ported. Call between PVR_BeginFrame and PVR_EndFrame.
==============
*/
void PVR_DrawTestTriangle (void)
{
	pvr_poly_cxt_t	cxt;
	pvr_poly_hdr_t	hdr;
	pvr_vertex_t	*vp;

	PVR_ListBegin (PVR_LIST_OP_POLY);

	pvr_poly_cxt_col (&cxt, PVR_LIST_OP_POLY);
	pvr_poly_compile (&hdr, &cxt);
	pvr_prim (&hdr, sizeof(hdr));

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX;      vp->x = 320; vp->y =  80; vp->z = 1.0f;
	vp->argb  = 0xffff0000;          pvr_dr_commit (vp);

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX;      vp->x = 560; vp->y = 400; vp->z = 1.0f;
	vp->argb  = 0xff00ff00;          pvr_dr_commit (vp);

	vp = pvr_dr_target (NULL);
	vp->flags = PVR_CMD_VERTEX_EOL;  vp->x =  80; vp->y = 400; vp->z = 1.0f;
	vp->argb  = 0xff0000ff;          pvr_dr_commit (vp);
}

void PVR_Backend_Shutdown (void)
{
	if (!pvr_inited)
		return;
	pvr_shutdown ();
	pvr_inited = false;
}

void PVR_SetClearColor (float r, float g, float b)
{
	pvr_clear[0] = r; pvr_clear[1] = g; pvr_clear[2] = b;
	if (pvr_inited)
		pvr_set_bg_color (r, g, b);
}

/*
==============
PVR_BeginFrame / PVR_ListBegin / PVR_EndFrame

One scene per frame. Lists are opened lazily as the render path needs them and
must be visited in hardware order (OP -> PT -> TR); PVR_ListBegin enforces that
by finishing any currently-open list first.
==============
*/
void PVR_BeginFrame (void)
{
	pvr_wait_ready ();
	pvr_scene_begin ();
	pvr_frame_list = -1;
}

void PVR_ListBegin (int list)
{
	if (pvr_frame_list == list)
		return;
	if (pvr_frame_list != -1)
		pvr_list_finish ();
	pvr_list_begin (list);
	pvr_frame_list = list;
}

void PVR_EndFrame (void)
{
	if (pvr_frame_list != -1)
	{
		pvr_list_finish ();
		pvr_frame_list = -1;
	}
	pvr_scene_finish ();
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
