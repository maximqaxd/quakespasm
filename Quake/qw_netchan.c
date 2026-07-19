/*
================================================================================
qw_netchan.c -- QuakeWorld sequenced packet channel (port of QW net_chan.c)

Packet header (8 bytes):
  31  sequence          1  reliable-payload flag
  31  acknowledge seq   1  reliable even/odd bit
  16  qport (client only)

Unreliable by default with a piggy-backed reliable sidechannel: reliable data is
staged in chan->message, moved to reliable_buf on send, and retransmitted until
the ack's even/odd bit matches. The qport lets the server keep the channel
matched even if a NAT router remaps the client's UDP source port.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_net.h"

#define	QW_PACKET_HEADER	8

// qw_net_drop is defined in qw_net.c (declared extern in qw_net.h).

static cvar_t	qw_showpackets = {"qw_showpackets", "0", CVAR_NONE};
static cvar_t	qw_showdrop    = {"qw_showdrop", "0", CVAR_NONE};
cvar_t		qw_qport       = {"qport", "0", CVAR_NONE};

static double	QWNetchan_Rate (void);	// seconds-per-byte send budget from "rate"

/*
===============
QWNetchan_Init -- register cvars and pick a random qport
===============
*/
void QWNetchan_Init (void)
{
	int	port;

	// a nice random-ish port so NAT routers give each client a distinct channel
	port = (int)(Sys_DoubleTime () * 1000.0) & 0xffff;

	Cvar_RegisterVariable (&qw_showpackets);
	Cvar_RegisterVariable (&qw_showdrop);
	Cvar_RegisterVariable (&qw_qport);
	Cvar_SetValue ("qport", (float)port);
	cls.qport = port;
}

/*
===============
QWNetchan_OutOfBand -- send a connectionless (sequence -1) datagram
===============
*/
void QWNetchan_OutOfBand (qw_netadr_t adr, int length, const byte *data)
{
	sizebuf_t	send;
	byte		send_buf[MAX_MSGLEN + QW_PACKET_HEADER];

	send.data = send_buf;
	send.maxsize = sizeof(send_buf);
	send.cursize = 0;
	send.allowoverflow = false;
	send.overflowed = false;

	MSG_WriteLong (&send, -1);	// -1 sequence == out of band
	SZ_Write (&send, data, length);

	QWNET_SendPacket (send.cursize, send.data, adr);
}

void QWNetchan_OutOfBandPrint (qw_netadr_t adr, const char *fmt, ...)
{
	va_list		argptr;
	static char	string[8192];

	va_start (argptr, fmt);
	q_vsnprintf (string, sizeof(string), fmt, argptr);
	va_end (argptr);

	QWNetchan_OutOfBand (adr, (int)strlen(string), (const byte *)string);
}

/*
==============
QWNetchan_Setup -- open a channel to a remote system
==============
*/
void QWNetchan_Setup (qw_netchan_t *chan, qw_netadr_t adr, int qport)
{
	memset (chan, 0, sizeof(*chan));

	chan->remote_address = adr;
	chan->last_received = realtime;

	chan->message.data = chan->message_buf;
	chan->message.allowoverflow = true;
	chan->message.maxsize = sizeof(chan->message_buf);

	chan->qport = qport;
	chan->rate = QWNetchan_Rate ();
}

#define	QW_MAX_BACKUP	200

/*
==============
QWNetchan_Rate -- seconds-per-byte send budget, taken live from the "rate" cvar
(clamped) rather than a fixed 2500 B/s. Reading it every call lets a rate change
(e.g. the Setup-menu presets: modem 2500, BBA/W5500 higher) take effect at once.
Pinning it to modem speed throttled broadcast adapters to ~25 packets/s, which is
what starved movement and forced constant entity-packet flushes at high ping.
==============
*/
static double QWNetchan_Rate (void)
{
	extern cvar_t	qw_rate;
	double		r = qw_rate.value;

	if (r < 500)	r = 500;
	if (r > 100000)	r = 100000;
	return 1.0 / r;
}

qboolean QWNetchan_CanPacket (qw_netchan_t *chan)
{
	return (chan->cleartime < realtime + QW_MAX_BACKUP * QWNetchan_Rate ());
}

qboolean QWNetchan_CanReliable (qw_netchan_t *chan)
{
	if (chan->reliable_length)
		return false;			// waiting for ack
	return QWNetchan_CanPacket (chan);
}

