/*
================================================================================
qw_cl_main.c -- QuakeWorld client connection glue

QuakeWorld is a runtime-selectable protocol family: the normal "connect" command
picks it (via cls.protofamily) and dispatches here instead of down the NetQuake
path. The NetQuake path is left completely untouched.

Phase 1: the connectionless handshake and netchan bring-up.
  1. send out-of-band "getchallenge"
  2. server replies 'c'<challenge>  -> send "connect 28 <qport> <challenge> <userinfo>"
  3. server replies 'j'             -> open the netchan, send "new", we're connected
The state machine is driven each frame from CLQW_RunConnection (Host_Frame),
retransmitting until the server answers. Parsing the serverdata that follows the
"new" request is phase 2.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

typedef enum
{
	QWCS_IDLE,		// not connecting
	QWCS_CHALLENGING,	// sent getchallenge, waiting for 'c'
	QWCS_CONNECTING,	// sent connect, waiting for 'j'
	QWCS_CONNECTED		// netchan up; serverdata parsing is phase 2
} qwconnstate_t;

static qwconnstate_t	qw_connstate = QWCS_IDLE;

// Smoothed round-trip estimate, from command send to server acknowledgement.
// Other-player forward prediction uses it to know how stale their state is.
double	qw_latency;

// Bytes/sec we ask the server to send us. Broadband default; set "rate 2500"
// (or lower) on a modem. Archived so it persists.
cvar_t	qw_rate = {"rate", "10000", CVAR_ARCHIVE};

// Ask to join as a spectator (free-fly watcher) rather than a player. The server
// must have a spectator slot free; it echoes the choice in serverdata.
cvar_t	qw_spectator = {"spectator", "0", CVAR_ARCHIVE};

// DNS resolver used only when the Dreamcast's network came up without one (see
// QWNET_EnsureDNS). Default is a public resolver; override for a LAN DNS.
cvar_t	net_dns = {"net_dns", "8.8.8.8", CVAR_ARCHIVE};

qboolean CLQW_Spectator (void)		{ return qw_spectator.value != 0; }
void CLQW_SetSpectator (qboolean on)	{ Cvar_SetValue ("spectator", on ? 1 : 0); }

// True once the netchan is up, so command forwarding can write to it.
qboolean CLQW_IsConnected (void)
{
	return qw_connstate == QWCS_CONNECTED;
}

// True when no QW connection is in progress -- the server browser only polls the
// socket then, so it never competes with the in-game netchan for packets.
qboolean CLQW_IsIdle (void)
{
	return qw_connstate == QWCS_IDLE;
}

#define	QW_RETRY_TIME	5.0	// seconds between handshake retransmits

/*
=====================
CLQW_BuildUserinfo -- minimal userinfo string the server needs to accept us
=====================
*/
static void CLQW_BuildUserinfo (char *out, size_t outsize)
{
	extern cvar_t	cl_name, cl_color;
	const char	*name = (cl_name.string && cl_name.string[0]) ? cl_name.string : "dreamcast";

	int	rate = (int) qw_rate.value;
	int	top = ((int) cl_color.value) >> 4;
	int	bottom = ((int) cl_color.value) & 15;

	if (rate < 500)		rate = 500;	// clamp to sane bounds
	if (rate > 100000)	rate = 100000;

	q_snprintf (out, outsize,
		"\\name\\%s\\rate\\%i\\msg\\1\\topcolor\\%i\\bottomcolor\\%i%s",
		name, rate, top, bottom,
		qw_spectator.value ? "\\spectator\\1" : "");
}

/*
=====================
CLQW_SendConnectPacket -- out-of-band "connect" once we have a challenge
=====================
*/
static void CLQW_SendConnectPacket (void)
{
	char	userinfo[256];

	CLQW_BuildUserinfo (userinfo, sizeof(userinfo));
	QWNetchan_OutOfBandPrint (cls.qw_server_adr, "connect %i %i %i \"%s\"\n",
		QW_PROTOCOL_VERSION, cls.qport, cls.challenge, userinfo);
}

/*
=====================
CLQW_CheckForResend -- (re)send getchallenge or connect on the retry timer
=====================
*/
static void CLQW_CheckForResend (void)
{
	if (qw_connstate != QWCS_CHALLENGING && qw_connstate != QWCS_CONNECTING)
		return;

	if (cls.qw_connect_time >= 0 && realtime - cls.qw_connect_time < QW_RETRY_TIME)
		return;

	cls.qw_connect_time = realtime;

	if (qw_connstate == QWCS_CHALLENGING)
	{
		Con_Printf ("Connecting to %s...\n", QWNET_AdrToString (cls.qw_server_adr));
		QWNetchan_OutOfBandPrint (cls.qw_server_adr, "getchallenge\n");
	}
	else	// QWCS_CONNECTING
	{
		CLQW_SendConnectPacket ();
	}
}

