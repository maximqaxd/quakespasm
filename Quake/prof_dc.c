/*
================================================================================
prof_dc.c -- Dreamcast function-instrumentation profiler with on-screen readout

When the engine is built with -finstrument-functions (Makefile USE_DC_PROFILER=1)
every function calls __cyg_profile_func_enter on entry and __cyg_profile_func_exit
on return. We time each call with the SH4 performance counter and accumulate:

  - per FUNCTION (g_tab):  call count, inclusive time (whole call) and exclusive
    "self" time (inclusive minus time spent in callees), via a shadow call stack.
  - per EDGE (g_edge):     caller -> callee time, so a function can be "opened" to
    see which of its callees cost what -- the internals view.

Readings accumulate continuously (reset when the profiler is switched on) and the
overlay divides by the frame count, so the columns are stable per-frame averages.

On-screen navigator (D-pad while the "prof" cvar is on and you're in-game):
  UP    -> top functions view          (back out to the overview)
  LEFT  -> open the selected function   (drill into its callees)
  RIGHT -> move the selection to the next row
  DOWN  -> profiler off

Function names resolve through /cd/prof.syms (address -> name, binary-searched);
without it, raw 0x8c... addresses are shown.

This file is compiled WITHOUT instrumentation (Makefile exclude + no_instrument_
function on every routine) so the hooks never recurse into themselves.
================================================================================
*/
#include "quakedef.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_DC_PROFILER)

#include <dc/perfctr.h>		/* perf_cntr_timer_ns, perf_cntr_timer_enable */
#include <stdio.h>

#define NOINSTR __attribute__((no_instrument_function))

// ---- tunables ----
#define PROF_HASH_SIZE	4096
#define PROF_HASH_MASK	(PROF_HASH_SIZE - 1)
#define PROF_EDGE_SIZE	8192
#define PROF_EDGE_MASK	(PROF_EDGE_SIZE - 1)
#define PROF_STACK_MAX	1024			// max nested call depth tracked
#define PROF_VIEWMAX	18			// most rows we display / navigate
#define PROF_FOCUS_MAX	8			// drill-in depth

typedef struct {
	uint32_t	addr;		// function address (0 = empty slot)
	uint32_t	calls;
	uint64_t	self_ns;	// exclusive (minus callees)
	uint64_t	incl_ns;	// inclusive (whole call)
} prof_entry_t;

typedef struct {
	uint32_t	parent;
	uint32_t	child;		// 0 = empty slot
	uint32_t	calls;
	uint64_t	incl_ns;	// time in child while called from parent
} prof_edge_t;

typedef struct {
	uint32_t	addr;
	uint64_t	enter_ns;
	uint64_t	child_ns;
} prof_stackframe_t;

static prof_entry_t	g_tab[PROF_HASH_SIZE];
static prof_edge_t	g_edge[PROF_EDGE_SIZE];
static prof_stackframe_t g_stack[PROF_STACK_MAX];
static int		g_depth;
static int		g_active;
static int		g_in_hook;
static int		g_frames;
static int		g_last_on;		// edge-detect prof 0 -> N to reset

// navigator state
static int		g_view;			// 0 = top, 1 = focus
static uint32_t		g_focus[PROF_FOCUS_MAX];	// drill-in stack of function addrs
static int		g_focus_sp;
static int		g_cursor;

// Default on (12 rows) in a profiler build -- this binary only exists to profile.
// Archived so the setting (and D-pad Down -> off) persists in config.cfg.
cvar_t	prof = {"prof", "12", CVAR_ARCHIVE};

// ---- symbol table (address -> name), sorted ascending by address ------------
static uint32_t		*g_sym_addr;
static char		**g_sym_name;
static int		 g_sym_count;
static char		*g_sym_pool;

static NOINSTR prof_entry_t *tab_find (uint32_t addr)
{
	uint32_t	h = (addr >> 2) & PROF_HASH_MASK;
	int		i;
	for (i = 0; i < PROF_HASH_SIZE; i++)
	{
		prof_entry_t *e = &g_tab[(h + i) & PROF_HASH_MASK];
		if (e->addr == addr)	return e;
		if (e->addr == 0)	{ e->addr = addr; return e; }
	}
	return NULL;
}

