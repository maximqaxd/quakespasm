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

// A QuakeWorld user command (movement input sent to the server each frame).
typedef struct
{
	byte	msec;			// duration of this command
	vec3_t	angles;			// view angles
	short	forwardmove, sidemove, upmove;
	byte	buttons;		// bit 0 attack, bit 1 jump
	byte	impulse;
} qw_usercmd_t;

#define	QW_UPDATE_BACKUP	64	// ring of recent commands (indexed by netchan seq)
#define	QW_UPDATE_MASK		(QW_UPDATE_BACKUP - 1)

// --- client-side movement prediction (phase 4b) ------------------------------
// The player physics (qw_pmove.c) run the same moves the server runs, so we can
// simulate our own position from the last acknowledged state forward through the
// not-yet-acked commands and render without waiting for the round trip.

typedef struct
{
	vec3_t	normal;
	float	dist;
} qw_pmplane_t;

typedef struct
{
	qboolean	allsolid;	// if true, plane is not valid
	qboolean	startsolid;	// if true, the initial point was in solid
	qboolean	inopen, inwater;
	float		fraction;	// time completed, 1.0 = didn't hit anything
	vec3_t		endpos;		// final position
	qw_pmplane_t	plane;		// surface normal at impact
	int		ent;		// entity the surface is on
} qw_pmtrace_t;

#define	QW_MAX_PHYSENTS	32
typedef struct
{
	vec3_t			origin;
	struct qmodel_s		*model;		// only for bsp models
	vec3_t			mins, maxs;	// only for non-bsp models
	int			info;		// identifies the entity
} qw_physent_t;

typedef struct
{
	int		sequence;	// just for debugging prints

	// player state
	vec3_t		origin;
	vec3_t		angles;
	vec3_t		velocity;
	int		oldbuttons;
	float		waterjumptime;
	qboolean	dead;
	int		spectator;

	// world state
	int		numphysent;
	qw_physent_t	physents[QW_MAX_PHYSENTS];	// 0 is the world

	// input
	qw_usercmd_t	cmd;

	// results
	int		numtouch;
	int		touchindex[QW_MAX_PHYSENTS];
} qw_playermove_t;

// Per-frame player state used by prediction: the server-authoritative snapshot
// for our slot, then overwritten with predicted results as later commands are
// replayed through the physics.
typedef struct
{
	vec3_t	origin;
	vec3_t	velocity;
	vec3_t	viewangles;
	int	weaponframe;
	int	onground;	// -1 = in air, else physent number
	int	oldbuttons;
	float	waterjumptime;
} qw_playerstate_t;

// A slot in the command/state ring, indexed by netchan sequence. The command is
// filled when we send it (outgoing sequence); the playerstate is filled from the
// server snapshot that acknowledges it (incoming_acknowledged).
typedef struct
{
	qw_usercmd_t		cmd;		// command built for this outgoing sequence
	double			senttime;	// realtime the command was sent
	qboolean		playervalid;	// playerstate holds a server snapshot
	qw_playerstate_t	playerstate;	// server truth (at ack), then predicted
} qw_frame_t;

extern qw_frame_t	qw_frames[QW_UPDATE_BACKUP];
extern int		qw_validsequence;	// netchan seq of the last good snapshot
extern vec3_t		qw_simorg;		// predicted view origin
extern vec3_t		qw_simvel;		// predicted velocity
extern vec3_t		qw_simangles;		// predicted view angles

// --- entity snapshots (phase 3b) ---------------------------------------------
// QW streams the world as delta-compressed packet-entity snapshots: each server
// frame carries only what changed since a previous frame the client still holds.
// We decode those into a snapshot ring, then stamp the engine's persistent
// cl_entities[] and push pointers into cl_visedicts[] each rendered frame.

// QW entity-effect bits above the four NetQuake shares (BRIGHTLIGHT/DIMLIGHT).
#define	QWEF_FLAG1	16
#define	QWEF_FLAG2	32
#define	QWEF_BLUE	64
#define	QWEF_RED	128

#define	QW_MAX_CLIENTS		32
#define	QW_MAX_EDICTS		512	// entity number is 9 bits in the delta word
#define	QW_MAX_PACKET_ENTITIES	64	// most entities in one snapshot

