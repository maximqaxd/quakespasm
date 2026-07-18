/*
================================================================================
pvr_vid.c -- Dreamcast video driver for the native PVR renderer (maximqad)

REPLACES gl_vidsdl.c when USE_PVR_RENDER is set (the Makefile swaps the object).
No SDL video, no GLdc: KOS drives the display and PVR directly. Provides the vid
interface the rest of the engine expects (VID_*, GL_Begin/EndRendering) plus the
handful of gl_ capability flags / gamma cvars other TUs reference.

Fixed 640x480 RGB565 (cable-aware) -- the DC's native mode. Frame boundaries open
and close a PVR scene; the render modules submit between them via pvr_dr.

Fill status: STEP 1. GL_EndRendering currently draws a test triangle to prove the
pipeline; that call is removed once pvr_rmain/pvr_rsurf drive real geometry.
================================================================================
*/
// pvr_local.h pulls <dc/pvr.h> BEFORE quakedef.h -- required, because quakedef's
// system includes #define HZ, which collides with arch.h's use of HZ as an
// identifier if pvr.h is pulled afterwards.
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#define WARP_WIDTH	320	/* local in gl_vidsdl.c; mirror it here */
#define WARP_HEIGHT	200

//------------------------------------------------------------------------------
// Globals other translation units reference (gl_vidsdl.c defined these).
//------------------------------------------------------------------------------
viddef_t	vid;				// global video state
modestate_t	modestate = MS_UNINIT;
qboolean	scr_skipupdate;
int		gl_stencilbits;
float		gl_max_anisotropy;
GLint		gl_max_texture_units = 0;

// GLdc extension proc pointers other TUs link against. Unused on the PVR path
// (no multitexture / VBO), kept NULL so the still-compiled gl_ render modules link.
PFNGLMULTITEXCOORD2FARBPROC	GL_MTexCoord2fFunc = NULL;
PFNGLACTIVETEXTUREARBPROC	GL_SelectTextureFunc = NULL;
PFNGLCLIENTACTIVETEXTUREARBPROC	GL_ClientActiveTextureFunc = NULL;
PFNGLBINDBUFFERARBPROC		GL_BindBufferFunc = NULL;
PFNGLBUFFERDATAARBPROC		GL_BufferDataFunc = NULL;
PFNGLBUFFERSUBDATAARBPROC	GL_BufferSubDataFunc = NULL;
PFNGLDELETEBUFFERSARBPROC	GL_DeleteBuffersFunc = NULL;
PFNGLGENBUFFERSARBPROC		GL_GenBuffersFunc = NULL;
QS_PFNGENERATEMIPMAP		GL_GenerateMipmap = NULL;

// GL capability flags: none apply to the PVR path, but non-render TUs still read
// some of them, so keep them defined and false.
qboolean gl_mtexable = false;
qboolean gl_packed_pixels = false;
qboolean gl_texture_env_combine = false;
qboolean gl_texture_env_add = false;
qboolean gl_swap_control = false;
qboolean gl_anisotropy_able = false;
qboolean gl_texture_NPOT = false;
qboolean gl_vbo_able = false;
qboolean gl_glsl_able = false;
qboolean gl_glsl_gamma_able = false;
qboolean gl_glsl_alias_able = false;

cvar_t	vid_gamma    = {"gamma", "1", CVAR_ARCHIVE};
cvar_t	vid_contrast = {"contrast", "1", CVAR_ARCHIVE};

// Registered so the menu/config don't choke; the PVR path ignores them.
static cvar_t	vid_fullscreen = {"vid_fullscreen", "1", CVAR_ARCHIVE};
static cvar_t	vid_width      = {"vid_width", "640", CVAR_ARCHIVE};
static cvar_t	vid_height     = {"vid_height", "480", CVAR_ARCHIVE};
static cvar_t	vid_bpp        = {"vid_bpp", "16", CVAR_ARCHIVE};
static cvar_t	vid_vsync      = {"vid_vsync", "1", CVAR_ARCHIVE};

static qboolean	vid_initialized = false;

//------------------------------------------------------------------------------
// Menu hooks -- the engine calls these through function pointers; keep no-ops so
// the options menu doesn't crash (there are no video modes to change on DC).
//------------------------------------------------------------------------------
static void PVR_VID_MenuCmd  (void) {}
static void PVR_VID_MenuDraw (void) {}
static void PVR_VID_MenuKey  (int key) { (void)key; }

//------------------------------------------------------------------------------
// Frame boundaries
//------------------------------------------------------------------------------
void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	*x = *y = 0;
	*width  = vid.width;
	*height = vid.height;

	PVR_BeginFrame ();
}

void GL_EndRendering (void)
{
	if (!scr_skipupdate)
	{
		// 2D (pvr_draw) has submitted the frame's geometry into the TR list between
		// BeginFrame and here; just close the scene and swap.
		PVR_EndFrame ();
	}
}

//------------------------------------------------------------------------------
// Init / shutdown
//------------------------------------------------------------------------------
void VID_Init (void)
{
	Cvar_RegisterVariable (&vid_fullscreen);
	Cvar_RegisterVariable (&vid_width);
	Cvar_RegisterVariable (&vid_height);
	Cvar_RegisterVariable (&vid_bpp);
	Cvar_RegisterVariable (&vid_vsync);
	Cvar_RegisterVariable (&vid_gamma);
	Cvar_RegisterVariable (&vid_contrast);

	PVR_Backend_Init ();		// KOS display + pvr_init

	// Fixed native mode.
	vid.width      = 640;
	vid.height     = 480;
	vid.conwidth   = 640;
	vid.conheight  = 480;
	vid.numpages   = 2;
	vid.maxwarpwidth  = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap   = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.recalc_refdef = 1;

	vid_menucmdfn  = PVR_VID_MenuCmd;
	vid_menudrawfn = PVR_VID_MenuDraw;
	vid_menukeyfn  = PVR_VID_MenuKey;

	vid_initialized = true;
}

void VID_Shutdown (void)
{
	if (!vid_initialized)
		return;
	PVR_Backend_Shutdown ();
	vid_initialized = false;
}

//------------------------------------------------------------------------------
// Interface stubs (no window system on DC)
//------------------------------------------------------------------------------
void	VID_Toggle (void)		{}
void	VID_SyncCvars (void)		{}
void	VID_Lock (void)			{}
void	*VID_GetWindow (void)		{ return NULL; }
qboolean VID_HasMouseOrInputFocus (void){ return true; }
qboolean VID_IsMinimized (void)		{ return false; }

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