/*
=====================
CLQW_ConnectionlessPacket -- handle an out-of-band (sequence -1) reply
=====================
*/
static void CLQW_ConnectionlessPacket (void)
{
	int		c;
	const char	*s;

	MSG_BeginReading ();
	MSG_ReadLong ();		// skip the -1 sequence
	c = MSG_ReadByte ();

	switch (c)
	{
	case QW_S2C_CHALLENGE:
		s = MSG_ReadString ();
		cls.challenge = atoi (s);
		qw_connstate = QWCS_CONNECTING;
		cls.qw_connect_time = -1;	// send the connect immediately
		CLQW_SendConnectPacket ();
		cls.qw_connect_time = realtime;
		break;

	case QW_S2C_CONNECTION:
		if (qw_connstate == QWCS_CONNECTED)
			break;			// duplicate accept
		QWNetchan_Setup (&cls.netchan, cls.qw_server_adr, cls.qport);
		MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
		MSG_WriteString (&cls.netchan.message, "new");
		qw_connstate = QWCS_CONNECTED;
		Con_Printf ("[QW] connection accepted -- requesting server data\n");
		break;

	case QW_A2C_PRINT:
		s = MSG_ReadString ();
		Con_Printf ("%s", s);
		break;

	default:
		Con_DPrintf ("[QW] unknown connectionless packet: %c\n", c);
		break;
	}
}

/*
=====================
CLQW_ReadPackets -- drain the QW socket: OOB replies and netchan messages
=====================
*/
static void CLQW_ReadPackets (void)
{
	while (QWNET_GetPacket ())
	{
		// connectionless packets start with the -1 sequence
		if (net_message.cursize >= 4 && *(int *)net_message.data == -1)
		{
			CLQW_ConnectionlessPacket ();
			continue;
		}

		if (qw_connstate != QWCS_CONNECTED)
			continue;		// not on the channel yet; ignore

		if (!QWNetchan_Process (&cls.netchan))
			continue;		// wasn't accepted (bad seq / wrong address)

		// latency sample: now minus when we sent the command this packet acks.
		// Drift like the reference: jump down to better samples, creep up.
		{
			double lat = realtime - qw_frames[cls.netchan.incoming_acknowledged & QW_UPDATE_MASK].senttime;
			if (lat >= 0 && lat <= 1.0)
			{
				if (lat < qw_latency || !qw_latency)
					qw_latency = lat;
				else
					qw_latency += 0.001;
			}
		}

		cl.last_received_message = realtime;	// keeps the net icon (SCR_DrawNet) off
		CLQW_ParseServerMessage ();
	}
}

/*
=====================
CLQW_RunConnection -- per-frame QW pump (called from Host_Frame for PROTO_QW)
=====================
*/
void CLQW_RunConnection (void)
{
	if (qw_connstate == QWCS_IDLE)
		return;

	// NetQuake advances cl.time in CL_ReadFromServer, which the QW pump replaces;
	// drive it here so temp-entity beams expire, dynamic lights decay and bonus
	// items keep rotating. cl.oldtime must track it too -- the particle sim runs
	// on (cl.time - cl.oldtime), and without it every burst aged a whole session
	// in one frame and vanished instantly.
	cl.oldtime = cl.time;
	// Render the world slightly in the past -- at the time the newest snapshot is
	// actually valid (realtime - latency), like the reference CL_PredictMove. This
	// is what lets the local player and entities interpolate smoothly instead of
	// extrapolating to "now" every frame (which reads as sloppy jitter). qw_latency
	// is already smoothed, so cl.time stays monotonic in practice.
	cl.time = realtime - qw_latency;
	if (cl.time > realtime)
		cl.time = realtime;
	if (cl.time < cl.oldtime)		// never step the effect clock backwards
		cl.time = cl.oldtime;

	// weapon kick (svc_smallkick/bigkick) eases back to level; NetQuake refreshes
	// punchangle from clientdata every frame, but QW only sends kick events.
	if (cl.punchangle[0] < 0)
	{
		cl.punchangle[0] += (cl.time - cl.oldtime) * 20.0;
		if (cl.punchangle[0] > 0)
			cl.punchangle[0] = 0;
	}

	CLQW_CheckForResend ();
	CLQW_ReadPackets ();

	// once on the server, send our input each frame (this also carries the acks
	// and keeps us from timing out); before that a bare packet keeps the channel
	// alive during the handshake.
	if (qw_connstate == QWCS_CONNECTED && QWNetchan_CanPacket (&cls.netchan))
		CLQW_SendMove ();

	// simulate our position forward from the last snapshot so the view follows
	// input immediately instead of after a network round trip.
	if (qw_connstate == QWCS_CONNECTED)
	{
		CLQW_PredictMove ();
		CLQW_EmitEntities ();	// rebuild the visible-entity list for this frame
	}
}

