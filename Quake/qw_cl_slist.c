/*
================================================================================
qw_cl_slist.c -- QuakeWorld master server browser

A QW master (default master.quakeworld.nu:27000) answers a bare "c\n" request
with a packed list of active servers: a "\xff\xff\xff\xffd\n" header followed by
6-byte records, each a 4-byte IPv4 address and a 2-byte port in network order.
The request goes out from the menu while we are not connected, so the reply is
drained here every frame rather than through the in-game netchan pump.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_netadr_t	qw_serverlist[QW_MAX_SERVERS];
int		qw_numservers;

static cvar_t	qw_master = {"qw_master", "master.quakeworld.nu", CVAR_ARCHIVE};
static qboolean	qw_slist_pending;
static double	qw_slist_time;

/*
==============
CLQW_ParseMasterReply -- unpack the 6-byte IP:port records into the list.
==============
*/
static void CLQW_ParseMasterReply (byte *p, byte *end)
{
	qw_numservers = 0;

	for ( ; p + 6 <= end && qw_numservers < QW_MAX_SERVERS; p += 6)
	{
		qw_netadr_t	*a = &qw_serverlist[qw_numservers];

		if (!p[0] && !p[1] && !p[2] && !p[3])
			continue;		// empty/terminator record

		memset (a, 0, sizeof(*a));
		a->ip[0] = p[0];
		a->ip[1] = p[1];
		a->ip[2] = p[2];
		a->ip[3] = p[3];
		memcpy (&a->port, p + 4, 2);	// already network byte order
		qw_numservers++;
	}

	qw_slist_pending = false;
	Con_Printf ("[QW] master returned %i servers\n", qw_numservers);
}

/*
==============
CLQW_SList_Query -- send the list request to a master (name or numeric address).
==============
*/
void CLQW_SList_Query (const char *master)
{
	qw_netadr_t	adr;
	static const byte req[2] = { 'c', '\n' };

	if (!master || !master[0])
		master = qw_master.string;

	if (!QWNET_StringToAdr (master, &adr))
	{
		Con_Printf ("[QW] bad master address \"%s\"\n", master);
		return;
	}
	if (!strchr (master, ':'))
		QWNET_SetPort (&adr, QW_PORT_MASTER);	// masters live on 27000

	qw_numservers = 0;
	qw_slist_pending = true;
	qw_slist_time = realtime;
	Con_Printf ("[QW] querying master %s ...\n", QWNET_AdrToString (adr));
	QWNET_SendPacket (2, req, adr);
}

/*
==============
CLQW_SList_Poll -- while a query is in flight, drain the socket for the reply.
Only runs when idle, so it never steals packets from an active game.
==============
*/
void CLQW_SList_Poll (void)
{
	if (!qw_slist_pending || !CLQW_IsIdle ())
		return;

	if (realtime - qw_slist_time > 5.0)
	{
		qw_slist_pending = false;
		Con_Printf ("[QW] master query timed out\n");
		return;
	}

	while (QWNET_GetPacket ())
	{
		byte	*p = net_message.data;
		byte	*end = net_message.data + net_message.cursize;

		// accept the reply with or without the connectionless -1 prefix
		if (end - p >= 4 && p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff)
			p += 4;
		if (p < end && *p == QW_M2C_MASTER_REPLY)
		{
			p++;
			if (p < end && *p == '\n')
				p++;
			CLQW_ParseMasterReply (p, end);
			return;
		}
	}
}

/*
==============
CLQW_SList_f -- "slist [master]" : request the server list.
==============
*/
static void CLQW_SList_f (void)
{
	CLQW_SList_Query (Cmd_Argc () >= 2 ? Cmd_Argv (1) : NULL);
}

/*
==============
CLQW_Servers_f -- "qwservers" : print the last-received list with indices.
==============
*/
static void CLQW_Servers_f (void)
{
	int	i;

	for (i = 0; i < qw_numservers; i++)
		Con_Printf ("%3i: %s\n", i, QWNET_AdrToString (qw_serverlist[i]));
	Con_Printf ("%i servers\n", qw_numservers);
}

void CLQW_SList_Init (void)
{
	Cvar_RegisterVariable (&qw_master);
	Cmd_AddCommand ("slist", CLQW_SList_f);
	Cmd_AddCommand ("qwservers", CLQW_Servers_f);
}

#endif	/* USE_QW_PROTOCOL */
