/*
================================================================================
pvr_local.h -- native Dreamcast PVR renderer for QuakeSpasm (maximqad)

WHY THIS EXISTS
---------------
GLdc accumulates the whole frame's OP/PT/TR vertex lists in main-RAM vectors that
grow to high-water mark and never shrink. On the big DOTM maps that balloons past
the 16MB budget and starves the Quake hunk (model-Cache thrash) -- the OOM/lag we
could never fully tune away. xash3d_dc's ref/pvr proves the fix: submit geometry
straight to the PVR Tile Accelerator via the store queues (pvr_dr_*), transform-
and-fire per small batch, nothing accumulated. Render RAM drops to a fixed few
hundred KB and the hunk/pool tug-of-war disappears.

This renderer REPLACES GLdc for the DC. It reuses QuakeSpasm's high-level render
logic (PVS, culling, texture chains, alias lerp -- all the gl_ and r_ smarts) but
swaps the low-level GL submission for direct PVR. Built only when USE_PVR_RENDER
is defined; otherwise the GLdc path is unchanged.

FRAME MODEL (mirrors KOS/ref_pvr)
---------------------------------
  PVR_BeginFrame()                      // pvr_scene_begin()
    PVR_ListBegin(PVR_LIST_OP_POLY)     //   opaque: world base, alias, sprites
      ... submit surfaces/models ...    //   pvr_dr_target/commit per vertex
    PVR_ListBegin(PVR_LIST_PT_POLY)     //   punch-through: alpha-tested (fences)
      ...
    PVR_ListBegin(PVR_LIST_TR_POLY)     //   translucent: lightmap pass, water, 2D
      ...
  PVR_EndFrame()                        // pvr_scene_finish()  (submits + waits + swaps)

Lists are auto-sorted (TR) by the PVR; nothing is held in main RAM between draws.

MODULE MAP (pvr_ .c  <-  ported-from QuakeSpasm gl_ / r_)
--------------------------------------------------------
  pvr_backend.c   frame/list driver, clear color, r_speeds   <- gl_vidsdl GL_Begin/EndRendering
  pvr_rmath.c     matrix stack + sh4zam xmtrx transform + 1/w <- gl_rmain R_SetupGL matrices
  pvr_context.c   poly-context cache (blend/txr/list -> hdr)  <- (new; ref_pvr pvr_context)
  pvr_alloc.c     VRAM texture allocator                      <- (new; ref_pvr pvr_alloc)
  pvr_image.c     texture upload (twiddle, RGB565, paletted)  <- gl_texmgr upload path
  pvr_clip.c      near-Z primitive clipping                   <- (new; ref_pvr pvr_clip)
  pvr_rmain.c     scene orchestration (R_RenderScene glue)    <- gl_rmain
  pvr_rsurf.c     world + brush surfaces, lightmaps           <- r_world + r_brush
  pvr_ralias.c    alias models (mdl)                          <- r_alias + gl_mesh
  pvr_rsprite.c   sprites                                     <- r_sprite
  pvr_rpart.c     particles                                   <- r_part
  pvr_warp.c      water + sky                                 <- gl_warp + gl_sky
  pvr_draw.c      2D: HUD / menu / console                    <- gl_draw + gl_screen(2D)

FILL ORDER (each step independently testable on hardware)
---------------------------------------------------------
  1. backend + rmath: init PVR, clear-screen swap, one test tri via pvr_dr.  <-- start here
  2. image + alloc: upload one texture, draw a textured quad.
  3. draw (2D): get the console/HUD on screen (proves PT/TR + text).
  4. rsurf base pass: world opaque, no lighting.
  5. rmath lightmaps + rsurf pass 2: per-pixel lighting.
  6. ralias: monsters. 7. rpart/rsprite. 8. warp (water/sky). 9. clip polish.

Math is sh4zam throughout (shz_xmtrx_* for the hot transform, shz_mat4x4_* for
matrix build). Vertices are fired as KOS pvr_vertex_t straight into the SQ.
================================================================================
*/
#ifndef PVR_LOCAL_H
#define PVR_LOCAL_H

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include <dc/pvr.h>
#include <dc/matrix.h>
#include "shz_matrix.h"
#include "shz_vector.h"

#include "quakedef.h"

//==============================================================================
// backend  (pvr_backend.c)
//==============================================================================
void	PVR_Backend_Init (void);		// pvr_init with our list/bin config
void	PVR_Backend_Shutdown (void);

void	PVR_BeginFrame (void);			// pvr_scene_begin
void	PVR_EndFrame (void);			// pvr_scene_finish (submit + swap)

// Open a PVR list for submission. Idempotent within a frame; finishing the
// previous list is handled internally. list = PVR_LIST_OP_POLY / PT / TR.
void	PVR_ListBegin (int list);

void	PVR_SetClearColor (float r, float g, float b);
void	PVR_DrawTestTriangle (void);		// pipeline smoke test (step 1)

// 2D (pvr_draw.c): flush the pending 2D batch into the TA. Called at end of frame.
void	PVR_Flush2D (void);

// World render (pvr_rmain.c / pvr_rsurf.c). PVR_SetupGLMatrices builds proj*modelview
// from r_refdef and loads the XMTRX + viewport (called from R_SetupGL under PVR);
// PVR_DrawWorld submits the opaque world texture chains to the OP list.
void	PVR_SetupGLMatrices (int scale);
void	PVR_DrawWorld (qmodel_t *model);

extern int	pvr_frame_list;			// currently open list, -1 if none

