/*
================================================================================
pvr_rmain.c -- 3D view/matrix setup for the native PVR renderer (maximqad)

QuakeSpasm's gl_rmain.c keeps all the renderer-agnostic scene logic (PVS marking,
frustum culling, texture chains, entity lists) and only touches GL for the matrix
pipeline and vertex submission. Rather than fork 1200 lines, gl_rmain.c delegates
its one GL-specific view step to here under USE_PVR_RENDER:

  R_SetupGL   ->  PVR_SetupGLMatrices

We build the modelview*projection straight into the SH4 hardware matrix bank
(XMTRX) with sh4zam's GL-style ops -- exactly like xash3d_dc's ref/pvr does, and a
1:1 translation of R_SetupGL's glFrustum + glRotate/glTranslate sequence. The MVP
then lives in XMTRX so the per-vertex transform in pvr_rmath is a single ftrv; no
main-memory matrix math, no store/load roundtrip. pvr_rsurf submits the geometry.

Fill status: STEP 4. Opaque world matrices + viewport. Near-plane clipping of polys
that cross z=NEARCLIP is still TODO (pvr_clip) -- until then, geometry very close to
the camera can streak.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include <math.h>

// NEARCLIP is a file-local #define in gl_rmain.c; r_fovx/r_fovy are globals it
// defines but doesn't export in a header. Mirror them here.
#define NEARCLIP	4
extern float r_fovx, r_fovy;

#define DEG2RAD_F(d)	((d) * (float)M_PI / 180.0f)

/*
==============
PVR_SetupGLMatrices -- replaces R_SetupGL's GL matrix/viewport calls under PVR

Builds screen * perspective * modelview straight into the XMTRX bank, matching
xash3d_dc's ref/pvr and gl_rmain.c R_SetupGL:
  - apply_screen(w,h): the NDC->screen viewport (so clip.x/w is a screen pixel).
  - apply_perspective(fov_y, aspect, near): PVR-friendly projection where clip.w is
    the eye-space distance and clip.z is near -- the near-plane test is then w >= z
    (see pvr_clip.c), and depth submitted is 1/w.
  - the -90/90 axis swap, roll/pitch/yaw view rotations, and -vieworg translation.
sh4zam's shz_xmtrx_* post-multiply (M = M * X) exactly like glRotatef/glTranslatef.
After this, XMTRX holds the full world->screen transform for the ftrv per vertex.
==============
*/
void PVR_SetupGLMatrices (int scale)
{
	// Drive the projection from r_refdef.vrect, exactly like R_SetupGL/GL_SetFrustum:
	// fov_y (CalcFovy) is computed for the vrect, which the status bar shrinks, so
	// aspect must be vrect.width/vrect.height (== tan(fovx/2)/tan(fovy/2)), NOT the
	// full framebuffer aspect -- otherwise our frustum is narrower than R_SetFrustum's
	// cull frustum and surfaces near the edges pop in/out as you move.
	// (vrect.x/y are 0 for the default full-width, sbar-at-bottom view; the offset
	//  isn't baked in yet, so a shrunk viewsize would render top-left-anchored.)
	float	vw = (float)r_refdef.vrect.width;
	float	vh = (float)r_refdef.vrect.height;
	float	aspect  = vw / vh;
	float	fovy_rad = r_fovy * (float)M_PI / 180.0f;

	(void)scale;	// r_scale downsampling not applied on the PVR path

	shz_xmtrx_init_identity ();
	shz_xmtrx_apply_screen (vw, vh);
	shz_xmtrx_apply_perspective (fovy_rad, aspect, (float)NEARCLIP);

	// put Z going up (glRotatef(-90,1,0,0); glRotatef(90,0,0,1))
	shz_xmtrx_rotate_x (DEG2RAD_F (-90.0f));
	shz_xmtrx_rotate_z (DEG2RAD_F ( 90.0f));

	// view angles: roll (X), pitch (Y), yaw (Z) -- negated, like R_SetupGL
	shz_xmtrx_rotate_x (DEG2RAD_F (-r_refdef.viewangles[2]));
	shz_xmtrx_rotate_y (DEG2RAD_F (-r_refdef.viewangles[0]));
	shz_xmtrx_rotate_z (DEG2RAD_F (-r_refdef.viewangles[1]));

	// move the world so the camera sits at the origin
	shz_xmtrx_translate (-r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