static NOINSTR prof_edge_t *edge_find (uint32_t parent, uint32_t child)
{
	uint32_t	h = ((parent * 2654435761u) ^ (child * 40503u)) & PROF_EDGE_MASK;
	int		i;
	for (i = 0; i < PROF_EDGE_SIZE; i++)
	{
		prof_edge_t *e = &g_edge[(h + i) & PROF_EDGE_MASK];
		if (e->child == 0)	{ e->parent = parent; e->child = child; return e; }
		if (e->parent == parent && e->child == child) return e;
	}
	return NULL;
}

// ---- the compiler-inserted hooks --------------------------------------------

void NOINSTR __cyg_profile_func_enter (void *this_fn, void *call_site)
{
	(void) call_site;
	if (!g_active || g_in_hook)
		return;
	g_in_hook = 1;
	{
		int d = g_depth++;
		if (d < PROF_STACK_MAX)
		{
			prof_stackframe_t *f = &g_stack[d];
			f->addr = (uint32_t)(uintptr_t) this_fn;
			f->child_ns = 0;
			f->enter_ns = perf_cntr_timer_ns ();
		}
	}
	g_in_hook = 0;
}

void NOINSTR __cyg_profile_func_exit (void *this_fn, void *call_site)
{
	(void) this_fn;
	(void) call_site;
	if (!g_active || g_in_hook)
		return;
	if (g_depth <= 0)
		return;
	g_in_hook = 1;
	{
		int d = --g_depth;
		if (d < PROF_STACK_MAX)
		{
			uint64_t		now = perf_cntr_timer_ns ();
			prof_stackframe_t	*f = &g_stack[d];
			uint64_t		elapsed = now - f->enter_ns;
			uint32_t		parent = (d > 0) ? g_stack[d - 1].addr : 0;
			prof_entry_t		*e = tab_find (f->addr);
			prof_edge_t		*ed;

			if (e)
			{
				e->calls++;
				e->incl_ns += elapsed;
				e->self_ns += (elapsed - f->child_ns);
			}
			ed = edge_find (parent, f->addr);
			if (ed)
			{
				ed->calls++;
				ed->incl_ns += elapsed;
			}
			if (d > 0)
				g_stack[d - 1].child_ns += elapsed;
		}
	}
	g_in_hook = 0;
}

// ---- symbol resolution ------------------------------------------------------

static NOINSTR void Prof_LoadSyms (void)
{
	FILE	*f;
	char	line[256];
	long	fsize;
	int	cap = 0, i, j;

	f = fopen ("/cd/prof.syms", "r");
	if (!f)
		f = fopen ("/cd/PROF.SYMS", "r");
	if (!f)
	{
		Con_Printf ("prof: no /cd/prof.syms -- showing raw addresses\n");
		return;
	}

	fseek (f, 0, SEEK_END);
	fsize = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (fsize <= 0) { fclose (f); return; }

	cap = (int)(fsize / 12) + 16;
	g_sym_addr = (uint32_t *) malloc (cap * sizeof(uint32_t));
	g_sym_name = (char **) malloc (cap * sizeof(char *));
	g_sym_pool = (char *) malloc (fsize + 1);
	if (!g_sym_addr || !g_sym_name || !g_sym_pool)
	{
		Con_Printf ("prof: out of memory loading syms\n");
		free (g_sym_addr); free (g_sym_name); free (g_sym_pool);
		g_sym_addr = NULL; g_sym_name = NULL; g_sym_pool = NULL;
		fclose (f);
		return;
	}

	{
		char *pool = g_sym_pool;
		while (g_sym_count < cap && fgets (line, sizeof(line), f))
		{
			unsigned long	addr;
			char		*p = line, *tok, *name = NULL;

			addr = strtoul (p, &p, 16);
			if (addr < 0x8c000000UL || addr >= 0x8d000000UL)
				continue;
			for (;;)		// last whitespace-token is the name
			{
				while (*p == ' ' || *p == '\t') p++;
				if (*p == '\0' || *p == '\n' || *p == '\r') break;
				tok = p;
				while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
				if (*p) *p++ = '\0';
				name = tok;
			}
			if (!name) continue;
			{
				size_t len = strlen (name) + 1;
				memcpy (pool, name, len);
				g_sym_addr[g_sym_count] = (uint32_t) addr;
				g_sym_name[g_sym_count] = pool;
				pool += len;
				g_sym_count++;
			}
		}
	}
	fclose (f);

	for (i = 1; i < g_sym_count; i++)	// insertion sort by address
	{
		uint32_t a = g_sym_addr[i];
		char *n = g_sym_name[i];
		for (j = i - 1; j >= 0 && g_sym_addr[j] > a; j--)
		{
			g_sym_addr[j + 1] = g_sym_addr[j];
			g_sym_name[j + 1] = g_sym_name[j];
		}
		g_sym_addr[j + 1] = a;
		g_sym_name[j + 1] = n;
	}
	Con_Printf ("prof: loaded %i symbols\n", g_sym_count);
}

