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
static qboolean	osk_open;		// opened with A, closed with B/enter

/*
==============
OSK_Active -- drawn/steering input only while explicitly opened over a focused
text field (and only without a hardware keyboard).
==============
*/
qboolean OSK_Active (void)
{
	return osk_open && osk_enable.value && Key_TextEntry () && !IN_HasKeyboard ();
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
		osk_open = false;			// and close the keyboard
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
	if (!osk_enable.value || IN_HasKeyboard () || !Key_TextEntry ())
	{
		osk_open = false;
		return false;
	}

	if (!osk_open)
	{	// closed: A opens the keyboard, everything else navigates the menu
		if (down && key == K_ABUTTON)
		{
			osk_open = true;
			osk_row = osk_col = 0;
			osk_shift = false;
			return true;
		}
		return false;
	}

	if (osk_sending)			// let our synthesized Enter through
	{
		osk_sending = false;
		return false;
	}

	if (key == K_BBUTTON || key == K_ESCAPE)
	{	// close, back to menu navigation
		if (down)
			osk_open = false;
		return true;
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
		return false;			// physical letters etc. pass through
	}

	if (osk_col >= OSK_MaxCol ())
		osk_col = OSK_MaxCol () - 1;
	return true;
}

// Draw a string in gold (default) or bright white -- the two colours the Quake
// console font provides via the high bit (128..255 are the gold glyphs).
static void OSK_DrawStr (int x, int y, const char *s, qboolean white)
{
	while (*s)
	{
		Draw_Character (x, y, white ? *s : (*s | 128));
		x += 8;
		s++;
	}
}

/*
==============
OSK_Draw -- render the keyboard near the bottom of the 320x200 menu canvas. The
highlighted key is drawn bright white, the rest gold; a Draw_Fill selection box
was invisible under the PVR renderer, so the font's two colours carry it.
==============
*/
void OSK_Draw (void)
{
	const int	cw = 13, x0 = 95, y0 = 156;
	int		r, c, x, y;

	if (!OSK_Active ())
		return;

	GL_SetCanvas (CANVAS_MENU);

	// Opaque backing (alpha 1): the PVR 2D path blends Draw_Fill, so the old
	// 0.75 wash let the menu/console text drawn underneath bleed through. A
	// solid panel hides it -- menu fields sit above the panel and stay visible.
	Draw_Fill (x0 - 8, y0 - 16, OSK_COLS * cw + 16, 5 * 10 + 22, 0, 1);

	// Top line. Over the console the real input line is at the bottom of the
	// screen, hidden behind this panel, so echo it here; scroll to keep the
	// caret in view. In menus the field is visible above, so just hint the keys.
	if (key_dest == key_console)
	{
		int		maxch = (OSK_COLS * cw) / 8;
		int		ofs = (key_linepos >= maxch) ? 1 + key_linepos - maxch : 0;
		const char	*s = key_lines[edit_line] + ofs;

		for (x = x0 - 4, c = 0; *s && c < maxch; s++, c++, x += 8)
			Draw_Character (x, y0 - 12, *s);
	}
	else
		OSK_DrawStr (x0 - 4, y0 - 12, "A type  B close", false);

	for (r = 0; r < OSK_ROWS; r++)
	{
		const char *row = (osk_shift ? osk_upper : osk_lower)[r];

		for (c = 0; c < OSK_COLS; c++)
		{
			qboolean sel = (r == osk_row && c == osk_col);
			x = x0 + c * cw;
			y = y0 + r * 10;
			Draw_Character (x, y, sel ? row[c] : (row[c] | 128));
		}
	}

	// special row
	y = y0 + OSK_ROWS * 10;
	for (c = 0; c < SP_COUNT; c++)
	{
		x = osk_special_x[c];
		OSK_DrawStr (x, y, osk_special[c], osk_row == OSK_ROWS && osk_col == c);
	}
}

void OSK_Init (void)
{
	Cvar_RegisterVariable (&osk_enable);
}
