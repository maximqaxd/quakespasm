/*
================================================================================
pvr_glstub.c -- no-op GL stubs for the native PVR build

The native PVR renderer does not link or include GLdc. The renderer-agnostic
gl_/r_ modules are still compiled, though, and their non-PVR code paths still
reference gl* symbols the linker needs to resolve. Rather than link GLdc, those
symbols are resolved here with harmless no-ops; any such path that is reached
simply draws nothing instead of hanging the TA.

The stubs use the same prototypes the rest of the engine sees (gl_pvr_types.h),
so LTO doesn't flag a type mismatch and the compiler checks the bodies against
the declarations.
================================================================================
*/
#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include "gl_pvr_types.h"

// --- functions with outputs / return values ----------------------------------
GLenum glGetError (void) { return GL_NO_ERROR; }
const GLubyte *glGetString (GLenum name) { (void)name; return (const GLubyte *)""; }
void glGenTextures (GLsizei n, GLuint *t) { GLsizei i; for (i = 0; i < n; i++) t[i] = 1; }
void glGetIntegerv (GLenum pname, GLint *v) { (void)pname; if (v) *v = 0; }
void glGetFloatv (GLenum pname, GLfloat *v) { (void)pname; if (v) *v = 0.0f; }

// --- pure side-effect entry points: no-op -----------------------------------
void glAlphaFunc (GLenum func, GLclampf ref) {}
void glBegin (GLenum mode) {}
void glEnd (void) {}
void glFlush (void) {}
void glFinish (void) {}
void glFrontFace (GLenum mode) {}
void glCullFace (GLenum mode) {}
void glLineWidth (GLfloat w) {}
void glPointSize (GLfloat s) {}
void glNormal3f (GLfloat x, GLfloat y, GLfloat z) {}
void glBindTexture (GLenum target, GLuint texture) {}
void glBlendFunc (GLenum s, GLenum d) {}
void glClear (GLbitfield mask) {}
void glClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a) {}
void glColor3f (GLfloat r, GLfloat g, GLfloat b) {}
void glColor3fv (const GLfloat *v) {}
void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a) {}
void glColor4fv (const GLfloat *v) {}
void glColor4ub (GLubyte r, GLubyte g, GLubyte b, GLubyte a) {}
void glColor4ubv (const GLubyte *v) {}
void glColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a) {}
void glColorPointer (GLint size, GLenum type, GLsizei stride, const void *ptr) {}
void glCopyTexSubImage2D (GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {}
void glDeleteTextures (GLsizei n, const GLuint *t) {}
void glDepthFunc (GLenum func) {}
void glDepthMask (GLboolean flag) {}
void glDepthRange (GLclampd n, GLclampd f) {}
void glDisable (GLenum cap) {}
void glDisableClientState (GLenum a) {}
void glDrawArrays (GLenum mode, GLint first, GLsizei count) {}
void glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices) {}
void glEnable (GLenum cap) {}
void glEnableClientState (GLenum a) {}
void glFogf (GLenum pname, GLfloat param) {}
void glFogfv (GLenum pname, const GLfloat *params) {}
void glFogi (GLenum pname, GLint param) {}
void glFrustum (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {}
void glOrtho (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {}
void glGetTexImage (GLenum target, GLint level, GLenum format, GLenum type, void *pixels) {}
void glHint (GLenum target, GLenum mode) {}
void glLoadIdentity (void) {}
void glLoadMatrixf (const GLfloat *m) {}
void glLoadTransposeMatrixf (const GLfloat *m) {}
void glMatrixMode (GLenum mode) {}
void glMultMatrixf (const GLfloat *m) {}
void glMultTransposeMatrixf (const GLfloat *m) {}
void glPixelStorei (GLenum pname, GLint param) {}
void glPolygonMode (GLenum face, GLenum mode) {}
void glPolygonOffset (GLfloat factor, GLfloat units) {}
void glPopMatrix (void) {}
void glPushMatrix (void) {}
void glReadBuffer (GLenum mode) {}
void glReadPixels (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels) {}
void glRectf (GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {}
void glRotatef (GLfloat a, GLfloat x, GLfloat y, GLfloat z) {}
void glScalef (GLfloat x, GLfloat y, GLfloat z) {}
void glScissor (GLint x, GLint y, GLsizei w, GLsizei h) {}
void glShadeModel (GLenum mode) {}
void glStencilFunc (GLenum func, GLint ref, GLuint mask) {}
void glStencilOp (GLenum sfail, GLenum zfail, GLenum zpass) {}
void glTexCoord2f (GLfloat s, GLfloat t) {}
void glTexCoord2fv (const GLfloat *v) {}
void glTexCoordPointer (GLint size, GLenum type, GLsizei stride, const void *ptr) {}
void glTexEnvf (GLenum target, GLenum pname, GLfloat param) {}
void glTexEnvi (GLenum target, GLenum pname, GLint param) {}
void glTexImage2D (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels) {}
void glTexParameterf (GLenum target, GLenum pname, GLfloat param) {}
void glTexParameteri (GLenum target, GLenum pname, GLint param) {}
void glTexSubImage2D (GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) {}
void glTranslatef (GLfloat x, GLfloat y, GLfloat z) {}
void glVertex2f (GLfloat x, GLfloat y) {}
void glVertex3f (GLfloat x, GLfloat y, GLfloat z) {}
void glVertex3fv (const GLfloat *v) {}
void glVertexPointer (GLint size, GLenum type, GLsizei stride, const void *ptr) {}
void glViewport (GLint x, GLint y, GLsizei w, GLsizei h) {}

// --- glKos*: not used by this renderer -- pvr_backend owns PVR init/swap.
// These exist solely to satisfy libSDL2's Dreamcast GL video backend
// (SDL_dreamcastopengl.c), which the SDL video subsystem drags into the link
// even though the PVR path never creates an SDL GL context. No-ops: never run.
// (void)-typed on purpose: matching the real GLdcConfig* argument would require
// pulling in <GL/glkos.h>, the header the PVR path deliberately avoids.
void glKosInit (void) {}
void glKosInitEx (void) {}
void glKosInitConfig (void) {}
void glKosSwapBuffers (void) {}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