//==============================================================================
// rmath -- matrices & vertex transform  (pvr_rmath.c)
//==============================================================================
// QuakeSpasm builds a projection (GL_SetFrustum) and a modelview (R_SetupGL /
// per-entity). We keep the combined MVP in the SH4 xmtrx bank and transform +
// perspective-divide vertices on the fly. Screen mapping (viewport, y-flip,
// z->1/w for the PVR) lives here too.
void	PVR_LoadProjection (const float m[16]);	// GL-order 4x4
void	PVR_LoadModelview  (const float m[16]);
void	PVR_UpdateMVP (void);			// proj*modelview -> xmtrx (call after either changes)

// Transform a world/eye-space position to a screen-space pvr_vertex_t (fills
// x,y = screen px, z = 1/w for depth; caller sets flags, u,v, argb).
// (Kept as a normal call in the skeleton; promote to a header static-inline for
// the hot path once the transform is settled.)
void	PVR_TransformVertex (pvr_vertex_t *out, const float pos[3]);

void	PVR_SetViewport (int x, int y, int w, int h);

//==============================================================================
// textures  (pvr_alloc.c / pvr_image.c)
//   A gltexture_t on DC carries a pvr_ptr_t (VRAM) + format instead of a GL name.
//==============================================================================
struct gltexture_s;
void	PVR_TexAlloc_Init (void);

// VRAM allocator (pvr_alloc.c) -- thin wrappers over pvr_mem_malloc/free for now.
void   *PVR_VramAlloc (unsigned bytes);		// returns pvr_ptr_t (NULL on OOM)
void	PVR_VramFree (void *ptr);

// Texture upload (pvr_image.c). The gltexture_t receives a VRAM pointer (pvr_vram)
// + a compiled PVR_TXRFMT_* (pvr_fmt). Entry points by source pixel type:
//   - PVR_UploadTextureIndexed: 8bpp indices into a shared palette bank -- HALF the
//                           VRAM of 16bpp. Twiddled (paletted is always twiddled).
//                           This is the primary path for Quake's indexed textures.
//   - PVR_UploadTexture:    32-bit RGBA source, converted to RGB565 (opaque) or
//                           ARGB4444 (alpha). For SRC_RGBA (external/replacement).
//   - PVR_UploadTexture565: source already 16-bit RGB565 (lightmaps), uploaded as-is.
// All expect power-of-two w,h (TexMgr pads before calling). Re-upload frees any
// previous VRAM first.
void	PVR_UploadTextureIndexed (struct gltexture_s *glt, const void *indices, int w, int h, int palbank);
void	PVR_UploadTexture (struct gltexture_s *glt, const void *rgba, int w, int h, unsigned flags);
void	PVR_UploadTexture565 (struct gltexture_s *glt, const void *rgb565, int w, int h);
void	PVR_FreeTexture (struct gltexture_s *glt);
void	PVR_BindTexture (struct gltexture_s *glt);	// records the bound texture for the context cache

// Palette RAM (pvr_image.c). The PVR has 1024 32-bit palette entries = four 256-
// entry banks for 8bpp textures. Quake's fixed palette + its fullbright/nobright/
// conchars variants map onto these banks; every indexed texture then shares them.
// Call PVR_PaletteInit once, then load each variant table (RGBA8888, as built by
// TexMgr_LoadPalette) into its bank.
enum {
	PVR_PALBANK_STD      = 0,	// d_8to24table          (world/model/fence)
	PVR_PALBANK_FBRIGHT  = 1,	// d_8to24table_fbright  (fullbright overlay)
	PVR_PALBANK_CONCHARS = 2,	// d_8to24table_conchars (console font)
	PVR_PALBANK_NOBRIGHT = 3	// d_8to24table_nobright (no-fullbright world)
};
void	PVR_PaletteInit (void);
void	PVR_PaletteSetBank (int bank, const unsigned int *rgba256);

// The texture the render path should sample next (set by GL_Bind -> PVR_BindTexture).
// pvr_context reads this when compiling the poly header. NULL = untextured.
extern struct gltexture_s *pvr_bound_texture;

//==============================================================================
// context cache  (pvr_context.c)
//   Maps (list, blend src/dst, texture, filter, env) -> a compiled pvr_poly_hdr.
//   Submitting a header only when state actually changes keeps the DR stream tight.
//==============================================================================
void	PVR_SetBlend (int src, int dst);	// GL_ONE etc. -> PVR blend
void	PVR_SetTexEnv (int env);		// GL_REPLACE / GL_MODULATE
void	PVR_FlushState (void);			// compile+submit header if dirty

//==============================================================================
// clipping  (pvr_clip.c)
//==============================================================================
// Our projection (shz_xmtrx_apply_perspective) yields clip.w = -z_eye (distance)
// and clip.z = near, so a vertex is in front of the near plane when w >= z. Verts
// failing this must never be submitted raw (w<=0 -> 1/w explodes -> TA hang/reboot).
#define PVR_NEAR_CLIP_EPSILON	1e-4f

// Clip one triangle (clip-space verts + per-vertex uv/color) against the near
// plane and submit the resulting 1 or 2 triangles to the current list via the
// store queues. No-op if the whole triangle is behind the near plane.
void	PVR_ClipAndSubmitTriangle (shz_vec4_t p0, shz_vec4_t p1, shz_vec4_t p2,
				   float u0, float v0, float u1, float v1, float u2, float v2,
				   uint32_t c0, uint32_t c1, uint32_t c2);

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
#endif	/* PVR_LOCAL_H */
