/*
 * gl_dc_compat.h -- Dreamcast (GLdc) compatibility shim for QuakeSpasm.
 *
 * GLdc ships a GL 1.x-ish <GL/gl.h> + <GL/glext.h> but omits several typedefs
 * that QuakeSpasm's glquake.h relies on (they normally come from a desktop
 * glext.h): GLchar, APIENTRYP, GLsizeiptr/GLintptr and the ARB extension
 * PFN function-pointer typedefs. Define just those here so glquake.h compiles.
 *
 * On Dreamcast we force fixed-function rendering: no GLSL, no VBOs. The ARB
 * multitexture entry points DO exist in GLdc (glActiveTextureARB,
 * glClientActiveTextureARB, glMultiTexCoord2fARB); the VBO typedefs below only
 * exist to satisfy the declarations in glquake.h -- gl_vbo_able is kept false
 * so the corresponding function pointers are never dereferenced.
 */
#ifndef GL_DC_COMPAT_H
#define GL_DC_COMPAT_H

#include <stddef.h>	/* ptrdiff_t */

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

/* Missing base types. */
#ifndef GL_DC_HAVE_GLCHAR
typedef char		GLchar;
#endif
typedef ptrdiff_t	GLsizeiptr;
typedef ptrdiff_t	GLintptr;
typedef char		GLcharARB;

/* ARB multitexture (GLdc provides the actual functions). */
typedef void  (APIENTRYP PFNGLMULTITEXCOORD2FARBPROC)     (GLenum target, GLfloat s, GLfloat t);
typedef void  (APIENTRYP PFNGLACTIVETEXTUREARBPROC)       (GLenum texture);
typedef void  (APIENTRYP PFNGLCLIENTACTIVETEXTUREARBPROC) (GLenum texture);

/* ARB vertex buffer objects -- NOT implemented by GLdc; typedefs only so the
   declarations in glquake.h are valid. Guarded by gl_vbo_able == false. */
typedef void  (APIENTRYP PFNGLBINDBUFFERARBPROC)    (GLenum target, GLuint buffer);
typedef void  (APIENTRYP PFNGLBUFFERDATAARBPROC)    (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void  (APIENTRYP PFNGLBUFFERSUBDATAARBPROC) (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
typedef void  (APIENTRYP PFNGLDELETEBUFFERSARBPROC) (GLsizei n, const GLuint *buffers);
typedef void  (APIENTRYP PFNGLGENBUFFERSARBPROC)    (GLsizei n, GLuint *buffers);

/* GL_ARB_vertex_buffer_object tokens used by gl_mesh.c (values are standard). */
#ifndef GL_ARRAY_BUFFER_ARB
#define GL_ARRAY_BUFFER_ARB		0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_ARB
#define GL_ELEMENT_ARRAY_BUFFER_ARB	0x8893
#endif
#ifndef GL_STATIC_DRAW_ARB
#define GL_STATIC_DRAW_ARB		0x88E4
#endif

/* Non-ARB spellings used directly in the VBO code paths (compiled but gated by
   gl_vbo_able == false, so never executed on DC). */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER			0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER		0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW			0x88E4
#endif

/* GLSL tokens referenced by gl_rmisc.c's shader compile helpers. GLdc has no
   GLSL; this code is gated by gl_glsl_able == false and never runs on DC. */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER		0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER		0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS		0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS			0x8B82
#endif

/* GL_ARB/EXT_texture_env_add combine mode -- used by the fixed-function
   multitexture alias path (r_alias.c). GLdc supports GL_ADD env mode. */
#ifndef GL_ADD
#define GL_ADD				0x0104
#endif

#endif /* GL_DC_COMPAT_H */
