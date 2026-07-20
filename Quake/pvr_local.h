/*
================================================================================
pvr_local.h -- native Dreamcast PVR renderer for QuakeSpasm

Replaces GLdc on the Dreamcast. GLdc accumulates the whole frame's OP/PT/TR
vertex lists in main-RAM vectors that grow to a high-water mark and never
shrink; on large maps that overruns the 16MB budget and starves the Quake hunk.
Instead, this renderer submits geometry straight to the PVR Tile Accelerator
through the store queues (pvr_dr_*), transforming and firing one small batch at
a time. Render RAM stays at a fixed few hundred KB.

The high-level render logic (PVS, culling, texture chains, alias lerp -- the gl_
and r_ modules) is reused unchanged; only the low-level GL submission is swapped
for direct PVR. Built only when USE_PVR_RENDER is defined; otherwise the GLdc
path is untouched.

Frame model:
  PVR_BeginFrame()                      // pvr_scene_begin()
    PVR_ListBegin(PVR_LIST_OP_POLY)     //   opaque: world base, alias, sprites
      ... submit surfaces/models ...    //   pvr_dr_target/commit per vertex
    PVR_ListBegin(PVR_LIST_PT_POLY)     //   punch-through: alpha-tested (fences)
      ...
    PVR_ListBegin(PVR_LIST_TR_POLY)     //   translucent: lightmap pass, water, 2D
      ...
  PVR_EndFrame()                        // pvr_scene_finish() (submits, waits, swaps)

The PVR auto-sorts the TR list; nothing is held in main RAM between draws.

Module map (each pvr_ file backs the matching gl_/r_ logic):
  pvr_backend.c   frame/list driver, clear color, r_speeds
  pvr_rmath.c     matrix stack, xmtrx transform, 1/w depth
  pvr_context.c   poly-context cache (blend/txr/list -> header)
  pvr_alloc.c     VRAM texture allocator
  pvr_image.c     texture upload (twiddle, RGB565, paletted)
  pvr_clip.c      near-Z primitive clipping
  pvr_rmain.c     scene orchestration (R_RenderScene glue)
  pvr_rsurf.c     world + brush surfaces, lightmaps
  pvr_alias.c     alias models (.mdl)
  pvr_sprite.c    sprites
  pvr_warp.c      water + sky
  pvr_draw.c      2D: HUD / menu / console

Math is sh4zam throughout (shz_xmtrx_* for the hot transform, shz_mat4x4_* for
matrix build). Vertices are fired as pvr_vertex_t straight into the store queue.
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

// Shared surface emit (pvr_rsurf.c): transform a world-space glpoly to clip space,
// near-plane clip, and emit as a triangle strip / clipped tris, with per-vertex
// texcoords (uu/vv) and colors (col) supplied by the caller (arrays >= this many).
// Used by both the lit world pass and pvr_warp's water.
#define PVR_MAX_POLY_VERTS	64
void	PVR_EmitPoly (struct glpoly_s *p, const float *uu, const float *vv, const uint32_t *col);

// Water / sky (pvr_warp.c) -- warped liquid + sky surfaces.
void	PVR_DrawWorld_Water (qmodel_t *model);
void	PVR_DrawWorld_WaterOpaque (qmodel_t *model);	// OP phase
void	PVR_DrawWorld_WaterTrans (qmodel_t *model);	// TR phase

// Fullbright / glow pass (pvr_rsurf.c) -- additive luma-map overlay.
void	PVR_DrawWorld_Fullbright (qmodel_t *model);
void	PVR_DrawBrushModel_Fullbright (qmodel_t *model);

extern int	pvr_frame_list;			// currently open list, -1 if none
extern qboolean	pvr_fog_active;			// hardware table fog on (pvr_fog.c)

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
void	PVR_TexAlloc_Shutdown (void);

// VRAM allocator (pvr_alloc.c) -- page-aware sub-allocator over one pvr_mem pool.
void   *PVR_VramAlloc (unsigned bytes);		// returns pvr_ptr_t (NULL on OOM)
void	PVR_VramFree (void *ptr);
size_t	PVR_VramFreeBytes (void);		// total free VRAM in the texture pool
size_t	PVR_VramLargestFreeBytes (void);	// largest contiguous free run
size_t	PVR_VramUsedBytes (void);		// bytes currently handed out
size_t	PVR_VramPoolBytes (void);		// total pool size

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
//   - PVR_UploadTextureMipmap: square-POT RGBA source -> twiddled 565/4444 mip chain.
void	PVR_UploadTextureMipmap (struct gltexture_s *glt, const void *rgba, int w, int h, unsigned flags);
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
// PVR_SetTexEnv takes GL_REPLACE / GL_MODULATE, plus this sentinel for the PVR's
// MODULATEALPHA mode (multiplies texel alpha by the vertex alpha -- used by
// translucent water so per-vertex alpha controls transparency).
#define PVR_TEXENV_MODULATEALPHA	0x2A01
void	PVR_SetBlend (int src, int dst);	// GL_ONE etc. -> PVR blend
void	PVR_SetTexEnv (int env);		// GL_REPLACE / GL_MODULATE / PVR_TEXENV_MODULATEALPHA
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
