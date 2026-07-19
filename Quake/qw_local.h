/*
================================================================================
qw_local.h -- QuakeWorld (protocol 28) client definitions

Backport of id's QuakeWorld network protocol as a runtime-selectable family
alongside QuakeSpasm's native NetQuake. Client-only: the Dreamcast connects to
existing QW servers; it does not host QW games.

All protocol constants are prefixed qw / QW because NetQuake already defines
svc_ and clc_ with different values -- the two families must coexist in one build.
Everything here is compiled only when USE_QW_PROTOCOL is defined.
================================================================================
*/
#ifndef QW_LOCAL_H
#define QW_LOCAL_H

#if defined(USE_QW_PROTOCOL)

#define	QW_PROTOCOL_VERSION	28
#define	QW_CHECK_HASH		0x5157

#define	QW_PORT_CLIENT		27001
#define	QW_PORT_MASTER		27000
#define	QW_PORT_SERVER		27500

// Out-of-band (connectionless) message id bytes. M=master S=server C=client A=any.
#define	QW_S2C_CHALLENGE	'c'
#define	QW_S2C_CONNECTION	'j'
#define	QW_A2A_PING		'k'
#define	QW_A2A_ACK		'l'
#define	QW_A2A_NACK		'm'
#define	QW_A2A_ECHO		'e'
#define	QW_A2C_PRINT		'n'
#define	QW_S2M_HEARTBEAT	'a'
#define	QW_A2C_CLIENT_COMMAND	'B'
#define	QW_S2M_SHUTDOWN		'C'

// Server -> client message opcodes (protocol 28).
#define	qwsvc_bad		0
#define	qwsvc_nop		1
#define	qwsvc_disconnect	2
#define	qwsvc_updatestat	3	// [byte] [byte]
#define	qwsvc_setview		5	// [short] entity number
#define	qwsvc_sound		6	// <see code>
#define	qwsvc_print		8	// [byte] id [string]
#define	qwsvc_stufftext		9	// [string] into client console
#define	qwsvc_setangle		10	// [angle3]
#define	qwsvc_serverdata	11	// [long] protocol ...
#define	qwsvc_lightstyle	12	// [byte] [string]
#define	qwsvc_updatefrags	14	// [byte] [short]
#define	qwsvc_stopsound		16	// <see code>
#define	qwsvc_damage		19
#define	qwsvc_spawnstatic	20
#define	qwsvc_spawnbaseline	22
#define	qwsvc_temp_entity	23	// variable
#define	qwsvc_setpause		24	// [byte] on/off
#define	qwsvc_centerprint	26	// [string]
#define	qwsvc_killedmonster	27
#define	qwsvc_foundsecret	28
#define	qwsvc_spawnstaticsound	29	// [coord3] [byte]*3
#define	qwsvc_intermission	30	// [vec3] origin [vec3] angle
#define	qwsvc_finale		31	// [string]
#define	qwsvc_cdtrack		32	// [byte] track
#define	qwsvc_sellscreen	33
#define	qwsvc_smallkick		34	// punchangle 2
#define	qwsvc_bigkick		35	// punchangle 4
#define	qwsvc_updateping	36	// [byte] [short]
#define	qwsvc_updateentertime	37	// [byte] [float]
#define	qwsvc_updatestatlong	38	// [byte] [long]
#define	qwsvc_muzzleflash	39	// [short] entity
#define	qwsvc_updateuserinfo	40	// [byte] slot [long] uid [string]
#define	qwsvc_download		41	// [short] size [bytes]
#define	qwsvc_playerinfo	42	// variable
#define	qwsvc_nails		43	// [byte] num [48 bits]*num
#define	qwsvc_chokecount	44	// [byte] packets choked
#define	qwsvc_modellist		45	// [strings]
#define	qwsvc_soundlist		46	// [strings]
#define	qwsvc_packetentities	47	// [...]
#define	qwsvc_deltapacketentities 48	// [...]
#define	qwsvc_maxspeed		49	// maxspeed change (prediction)
#define	qwsvc_entgravity	50	// gravity change (prediction)
#define	qwsvc_setinfo		51	// setinfo on a client
#define	qwsvc_serverinfo	52	// serverinfo
#define	qwsvc_updatepl		53	// [byte] [byte]

