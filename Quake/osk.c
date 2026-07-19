/*
================================================================================
osk.c -- on-screen keyboard for controller-only input

The Dreamcast is usually played without a keyboard, but the console and the
menu text fields (server address, player name) need typed characters. This draws
a small keyboard with the Quake font that the D-pad/stick navigates and A/Enter
types on. It auto-hides the moment a real maple keyboard is plugged in.

Hooked in three places: Key_Event routes input through OSK_KeyEvent (which
consumes it while active), SCR_UpdateScreen calls OSK_Draw, and Key_Init calls
OSK_Init. Everything is gated on OSK_Active so it only appears when a text field
is focused and no hardware keyboard is present.
================================================================================
*/
#include "quakedef.h"

extern qboolean IN_HasKeyboard (void);		// in_sdl.c

cvar_t	osk_enable = {"osk_enable", "1", CVAR_ARCHIVE};

#define	OSK_COLS	10
#define	OSK_ROWS	4

static const char *osk_lower[OSK_ROWS] =
{
	"1234567890",
	"qwertyuiop",
	"asdfghjkl.",
	"zxcvbnm:/-",
};
static const char *osk_upper[OSK_ROWS] =
{
	"!@#$%^&*()",
	"QWERTYUIOP",
	"ASDFGHJKL,",
	"ZXCVBNM;?_",
};

enum { SP_SHIFT, SP_SPACE, SP_BKSP, SP_ENTER, SP_COUNT };
static const char	*osk_special[SP_COUNT] = { "shift", "space", "bksp", "enter" };
static const int	osk_special_x[SP_COUNT] = { 96, 140, 190, 232 };

static int	osk_row, osk_col;
static qboolean	osk_shift;
static qboolean	osk_sending;		// pass through the Enter we synthesize

/*
==============
OSK_Active -- shown only for a focused text field, and only without a keyboard.
==============
*/
qboolean OSK_Active (void)
{
	return osk_enable.value && Key_TextEntry () && !IN_HasKeyboard ();
}

static int OSK_MaxCol (void)
{
	return (osk_row < OSK_ROWS) ? OSK_COLS : SP_COUNT;
}

static void OSK_Select (void)
{
	if (osk_row < OSK_ROWS)
	{
		int ch = (osk_shift ? osk_upper : osk_lower)[osk_row][osk_col];
		if (ch)
			Char_Event (ch);
	}
	else switch (osk_col)
	{
	case SP_SHIFT:
		osk_shift = !osk_shift;
		break;
	case SP_SPACE:
		Char_Event (' ');
		break;
	case SP_BKSP:
		Key_Event (K_BACKSPACE, true);
		Key_Event (K_BACKSPACE, false);
		break;
	case SP_ENTER:
		osk_sending = true;			// the console/menu must see this one
		Key_Event (K_ENTER, true);
		Key_Event (K_ENTER, false);
		break;
	}
}

/*
==============
OSK_KeyEvent -- consume navigation/select while active; returns true if handled.
==============
*/
qboolean OSK_KeyEvent (int key, qboolean down)
{
	if (!OSK_Active ())
		return false;

	if (osk_sending)			// let our synthesized Enter through
	{
		osk_sending = false;
		return false;
	}

	if (!down)
	{	// swallow the release of keys we act on, pass the rest
		switch (key)
		{
		case K_UPARROW: case K_DOWNARROW: case K_LEFTARROW: case K_RIGHTARROW:
		case K_ABUTTON: case K_ENTER:
			return true;
		default:
			return false;
		}
	}

	switch (key)
	{
	case K_UPARROW:
		if (--osk_row < 0) osk_row = OSK_ROWS;
		break;
	case K_DOWNARROW:
		if (++osk_row > OSK_ROWS) osk_row = 0;
		break;
	case K_LEFTARROW:
		if (--osk_col < 0) osk_col = OSK_MaxCol () - 1;
		break;
	case K_RIGHTARROW:
		if (++osk_col >= OSK_MaxCol ()) osk_col = 0;
		break;
	case K_ABUTTON:
	case K_ENTER:
		OSK_Select ();
		break;
	default:
		return false;			// ESC, physical letters, etc. pass through
	}

	if (osk_col >= OSK_MaxCol ())
		osk_col = OSK_MaxCol () - 1;
	return true;
}

static void OSK_DrawStr (int x, int y, const char *s)
{
	while (*s)
	{
		Draw_Character (x, y, *s);
		x += 8;
		s++;
	}
}

/*
==============
OSK_Draw -- render the keyboard near the bottom of the 320x200 menu canvas.
==============
*/
void OSK_Draw (void)
{
	const int	cw = 13, x0 = 95, y0 = 150;
	int		r, c, x, y;

	if (!OSK_Active ())
		return;

	GL_SetCanvas (CANVAS_MENU);

	Draw_Fill (x0 - 8, y0 - 8, OSK_COLS * cw + 16, 5 * 10 + 14, 0, 0.7);

	for (r = 0; r < OSK_ROWS; r++)
	{
		const char *row = (osk_shift ? osk_upper : osk_lower)[r];

		for (c = 0; c < OSK_COLS; c++)
		{
			x = x0 + c * cw;
			y = y0 + r * 10;
			if (r == osk_row && c == osk_col)
				Draw_Fill (x - 2, y - 1, 11, 10, 15, 0.5);
			Draw_Character (x, y, row[c]);
		}
	}

	// special row
	y = y0 + OSK_ROWS * 10;
	for (c = 0; c < SP_COUNT; c++)
	{
		x = osk_special_x[c];
		if (osk_row == OSK_ROWS && osk_col == c)
			Draw_Fill (x - 2, y - 1, (int)strlen (osk_special[c]) * 8 + 3, 10, 15, 0.5);
		OSK_DrawStr (x, y, osk_special[c]);
	}
}

void OSK_Init (void)
{
	Cvar_RegisterVariable (&osk_enable);
}
