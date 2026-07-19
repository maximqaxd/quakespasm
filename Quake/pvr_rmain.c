/*
================================================================================
pvr_rmain.c -- 3D view and matrix setup for the native PVR renderer

gl_rmain.c holds the renderer-agnostic scene logic (PVS marking, frustum
culling, texture chains, entity lists). Its one GL-specific view step,
R_SetupGL, is replaced here under USE_PVR_RENDER by PVR_SetupGLMatrices.

The modelview*projection is built straight into the SH4 hardware matrix bank
(XMTRX) with sh4zam's GL-style operations, mirroring R_SetupGL's glFrustum +
glRotate/glTranslate sequence. With the MVP resident in XMTRX the per-vertex
transform in pvr_rmath is a single ftrv -- no main-memory matrix math and no
store/load roundtrip. pvr_rsurf submits the geometry.

Near-plane clipping of polygons that cross z=NEARCLIP is handled in pvr_clip.
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

// world MVP (screen*persp*view), saved so brush-model entities can restore it
static shz_mat4x4_t	pvr_world_mvp;

/*
==============
PVR_SetupGLMatrices -- replaces R_SetupGL's GL matrix/viewport calls under PVR

Builds screen * perspective * modelview straight into the XMTRX bank:
  - apply_screen(w,h): the NDC->screen viewport (so clip.x/w is a screen pixel).
  - apply_perspective(fov_y, aspect, near): projection where clip.w is the
    eye-space distance and clip.z is near -- the near-plane test is then w >= z
    (see pvr_clip.c), and submitted depth is 1/w.
  - the -90/90 axis swap, roll/pitch/yaw view rotations, and -vieworg translation.
sh4zam's shz_xmtrx_* calls post-multiply (M = M * X) like glRotatef/glTranslatef.
After this, XMTRX holds the full world->screen transform for the per-vertex ftrv.
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
	float	fovy_rad = r_fovy * (float)M_PI / 180.0f;
	// Aspect from the render fovs, not the fixed vrect ratio: it equals
	// vrect.width/vrect.height in the normal case, but when r_waterwarp wobbles
	// r_fovx/r_fovy independently underwater this tracks R_SetFrustum(r_fovx,r_fovy)
	// -- giving the classic underwater screen warp for free.
	float	aspect = tanf (r_fovx * (0.5f * (float)M_PI / 180.0f))
		       / tanf (r_fovy * (0.5f * (float)M_PI / 180.0f));

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

	// Save the world MVP so brush-model entities can restore it and post-multiply
	// their own object transform (XMTRX is otherwise only read by the vertex ftrv).
	shz_xmtrx_store_4x4 (&pvr_world_mvp);
}

/*
==============
PVR_SetupEntityMatrices -- world MVP * entity object transform, into XMTRX

Mirrors R_RotateForEntity (glTranslate origin; rotate yaw/-pitch/roll; scale),
post-multiplied onto the saved world MVP so a brush model's model-local verts map
straight to the screen. Restore with PVR_RestoreWorldMatrix when done.
==============
*/
void PVR_SetupEntityMatrices (vec3_t origin, vec3_t angles, unsigned char scale)
{
	float sf = ENTSCALE_DECODE (scale);

	shz_xmtrx_load_4x4 (&pvr_world_mvp);
	shz_xmtrx_translate (origin[0], origin[1], origin[2]);
	shz_xmtrx_rotate_z (DEG2RAD_F ( angles[1]));
	shz_xmtrx_rotate_y (DEG2RAD_F (-angles[0]));
	shz_xmtrx_rotate_x (DEG2RAD_F ( angles[2]));
	if (sf != 1.0f)
		shz_xmtrx_scale (sf, sf, sf);
}

/*
==============
PVR_SetupAliasMatrices -- entity MVP with the alias byte-vertex decode baked in

Alias .mdl positions are stored as bytes (0..255) that map to model space via
scale*v + scale_origin (per-model constants). Mirrors R_DrawAliasModel's GL
matrix sequence: R_RotateForEntity, then glTranslate(scale_origin) and
glScale(scale). Post-multiplying those onto the entity transform means the raw
(possibly lerped) byte positions can be fed straight through the XMTRX ftrv --
one matrix decodes + transforms every vertex. fovscale (>1 only for a wide-fov
view weapon) scales Y/Z like the GL path. Restore with PVR_RestoreWorldMatrix.
==============
*/
void PVR_SetupAliasMatrices (vec3_t origin, vec3_t angles, unsigned char scale,
			     vec3_t hdr_scale, vec3_t hdr_scale_origin, float fovscale)
{
	PVR_SetupEntityMatrices (origin, angles, scale);
	shz_xmtrx_translate (hdr_scale_origin[0], hdr_scale_origin[1] * fovscale, hdr_scale_origin[2] * fovscale);
	shz_xmtrx_scale (hdr_scale[0], hdr_scale[1] * fovscale, hdr_scale[2] * fovscale);
}

/*
==============
PVR_SetupAliasShadowMatrices -- entity MVP that flattens the model onto the floor

Mirrors GL_DrawAliasShadow's matrix chain: translate to the entity, drop to the
light plane, apply the skew/flatten shadow matrix, come back up, then the entity
rotation and the byte-vertex decode (scale_origin + scale). No entity scale, like
the GL path. lheight is origin.z - lightspot.z. The alias byte positions then
project straight to a blob on the floor through the XMTRX ftrv.
==============
*/
#define SHADOW_SKEW_X	-0.7f	/* match r_alias.c GL_DrawAliasShadow */
#define SHADOW_SKEW_Y	 0.0f
#define SHADOW_VSCALE	 0.0f	/* 0 == completely flat */
#define SHADOW_HEIGHT	 0.1f	/* lift off the floor to avoid z-fighting */

void PVR_SetupAliasShadowMatrices (vec3_t origin, vec3_t angles, vec3_t hdr_scale,
				   vec3_t hdr_scale_origin, float lheight)
{
	// column-major (glMultMatrixf layout), same constant as GL_DrawAliasShadow
	static const float shadowmatrix[16] =
	{
		1.0f,          0.0f,          0.0f,          0.0f,
		0.0f,          1.0f,          0.0f,          0.0f,
		SHADOW_SKEW_X, SHADOW_SKEW_Y, SHADOW_VSCALE, 0.0f,
		0.0f,          0.0f,          SHADOW_HEIGHT, 1.0f
	};

	shz_xmtrx_load_4x4 (&pvr_world_mvp);
	shz_xmtrx_translate (origin[0], origin[1], origin[2]);
	shz_xmtrx_translate (0.0f, 0.0f, -lheight);
	shz_xmtrx_apply_unaligned_4x4 (shadowmatrix);
	shz_xmtrx_translate (0.0f, 0.0f, lheight);
	shz_xmtrx_rotate_z (DEG2RAD_F ( angles[1]));
	shz_xmtrx_rotate_y (DEG2RAD_F (-angles[0]));
	shz_xmtrx_rotate_x (DEG2RAD_F ( angles[2]));
	shz_xmtrx_translate (hdr_scale_origin[0], hdr_scale_origin[1], hdr_scale_origin[2]);
	shz_xmtrx_scale (hdr_scale[0], hdr_scale[1], hdr_scale[2]);
}

void PVR_RestoreWorldMatrix (void)
{
	shz_xmtrx_load_4x4 (&pvr_world_mvp);
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