static NOINSTR const char *Prof_Sym (uint32_t addr)
{
	static char	hex[16];
	int		lo = 0, hi = g_sym_count - 1, best = -1;

	if (addr == 0)
		return "(root)";
	while (lo <= hi)
	{
		int mid = (lo + hi) >> 1;
		if (g_sym_addr[mid] <= addr) { best = mid; lo = mid + 1; }
		else hi = mid - 1;
	}
	if (best >= 0)
		return g_sym_name[best];
	q_snprintf (hex, sizeof(hex), "0x%08x", (unsigned) addr);
	return hex;
}

// ---- view construction ------------------------------------------------------

typedef struct { uint32_t addr; uint64_t self_ns; uint64_t incl_ns; uint32_t calls; } prof_row_t;

/*
==============
Prof_BuildView -- fill rows[] for the current view, sorted by cost descending:
  top view   -> every function by self time
  focus view -> the callees of the focused function (edges) by inclusive time
Returns the row count.
==============
*/
static NOINSTR int Prof_BuildView (prof_row_t *rows, int max)
{
	int	count = 0, i, s;

	if (g_view == 0)
	{
		for (s = 0; s < max; s++)
		{
			uint64_t best = 0; int bi = -1;
			for (i = 0; i < PROF_HASH_SIZE; i++)
			{
				int k, taken = 0;
				if (g_tab[i].addr == 0 || g_tab[i].self_ns == 0) continue;
				for (k = 0; k < count; k++)
					if (rows[k].addr == g_tab[i].addr) { taken = 1; break; }
				if (taken) continue;
				if (g_tab[i].self_ns > best) { best = g_tab[i].self_ns; bi = i; }
			}
			if (bi < 0) break;
			rows[count].addr = g_tab[bi].addr;
			rows[count].self_ns = g_tab[bi].self_ns;
			rows[count].incl_ns = g_tab[bi].incl_ns;
			rows[count].calls = g_tab[bi].calls;
			count++;
		}
	}
	else
	{
		uint32_t focus = g_focus[g_focus_sp - 1];
		for (s = 0; s < max; s++)
		{
			uint64_t best = 0; int bi = -1;
			for (i = 0; i < PROF_EDGE_SIZE; i++)
			{
				int k, taken = 0;
				if (g_edge[i].child == 0 || g_edge[i].parent != focus) continue;
				for (k = 0; k < count; k++)
					if (rows[k].addr == g_edge[i].child) { taken = 1; break; }
				if (taken) continue;
				if (g_edge[i].incl_ns > best) { best = g_edge[i].incl_ns; bi = i; }
			}
			if (bi < 0) break;
			rows[count].addr = g_edge[bi].child;
			rows[count].self_ns = 0;
			rows[count].incl_ns = g_edge[bi].incl_ns;
			rows[count].calls = g_edge[bi].calls;
			count++;
		}
	}
	return count;
}

// ---- public: navigation, formatting, frame mark -----------------------------

qboolean NOINSTR Prof_Active (void)
{
	return prof.value != 0;
}

