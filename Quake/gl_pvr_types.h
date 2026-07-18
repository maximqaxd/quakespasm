/*
 * gl_pvr_types.h -- minimal GL type/token/prototype shim for the native PVR
 * renderer (maximqad).
 *
 * The USE_PVR_RENDER build no longer links or includes GLdc: <GL/gl.h>,
 * <GL/glext.h> and <GL/glkos.h> are gone. But the engine's still-compiled
 * gl_*.c / r_*.c render modules (and glquake.h) reference GL base types
 * (GLuint, GLenum, ...), a pile of GL_* enum tokens, and a set of gl* entry
 * points. This header supplies exactly those, and nothing else -- the gl*
 * entry points resolve at link time to the no-op stubs in pvr_glstub.c until
 * each path is ported to a pvr_ module and drops off the list.
 *
 * Values match desktop OpenGL where a real value exists; the handful of GLdc
 * "KOS" internal-format tokens get locally-unique values (their only use on the
 * PVR path is compile-time switch/case distinctness -- the code that consumed
 * them at runtime is a no-op stub here).
 */
#ifndef GL_PVR_TYPES_H
#define GL_PVR_TYPES_H

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

/* ---- base types ---------------------------------------------------------- */
typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void		GLvoid;
typedef signed char	GLbyte;
typedef short		GLshort;
typedef int		GLint;
typedef int		GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float		GLfloat;
typedef float		GLclampf;
typedef double		GLdouble;
typedef double		GLclampd;
typedef char		GLchar;
typedef char		GLcharARB;
typedef ptrdiff_t	GLsizeiptr;
typedef ptrdiff_t	GLintptr;

/* ---- enum tokens --------------------------------------------------------- */
/* booleans */
#define GL_FALSE			0
#define GL_TRUE				1
#define GL_ZERO				0
#define GL_ONE				1

/* data types */
#define GL_BYTE				0x1400
#define GL_UNSIGNED_BYTE		0x1401
#define GL_SHORT			0x1402
#define GL_UNSIGNED_SHORT		0x1403
#define GL_INT				0x1404
#define GL_UNSIGNED_INT			0x1405
#define GL_FLOAT			0x1406
#define GL_UNSIGNED_SHORT_5_6_5		0x8363
#define GL_UNSIGNED_INT_8_8_8_8_REV	0x8367
#define GL_UNSIGNED_INT_10_10_10_2	0x8036

/* primitive modes */
#define GL_LINES			0x0001
#define GL_TRIANGLES			0x0004
#define GL_TRIANGLE_STRIP		0x0005
#define GL_TRIANGLE_FAN			0x0006
#define GL_QUADS			0x0007
#define GL_QUAD_STRIP			0x0008
#define GL_POLYGON			0x0009

/* matrix modes */
#define GL_MODELVIEW			0x1700
#define GL_PROJECTION			0x1701
#define GL_TEXTURE			0x1702

/* clear-buffer bits */
#define GL_DEPTH_BUFFER_BIT		0x00000100
#define GL_STENCIL_BUFFER_BIT		0x00000400
#define GL_COLOR_BUFFER_BIT		0x00004000

/* capabilities */
#define GL_CULL_FACE			0x0B44
#define GL_DEPTH_TEST			0x0B71
#define GL_STENCIL_TEST			0x0B90
#define GL_BLEND			0x0BE2
#define GL_FOG				0x0B60
#define GL_ALPHA_TEST			0x0BC0
#define GL_TEXTURE_2D			0x0DE1
#define GL_POLYGON_OFFSET_FILL		0x8037
#define GL_POLYGON_OFFSET_LINE		0x2A02

/* blend factors */
#define GL_SRC_COLOR			0x0300
#define GL_ONE_MINUS_SRC_COLOR		0x0301
#define GL_SRC_ALPHA			0x0302
#define GL_ONE_MINUS_SRC_ALPHA		0x0303
#define GL_DST_ALPHA			0x0304
#define GL_ONE_MINUS_DST_ALPHA		0x0305
#define GL_DST_COLOR			0x0306
#define GL_ONE_MINUS_DST_COLOR		0x0307