// Client -> server message opcodes.
#define	qwclc_bad		0
#define	qwclc_nop		1
#define	qwclc_move		3	// [[usercmd_t]
#define	qwclc_stringcmd		4	// [string]
#define	qwclc_delta		5	// [byte] sequence
#define	qwclc_tmove		6	// spectator teleport
#define	qwclc_upload		7

// playerinfo flags (always: playernum, flags, origin[], framenumber)
#define	QWPF_MSEC		(1<<0)
#define	QWPF_COMMAND		(1<<1)
#define	QWPF_VELOCITY1		(1<<2)
#define	QWPF_VELOCITY2		(1<<3)
#define	QWPF_VELOCITY3		(1<<4)
#define	QWPF_MODEL		(1<<5)
#define	QWPF_SKINNUM		(1<<6)
#define	QWPF_EFFECTS		(1<<7)
#define	QWPF_WEAPONFRAME	(1<<8)	// only for view player
#define	QWPF_DEAD		(1<<9)
#define	QWPF_GIB		(1<<10)
#define	QWPF_NOGRAV		(1<<11)

// usercmd delta-compression bits (high bit of the cmd byte flags move bits)
#define	QWCM_ANGLE1		(1<<0)
#define	QWCM_ANGLE3		(1<<1)
#define	QWCM_FORWARD		(1<<2)
#define	QWCM_SIDE		(1<<3)
#define	QWCM_UP			(1<<4)
#define	QWCM_BUTTONS		(1<<5)
#define	QWCM_IMPULSE		(1<<6)
#define	QWCM_ANGLE2		(1<<7)

// packetentities update bits: first 16 bits = 9-bit entity num + 7 flag bits
#define	QWU_ORIGIN1		(1<<9)
#define	QWU_ORIGIN2		(1<<10)
#define	QWU_ORIGIN3		(1<<11)
#define	QWU_ANGLE2		(1<<12)
#define	QWU_FRAME		(1<<13)
#define	QWU_REMOVE		(1<<14)
#define	QWU_MOREBITS		(1<<15)
// with MOREBITS set, these come next
#define	QWU_ANGLE1		(1<<0)
#define	QWU_ANGLE3		(1<<1)
#define	QWU_MODEL		(1<<2)
#define	QWU_COLORMAP		(1<<3)
#define	QWU_SKIN		(1<<4)
#define	QWU_EFFECTS		(1<<5)
#define	QWU_SOLID		(1<<6)	// solid for prediction

// sound: bits 0-2 channel, 3-12 entity
#define	QWSND_VOLUME		(1<<15)
#define	QWSND_ATTENUATION	(1<<14)

// Protocol family selected per connection. NetQuake stays the default; QW is
// chosen by the qwconnect command / a QW server address.
typedef enum
{
	PROTO_NQ = 0,	// native NetQuake (net_dgrm / protocols 15/666/999)
	PROTO_QW	// QuakeWorld protocol 28 (netchan)
} protofamily_t;

// Server-side movement constants sent in svc_serverdata; needed by client-side
// prediction (phase 4). Stored now so the serverdata parse stays in sync.
typedef struct
{
	float	gravity;
	float	stopspeed;
	float	maxspeed;
	float	spectatormaxspeed;
	float	accelerate;
	float	airaccelerate;
	float	wateraccelerate;
	float	friction;
	float	waterfriction;
	float	entgravity;
} qw_movevars_t;

extern qw_movevars_t	qw_movevars;

// QuakeWorld signon/connection state that has no NetQuake equivalent.
typedef struct
{
	int		servercount;	// server's spawn count, echoed in every request
	int		playernum;	// our client slot
	qboolean	spectator;
	char		gamedir[64];	// server's gamedir
	char		levelname[40];	// full level name for the console/scoreboard
	int		num_models;	// running precache counts (filled from svc_modellist/soundlist)
	int		num_sounds;
} qwcl_state_t;

extern qwcl_state_t	qwcl;

// --- module entry points (filled in across the port phases) ------------------
void	CLQW_Init (void);				// one-time QW client init (from CL_Init)
void	CLQW_EstablishConnection (const char *host);	// connectionless handshake start
void	CLQW_RunConnection (void);			// per-frame pump (from Host_Frame)
void	CLQW_ParseServerMessage (void);			// parse a netchan message (qw_cl_parse.c)
qboolean CLQW_IsConnected (void);			// netchan established?

#endif	/* USE_QW_PROTOCOL */

#endif	/* QW_LOCAL_H */
