/*
================================================================================
qw_cl_slist.c -- QuakeWorld master server browser

Two-stage discovery, all connectionless and driven from the menu while idle:
  1. Ask every master in qw_masters for its list ("c\n" -> 6-byte IP:port records).
  2. Ask each discovered server for its status ("status\n" -> A2C_PRINT with the
     serverinfo line and one line per player), which gives the hostname, map,
     player counts and, from the round trip, a ping.

The menu shows the summary per server and, on selection, the player list from a
fresh status probe.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_server_t	qw_serverlist[QW_MAX_SERVERS];
int		qw_numservers;

qw_playerinfo_t	qw_detail_players[QW_MAX_CLIENTS];
int		qw_detail_numplayers;
int		qw_detail_index = -1;

static cvar_t	qw_masters = {"qw_masters",
	"master.quakeworld.nu master.quakeservers.net qwmaster.fodquake.net",
	CVAR_ARCHIVE};

static qboolean	qw_slist_active;	// browser open -- keep probing
static double	qw_master_deadline;	// stop accepting master replies after this

#define	QW_STATUS_TIMEOUT	3.0	// seconds before a silent server is "no answer"
#define	QW_STATUS_PER_FRAME	3	// stagger status probes to avoid a burst

// ---------------------------------------------------------------------------

/*
==============
CLQW_ServerCompare -- order populated servers first (by player count), then
responsive empty ones by ping; unknown/unreachable sink to the bottom.
==============
*/
static int CLQW_ServerCompare (const void *pa, const void *pb)
{
	const qw_server_t	*a = (const qw_server_t *)pa;
	const qw_server_t	*b = (const qw_server_t *)pb;
	int			ap, bp;

	if ((a->curplayers > 0) != (b->curplayers > 0))
		return (b->curplayers > 0) - (a->curplayers > 0);	// players first
	if (a->curplayers != b->curplayers)
		return b->curplayers - a->curplayers;			// more players first

	ap = (a->ping < 0) ? 1000 : a->ping;				// unknown -> last
	bp = (b->ping < 0) ? 1000 : b->ping;
	return ap - bp;
}

static qw_server_t *CLQW_FindServer (const qw_netadr_t *a)
{
	int	i;

	for (i = 0; i < qw_numservers; i++)
		if (!memcmp (qw_serverlist[i].adr.ip, a->ip, 4) && qw_serverlist[i].adr.port == a->port)
			return &qw_serverlist[i];
	return NULL;
}

/*
==============
CLQW_AddServer -- add one master record (4-byte IP + 2-byte port), skipping dups.
==============
*/
static void CLQW_AddServer (const byte *rec)
{
	qw_server_t	*s;
	qw_netadr_t	a;

	if (!rec[0] && !rec[1] && !rec[2] && !rec[3])
		return;

	memset (&a, 0, sizeof(a));
	memcpy (a.ip, rec, 4);
	memcpy (&a.port, rec + 4, 2);		// already network byte order

	if (CLQW_FindServer (&a))
		return;
	if (qw_numservers >= QW_MAX_SERVERS)
		return;

	s = &qw_serverlist[qw_numservers++];
	memset (s, 0, sizeof(*s));
	s->adr = a;
	s->ping = -1;
	q_strlcpy (s->name, QWNET_AdrToString (a), sizeof(s->name));	// until it answers
}

/*
==============
CLQW_InfoValue -- look up a key in a "\key\value..." serverinfo string.
==============
*/
static const char *CLQW_InfoValue (const char *info, const char *key)
{
	static char	value[64];
	char		pkey[64];
	char		*o;

	if (*info == '\\')
		info++;
	while (*info)
	{
		o = pkey;
		while (*info && *info != '\\')
			{ if (o < pkey + 63) *o++ = *info; info++; }
		*o = 0;
		if (*info) info++;

		o = value;
		while (*info && *info != '\\')
			{ if (o < value + 63) *o++ = *info; info++; }
		*o = 0;
		if (*info) info++;

		if (!strcmp (pkey, key))
			return value;
	}
	return "";
}

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
		QWNET_SetPort (&adr, QW_PORT_MASTER);

	QWNET_SendPacket (2, req, adr);
}

static void CLQW_SendStatus (qw_server_t *s)
{
	static const byte req[] = { 0xff, 0xff, 0xff, 0xff, 's','t','a','t','u','s','\n' };

	QWNET_SendPacket (sizeof(req), req, s->adr);
	s->sent = realtime;
	s->state = 1;
}