/* misc */
#define GL_NO_ERROR			0

/* comparison funcs */
#define GL_EQUAL			0x0202
#define GL_GREATER			0x0204
#define GL_LEQUAL			0x0203
#define GL_GEQUAL			0x0206

/* scissor */
#define GL_SCISSOR_TEST			0x0C11

/* faces / polygon fill */
#define GL_BACK				0x0405
#define GL_FRONT_AND_BACK		0x0408
#define GL_LINE				0x1B01
#define GL_FILL				0x1B02

/* shade model */
#define GL_FLAT				0x1D00
#define GL_SMOOTH			0x1D01

/* stencil ops */
#define GL_KEEP				0x1E00
#define GL_INCR				0x1E02

/* fog */
#define GL_EXP2				0x0801
#define GL_FOG_DENSITY			0x0B62
#define GL_FOG_MODE			0x0B65
#define GL_FOG_COLOR			0x0B66
#define GL_LINEAR			0x2601

/* hints */
#define GL_PERSPECTIVE_CORRECTION_HINT	0x0C50
#define GL_FASTEST			0x1101
#define GL_NICEST			0x1102

/* pixel store */
#define GL_PACK_ALIGNMENT		0x0D05

/* texture filters / params */
#define GL_NEAREST			0x2600
#define GL_LINEAR_TEX			0x2601	/* == GL_LINEAR; alias unused */
#define GL_NEAREST_MIPMAP_NEAREST	0x2700
#define GL_LINEAR_MIPMAP_NEAREST	0x2701
#define GL_NEAREST_MIPMAP_LINEAR	0x2702
#define GL_LINEAR_MIPMAP_LINEAR		0x2703
#define GL_TEXTURE_MAG_FILTER		0x2800
#define GL_TEXTURE_MIN_FILTER		0x2801
#define GL_TEXTURE_WRAP_S		0x2802
#define GL_TEXTURE_WRAP_T		0x2803
#define GL_REPEAT			0x2901
#define GL_MAX_TEXTURE_SIZE		0x0D33
#define GL_MAX_TEXTURE_UNITS		0x84E2

/* glGetString names */
#define GL_VENDOR			0x1F00
#define GL_RENDERER			0x1F01
#define GL_VERSION			0x1F02
#define GL_EXTENSIONS			0x1F03

/* texture env */
#define GL_TEXTURE_ENV			0x2300
#define GL_TEXTURE_ENV_MODE		0x2200
#define GL_DECAL			0x2101
#define GL_MODULATE			0x2100
#define GL_ADD				0x0104
#define GL_REPLACE			0x1E01

/* pixel formats */
#define GL_RGB				0x1907
#define GL_RGBA				0x1908
#define GL_RGBA4			0x8056
#define GL_BGRA				0x80E1
#define GL_RGBA8			0x8058
#define GL_RGB10_A2			0x8059

/* client arrays */
#define GL_VERTEX_ARRAY			0x8074
#define GL_COLOR_ARRAY			0x8076
#define GL_TEXTURE_COORD_ARRAY		0x8078

/* multitexture units (ARB spellings share the base values) */
#define GL_TEXTURE0			0x84C0
#define GL_TEXTURE1			0x84C1
#define GL_TEXTURE2			0x84C2
#define GL_TEXTURE0_ARB			0x84C0
#define GL_TEXTURE1_ARB			0x84C1

