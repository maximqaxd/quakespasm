/*
================================================================================
qw_cl_slist.c -- QuakeWorld master server browser

A QW master (default master.quakeworld.nu:27000) answers a bare "c\n" request
with a packed list of active servers: a "\xff\xff\xff\xffd\n" header followed by
6-byte records, each a 4-byte IPv4 address and a 2-byte port in network order.
The qw_masters cvar holds a space-separated list of masters, all queried at once;
their replies are drained and merged here over a short window while we sit in the
menu, not connected.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_netadr_t	qw_serverlist[QW_MAX_SERVERS];
int		qw_numservers;

// Space-separated master list; entries without a port default to 27000.
static cvar_t	qw_masters = {"qw_masters",
	"master.quakeworld.nu master.quakeservers.net qwmaster.fodquake.net",
	CVAR_ARCHIVE};

static qboolean	qw_slist_pending;
static double	qw_slist_time;

#define	QW_SLIST_WINDOW	4.0	// seconds to gather replies from all masters

/*
==============
CLQW_AddServer -- append one 6-byte record (4-byte IP + 2-byte port), skipping
duplicates so overlapping master lists merge cleanly.
==============
*/
static void CLQW_AddServer (const byte *rec)
{
	qw_netadr_t	a;
	int		i;

	if (!rec[0] && !rec[1] && !rec[2] && !rec[3])
		return;				// empty/terminator record

	memset (&a, 0, sizeof(a));
	memcpy (a.ip, rec, 4);
	memcpy (&a.port, rec + 4, 2);		// already network byte order

	for (i = 0; i < qw_numservers; i++)
		if (!memcmp (qw_serverlist[i].ip, a.ip, 4) && qw_serverlist[i].port == a.port)
			return;				// already listed

	if (qw_numservers < QW_MAX_SERVERS)
		qw_serverlist[qw_numservers++] = a;
}

/*
==============
CLQW_SendOneMaster -- resolve a master and fire the list request at it.
==============
*/
static void CLQW_SendOneMaster (const char *host)
{
	qw_netadr_t	adr;
	static const byte req[2] = { 'c', '\n' };

	if (!QWNET_StringToAdr (host, &adr))
	{
		Con_Printf ("[QW] bad master \"%s\"\n", host);
		return;
	}
	if (!strchr (host, ':'))
		QWNET_SetPort (&adr, QW_PORT_MASTER);	// masters live on 27000

	Con_Printf ("[QW] querying master %s ...\n", QWNET_AdrToString (adr));
	QWNET_SendPacket (2, req, adr);
}

/*
==============
CLQW_SList_Query -- query one master (if given) or every master in qw_masters.
==============
*/
void CLQW_SList_Query (const char *master)
{
	qw_numservers = 0;
	qw_slist_pending = true;
	qw_slist_time = realtime;

	if (master && master[0])
	{
		CLQW_SendOneMaster (master);
		return;
	}

	{	// walk the space/tab separated list
		char	list[512], *tok;

		q_strlcpy (list, qw_masters.string, sizeof(list));
		for (tok = strtok (list, " \t"); tok; tok = strtok (NULL, " \t"))
			CLQW_SendOneMaster (tok);
	}
}

/*
==============
CLQW_SList_Poll -- while a query is in flight, merge any master replies. Runs
only when idle, so it never steals packets from an active game.
==============
*/
void CLQW_SList_Poll (void)
{
	if (!qw_slist_pending || !CLQW_IsIdle ())
		return;

	if (realtime - qw_slist_time > QW_SLIST_WINDOW)
	{
		qw_slist_pending = false;
		Con_Printf ("[QW] %i servers\n", qw_numservers);
		return;
	}

	while (QWNET_GetPacket ())
	{
		byte	*p = net_message.data;
		byte	*end = net_message.data + net_message.cursize;

		if (end - p >= 4 && p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff)
			p += 4;
		if (p >= end || *p != QW_M2C_MASTER_REPLY)
			continue;		// not a master reply

		p++;				// 'd'
		if (p < end && *p == '\n')
			p++;
		for ( ; p + 6 <= end; p += 6)
			CLQW_AddServer (p);
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
	Cvar_RegisterVariable (&qw_masters);
	Cmd_AddCommand ("slist", CLQW_SList_f);
	Cmd_AddCommand ("qwservers", CLQW_Servers_f);
}

#endif	/* USE_QW_PROTOCOL */
