/*
================================================================================
qw_cl_parse.c -- QuakeWorld server-message parser (protocol 28)

Phase 2a: the dispatch loop plus svc_serverdata (which starts the signon by
requesting the sound list) and the signon-safe opcodes. Opcodes that carry the
precache lists, baselines and entity deltas (soundlist/modellist/spawnbaseline/
packetentities/playerinfo/...) are phase 2b/3 -- for now they stop the parse
cleanly rather than desync the stream.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_movevars_t	qw_movevars;
qwcl_state_t	qwcl;

/*
==============
CLQW_ParseServerData -- svc_serverdata: protocol, gamedir, our slot, level,
movevars. Wipes the client state and asks for the sound list to begin the signon.
==============
*/
static void CLQW_ParseServerData (void)
{
	const char	*str;
	int		protover;

	Con_DPrintf ("[QW] serverdata packet received\n");

	CL_ClearState ();

	protover = MSG_ReadLong ();
	if (protover != QW_PROTOCOL_VERSION)
	{
		Con_Printf ("[QW] server returned protocol %i, expected %i\n", protover, QW_PROTOCOL_VERSION);
		CL_Disconnect ();
		return;
	}

	qwcl.servercount = MSG_ReadLong ();

	str = MSG_ReadString ();		// gamedir
	q_strlcpy (qwcl.gamedir, str, sizeof(qwcl.gamedir));

	qwcl.playernum = MSG_ReadByte ();	// high bit = spectator
	qwcl.spectator = (qwcl.playernum & 128) != 0;
	qwcl.playernum &= ~128;

	str = MSG_ReadString ();		// full level name
	q_strlcpy (qwcl.levelname, str, sizeof(qwcl.levelname));
	q_strlcpy (cl.levelname, str, sizeof(cl.levelname));

	qw_movevars.gravity           = MSG_ReadFloat ();
	qw_movevars.stopspeed         = MSG_ReadFloat ();
	qw_movevars.maxspeed          = MSG_ReadFloat ();
	qw_movevars.spectatormaxspeed = MSG_ReadFloat ();
	qw_movevars.accelerate        = MSG_ReadFloat ();
	qw_movevars.airaccelerate     = MSG_ReadFloat ();
	qw_movevars.wateraccelerate   = MSG_ReadFloat ();
	qw_movevars.friction          = MSG_ReadFloat ();
	qw_movevars.waterfriction     = MSG_ReadFloat ();
	qw_movevars.entgravity        = MSG_ReadFloat ();

	Con_Printf ("[QW] connected to \"%s\" (gamedir %s, slot %i%s)\n",
		qwcl.levelname, qwcl.gamedir, qwcl.playernum,
		qwcl.spectator ? ", spectator" : "");

	// ask for the sound list to begin the signon sequence (phase 2b parses it)
	MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
	MSG_WriteString (&cls.netchan.message, va("soundlist %i 0", qwcl.servercount));
}

/*
==============
CLQW_ParseServerMessage -- decode one netchan message worth of svc_ commands
==============
*/
void CLQW_ParseServerMessage (void)
{
	int	cmd, i;
	const char *s;

	while (1)
	{
		if (msg_badread)
		{
			Con_Printf ("[QW] bad server message\n");
			CL_Disconnect ();
			return;
		}

		if (msg_readcount == net_message.cursize)
			break;			// end of message

		cmd = MSG_ReadByte ();
		if (cmd == -1)
			break;

		switch (cmd)
		{
		case qwsvc_nop:
			break;

		case qwsvc_disconnect:
			Con_Printf ("[QW] server disconnected\n");
			CL_Disconnect ();
			return;

		case qwsvc_print:
			(void) MSG_ReadByte ();		// print level
			Con_Printf ("%s", MSG_ReadString ());
			break;

		case qwsvc_centerprint:
			SCR_CenterPrint (MSG_ReadString ());
			break;

		case qwsvc_stufftext:
			s = MSG_ReadString ();
			Cbuf_AddText (s);
			break;

		case qwsvc_serverdata:
			CLQW_ParseServerData ();
			break;

		case qwsvc_setangle:
			for (i = 0; i < 3; i++)
				cl.viewangles[i] = MSG_ReadAngle (0);
			break;

		case qwsvc_lightstyle:
			i = MSG_ReadByte ();
			if (i < MAX_LIGHTSTYLES)
				q_strlcpy (cl_lightstyle[i].map, MSG_ReadString (), MAX_STYLESTRING);
			else
				(void) MSG_ReadString ();
			break;

		case qwsvc_serverinfo:
			(void) MSG_ReadString ();	// key
			(void) MSG_ReadString ();	// value
			break;

		case qwsvc_cdtrack:
			cl.cdtrack = MSG_ReadByte ();
			break;

		case qwsvc_updatestat:
			i = MSG_ReadByte ();
			if (i >= 0 && i < MAX_CL_STATS)
				cl.stats[i] = MSG_ReadByte ();
			else
				(void) MSG_ReadByte ();
			break;

		case qwsvc_updatestatlong:
			i = MSG_ReadByte ();
			if (i >= 0 && i < MAX_CL_STATS)
				cl.stats[i] = MSG_ReadLong ();
			else
				(void) MSG_ReadLong ();
			break;

		case qwsvc_updateuserinfo:
			(void) MSG_ReadByte ();		// slot
			(void) MSG_ReadLong ();		// userid
			(void) MSG_ReadString ();	// userinfo
			break;

		case qwsvc_setinfo:
			(void) MSG_ReadByte ();		// slot
			(void) MSG_ReadString ();	// key
			(void) MSG_ReadString ();	// value
			break;

		case qwsvc_updatefrags:
			(void) MSG_ReadByte ();
			(void) MSG_ReadShort ();
			break;

		case qwsvc_updateping:
			(void) MSG_ReadByte ();
			(void) MSG_ReadShort ();
			break;

		case qwsvc_updatepl:
			(void) MSG_ReadByte ();
			(void) MSG_ReadByte ();
			break;

		case qwsvc_updateentertime:
			(void) MSG_ReadByte ();
			(void) MSG_ReadFloat ();
			break;

		default:
			// precache lists, baselines, entity deltas -- phase 2b/3.
			Con_DPrintf ("[QW] svc %i not handled yet; stopping parse\n", cmd);
			return;
		}
	}
}

#endif	/* USE_QW_PROTOCOL */