/* ARB/EXT texture_env_combine */
#define GL_COMBINE_EXT			0x8570
#define GL_COMBINE_RGB_EXT		0x8571
#define GL_COMBINE_ALPHA_EXT		0x8572
#define GL_RGB_SCALE_EXT		0x8573
#define GL_SOURCE0_RGB_EXT		0x8580
#define GL_SOURCE1_RGB_EXT		0x8581
#define GL_SOURCE0_ALPHA_EXT		0x8588
#define GL_SOURCE1_ALPHA_EXT		0x8589
#define GL_CONSTANT_EXT			0x8576
#define GL_PRIMARY_COLOR_EXT		0x8577
#define GL_PREVIOUS_EXT			0x8578

/* anisotropy */
#define GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF

/* VBO (declarations only; gl_vbo_able == false gates all real use) */
#define GL_ARRAY_BUFFER			0x8892
#define GL_ELEMENT_ARRAY_BUFFER		0x8893
#define GL_STATIC_DRAW			0x88E4
#define GL_ARRAY_BUFFER_ARB		0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ARB	0x8893
#define GL_STATIC_DRAW_ARB		0x88E4

/* GLSL (declarations only; gl_glsl_able == false gates all real use) */
#define GL_FRAGMENT_SHADER		0x8B30
#define GL_VERTEX_SHADER		0x8B31
#define GL_COMPILE_STATUS		0x8B81
#define GL_LINK_STATUS			0x8B82

/* GLdc "KOS" internal-format tokens. No desktop equivalent; values are chosen
   locally-unique (only used for compile-time switch/case distinctness -- the
   consuming upload paths are no-op stubs on the PVR build). */
#define GL_RGB565_KOS			0x5650
#define GL_ARGB4444_KOS			0x5651
#define GL_ARGB1555_KOS			0x5652