/*
===============
QWNetchan_Transmit -- send an unreliable message, handling reliable (re)transmit.
A zero-length message still emits a packet (and services the reliable channel).
===============
*/
void QWNetchan_Transmit (qw_netchan_t *chan, int length, byte *data)
{
	sizebuf_t	send;
	byte		send_buf[MAX_MSGLEN + QW_PACKET_HEADER];
	qboolean	send_reliable;
	unsigned	w1, w2;
	int		i;

	if (chan->message.overflowed)
	{
		chan->fatal_error = true;
		Con_Printf ("%s: outgoing message overflow\n", QWNET_AdrToString (chan->remote_address));
		return;
	}

	// resend the last reliable if the far side dropped it
	send_reliable = false;
	if (chan->incoming_acknowledged > chan->last_reliable_sequence
	 && chan->incoming_reliable_acknowledged != chan->reliable_sequence)
		send_reliable = true;

	// if the reliable holding buffer is empty, stage the pending message
	if (!chan->reliable_length && chan->message.cursize)
	{
		memcpy (chan->reliable_buf, chan->message_buf, chan->message.cursize);
		chan->reliable_length = chan->message.cursize;
		chan->message.cursize = 0;
		chan->reliable_sequence ^= 1;
		send_reliable = true;
	}

	// build the header
	send.data = send_buf;
	send.maxsize = sizeof(send_buf);
	send.cursize = 0;
	send.allowoverflow = false;
	send.overflowed = false;

	w1 = chan->outgoing_sequence | (send_reliable << 31);
	w2 = chan->incoming_sequence | (chan->incoming_reliable_sequence << 31);

	chan->outgoing_sequence++;

	MSG_WriteLong (&send, w1);
	MSG_WriteLong (&send, w2);
	MSG_WriteShort (&send, (short)cls.qport);	// client sends its qport

	if (send_reliable)
	{
		SZ_Write (&send, chan->reliable_buf, chan->reliable_length);
		chan->last_reliable_sequence = chan->outgoing_sequence;
	}

	// tack on the unreliable payload if it fits
	if (length && send.maxsize - send.cursize >= length)
		SZ_Write (&send, data, length);

	i = chan->outgoing_sequence & (QW_MAX_LATENT - 1);
	chan->outgoing_size[i] = send.cursize;
	chan->outgoing_time[i] = realtime;

	QWNET_SendPacket (send.cursize, send.data, chan->remote_address);

	if (chan->cleartime < realtime)
		chan->cleartime = realtime + send.cursize * QWNetchan_Rate ();
	else
		chan->cleartime += send.cursize * QWNetchan_Rate ();

	if (qw_showpackets.value)
		Con_Printf ("--> s=%i(%i) a=%i(%i) %i\n", chan->outgoing_sequence,
			send_reliable, chan->incoming_sequence, chan->incoming_reliable_sequence,
			send.cursize);
}

/*
=================
QWNetchan_Process -- the current net_message is from qw_net_from; validate the
header, drop stale/duplicate/spoofed packets, and leave net_message pointing at
the payload for the caller to parse. Returns false if the packet should be dropped.
=================
*/
qboolean QWNetchan_Process (qw_netchan_t *chan)
{
	unsigned	sequence, sequence_ack;
	unsigned	reliable_ack, reliable_message;

	if (!QWNET_CompareAdr (qw_net_from, chan->remote_address))
		return false;

	// read the header
	MSG_BeginReading ();
	sequence = MSG_ReadLong ();
	sequence_ack = MSG_ReadLong ();

	reliable_message = sequence >> 31;
	reliable_ack = sequence_ack >> 31;
	sequence &= ~(1u << 31);
	sequence_ack &= ~(1u << 31);

	if (qw_showpackets.value)
		Con_Printf ("<-- s=%i(%i) a=%i(%i) %i\n", sequence, reliable_message,
			sequence_ack, reliable_ack, net_message.cursize);

	// discard stale or duplicated packets
	if (sequence <= (unsigned)chan->incoming_sequence)
	{
		if (qw_showdrop.value)
			Con_Printf ("%s: out of order packet %i at %i\n",
				QWNET_AdrToString (chan->remote_address), sequence, chan->incoming_sequence);
		return false;
	}

	// count dropped packets (they don't stop this message being used)
	qw_net_drop = sequence - (chan->incoming_sequence + 1);
	if (qw_net_drop > 0)
	{
		chan->drop_count += 1;
		if (qw_showdrop.value)
			Con_Printf ("%s: dropped %i packets at %i\n",
				QWNET_AdrToString (chan->remote_address), qw_net_drop, sequence);
	}

	// if our outgoing reliable was acked, free the holding buffer
	if (reliable_ack == (unsigned)chan->reliable_sequence)
		chan->reliable_length = 0;

	chan->incoming_sequence = sequence;
	chan->incoming_acknowledged = sequence_ack;
	chan->incoming_reliable_acknowledged = reliable_ack;
	if (reliable_message)
		chan->incoming_reliable_sequence ^= 1;

	// update rolling stats
	chan->frame_latency = chan->frame_latency * QW_OLD_AVG
		+ (chan->outgoing_sequence - sequence_ack) * (1.0 - QW_OLD_AVG);
	chan->frame_rate = chan->frame_rate * QW_OLD_AVG
		+ (realtime - chan->last_received) * (1.0 - QW_OLD_AVG);
	chan->good_count += 1;
	chan->last_received = realtime;

	return true;
}

#endif	/* USE_QW_PROTOCOL */
