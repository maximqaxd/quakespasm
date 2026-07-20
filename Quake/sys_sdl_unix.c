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

#include "arch_def.h"
#include "quakedef.h"

#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#if defined(PLATFORM_OSX) || defined(PLATFORM_HAIKU)
#include <libgen.h>	/* dirname() and basename() */
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#ifdef DO_USERDIRS
#include <pwd.h>
#endif

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif


qboolean		isDedicated;
cvar_t		sys_throttle = {"sys_throttle", "0.02", CVAR_ARCHIVE};

#define	MAX_HANDLES		32	/* johnfitz -- was 10 */
static FILE		*sys_handles[MAX_HANDLES];
static qboolean		stdinIsATTY;	/* from ioquake3 source */


static int findhandle (void)
{
	int i;

	for (i = 1; i < MAX_HANDLES; i++)
	{
		if (!sys_handles[i])
			return i;
	}
	Sys_Error ("out of handles");
	return -1;
}

long Sys_filelength (FILE *f)
{
	long		pos, end;

	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	end = ftell (f);
	fseek (f, pos, SEEK_SET);

	return end;
}

int Sys_FileOpenRead (const char *path, int *hndl)
{
	FILE	*f;
	int	i, retval;

	i = findhandle ();
	f = fopen(path, "rb");

	if (!f)
	{
		*hndl = -1;
		retval = -1;
	}
	else
	{
		sys_handles[i] = f;
		*hndl = i;
		retval = Sys_filelength(f);
	}

	return retval;
}

int Sys_FileOpenWrite (const char *path)
{
	FILE	*f;
	int		i;

	i = findhandle ();
	f = fopen(path, "wb");

	if (!f)
		Sys_Error ("Error opening %s: %s", path, strerror(errno));

	sys_handles[i] = f;
	return i;
}