/* ---- ARB extension function-pointer typedefs ----------------------------- */
/* multitexture -- referenced through the GL_*Func pointers (kept NULL on DC) */
typedef void (APIENTRYP PFNGLMULTITEXCOORD2FARBPROC)     (GLenum target, GLfloat s, GLfloat t);
typedef void (APIENTRYP PFNGLACTIVETEXTUREARBPROC)       (GLenum texture);
typedef void (APIENTRYP PFNGLCLIENTACTIVETEXTUREARBPROC) (GLenum texture);
/* VBO -- typedefs only; gl_vbo_able == false, pointers never dereferenced */
typedef void (APIENTRYP PFNGLBINDBUFFERARBPROC)    (GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAARBPROC)    (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void (APIENTRYP PFNGLBUFFERSUBDATAARBPROC) (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
typedef void (APIENTRYP PFNGLDELETEBUFFERSARBPROC) (GLsizei n, const GLuint *buffers);
typedef void (APIENTRYP PFNGLGENBUFFERSARBPROC)    (GLsizei n, GLuint *buffers);

/* ---- gl* entry points referenced by the still-compiled render modules ----
   All resolve to pvr_glstub.c no-ops until their path is ported. */
GLAPI void glAlphaFunc (GLenum func, GLclampf ref);
GLAPI void glBegin (GLenum mode);
GLAPI void glEnd (void);
GLAPI void glFlush (void);
GLAPI void glFrontFace (GLenum mode);
GLAPI void glLineWidth (GLfloat width);
GLAPI void glPointSize (GLfloat size);
GLAPI void glNormal3f (GLfloat x, GLfloat y, GLfloat z);
GLAPI void glDrawArrays (GLenum mode, GLint first, GLsizei count);
GLAPI void glOrtho (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
GLAPI void glScissor (GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void glLoadMatrixf (const GLfloat *m);
GLAPI void glLoadTransposeMatrixf (const GLfloat *m);
GLAPI void glMultTransposeMatrixf (const GLfloat *m);
GLAPI void glReadBuffer (GLenum mode);
GLAPI void glRectf (GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
GLAPI void glColor4ub (GLubyte r, GLubyte g, GLubyte b, GLubyte a);
GLAPI void glTexCoord2fv (const GLfloat *v);
GLAPI GLenum glGetError (void);
GLAPI const GLubyte *glGetString (GLenum name);
GLAPI void glGetFloatv (GLenum pname, GLfloat *params);
GLAPI void glBindTexture (GLenum target, GLuint texture);
GLAPI void glBlendFunc (GLenum sfactor, GLenum dfactor);
GLAPI void glClear (GLbitfield mask);
GLAPI void glClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a);
GLAPI void glColor3f (GLfloat r, GLfloat g, GLfloat b);
GLAPI void glColor3fv (const GLfloat *v);
GLAPI void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a);
GLAPI void glColor4fv (const GLfloat *v);
GLAPI void glColor4ubv (const GLubyte *v);
GLAPI void glColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a);
GLAPI void glColorPointer (GLint size, GLenum type, GLsizei stride, const void *ptr);
GLAPI void glCopyTexSubImage2D (GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void glCullFace (GLenum mode);
GLAPI void glDeleteTextures (GLsizei n, const GLuint *textures);
GLAPI void glDepthFunc (GLenum func);
GLAPI void glDepthMask (GLboolean flag);
GLAPI void glDepthRange (GLclampd n, GLclampd f);
GLAPI void glDisable (GLenum cap);
GLAPI void glDisableClientState (GLenum array);
GLAPI void glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices);
GLAPI void glEnable (GLenum cap);
GLAPI void glEnableClientState (GLenum array);
GLAPI void glFinish (void);
GLAPI void glFogf (GLenum pname, GLfloat param);
GLAPI void glFogfv (GLenum pname, const GLfloat *params);
GLAPI void glFogi (GLenum pname, GLint param);
GLAPI void glFrustum (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
GLAPI void glGenTextures (GLsizei n, GLuint *textures);
GLAPI void glGetIntegerv (GLenum pname, GLint *params);
GLAPI void glGetTexImage (GLenum target, GLint level, GLenum format, GLenum type, void *pixels);
GLAPI void glHint (GLenum target, GLenum mode);
GLAPI void glLoadIdentity (void);
GLAPI void glMatrixMode (GLenum mode);
GLAPI void glMultMatrixf (const GLfloat *m);
GLAPI void glPixelStorei (GLenum pname, GLint param);
GLAPI void glPolygonMode (GLenum face, GLenum mode);
GLAPI void glPolygonOffset (GLfloat factor, GLfloat units);
GLAPI void glPopMatrix (void);
GLAPI void glPushMatrix (void);
GLAPI void glReadPixels (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels);
GLAPI void glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
GLAPI void glScalef (GLfloat x, GLfloat y, GLfloat z);
GLAPI void glShadeModel (GLenum mode);
GLAPI void glStencilFunc (GLenum func, GLint ref, GLuint mask);
GLAPI void glStencilOp (GLenum fail, GLenum zfail, GLenum zpass);
GLAPI void glTexCoord2f (GLfloat s, GLfloat t);
GLAPI void glTexCoordPointer (GLint size, GLenum type, GLsizei stride, const void *ptr);
GLAPI void glTexEnvf (GLenum target, GLenum pname, GLfloat param);
GLAPI void glTexEnvi (GLenum target, GLenum pname, GLint param);
GLAPI void glTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels);
GLAPI void glTexParameterf (GLenum target, GLenum pname, GLfloat param);
GLAPI void glTexParameteri (GLenum target, GLenum pname, GLint param);
GLAPI void glTexSubImage2D (GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels);
GLAPI void glTranslatef (GLfloat x, GLfloat y, GLfloat z);
GLAPI void glVertex2f (GLfloat x, GLfloat y);
GLAPI void glVertex3f (GLfloat x, GLfloat y, GLfloat z);
GLAPI void glVertex3fv (const GLfloat *v);
GLAPI void glVertexPointer (GLint size, GLenum type, GLsizei stride, const void *ptr);
GLAPI void glViewport (GLint x, GLint y, GLsizei w, GLsizei h);

#endif /* GL_PVR_TYPES_H */
