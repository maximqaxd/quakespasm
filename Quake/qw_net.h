/*
================================================================================
qw_net.h -- QuakeWorld raw UDP + netchan (protocol 28)

QuakeWorld is connectionless: the client talks to a bare UDP address (qw_netadr_t)
rather than QuakeSpasm's connection-oriented qsocket. This layer provides the raw
send/recv and the sequenced reliable/unreliable netchan the QW client rides on,
kept entirely separate from the NetQuake net_dgrm/net_udp path. Everything is
namespaced QW / qw_ so both families coexist in one build.

Received packets land in QuakeSpasm's global net_message so the shared MSG_Read*
machinery parses them.
================================================================================
*/
#ifndef QW_NET_H
#define QW_NET_H

#if defined(USE_QW_PROTOCOL)

// A QuakeWorld network address: IPv4 + UDP port (port in network byte order).
typedef struct
{
	byte		ip[4];
	unsigned short	port;
	unsigned short	pad;
} qw_netadr_t;

extern qw_netadr_t	qw_net_from;	// source address of the last received packet
extern int		qw_net_drop;	// packets dropped before the current one

// --- raw connectionless UDP (qw_net.c) ---------------------------------------
void		QWNET_Init (void);
void		QWNET_Shutdown (void);
qboolean	QWNET_GetPacket (void);	// fills net_message + qw_net_from; false if none
void		QWNET_SendPacket (int length, const void *data, qw_netadr_t to);
qboolean	QWNET_CompareAdr (qw_netadr_t a, qw_netadr_t b);
qboolean	QWNET_CompareBaseAdr (qw_netadr_t a, qw_netadr_t b);	// ignores port
const char     *QWNET_AdrToString (qw_netadr_t a);
qboolean	QWNET_StringToAdr (const char *s, qw_netadr_t *a);	// "1.2.3.4[:port]"

// --- netchan (qw_netchan.c) --------------------------------------------------
#define	QW_MAX_LATENT	32
#define	QW_OLD_AVG	0.99		// rolling-average weight for latency/rate stats

typedef struct
{
	qboolean	fatal_error;
	float		last_received;		// for timeouts

	// stats (cleared each level)
	float		frame_latency;		// rolling average
	float		frame_rate;
	int		drop_count;
	int		good_count;

	qw_netadr_t	remote_address;
	int		qport;

	// bandwidth estimator
	double		cleartime;		// realtime > cleartime -> free to send
	double		rate;			// seconds / byte

	// sequencing
	int		incoming_sequence;
	int		incoming_acknowledged;
	int		incoming_reliable_acknowledged;	// single bit
	int		incoming_reliable_sequence;	// single bit, maintained local
	int		outgoing_sequence;
	int		reliable_sequence;		// single bit
	int		last_reliable_sequence;

	// reliable staging + holding
	sizebuf_t	message;		// writing buffer to send to server
	byte		message_buf[MAX_MSGLEN];
	int		reliable_length;
	byte		reliable_buf[MAX_MSGLEN];	// unacked reliable message

	// bandwidth history
	int		outgoing_size[QW_MAX_LATENT];
	double		outgoing_time[QW_MAX_LATENT];
} qw_netchan_t;

void		QWNetchan_Init (void);
void		QWNetchan_Setup (qw_netchan_t *chan, qw_netadr_t adr, int qport);
void		QWNetchan_Transmit (qw_netchan_t *chan, int length, byte *data);
void		QWNetchan_OutOfBand (qw_netadr_t adr, int length, const byte *data);
void		QWNetchan_OutOfBandPrint (qw_netadr_t adr, const char *fmt, ...) FUNC_PRINTF(2,3);
qboolean	QWNetchan_Process (qw_netchan_t *chan);
qboolean	QWNetchan_CanPacket (qw_netchan_t *chan);
qboolean	QWNetchan_CanReliable (qw_netchan_t *chan);

#endif	/* USE_QW_PROTOCOL */

#endif	/* QW_NET_H */