typedef struct
{
	int	number;		// edict index this state belongs to
	int	flags;		// QWU_* bits that were present
	vec3_t	origin;
	vec3_t	angles;
	int	modelindex;
	int	frame;
	int	colormap;
	int	skinnum;
	int	effects;
} qw_entity_state_t;

typedef struct
{
	int			sequence;	// which server frame this snapshot is
	int			num_entities;
	qw_entity_state_t	entities[QW_MAX_PACKET_ENTITIES];
} qw_packet_entities_t;

extern qw_entity_state_t	*qw_baselines;	// [QW_MAX_EDICTS], hunk-allocated

// Other players are delivered as playerinfo, not packet entities; keep the last
// state per slot for rendering (our own slot is driven by prediction instead).
// Their last command + the time their state was valid lets us simulate them
// forward to now, the reference client's answer to laggy player models.
typedef struct
{
	int		messagenum;	// == qw_parsecount on the frame this was received
	double		state_time;	// when this state was valid on the server
	vec3_t		origin;
	vec3_t		viewangles;
	vec3_t		velocity;
	qw_usercmd_t	cmd;		// the player's last move command
	int		modelindex;
	int		frame;
	int		skinnum;
	int		effects;
	int		flags;
} qw_player_render_t;

extern qw_player_render_t	qw_players[QW_MAX_CLIENTS];
extern int			qw_parsecount;	// bumped per server message

// Model indices resolved from the precache list once the model list is in.
extern int	qw_playerindex;		// progs/player.mdl (default player model)
extern int	qw_spikeindex;		// progs/spike.mdl (nail projectiles)
extern int	qw_flagindex;		// progs/flag.mdl (CTF)

// entity parsing / linking (qw_cl_ents.c)
void	CLQW_ClearEntities (void);		// wipe snapshot/baseline state
void	CLQW_FindModelNumbers (void);		// resolve qw_*index from precache
void	CLQW_ParseBaseline (int num);		// into qw_baselines[num]
void	CLQW_ParsePacketEntities (qboolean delta);
int	CLQW_DeltaSequence (void);		// snapshot to request deltas from, -1 = none
void	CLQW_ParseProjectiles (void);		// svc_nails
void	CLQW_ClearProjectiles (void);
void	CLQW_EmitEntities (void);		// build cl_visedicts for this frame

// player physics (qw_pmove.c)
extern qw_playermove_t	qw_pmove;
extern int		qw_onground;
extern int		qw_waterlevel;
extern int		qw_watertype;

void	QWPM_Init (void);				// one-time box-hull setup
void	QWPM_PlayerMove (void);				// run one command's physics
void	QWPM_AddBoxPhysent (const vec3_t origin);	// player-sized solid box

extern double	qw_latency;		// smoothed round-trip estimate (qw_cl_main.c)

// prediction (qw_cl_pred.c)
extern cvar_t	cl_predict_players;	// simulate other players forward to now
extern cvar_t	cl_solid_players;	// clip our movement against other players
void	CLQW_InitPrediction (void);
void	CLQW_PredictMove (void);
void	CLQW_PredictUsercmd (qw_playerstate_t *from, qw_playerstate_t *to, qw_usercmd_t *u, qboolean spectator);
void	CLQW_CalcPredictionError (const vec3_t predicted, const vec3_t server);

// --- module entry points (filled in across the port phases) ------------------
void	CLQW_Init (void);				// one-time QW client init (from CL_Init)
void	CLQW_EstablishConnection (const char *host);	// connectionless handshake start
void	CLQW_RunConnection (void);			// per-frame pump (from Host_Frame)
void	CLQW_ParseServerMessage (void);			// parse a netchan message (qw_cl_parse.c)
qboolean CLQW_IsConnected (void);			// netchan established?
void	CLQW_SendMove (void);				// build + send clc_move (qw_cl_input.c)
unsigned Com_BlockChecksum (const void *buffer, int length);	// MD4 fold (qw_md4.c)
byte	COM_BlockSequenceCRCByte (byte *base, int length, int sequence);	// move CRC (qw_crc.c)

#endif	/* USE_QW_PROTOCOL */

#endif	/* QW_LOCAL_H */