/*
=====================
CLQW_Disconnect -- tear down the QW connection so the per-frame pump stops. Called
from CL_Disconnect whenever the active family is QuakeWorld (real disconnect, a
missing map, or reconnecting elsewhere).
=====================
*/
void CLQW_Disconnect (void)
{
	// Stop the per-frame pump. We don't send an explicit "drop" -- the server
	// times the connection out on its own, and forcing a transmit here would
	// have to touch a netchan that may already be in a half-torn-down state.
	qw_connstate = QWCS_IDLE;
}

/*
=====================
CLQW_EstablishConnection -- QuakeWorld counterpart of CL_EstablishConnection
=====================
*/
void CLQW_EstablishConnection (const char *host)
{
	if (!QWNET_StringToAdr (host, &cls.qw_server_adr))
	{
		Con_Printf ("[QW] bad server address \"%s\" (numeric IP[:port] only)\n", host);
		qw_connstate = QWCS_IDLE;
		return;
	}

	qw_connstate = QWCS_CHALLENGING;
	cls.qw_connect_time = -1;	// CLQW_CheckForResend fires immediately
	cls.challenge = 0;
	qw_latency = 0;
	Con_DPrintf ("CLQW_EstablishConnection: %s\n", QWNET_AdrToString (cls.qw_server_adr));
}

static char	qwcl_serverinfo[512];	// last fullserverinfo string from the server

/*
=====================
CLQW_FullServerinfo_f -- "fullserverinfo \key\val..." : the server's serverinfo.
We just keep the string; parsing individual keys can come with the HUD/rules work.
=====================
*/
static void CLQW_FullServerinfo_f (void)
{
	if (Cmd_Argc () != 2)
		return;
	q_strlcpy (qwcl_serverinfo, Cmd_Argv (1), sizeof(qwcl_serverinfo));
}

/*
=====================
CLQW_Packet_f -- "packet <address> <contents>" : send a connectionless packet,
translating a literal backslash-n into a newline (used for server redirects).
=====================
*/
static void CLQW_Packet_f (void)
{
	qw_netadr_t	adr;
	char		send[1024];
	const char	*in;
	char		*out;
	int		i, len;

	if (Cmd_Argc () != 3)
	{
		Con_Printf ("packet <destination> <contents>\n");
		return;
	}
	if (!QWNET_StringToAdr (Cmd_Argv (1), &adr))
	{
		Con_DPrintf ("[QW] packet: bad address \"%s\"\n", Cmd_Argv (1));
		return;
	}

	in = Cmd_Argv (2);
	out = send + 4;
	send[0] = send[1] = send[2] = send[3] = (char)0xff;

	len = (int) strlen (in);
	for (i = 0; i < len && out < send + sizeof(send) - 1; i++)
	{
		if (in[i] == '\\' && in[i+1] == 'n')
		{
			*out++ = '\n';
			i++;
		}
		else
			*out++ = in[i];
	}
	*out = 0;

	QWNET_SendPacket ((int)(out - send), send, adr);
}

/*
=====================
CLQW_Changing_f -- "changing" : the server is switching maps and will resend the
signon. Keep the netchan; just note it so we don't spam the console.
=====================
*/
static void CLQW_Changing_f (void)
{
	Con_DPrintf ("[QW] server changing map...\n");
}

/*
=====================
CLQW_Skins_f -- "skins" : QuakeWorld loads player skins here and then sends
"begin" to enter the game. We don't download skins, so go straight to begin --
without this the signon stalls after spawn and no player frames ever arrive.
=====================
*/
static void CLQW_Skins_f (void)
{
	MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
	MSG_WriteString (&cls.netchan.message, va("begin %i", qwcl.servercount));
}

/*
=====================
CLQW_Init -- one-time QW client init (called from CL_Init)
=====================
*/
void CLQW_Init (void)
{
	QWNET_Init ();
	QWNetchan_Init ();
	CLQW_InitPrediction ();
	CLQW_SList_Init ();
	Cvar_RegisterVariable (&qw_rate);
	Cvar_RegisterVariable (&qw_spectator);
	Cvar_RegisterVariable (&net_dns);
	Cmd_AddCommand ("fullserverinfo", CLQW_FullServerinfo_f);
	Cmd_AddCommand ("packet", CLQW_Packet_f);
	Cmd_AddCommand ("changing", CLQW_Changing_f);
	Cmd_AddCommand ("skins", CLQW_Skins_f);
}

#endif	/* USE_QW_PROTOCOL */
