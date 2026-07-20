/*
================================================================================
prof_dc.h -- Dreamcast on-screen sampling/instrumentation profiler (public API)

Built only when USE_DC_PROFILER is defined (the Makefile then also compiles the
rest of the engine with -finstrument-functions so every function calls the
__cyg_profile_func_enter/exit hooks in prof_dc.c). Off otherwise -- zero code.

Turn it on at runtime with the "prof" cvar (0 = off, N = show the top N funcs).
Function names come from a prof.syms file loaded at boot (see tools/map2profsyms.py
or just `sh-elf-nm -n quakespasm.elf`); without it, raw addresses are shown.
================================================================================
*/
#ifndef PROF_DC_H
#define PROF_DC_H

#if defined(PLATFORM_DREAMCAST) && defined(USE_DC_PROFILER)

extern cvar_t prof;					// "prof" (0 = off, N = show top N funcs)

void	Prof_Init (void);				// register cvar + load prof.syms (Host_Init)
void	Prof_FrameMark (void);				// once per frame at the loop top
qboolean Prof_Active (void);				// prof cvar != 0
void	Prof_Nav (int dir);				// D-pad: 0 up, 1 down, 2 left, 3 right
const char *Prof_ViewHeader (void);			// header line for the current view
int	Prof_FormatView (char lines[][40], int maxlines);// fill display lines, return count

#endif	/* PLATFORM_DREAMCAST && USE_DC_PROFILER */

#endif	/* PROF_DC_H */
