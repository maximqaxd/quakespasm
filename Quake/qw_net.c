/*
================================================================================
qw_net.c -- QuakeWorld raw connectionless UDP layer

QuakeWorld is connectionless: it talks to bare UDP addresses (qw_netadr_t) rather
than the NetQuake qsocket. Rather than duplicate the socket syscalls, this rides
on net_udp.c's generic helpers (UDP_OpenSocket/Read/Write/CloseSocket) -- it just
owns its own client socket and converts between qw_netadr_t and struct qsockaddr.
Received datagrams land in the shared net_message so MSG_Read* parses them.
================================================================================
*/
// arch_def.h sets PLATFORM_UNIX (the DC/KOS BSD-socket path) before net_sys.h
// pulls in <netinet/in.h> etc. for struct sockaddr_in / AF_INET.
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_net.h"
#include "net_udp.h"	/* shared UDP_OpenSocket / UDP_Read / UDP_Write / UDP_CloseSocket */
#include <netdb.h>	/* gethostbyname -- KOS resolves DNS on the Dreamcast */

static sys_socket_t	qw_socket = INVALID_SOCKET;

qw_netadr_t	qw_net_from;
int		qw_net_drop;

// struct qsockaddr is a sockaddr; when qsa_family == AF_INET it is a sockaddr_in.
static void NetadrToQsockaddr (const qw_netadr_t *a, struct qsockaddr *qs)
{
	struct sockaddr_in *s = (struct sockaddr_in *)qs;

	memset (s, 0, sizeof(*s));
	s->sin_family = AF_INET;
	memcpy (&s->sin_addr, a->ip, 4);
	s->sin_port = a->port;
}

static void QsockaddrToNetadr (const struct qsockaddr *qs, qw_netadr_t *a)
{
	const struct sockaddr_in *s = (const struct sockaddr_in *)qs;

	memcpy (a->ip, &s->sin_addr, 4);
	a->port = s->sin_port;
	a->pad = 0;
}

qboolean QWNET_CompareBaseAdr (qw_netadr_t a, qw_netadr_t b)
{
	return (memcmp (a.ip, b.ip, 4) == 0);
}

qboolean QWNET_CompareAdr (qw_netadr_t a, qw_netadr_t b)
{
	return (memcmp (a.ip, b.ip, 4) == 0 && a.port == b.port);
}

const char *QWNET_AdrToString (qw_netadr_t a)
{
	static char	s[32];
	q_snprintf (s, sizeof(s), "%i.%i.%i.%i:%i",
		a.ip[0], a.ip[1], a.ip[2], a.ip[3], (int) ntohs(a.port));
	return s;
}

/*
==============
QWNET_StringToAdr -- parse "host[:port]", where host is a dotted-quad or a name.
Names are resolved through KOS's DNS resolver (blocking, but only used for the
one-off connect/master queries). Port defaults to the QW server port.
==============
*/
qboolean QWNET_StringToAdr (const char *s, qw_netadr_t *a)
{
	char		copy[256], *colon;
	int		b[4], port = QW_PORT_SERVER;
	struct hostent	*h;

	memset (a, 0, sizeof(*a));

	q_strlcpy (copy, s, sizeof(copy));
	colon = strchr (copy, ':');
	if (colon)
	{
		*colon = '\0';
		port = atoi (colon + 1);
	}

	if (sscanf (copy, "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3]) == 4)
	{	// numeric dotted-quad -- no lookup needed
		a->ip[0] = (byte)b[0];
		a->ip[1] = (byte)b[1];
		a->ip[2] = (byte)b[2];
		a->ip[3] = (byte)b[3];
		a->port  = htons ((unsigned short)port);
		return true;
	}

	h = gethostbyname (copy);
	if (!h || !h->h_addr_list || !h->h_addr_list[0] || h->h_length != 4)
	{
		Con_Printf ("[QW] can't resolve \"%s\"\n", copy);
		return false;
	}

	memcpy (a->ip, h->h_addr_list[0], 4);
	a->port = htons ((unsigned short)port);
	return true;
}

/*
==============
QWNET_GetPacket -- read one datagram into net_message; false if nothing waiting
==============
*/
qboolean QWNET_GetPacket (void)
{
	struct qsockaddr	addr;
	int			ret;

	if (qw_socket == INVALID_SOCKET)
		return false;

	ret = UDP_Read (qw_socket, net_message.data, net_message.maxsize, &addr);
	if (ret <= 0)	// 0 = would-block/refused, -1 = error (already reported)
		return false;

	QsockaddrToNetadr (&addr, &qw_net_from);
	net_message.cursize = ret;
	return true;
}

void QWNET_SendPacket (int length, const void *data, qw_netadr_t to)
{
	struct qsockaddr	addr;

	if (qw_socket == INVALID_SOCKET)
		return;

	NetadrToQsockaddr (&to, &addr);
	UDP_Write (qw_socket, (byte *)data, length, &addr);
}

/*
==============
QWNET_Init -- open the client UDP socket (ephemeral port, non-blocking).
UDP_OpenSocket already binds INADDR_ANY and sets FIONBIO.
==============
*/
void QWNET_Init (void)
{
	if (qw_socket != INVALID_SOCKET)
		return;

	qw_socket = UDP_OpenSocket (0);	// ephemeral; the qport field handles NAT remaps
	if (qw_socket == INVALID_SOCKET)
		Con_Printf ("QWNET_Init: UDP_OpenSocket failed; QuakeWorld unavailable\n");
}

void QWNET_Shutdown (void)
{
	if (qw_socket != INVALID_SOCKET)
	{
		UDP_CloseSocket (qw_socket);
		qw_socket = INVALID_SOCKET;
	}
}

#endif	/* USE_QW_PROTOCOL */