/*
==============
CLQW_ParseStatus -- an A2C_PRINT status reply: the serverinfo line then one line
per player. Fills the matching server's summary and, if it is the selected one,
the detail player list.
==============
*/
static void CLQW_ParseStatus (const qw_netadr_t *from, char *text)
{
	qw_server_t	*s = CLQW_FindServer (from);
	char		*line, *next;
	int		nplayers = 0;
	qboolean	isdetail;

	isdetail = (qw_detail_index >= 0 && qw_detail_index < qw_numservers &&
		!memcmp (qw_serverlist[qw_detail_index].adr.ip, from->ip, 4) &&
		qw_serverlist[qw_detail_index].adr.port == from->port);
	if (isdetail)
		qw_detail_numplayers = 0;

	while (*text == '\n' || *text == '\r')
		text++;

	// first line: the serverinfo string
	line = text;
	next = strchr (line, '\n');
	if (next)
		*next++ = 0;
	if (s)
	{
		const char *host = CLQW_InfoValue (line, "hostname");
		int	ms = (int)((realtime - s->sent) * 1000);

		if (host[0])
			q_strlcpy (s->name, host, sizeof(s->name));
		q_strlcpy (s->map, CLQW_InfoValue (line, "map"), sizeof(s->map));
		s->maxplayers = (byte) atoi (CLQW_InfoValue (line, "maxclients"));
		s->ping = (short) CLAMP (0, ms, 999);
		s->state = 2;
	}

	// player lines: userid frags mins ping "name" "skin" top bottom
	for (line = next; line && *line; line = next)
	{
		int	userid, frags, mins, png;
		char	*q1, *q2;

		next = strchr (line, '\n');
		if (next)
			*next++ = 0;
		if (sscanf (line, "%d %d %d %d", &userid, &frags, &mins, &png) < 4)
			continue;
		q1 = strchr (line, '"');
		if (!q1++)
			continue;
		q2 = strchr (q1, '"');
		if (!q2)
			continue;

		nplayers++;
		if (isdetail && qw_detail_numplayers < QW_MAX_CLIENTS)
		{
			qw_playerinfo_t	*p = &qw_detail_players[qw_detail_numplayers++];
			int		n = (int)(q2 - q1);

			if (n > (int)sizeof(p->name) - 1)
				n = sizeof(p->name) - 1;
			memcpy (p->name, q1, n);
			p->name[n] = 0;
			p->frags = frags;
			p->mins = mins;
		}
	}

	if (s)
		s->curplayers = (byte) nplayers;
}

/*
==============
CLQW_SList_Poll -- per-frame browser tick: drain master/status replies, then
probe a few not-yet-queried servers and time out silent ones.
==============
*/
void CLQW_SList_Poll (void)
{
	int		i, probed;
	qboolean	dirty = false;

	if (!qw_slist_active || !CLQW_IsIdle ())
		return;

	while (QWNET_GetPacket ())
	{
		byte	*p = net_message.data;
		byte	*end = net_message.data + net_message.cursize;

		if (end - p >= 4 && p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff)
			p += 4;
		if (p >= end)
			continue;

		if (*p == QW_M2C_MASTER_REPLY && realtime < qw_master_deadline)
		{
			p++;				// 'd'
			if (p < end && *p == '\n')
				p++;
			for ( ; p + 6 <= end; p += 6)
				CLQW_AddServer (p);
		}
		else if (*p == QW_A2C_PRINT)
		{	// 'n' -- status reply text
			char	text[1400];
			int	n = (int)(end - (p + 1));

			if (n > (int)sizeof(text) - 1)
				n = sizeof(text) - 1;
			if (n < 0)
				n = 0;
			memcpy (text, p + 1, n);
			text[n] = 0;
			CLQW_ParseStatus (&qw_net_from, text);
			dirty = true;
		}
	}

	for (i = 0, probed = 0; i < qw_numservers; i++)
	{
		qw_server_t	*s = &qw_serverlist[i];

		if (s->state == 0 && probed < QW_STATUS_PER_FRAME)
		{
			CLQW_SendStatus (s);
			probed++;
		}
		else if (s->state == 1 && realtime - s->sent > QW_STATUS_TIMEOUT)
		{
			s->ping = 999;			// no answer
			s->state = 2;
			dirty = true;
		}
	}

	// re-sort as new info lands so populated servers rise to the top. Only in
	// the list view: qw_detail_index is an array index, and sorting would move
	// it to a different server.
	if (dirty && qw_detail_index < 0)
		qsort (qw_serverlist, qw_numservers, sizeof(qw_serverlist[0]), CLQW_ServerCompare);
}

/*
==============
CLQW_SList_Query -- (re)query one master, or all in qw_masters.
==============
*/
void CLQW_SList_Query (const char *master)
{
	qw_numservers = 0;
	qw_detail_index = -1;
	qw_master_deadline = realtime + 4.0;

	if (master && master[0])
	{
		CLQW_SendOneMaster (master);
		return;
	}

	{
		char	list[512], *tok;

		q_strlcpy (list, qw_masters.string, sizeof(list));
		for (tok = strtok (list, " \t"); tok; tok = strtok (NULL, " \t"))
			CLQW_SendOneMaster (tok);
	}
}

void CLQW_SList_Activate (void)
{
	qw_slist_active = true;
	if (qw_numservers == 0)
		CLQW_SList_Query (NULL);		// auto-refresh the first time
}

void CLQW_SList_Deactivate (void)
{
	qw_slist_active = false;
}

void CLQW_SList_OpenDetail (int index)
{
	if (index < 0 || index >= qw_numservers)
		return;
	qw_detail_index = index;
	qw_detail_numplayers = 0;
	CLQW_SendStatus (&qw_serverlist[index]);	// fresh player list
}

// --- console access --------------------------------------------------------

static void CLQW_SList_f (void)
{
	qw_slist_active = true;
	CLQW_SList_Query (Cmd_Argc () >= 2 ? Cmd_Argv (1) : NULL);
}

static void CLQW_Servers_f (void)
{
	int	i;

	for (i = 0; i < qw_numservers; i++)
		Con_Printf ("%3i: %-21s %-8s %i/%i %ims\n", i,
			QWNET_AdrToString (qw_serverlist[i].adr),
			qw_serverlist[i].map, qw_serverlist[i].curplayers,
			qw_serverlist[i].maxplayers, qw_serverlist[i].ping);
	Con_Printf ("%i servers\n", qw_numservers);
}

void CLQW_SList_Init (void)
{
	Cvar_RegisterVariable (&qw_masters);
	Cmd_AddCommand ("slist", CLQW_SList_f);
	Cmd_AddCommand ("qwservers", CLQW_Servers_f);
}

#endif	/* USE_QW_PROTOCOL */
