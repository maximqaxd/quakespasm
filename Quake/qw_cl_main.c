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

// True once the netchan is up, so command forwarding can write to it.
qboolean CLQW_IsConnected (void)
{
	return qw_connstate == QWCS_CONNECTED;
}

#define	QW_RETRY_TIME	5.0	// seconds between handshake retransmits

/*
=====================
CLQW_BuildUserinfo -- minimal userinfo string the server needs to accept us
=====================
*/
static void CLQW_BuildUserinfo (char *out, size_t outsize)
{
	extern cvar_t	cl_name;
	const char	*name = (cl_name.string && cl_name.string[0]) ? cl_name.string : "dreamcast";

	q_snprintf (out, outsize, "\\name\\%s\\rate\\2500\\msg\\1\\topcolor\\0\\bottomcolor\\0", name);
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

	CLQW_CheckForResend ();
	CLQW_ReadPackets ();

	// keep the channel alive once connected (an empty packet carries acks)
	if (qw_connstate == QWCS_CONNECTED && QWNetchan_CanPacket (&cls.netchan))
		QWNetchan_Transmit (&cls.netchan, 0, NULL);
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
		Con_Printf ("Bad address\n");
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
CLQW_Init -- one-time QW client init (called from CL_Init)
=====================
*/
void CLQW_Init (void)
{
	QWNET_Init ();
	QWNetchan_Init ();
	Cmd_AddCommand ("fullserverinfo", CLQW_FullServerinfo_f);
	Cmd_AddCommand ("packet", CLQW_Packet_f);
	Cmd_AddCommand ("changing", CLQW_Changing_f);
}

#endif	/* USE_QW_PROTOCOL */