/*
==============
Prof_Nav -- D-pad navigator (menu-style). dir: 0 up, 1 down, 2 left, 3 right.

  UP / DOWN : move the selection cursor (scroll the list, wraps)
  RIGHT     : drill INTO the selected function (its callees) -- no-op if it has
              no instrumented callees, so the view never blanks
  LEFT      : back out one level; at the top level this turns the profiler OFF

Off lives on "back out past the top" (not a bare Down press) so you don't lose the
overlay -- and hand the D-pad back to weapons/movement -- by accident.
==============
*/
void NOINSTR Prof_Nav (int dir)
{
	prof_row_t	rows[PROF_VIEWMAX];
	int		n;

	switch (dir)
	{
	case 0:		// UP -> previous row
		n = Prof_BuildView (rows, PROF_VIEWMAX);
		if (n > 0)
			g_cursor = (g_cursor + n - 1) % n;
		break;

	case 1:		// DOWN -> next row
		n = Prof_BuildView (rows, PROF_VIEWMAX);
		if (n > 0)
			g_cursor = (g_cursor + 1) % n;
		break;

	case 3:		// RIGHT -> drill into the selected function
		n = Prof_BuildView (rows, PROF_VIEWMAX);
		if (n > 0 && g_cursor < n && g_focus_sp < PROF_FOCUS_MAX)
		{
			g_focus[g_focus_sp++] = rows[g_cursor].addr;
			g_view = 1;
			if (Prof_BuildView (rows, PROF_VIEWMAX) == 0)
			{	// leaf (callees not instrumented): don't blank -- revert
				g_focus_sp--;
				g_view = (g_focus_sp > 0);
			}
			else
				g_cursor = 0;
		}
		break;

	case 2:		// LEFT -> back out one level; off past the top
		if (g_focus_sp > 0)
		{
			g_focus_sp--;
			g_view = (g_focus_sp > 0);
			g_cursor = 0;
		}
		else
			Cvar_Set ("prof", "0");
		break;
	}
}

const char * NOINSTR Prof_ViewHeader (void)
{
	static char	hdr[40];

	if (g_view == 0)
		q_snprintf (hdr, sizeof(hdr), "PROF self incl cll  R:in L:off");
	else
	{
		uint32_t f = g_focus[g_focus_sp - 1];
		prof_entry_t *e = tab_find (f);
		int frames = g_frames > 0 ? g_frames : 1;
		float ims = e ? (float)(e->incl_ns / 1000) / (1000.0f * frames) : 0.0f;
		q_snprintf (hdr, sizeof(hdr), "IN %-.16s i%5.2f L:back", Prof_Sym (f), ims);
	}
	return hdr;
}

/*
==============
Prof_FormatView -- fill up to maxlines display strings for the current view; the
selected row is prefixed with '>'. Columns are per-frame averages.
==============
*/
int NOINSTR Prof_FormatView (char lines[][40], int maxlines)
{
	prof_row_t	rows[PROF_VIEWMAX];
	int		n, i, frames;

	n = Prof_BuildView (rows, PROF_VIEWMAX);
	if (n > maxlines) n = maxlines;
	if (g_cursor >= n) g_cursor = n > 0 ? n - 1 : 0;
	frames = g_frames > 0 ? g_frames : 1;

	for (i = 0; i < n; i++)
	{
		char	mark = (i == g_cursor) ? '>' : ' ';
		float	self_ms = (float)(rows[i].self_ns / 1000) / (1000.0f * frames);
		float	incl_ms = (float)(rows[i].incl_ns / 1000) / (1000.0f * frames);
		int	cpf = (int)(rows[i].calls / frames);

		if (g_view == 0)
			q_snprintf (lines[i], 40, "%c%5.2f %5.2f %3d %-.19s",
				    mark, self_ms, incl_ms, cpf, Prof_Sym (rows[i].addr));
		else
			q_snprintf (lines[i], 40, "%c%5.2f %3d %-.25s",
				    mark, incl_ms, cpf, Prof_Sym (rows[i].addr));
	}
	return n;
}

/*
==============
Prof_FrameMark -- once per frame at the top of the game loop (call depth 0). Syncs
g_active to the cvar, resets the shadow stack (recovering from any longjmp), resets
the accumulators the frame the profiler is switched on, and counts frames.
==============
*/
void NOINSTR Prof_FrameMark (void)
{
	int on = (prof.value != 0);

	g_depth = 0;
	g_active = on;

	if (on && !g_last_on)		// just switched on: start a fresh accumulation
	{
		memset (g_tab, 0, sizeof(g_tab));
		memset (g_edge, 0, sizeof(g_edge));
		g_frames = 0;
		g_view = 0; g_focus_sp = 0; g_cursor = 0;
	}
	g_last_on = on;

	if (on)
		g_frames++;
}

void NOINSTR Prof_Init (void)
{
	Cvar_RegisterVariable (&prof);
	perf_cntr_timer_enable ();
	Prof_LoadSyms ();
}

#endif	/* PLATFORM_DREAMCAST && USE_DC_PROFILER */
