/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#if defined(_arch_dreamcast)
#include <kos.h>
#endif
#include "quakedef.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <gl4esinit.h>
#endif

#if defined(PLATFORM_DREAMCAST)
#include <malloc.h>
#include <unistd.h>	/* sbrk */
#include <kos/thread.h>
#include <arch/arch.h>	/* _arch_mem_top */
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_CDROM | INIT_CONTROLLER | INIT_KEYBOARD |
               INIT_MOUSE | INIT_VMU | INIT_NET);

#define MAIN_STACK_SIZE (32 * 1024)
static void DC_InitThreadStack (void)
{
	kthread_t *current = thd_get_current ();
	if (current)
	{
		void *new_stack = malloc (MAIN_STACK_SIZE);
		if (new_stack)
		{
			current->stack = new_stack;
			current->stack_size = MAIN_STACK_SIZE;
			current->flags |= THD_OWNS_STACK;
		}
	}
}

static int DC_HeapSize (void)
{
	/* GLdc is gone: the native PVR path submits vertices straight to the TA, so
	   there's no grow-forever main-RAM vertex pool to reserve for. That freed
	   headroom goes to the hunk -- ceiling up to 11MB, reserve trimmed to ~1.5MB
	   for SDL/sound/net/temp allocs that live outside the Quake heap. The extra
	   hunk gives the resident model heap (DC_MHeap) and Cache more room. */
	const size_t reserve = 3 * 512 * 1024;		/* 1.5MB */
	const size_t floor   = MINIMUM_MEMORY_LEVELPAK;	/* never return less than Quake requires */
	const size_t ceiling = 11 * 1024 * 1024;
	uintptr_t brk = (uintptr_t) sbrk (0);
	size_t avail;

	if (brk == 0 || brk == (uintptr_t)-1 || brk >= _arch_mem_top)
		return (int) floor;

	avail = (size_t) (_arch_mem_top - brk);
	if (avail > reserve + floor)
		avail -= reserve;
	if (avail > ceiling)
		avail = ceiling;
	if (avail < floor)
		avail = floor;

	return (int) avail;
}
#endif

static void Sys_AtExit (void)
{
	SDL_Quit();
}

static void Sys_InitSDL (void)
{
#if defined(USE_SDL2)
	SDL_version v;
	SDL_version *sdl_version = &v;
	SDL_GetVersion(&v);
#else
	const SDL_version *sdl_version = SDL_Linked_Version();
#endif

	Sys_Printf("Found SDL version %i.%i.%i\n",sdl_version->major,sdl_version->minor,sdl_version->patch);

	if (SDL_Init(0) < 0) {
		Sys_Error("Couldn't init SDL: %s", SDL_GetError());
	}
	atexit(Sys_AtExit);
}

#define DEFAULT_MEMORY (256 * 1024 * 1024) // ericw -- was 72MB (64-bit) / 64MB (32-bit)

static quakeparms_t	parms;

// On OS X we call SDL_main from the launcher, but SDL2 doesn't redefine main
// as SDL_main on OS X anymore, so we do it ourselves.
#if defined(USE_SDL2) && defined(__APPLE__)
#define main SDL_main
#endif

int main(int argc, char *argv[])
{
#ifdef __EMSCRIPTEN__
	initialize_gl4es();
#endif
	int		t;
	double		time, oldtime, newtime;

#if defined(PLATFORM_DREAMCAST)
	DC_InitThreadStack ();
	SDL_SetHint ("SDL_DC_VIDEO_MODE", "SDL_DC_OPENGL_VIDEO");
	SDL_SetHint ("SDL_VIDEO_DOUBLE_BUFFER", "1");
#endif

	host_parms = &parms;
#if defined(PLATFORM_DREAMCAST)
	parms.basedir = "/cd";
#else
	parms.basedir = ".";
#endif

	parms.argc = argc;
	parms.argv = argv;

	parms.errstate = 0;

	COM_InitArgv(parms.argc, parms.argv);

	isDedicated = (COM_CheckParm("-dedicated") != 0);

	Sys_InitSDL ();

	Sys_Init();

	Sys_Printf("Initializing QuakeSpasm v%s\n", QUAKESPASM_VER_STRING);

#if defined(PLATFORM_DREAMCAST)
	parms.memsize = DC_HeapSize ();
#else
	parms.memsize = DEFAULT_MEMORY;
#endif
	if (COM_CheckParm("-heapsize"))
	{
		t = COM_CheckParm("-heapsize") + 1;
		if (t < com_argc)
			parms.memsize = Q_atoi(com_argv[t]) * 1024;
	}

	parms.membase = malloc (parms.memsize);

	if (!parms.membase)
		Sys_Error ("Not enough memory free; check disk space\n");

	Sys_Printf("Host_Init\n");
	Host_Init();

	oldtime = Sys_DoubleTime();
	if (isDedicated)
	{
		while (1)
		{
			newtime = Sys_DoubleTime ();
			time = newtime - oldtime;

			while (time < sys_ticrate.value )
			{
				SDL_Delay(1);
				newtime = Sys_DoubleTime ();
				time = newtime - oldtime;
			}

			Host_Frame (time);
			oldtime = newtime;
		}
	}
	else
	while (1)
	{
		/* If we have no input focus at all, sleep a bit */
		if (!VID_HasMouseOrInputFocus() || cl.paused)
		{
			SDL_Delay(16);
		}
		/* If we're minimised, sleep a bit more */
		if (VID_IsMinimized())
		{
			scr_skipupdate = 1;
			SDL_Delay(32);
		}
		else
		{
			scr_skipupdate = 0;
		}
		newtime = Sys_DoubleTime ();
		time = newtime - oldtime;

		Host_Frame (time);

		if (time < sys_throttle.value && !cls.timedemo)
			SDL_Delay(1);

		oldtime = newtime;
	}

	return 0;
}
