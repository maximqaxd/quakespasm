/*
================================================================================
pvr_glstub.c -- no-op GLdc stubs for the native PVR build (maximqad)

De-GLfying, step 1 (linking): the native PVR renderer no longer links libGL. But
several render paths that aren't ported yet (alias models, sprites, particles,
sky, fog, screen-scale) are still compiled and reference gl* / glKos* symbols.
Rather than keep linking GLdc -- whose entry points would crash if reached
uninitialized -- we resolve those symbols here with harmless no-ops. Any not-yet-
ported path that slips through simply draws nothing instead of hanging the TA.

Each stub is defined with (void) parameters: this TU never includes <GL/gl.h>, so
there's no prototype conflict, and callers in other TUs pass their arguments per
the real prototype -- the stub just ignores them in-register. As each path is
ported to a pvr_ module and swapped out of the build, its symbols drop off this
list; when the list is empty this file goes away.
================================================================================
*/
#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

// --- functions with outputs / return values: real-enough signatures ----------
unsigned int glGetError (void) { return 0; }			// GL_NO_ERROR
const unsigned char *glGetString (void) { return (const unsigned char *)""; }
void glGenTextures (int n, unsigned int *t) { int i; for (i = 0; i < n; i++) t[i] = 1; }
void glGetIntegerv (unsigned int pname, int *v) { (void)pname; if (v) *v = 0; }
void glGetFloatv (unsigned int pname, float *v) { (void)pname; if (v) *v = 0.0f; }

// --- pure side-effect entry points: no-op (args ignored) ---------------------
void glAlphaFunc (void) {}
void glBegin (void) {}
void glBindTexture (void) {}
void glBlendFunc (void) {}
void glClear (void) {}
void glClearColor (void) {}
void glColor3f (void) {}
void glColor3fv (void) {}
void glColor4f (void) {}
void glColor4fv (void) {}
void glColor4ub (void) {}
void glColorMask (void) {}
void glColorPointer (void) {}
void glCullFace (void) {}
void glDeleteTextures (void) {}
void glDepthFunc (void) {}
void glDepthMask (void) {}
void glDisable (void) {}
void glDisableClientState (void) {}
void glDrawArrays (void) {}
void glEnable (void) {}
void glEnableClientState (void) {}
void glEnd (void) {}
void glFinish (void) {}
void glFlush (void) {}
void glFogf (void) {}
void glFogfv (void) {}
void glFogi (void) {}
void glFrontFace (void) {}
void glHint (void) {}
void glLineWidth (void) {}
void glLoadIdentity (void) {}
void glLoadMatrixf (void) {}
void glLoadTransposeMatrixf (void) {}
void glMatrixMode (void) {}
void glMultMatrixf (void) {}
void glMultTransposeMatrixf (void) {}
void glNormal3f (void) {}
void glOrtho (void) {}
void glPixelStorei (void) {}
void glPointSize (void) {}
void glPopMatrix (void) {}
void glPushMatrix (void) {}
void glReadBuffer (void) {}
void glReadPixels (void) {}
void glRectf (void) {}
void glRotatef (void) {}
void glScalef (void) {}
void glScissor (void) {}
void glShadeModel (void) {}
void glTexCoord2f (void) {}
void glTexCoord2fv (void) {}
void glTexCoordPointer (void) {}
void glTexEnvf (void) {}
void glTexEnvi (void) {}
void glTexImage2D (void) {}
void glTexParameteri (void) {}
void glTexSubImage2D (void) {}
void glTranslatef (void) {}
void glVertex2f (void) {}
void glVertex3fv (void) {}
void glVertexPointer (void) {}
void glViewport (void) {}

// --- GLdc/KOS GL init + swap: never used on the PVR path (pvr_backend owns it) -
void glKosInit (void) {}
void glKosInitEx (void) {}
void glKosInitConfig (void) {}
void glKosSwapBuffers (void) {}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