void Sys_FileClose (int handle)
{
	fclose (sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	fseek (sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	return fread (dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite (int handle, const void *data, int count)
{
	return fwrite (data, 1, count, sys_handles[handle]);
}

int Sys_FileType (const char *path)
{
	/*
	if (access(path, R_OK) == -1)
		return 0;
	*/
	struct stat	st;

	if (stat(path, &st) != 0)
		return FS_ENT_NONE;
	if (S_ISDIR(st.st_mode))
		return FS_ENT_DIRECTORY;
	if (S_ISREG(st.st_mode))
		return FS_ENT_FILE;

	return FS_ENT_NONE;
}


#if defined(__linux__) || defined(__sun) || defined(sun) || defined(_AIX)
static int Sys_NumCPUs (void)
{
	int numcpus = sysconf(_SC_NPROCESSORS_ONLN);
	return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(PLATFORM_OSX)
#include <sys/sysctl.h>
#if !defined(HW_AVAILCPU)	/* using an ancient SDK? */
#define HW_AVAILCPU		25	/* needs >= 10.2 */
#endif
static int Sys_NumCPUs (void)
{
	int numcpus;
	int mib[2];
	size_t len;

#if defined(_SC_NPROCESSORS_ONLN)	/* needs >= 10.5 */
	numcpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (numcpus != -1)
		return (numcpus < 1) ? 1 : numcpus;
#endif
	len = sizeof(numcpus);
	mib[0] = CTL_HW;
	mib[1] = HW_AVAILCPU;
	sysctl(mib, 2, &numcpus, &len, NULL, 0);
	if (sysctl(mib, 2, &numcpus, &len, NULL, 0) == -1)
	{
		mib[1] = HW_NCPU;
		if (sysctl(mib, 2, &numcpus, &len, NULL, 0) == -1)
			return 1;
	}
	return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(__sgi) || defined(sgi) || defined(__sgi__) /* IRIX */
static int Sys_NumCPUs (void)
{
	int numcpus = sysconf(_SC_NPROC_ONLN);
	if (numcpus < 1)
		numcpus = 1;
	return numcpus;
}

#elif defined(PLATFORM_BSD)
#include <sys/sysctl.h>
static int Sys_NumCPUs (void)
{
	int numcpus;
	int mib[2];
	size_t len;

#if defined(_SC_NPROCESSORS_ONLN)
	numcpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (numcpus != -1)
		return (numcpus < 1) ? 1 : numcpus;
#endif
	len = sizeof(numcpus);
	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;
	if (sysctl(mib, 2, &numcpus, &len, NULL, 0) == -1)
		return 1;
	return (numcpus < 1) ? 1 : numcpus;
}

#elif defined(__hpux) || defined(__hpux__) || defined(_hpux)
#include <sys/mpctl.h>
static int Sys_NumCPUs (void)
{
	int numcpus = mpctl(MPC_GETNUMSPUS, NULL, NULL);
	return numcpus;
}

#elif defined(PLATFORM_DREAMCAST)
static int Sys_NumCPUs (void)
{
	return 1;	/* single-core SH4 */
}

// Declared here rather than via <arch/arch.h>: that header defines a symbol
// named HZ, which collides with Quake's HZ macro. HW_TYPE_NAOMI is 0xa.
extern int hardware_sys_mode (int *region);
#define DC_HW_TYPE_NAOMI	0xa
/*
================
Sys_IsNaomi -- true on Sega NAOMI arcade hardware (32MB RAM / 16MB VRAM) versus a
retail Dreamcast (16MB / 8MB). Cached: the system-mode register never changes.
================
*/
qboolean Sys_IsNaomi (void)
{
	static int naomi = -1;
	if (naomi < 0)
		naomi = (hardware_sys_mode (NULL) == DC_HW_TYPE_NAOMI);
	return naomi;
}

#include <dc/video.h>		/* vram_s, vid_empty */
#include <dc/biosfont.h>	/* bfont_draw_str, BFONT_* */
#include <kos/dbgio.h>		/* dbgio_dev_select */
extern uint32_t	_arch_mem_top;	/* KOS symbols (avoid <arch/arch.h> -- HZ clash) */
extern char	_etext;

// bfont-draw a string horizontally centered on the 640-wide framebuffer.
static void DC_FatalLine (int y, const char *s)
{
	int	len = (int) strlen (s);
	int	x = (640 - len * BFONT_THIN_WIDTH) / 2;
	if (x < 0) x = 0;
	bfont_draw_str (vram_s + (unsigned) y * 640 + (unsigned) x, 640, true, s);
}

/*
================
Sys_DC_FatalScreen -- clear the screen and print the fatal error centered, with a
stack trace: walk the SH4 stack for words that point just after a call (BSR/BSRF/
JSR) instruction -- those are return addresses. Resolve them offline with
sh-elf-addr2line against the unstripped .elf.
================
*/
static void Sys_DC_FatalScreen (const char *text)
{
	uint32_t	sp = 0, pr = 0;
	char		line[96], tmp[16];
	int		y, so, found = 0, col;

	__asm__ __volatile__ ("mov r15,%0\n\tsts pr,%1\n" : "+r" (sp), "+r" (pr));

	dbgio_dev_select ("scif");	// mirror the trace to the serial port for addr2line
	vid_empty ();			// clear the framebuffer

	y = 60;
	DC_FatalLine (y, "======== FATAL ERROR ========");	y += 40;
	DC_FatalLine (y, text);					y += 40;
	DC_FatalLine (y, "(system halted)");			y += 48;
	DC_FatalLine (y, "STACK TRACE");			y += 30;

	printf ("\nSTACK: %08lx ", (unsigned long) pr);
	q_snprintf (line, sizeof(line), "%08lx ", (unsigned long) pr);
	col = 1;

	if (!(sp & 3) && sp > 0x8c000000 && sp < _arch_mem_top)
	{
		char	**spp = (char **) sp;
		for (so = 0; so < 16384; so++)
		{
			char		*p;
			unsigned short	*ip, in;

			if ((uintptr_t) &spp[so] >= _arch_mem_top)
				break;
			p = spp[so];
			if (p <= (char *) 0x8c000000 || p >= &_etext || ((uintptr_t) p & 1))
				continue;
			ip = (unsigned short *) p;
			in = ip[-2];	// the word before a return address is the call
			if (((in & 0xf000) == 0xb000) ||	// BSR
			    ((in & 0xf0ff) == 0x0003) ||	// BSRF Rn
			    ((in & 0xf0ff) == 0x400b))		// JSR @Rn
			{
				q_snprintf (tmp, sizeof(tmp), "%08lx ", (unsigned long) (uintptr_t) p);
				printf ("%s", tmp);
				q_strlcat (line, tmp, sizeof(line));
				if (++col >= 5)
				{
					DC_FatalLine (y, line);
					y += 28;
					line[0] = 0;
					col = 0;
				}
				if (++found > 20)
					break;
			}
		}
	}
	if (line[0])
		DC_FatalLine (y, line);
	printf ("\n");
	fflush (stdout);
}

#else /* unknown OS */
static int Sys_NumCPUs (void)
{
	return -2;
}
#endif

static char	cwd[MAX_OSPATH];
#ifdef DO_USERDIRS
static char	userdir[MAX_OSPATH];
#ifdef PLATFORM_OSX
#define SYS_USERDIR	"Library/Application Support/QuakeSpasm"
#elif defined(PLATFORM_HAIKU)
#define SYS_USERDIR	"QuakeSpasm"
#else
#define SYS_USERDIR	".quakespasm"
#endif

static qboolean Sys_GetUserdirArgs (int argc, char **argv, char *dst, size_t dstsize)
{
	int i = 1;
	for (; i < argc - 1; ++i)
	{
		if (strcmp(argv[i], "-userdir") == 0)
		{
			char *p = dst;
			const char * arg = argv[i + 1];
			const int n = (int)strlen(arg);
			if (n < 1) Sys_Error("Bad argument to -userdir");
			if (q_strlcpy(dst, arg, dstsize) >= dstsize)
				Sys_Error ("Insufficient array size for userspace directory");
			if (dst[n - 1] == '/') dst[n - 1] = 0;
			if (*p == '/') p++;
			for (; *p; p++) {
				const char c = *p;
				if (c == '/') {
					*p = 0;
					Sys_mkdir (dst);
					*p = c;
				}
			}
			return true;
		}
	}
	return false;
}

#ifdef PLATFORM_HAIKU
#include <FindDirectory.h>
#include <fs_info.h>

static void Sys_GetUserdir (int argc, char **argv, char *dst, size_t dstsize)
{
	dev_t volume = dev_for_path("/boot");
	char buffer[B_PATH_NAME_LENGTH];
	status_t result;

	if (Sys_GetUserdirArgs(argc, argv, dst, dstsize))
		return;

	result = find_directory(B_USER_NONPACKAGED_DATA_DIRECTORY, volume, false, buffer, sizeof(buffer));
	if (result != B_OK)
		Sys_Error ("Couldn't determine userspace directory");

	q_snprintf (dst, dstsize, "%s/%s", buffer, SYS_USERDIR);
}
#else
static void Sys_GetUserdir (int argc, char **argv, char *dst, size_t dstsize)
{
	size_t		n;
	const char	*home_dir = NULL;
	struct passwd	*pwent;

	if (Sys_GetUserdirArgs(argc, argv, dst, dstsize))
		return;

	pwent = getpwuid( getuid() );
	if (pwent == NULL)
		perror("getpwuid");
	else
		home_dir = pwent->pw_dir;
	if (home_dir == NULL)
		home_dir = getenv("HOME");
	if (home_dir == NULL)
		Sys_Error ("Couldn't determine userspace directory");

/* what would be a maximum path for a file in the user's directory...
 * $HOME/SYS_USERDIR/game_dir/dirname1/dirname2/dirname3/filename.ext
 * still fits in the MAX_OSPATH == 256 definition, but just in case :
 */
	n = strlen(home_dir) + strlen(SYS_USERDIR) + 50;
	if (n >= dstsize)
		Sys_Error ("Insufficient array size for userspace directory");

	q_snprintf (dst, dstsize, "%s/%s", home_dir, SYS_USERDIR);
}
#endif	/* PLATFORM_HAIKU */
#endif	/* DO_USERDIRS */

#ifdef PLATFORM_OSX
static char *OSX_StripAppBundle (char *dir)
{ /* based on the ioquake3 project at icculus.org. */
	static char	osx_path[MAX_OSPATH];

	q_strlcpy (osx_path, dir, sizeof(osx_path));
	if (strcmp(basename(osx_path), "MacOS"))
		return dir;
	q_strlcpy (osx_path, dirname(osx_path), sizeof(osx_path));
	if (strcmp(basename(osx_path), "Contents"))
		return dir;
	q_strlcpy (osx_path, dirname(osx_path), sizeof(osx_path));
	if (!strstr(basename(osx_path), ".app"))
		return dir;
	q_strlcpy (osx_path, dirname(osx_path), sizeof(osx_path));
	return osx_path;
}

static void Sys_GetBasedir (char *argv0, char *dst, size_t dstsize)
{
	char	*tmp;

	if (realpath(argv0, dst) == NULL)
	{
		perror("realpath");
		if (getcwd(dst, dstsize - 1) == NULL)
	_fail:		Sys_Error ("Couldn't determine current directory");
	}
	else
	{
		/* strip off the binary name */
		if (! (tmp = strdup (dst))) goto _fail;
		q_strlcpy (dst, dirname(tmp), dstsize);
		free (tmp);
	}

	tmp = OSX_StripAppBundle(dst);
	if (tmp != dst)
		q_strlcpy (dst, tmp, dstsize);
}
#else
static void Sys_GetBasedir (char *argv0, char *dst, size_t dstsize)
{
	char	*tmp;

	#ifdef PLATFORM_HAIKU
	if (realpath(argv0, dst) == NULL)
	{
		perror("realpath");
		if (getcwd(dst, dstsize - 1) == NULL)
	_fail:		Sys_Error ("Couldn't determine current directory");
	}
	else
	{
		/* strip off the binary name */
		if (! (tmp = strdup (dst))) goto _fail;
		q_strlcpy (dst, dirname(tmp), dstsize);
		free (tmp);
	}
	#else
	if (getcwd(dst, dstsize - 1) == NULL)
		Sys_Error ("Couldn't determine current directory");

	tmp = dst;
	while (*tmp != 0)
		tmp++;
	while (*tmp == 0 && tmp != dst)
	{
		--tmp;
		if (tmp != dst && *tmp == '/')
			*tmp = 0;
	}
	#endif
}
#endif

void Sys_Init (void)
{
	const char* term = getenv("TERM");
	stdinIsATTY = isatty(STDIN_FILENO) &&
			!(term && (!strcmp(term, "raw") || !strcmp(term, "dumb")));
	if (!stdinIsATTY)
		Sys_Printf("Terminal input not available.\n");

	memset (cwd, 0, sizeof(cwd));
	Sys_GetBasedir(host_parms->argv[0], cwd, sizeof(cwd));
#if !defined(PLATFORM_DREAMCAST)
	host_parms->basedir = cwd;
#else
	/* Dreamcast: cwd is "/" (KOS root); keep the "/cd" basedir set in main(). */
#endif
#ifndef DO_USERDIRS
	host_parms->userdir = host_parms->basedir; /* code elsewhere relies on this ! */
#else
	memset (userdir, 0, sizeof(userdir));
	Sys_GetUserdir (host_parms->argc, host_parms->argv, userdir, sizeof(userdir));
	Sys_mkdir (userdir);
	host_parms->userdir = userdir;
#endif
	host_parms->numcpus = Sys_NumCPUs ();
	Sys_Printf("Detected %d CPUs.\n", host_parms->numcpus);
}

void Sys_mkdir (const char *path)
{
	int rc = mkdir (path, 0777);
	if (rc != 0 && errno == EEXIST)
	{
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			rc = 0;
	}
	if (rc != 0)
	{
		rc = errno;
		Sys_Error("Unable to create directory %s: %s", path, strerror(rc));
	}
}

static const char errortxt1[] = "\nERROR-OUT BEGIN\n\n";
static const char errortxt2[] = "\nQUAKE ERROR: ";

void Sys_Error (const char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	host_parms->errstate++;

	va_start (argptr, error);
	q_vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

#if defined(PLATFORM_DREAMCAST)
	// No terminal on the Dreamcast, and exiting drops straight to the BIOS before
	// the error can be read. Draw the error centered on screen with a stack trace,
	// and hang so it stays on the TV.
	Sys_DC_FatalScreen (text);
	for (;;)
		usleep (100000);
#endif

	fputs (errortxt1, stderr);
	Host_Shutdown ();
	fputs (errortxt2, stderr);
	fputs (text, stderr);
	fputs ("\n\n", stderr);
	if (!isDedicated)
		PL_ErrorDialog(text);

	exit (1);
}

void Sys_Printf (const char *fmt, ...)
{
	va_list argptr;
#if defined(PLATFORM_DREAMCAST)
	char		text[1024];
	unsigned char	*p;

	va_start (argptr, fmt);
	q_vsnprintf (text, sizeof(text), fmt, argptr);
	va_end (argptr);

	// Quake's console font stores its glyphs (colored/brown text) in the high
	// bit; strip it so the real terminal shows plain ASCII instead of codepage
	// garbage. The in-game console decodes the high bit itself.
	for (p = (unsigned char *)text; *p; p++)
		*p &= 0x7f;
	fputs (text, stdout);
	fflush (stdout);
#else
	va_start(argptr, fmt);
	vprintf(fmt, argptr);
	va_end(argptr);
#endif
}

void Sys_Quit (void)
{
	Host_Shutdown();

	exit (0);
}

double Sys_DoubleTime (void)
{
	return SDL_GetTicks() / 1000.0;
}

const char *Sys_ConsoleInput (void)
{
	static qboolean	con_eof = false;
	static char	con_text[256];
	static int	textlen;
	char		c;
	fd_set		set;
	struct timeval	timeout;

	if (!stdinIsATTY || con_eof)
		return NULL;

	FD_ZERO (&set);
	FD_SET (0, &set);	// stdin
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;

	while (select (1, &set, NULL, NULL, &timeout))
	{
		if (read(0, &c, 1) <= 0)
		{
			// Finish processing whatever is already in the
			// buffer (if anything), then stop reading
			con_eof = true;
			c = '\n';
		}
		if (c == '\n' || c == '\r')
		{
			con_text[textlen] = '\0';
			textlen = 0;
			return con_text;
		}
		else if (c == 8)
		{
			if (textlen)
			{
				textlen--;
				con_text[textlen] = '\0';
			}
			continue;
		}
		con_text[textlen] = c;
		textlen++;
		if (textlen < (int) sizeof(con_text))
			con_text[textlen] = '\0';
		else
		{
		// buffer is full
			textlen = 0;
			con_text[0] = '\0';
			Sys_Printf("\nConsole input too long!\n");
			break;
		}
	}

	return NULL;
}

void Sys_Sleep (unsigned long msecs)
{
/*	usleep (msecs * 1000);*/
	SDL_Delay (msecs);
}

void Sys_SendKeyEvents (void)
{
	IN_Commands();		//ericw -- allow joysticks to add keys so they can be used to confirm SCR_ModalMessage
	IN_SendKeyEvents();
}

