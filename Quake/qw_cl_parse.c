/*
================================================================================
qw_cl_parse.c -- QuakeWorld server-message parser (protocol 28)

Phase 2a: the dispatch loop plus svc_serverdata (which starts the signon by
requesting the sound list) and the signon-safe opcodes. Opcodes that carry the
precache lists, baselines and entity deltas (soundlist/modellist/spawnbaseline/
packetentities/playerinfo/...) are phase 2b/3 -- for now they stop the parse
cleanly rather than desync the stream.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_movevars_t	qw_movevars;
qwcl_state_t	qwcl;

/*
==============
CLQW_ParseServerData -- svc_serverdata: protocol, gamedir, our slot, level,
movevars. Wipes the client state and asks for the sound list to begin the signon.
==============
*/
static void CLQW_ParseServerData (void)
{
	const char	*str;
	int		protover;

	Con_DPrintf ("[QW] serverdata packet received\n");

	CL_ClearState ();
	CLQW_ClearEntities ();

	// QW is always multiplayer: size the scoreboard (names, frags, colors) for
	// every slot. This also arms the renderer's player-color path, which keys
	// off cl.maxclients.
	cl.maxclients = QW_MAX_CLIENTS;
	cl.scores = (scoreboard_t *) Hunk_AllocName (cl.maxclients * sizeof(*cl.scores), "scores");

	protover = MSG_ReadLong ();
	if (protover != QW_PROTOCOL_VERSION)
	{
		Con_Printf ("[QW] server returned protocol %i, expected %i\n", protover, QW_PROTOCOL_VERSION);
		CL_Disconnect ();
		return;
	}

	qwcl.servercount = MSG_ReadLong ();

	str = MSG_ReadString ();		// gamedir
	q_strlcpy (qwcl.gamedir, str, sizeof(qwcl.gamedir));

	qwcl.playernum = MSG_ReadByte ();	// high bit = spectator
	qwcl.spectator = (qwcl.playernum & 128) != 0;
	qwcl.playernum &= ~128;

	str = MSG_ReadString ();		// full level name
	q_strlcpy (qwcl.levelname, str, sizeof(qwcl.levelname));
	q_strlcpy (cl.levelname, str, sizeof(cl.levelname));

	qw_movevars.gravity           = MSG_ReadFloat ();
	qw_movevars.stopspeed         = MSG_ReadFloat ();
	qw_movevars.maxspeed          = MSG_ReadFloat ();
	qw_movevars.spectatormaxspeed = MSG_ReadFloat ();
	qw_movevars.accelerate        = MSG_ReadFloat ();
	qw_movevars.airaccelerate     = MSG_ReadFloat ();
	qw_movevars.wateraccelerate   = MSG_ReadFloat ();
	qw_movevars.friction          = MSG_ReadFloat ();
	qw_movevars.waterfriction     = MSG_ReadFloat ();
	qw_movevars.entgravity        = MSG_ReadFloat ();

	Con_Printf ("[QW] connected to \"%s\" (gamedir %s, slot %i%s)\n",
		qwcl.levelname, qwcl.gamedir, qwcl.playernum,
		qwcl.spectator ? ", spectator" : "");

	// ask for the sound list to begin the signon sequence (phase 2b parses it)
	MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
	MSG_WriteString (&cls.netchan.message, va("soundlist %i 0", qwcl.servercount));
}

/*
==============
CLQW_ParseSoundlist -- svc_soundlist: a chunk of sound names. Precache each; if
more chunks remain request the next, else move on to the model list.
==============
*/
static void CLQW_ParseSoundlist (void)
{
	const char	*str;
	int		numsounds, n;

	numsounds = MSG_ReadByte ();	// index the server is starting this chunk from
	for (;;)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (++numsounds >= MAX_SOUNDS)
		{
			Con_Printf ("[QW] too many sounds\n");
			CL_Disconnect ();
			return;
		}
		cl.sound_precache[numsounds] = S_PrecacheSound (str);
	}
	qwcl.num_sounds = numsounds;

	n = MSG_ReadByte ();
	if (n)		// more chunks
	{
		MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
		MSG_WriteString (&cls.netchan.message, va("soundlist %i %i", qwcl.servercount, n));
		return;
	}

	// sounds done -> request the model list
	MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
	MSG_WriteString (&cls.netchan.message, va("modellist %i 0", qwcl.servercount));
}

/*
==============
CLQW_ParseModellist -- svc_modellist: a chunk of model names. model_precache[1]
is the world. When the last chunk arrives, load the map and ask to prespawn.
==============
*/
static void CLQW_ParseModellist (void)
{
	const char	*str;
	int		nummodels, n;

	nummodels = MSG_ReadByte ();
	for (;;)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (++nummodels >= MAX_MODELS)
		{
			Con_Printf ("[QW] too many models\n");
			CL_Disconnect ();
			return;
		}
		cl.model_precache[nummodels] = Mod_ForName (str, false);
		if (!cl.model_precache[nummodels])
			Con_Printf ("[QW] missing model \"%s\"\n", str);
	}
	qwcl.num_models = nummodels;

	n = MSG_ReadByte ();
	if (n)		// more chunks
	{
		MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
		MSG_WriteString (&cls.netchan.message, va("modellist %i %i", qwcl.servercount, n));
		return;
	}

	// models done -> bring up the world
	cl.worldmodel = cl.model_precache[1];
	if (!cl.worldmodel)
	{
		Con_Printf ("[QW] worldmodel not found -- map data missing on the client\n");
		CL_Disconnect ();
		return;
	}
	R_NewMap ();
	CLQW_FindModelNumbers ();	// resolve player/spike/flag model indices
	Con_Printf ("[QW] map loaded: %s\n", cl.worldmodel->name);

	// The world is up: let the engine render it. (No player position until the
	// first frame arrives in phase 3, so the view starts at the origin.)
	cls.signon = SIGNONS;
	cls.state = ca_connected;

	// prespawn with the QW map checksum so the server accepts us
	MSG_WriteByte (&cls.netchan.message, qwclc_stringcmd);
	MSG_WriteString (&cls.netchan.message,
		va("prespawn %i 0 %u", qwcl.servercount, cl.worldmodel->checksum2));
}

/*
==============
CLQW_ParseStatic -- svc_spawnstatic: a permanent, unmoving entity (torches,
flames, static lights). Build a renderable entity and insert it into the world's
efrag lists the same way NetQuake's CL_ParseStatic does.
==============
*/
static void CLQW_ParseStatic (void)
{
	entity_t	*ent;
	int		i, modelindex, frame, skinnum;
	vec3_t		origin, angles;

	modelindex = MSG_ReadByte ();
	frame = MSG_ReadByte ();
	(void) MSG_ReadByte ();		// colormap (unused for statics)
	skinnum = MSG_ReadByte ();
	for (i = 0; i < 3; i++)
	{
		origin[i] = MSG_ReadCoord (0);
		angles[i] = MSG_ReadAngle (0);
	}

	if (cl.num_statics >= MAX_STATIC_ENTITIES)
		return;			// drop extras rather than error out
	if (modelindex <= 0 || modelindex >= MAX_MODELS || !cl.model_precache[modelindex])
		return;			// nothing to draw, but bytes are consumed

	ent = &cl_static_entities[cl.num_statics++];
	memset (ent, 0, sizeof(*ent));
	ent->model = cl.model_precache[modelindex];
	ent->lerpflags |= LERP_RESETANIM;
	ent->frame = frame;
	ent->colormap = vid.colormap;
	ent->skinnum = skinnum;
	ent->alpha = 0;			// ENTALPHA_DEFAULT
	ent->scale = ENTSCALE_DEFAULT;
	VectorCopy (origin, ent->origin);
	VectorCopy (angles, ent->angles);
	R_AddEfrags (ent);
}

// ---------------------------------------------------------------------------
// Game-frame parsing: consume every per-frame opcode, drive the view from our
// own player's origin (prediction), and hand entity snapshots to qw_cl_ents.c.
// ---------------------------------------------------------------------------

// A QW usercmd delta (inside playerinfo PF_COMMAND), against the null command.
// It is the player's last move -- forward-prediction replays it to bring their
// model up to the present.
static void CLQW_ReadDeltaUsercmd (qw_usercmd_t *cmd)
{
	int	bits = MSG_ReadByte ();

	memset (cmd, 0, sizeof(*cmd));
	if (bits & QWCM_ANGLE1)  cmd->angles[0] = MSG_ReadAngle16 (0);
	if (bits & QWCM_ANGLE2)  cmd->angles[1] = MSG_ReadAngle16 (0);
	if (bits & QWCM_ANGLE3)  cmd->angles[2] = MSG_ReadAngle16 (0);
	if (bits & QWCM_FORWARD) cmd->forwardmove = MSG_ReadShort ();
	if (bits & QWCM_SIDE)    cmd->sidemove = MSG_ReadShort ();
	if (bits & QWCM_UP)      cmd->upmove = MSG_ReadShort ();
	if (bits & QWCM_BUTTONS) cmd->buttons = MSG_ReadByte ();
	if (bits & QWCM_IMPULSE) cmd->impulse = MSG_ReadByte ();
	cmd->msec = MSG_ReadByte ();
}

// svc_playerinfo: a player's state this frame. Every player is stored for
// rendering; for our own slot we also stash the server-authoritative
// origin/velocity so prediction can run forward from it.
static void CLQW_ParsePlayerinfo (void)
{
	int		num, flags, i;
	vec3_t		origin, velocity;
	qw_usercmd_t	plcmd;
	double		statetime;
	int		frame, modelindex, skinnum = 0, effects = 0, weaponframe = 0;

	num = MSG_ReadByte ();
	flags = MSG_ReadShort ();
	origin[0] = MSG_ReadCoord (0);
	origin[1] = MSG_ReadCoord (0);
	origin[2] = MSG_ReadCoord (0);
	frame = MSG_ReadByte ();

	// the player's move ran some time before this packet was sent; anchor to
	// when we sent the command this snapshot acknowledges, minus that delay
	statetime = qw_frames[cls.netchan.incoming_acknowledged & QW_UPDATE_MASK].senttime;
	if (flags & QWPF_MSEC)
		statetime -= MSG_ReadByte () * 0.001;

	if (flags & QWPF_COMMAND)
		CLQW_ReadDeltaUsercmd (&plcmd);
	VectorCopy (vec3_origin, velocity);
	for (i = 0; i < 3; i++)
		if (flags & (QWPF_VELOCITY1 << i))
			velocity[i] = MSG_ReadShort ();
	modelindex = (flags & QWPF_MODEL) ? MSG_ReadByte () : qw_playerindex;
	if (flags & QWPF_SKINNUM)     skinnum = MSG_ReadByte ();
	if (flags & QWPF_EFFECTS)     effects = MSG_ReadByte ();
	if (flags & QWPF_WEAPONFRAME) weaponframe = MSG_ReadByte ();

	// remember every player's state so CLQW_LinkPlayers can draw them
	if (num < QW_MAX_CLIENTS)
	{
		qw_player_render_t *pl = &qw_players[num];
		pl->messagenum = qw_parsecount;
		pl->state_time = statetime;
		VectorCopy (origin, pl->origin);
		VectorCopy (velocity, pl->velocity);
		if (flags & QWPF_COMMAND)
		{	// bots and dead players come without a command; keep the last
			// one so they don't snap to yaw 0 and stall the forward sim
			VectorCopy (plcmd.angles, pl->viewangles);
			pl->cmd = plcmd;
		}
		pl->modelindex = modelindex;
		pl->frame = frame;
		pl->skinnum = skinnum;
		pl->effects = effects;
		pl->flags = flags;
	}

	if (num == qwcl.playernum)
	{	// this is us: the snapshot acknowledges commands up through
		// incoming_acknowledged, so store it under that sequence
		int			seq = cls.netchan.incoming_acknowledged;
		qw_playerstate_t	*ps = &qw_frames[seq & QW_UPDATE_MASK].playerstate;

		// the slot still holds our prediction for this command; measure the
		// miss (and smooth it out) before overwriting with server truth
		if (qw_validsequence)
			CLQW_CalcPredictionError (ps->origin, origin);

		VectorCopy (origin, ps->origin);
		VectorCopy (velocity, ps->velocity);
		ps->weaponframe = weaponframe;
		// oldbuttons/waterjumptime/onground are not sent by the server; leave
		// the predicted values in the slot so jump-hold and waterjump stay
		// continuous across the snapshot boundary
		qw_frames[seq & QW_UPDATE_MASK].playervalid = true;
		qw_validsequence = seq;

		cl.viewentity = num + 1;
		cl.viewheight = DEFAULT_VIEWHEIGHT;

		// QW carries the view weapon's animation frame in playerinfo, not as a
		// stat; feed it to STAT_WEAPONFRAME where V_CalcRefdef reads it so the
		// gun animates when firing.
		cl.stats[STAT_WEAPONFRAME] = weaponframe;
	}
}

// svc_sound: play a precached sound at a world position.
static void CLQW_ParseSound (void)
{
	int	channel, ent, sound_num, i;
	float	volume = 255.0f, attenuation = 1.0f;
	vec3_t	pos;

	channel = (unsigned short) MSG_ReadShort ();
	if (channel & QWSND_VOLUME)
		volume = MSG_ReadByte ();
	if (channel & QWSND_ATTENUATION)
		attenuation = MSG_ReadByte () / 64.0f;
	sound_num = MSG_ReadByte ();
	for (i = 0; i < 3; i++)
		pos[i] = MSG_ReadCoord (0);

	ent = (channel >> 3) & 1023;
	channel &= 7;
	if (sound_num < MAX_SOUNDS && cl.sound_precache[sound_num])
		S_StartSound (ent, channel, cl.sound_precache[sound_num], pos, volume / 255.0f, attenuation);
}

// QuakeWorld temp-entity types. They differ from NetQuake's: GUNSHOT and BLOOD
// carry a particle count, and 12/13 are BLOOD/LIGHTNINGBLOOD (not EXPLOSION2/BEAM).
#define	QWTE_SPIKE		0
#define	QWTE_SUPERSPIKE		1
#define	QWTE_GUNSHOT		2
#define	QWTE_EXPLOSION		3
#define	QWTE_TAREXPLOSION	4
#define	QWTE_LIGHTNING1		5
#define	QWTE_LIGHTNING2		6
#define	QWTE_WIZSPIKE		7
#define	QWTE_KNIGHTSPIKE	8
#define	QWTE_LIGHTNING3		9
#define	QWTE_LAVASPLASH		10
#define	QWTE_TELEPORT		11
#define	QWTE_BLOOD		12
#define	QWTE_LIGHTNINGBLOOD	13

// shared temp-entity sounds and beam parser, defined in cl_tent.c
extern sfx_t	*cl_sfx_wizhit, *cl_sfx_knighthit, *cl_sfx_tink1;
extern sfx_t	*cl_sfx_ric1, *cl_sfx_ric2, *cl_sfx_ric3, *cl_sfx_r_exp3;
void CL_ParseBeam (qmodel_t *m);

// spike/superspike ricochet: mostly a tink, occasionally a random ricochet
static void CLQW_RicochetSound (vec3_t pos)
{
	if (rand () % 5)
		S_StartSound (-1, 0, cl_sfx_tink1, pos, 1, 1);
	else
	{
		int rnd = rand () & 3;
		if (rnd == 1)      S_StartSound (-1, 0, cl_sfx_ric1, pos, 1, 1);
		else if (rnd == 2) S_StartSound (-1, 0, cl_sfx_ric2, pos, 1, 1);
		else               S_StartSound (-1, 0, cl_sfx_ric3, pos, 1, 1);
	}
}

// svc_temp_entity: one-shot particle/light/beam effects.
static void CLQW_ParseTEnt (void)
{
	int		type = MSG_ReadByte ();
	vec3_t		pos;
	dlight_t	*dl;
	int		cnt, i;

	switch (type)
	{
	case QWTE_LIGHTNING1:
		CL_ParseBeam (Mod_ForName ("progs/bolt.mdl", true));
		return;
	case QWTE_LIGHTNING2:
		CL_ParseBeam (Mod_ForName ("progs/bolt2.mdl", true));
		return;
	case QWTE_LIGHTNING3:
		CL_ParseBeam (Mod_ForName ("progs/bolt3.mdl", true));
		return;
	}

	cnt = -1;
	if (type == QWTE_GUNSHOT || type == QWTE_BLOOD)
		cnt = MSG_ReadByte ();		// particle multiplier
	for (i = 0; i < 3; i++)
		pos[i] = MSG_ReadCoord (0);

	switch (type)
	{
	case QWTE_GUNSHOT:
		R_RunParticleEffect (pos, vec3_origin, 0, 20 * cnt);
		break;
	case QWTE_BLOOD:
		R_RunParticleEffect (pos, vec3_origin, 73, 20 * cnt);
		break;
	case QWTE_LIGHTNINGBLOOD:
		R_RunParticleEffect (pos, vec3_origin, 225, 50);
		break;

	case QWTE_WIZSPIKE:
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
		S_StartSound (-1, 0, cl_sfx_wizhit, pos, 1, 1);
		break;
	case QWTE_KNIGHTSPIKE:
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
		S_StartSound (-1, 0, cl_sfx_knighthit, pos, 1, 1);
		break;

	case QWTE_SPIKE:
		R_RunParticleEffect (pos, vec3_origin, 0, 10);
		CLQW_RicochetSound (pos);
		break;
	case QWTE_SUPERSPIKE:
		R_RunParticleEffect (pos, vec3_origin, 0, 20);
		CLQW_RicochetSound (pos);
		break;

	case QWTE_EXPLOSION:
		R_ParticleExplosion (pos);
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;
	case QWTE_TAREXPLOSION:
		R_BlobExplosion (pos);
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case QWTE_LAVASPLASH:
		R_LavaSplash (pos);
		break;
	case QWTE_TELEPORT:
		R_TeleportSplash (pos);
		break;

	default:
		Con_DPrintf ("[QW] unknown temp entity %i\n", type);
		break;
	}
}

/*
=====================
CLQW_InfoValue -- look up a key in a QW info string ("\key\value\key\value...").
Returns a static buffer; use the value before the next call.
=====================
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
			if (o < pkey + sizeof(pkey) - 1) *o++ = *info++; else info++;
		*o = 0;
		if (*info)
			info++;

		o = value;
		while (*info && *info != '\\')
			if (o < value + sizeof(value) - 1) *o++ = *info++; else info++;
		*o = 0;
		if (*info)
			info++;

		if (!strcmp (pkey, key))
			return value;
	}
	return "";
}

/*
=====================
CLQW_ProcessUserinfo -- pull the fields we display (name, shirt/pants colors)
out of a player's userinfo and rebuild their translated skin.
=====================
*/
static void CLQW_ProcessUserinfo (int slot, const char *info)
{
	int	top, bottom;

	q_strlcpy (cl.scores[slot].name, CLQW_InfoValue (info, "name"), MAX_SCOREBOARDNAME);
	top = CLAMP (0, atoi (CLQW_InfoValue (info, "topcolor")), 13);
	bottom = CLAMP (0, atoi (CLQW_InfoValue (info, "bottomcolor")), 13);
	cl.scores[slot].colors = (top << 4) | bottom;

	if (slot < MAX_SCOREBOARD)	// playertextures[] only covers this many
		CL_NewTranslation (slot);
}

/*
=====================
CLQW_SetInfoKey -- svc_setinfo: one changed userinfo key for a player.
=====================
*/
static void CLQW_SetInfoKey (int slot, const char *key, const char *value)
{
	if (!strcmp (key, "name"))
		q_strlcpy (cl.scores[slot].name, value, MAX_SCOREBOARDNAME);
	else if (!strcmp (key, "topcolor"))
		cl.scores[slot].colors = (cl.scores[slot].colors & 0x0f) | (CLAMP (0, atoi (value), 13) << 4);
	else if (!strcmp (key, "bottomcolor"))
		cl.scores[slot].colors = (cl.scores[slot].colors & 0xf0) | CLAMP (0, atoi (value), 13);
	else
		return;

	if (slot < MAX_SCOREBOARD)
		CL_NewTranslation (slot);
}

/*
==============
CLQW_ParseServerMessage -- decode one netchan message worth of svc_ commands
==============
*/
void CLQW_ParseServerMessage (void)
{
	int	cmd, i;
	const char *s;

	qw_parsecount++;	// one server message = one frame of entity/player state
	CLQW_ClearProjectiles ();	// nails are resent in full each frame

	while (1)
	{
		if (msg_badread)
		{
			Con_Printf ("[QW] bad server message\n");
			CL_Disconnect ();
			return;
		}

		if (msg_readcount == net_message.cursize)
			break;			// end of message

		cmd = MSG_ReadByte ();
		if (cmd == -1)
			break;

		switch (cmd)
		{
		case qwsvc_nop:
			break;

		case qwsvc_disconnect:
			Con_Printf ("[QW] server disconnected\n");
			CL_Disconnect ();
			return;

		case qwsvc_print:
			(void) MSG_ReadByte ();		// print level
			Con_Printf ("%s", MSG_ReadString ());
			break;

		case qwsvc_centerprint:
			SCR_CenterPrint (MSG_ReadString ());
			break;

		case qwsvc_stufftext:
			s = MSG_ReadString ();
			Cbuf_AddText (s);
			break;

		case qwsvc_serverdata:
			CLQW_ParseServerData ();
			break;

		case qwsvc_soundlist:
			CLQW_ParseSoundlist ();
			break;

		case qwsvc_modellist:
			CLQW_ParseModellist ();
			break;

		case qwsvc_spawnbaseline:
			i = MSG_ReadShort ();		// entity number
			CLQW_ParseBaseline (i);
			break;

		case qwsvc_spawnstatic:
			CLQW_ParseStatic ();
			break;

		case qwsvc_spawnstaticsound:
			for (i = 0; i < 3; i++)
				(void) MSG_ReadCoord (0);	// origin
			(void) MSG_ReadByte ();		// sound number
			(void) MSG_ReadByte ();		// volume
			(void) MSG_ReadByte ();		// attenuation
			break;

		// --- per-frame game state (phase 3a: consume + drive the view) ---
		case qwsvc_setview:
			cl.viewentity = MSG_ReadShort ();
			break;

		case qwsvc_playerinfo:
			CLQW_ParsePlayerinfo ();
			break;

		case qwsvc_packetentities:
			CLQW_ParsePacketEntities (false);
			break;

		case qwsvc_deltapacketentities:
			CLQW_ParsePacketEntities (true);
			break;

		case qwsvc_sound:
			CLQW_ParseSound ();
			break;

		case qwsvc_stopsound:
			(void) MSG_ReadShort ();
			break;

		case qwsvc_temp_entity:
			CLQW_ParseTEnt ();
			break;

		case qwsvc_damage:
			(void) MSG_ReadByte ();		// armor
			(void) MSG_ReadByte ();		// blood
			for (i = 0; i < 3; i++)
				(void) MSG_ReadCoord (0);
			break;

		case qwsvc_muzzleflash:
			{
				int e = (unsigned short) MSG_ReadShort ();	// player entity
				if (e >= 1 && e <= QW_MAX_CLIENTS)
				{
					qw_player_render_t *pl = &qw_players[e - 1];
					vec3_t	fv, rv, uv, org;

					AngleVectors (pl->viewangles, fv, rv, uv);
					VectorMA (pl->origin, 18, fv, org);
					CLQW_Dlight (e, org, 200 + (rand() & 31), 0.1);
				}
			}
			break;

		case qwsvc_nails:
			CLQW_ParseProjectiles ();
			break;

		case qwsvc_chokecount:
			(void) MSG_ReadByte ();
			break;

		case qwsvc_smallkick:
		case qwsvc_bigkick:
		case qwsvc_killedmonster:
		case qwsvc_foundsecret:
		case qwsvc_sellscreen:
			break;			// no payload

		case qwsvc_intermission:
			for (i = 0; i < 3; i++) (void) MSG_ReadCoord (0);	// origin
			for (i = 0; i < 3; i++) (void) MSG_ReadAngle (0);	// angles
			break;

		case qwsvc_finale:
			SCR_CenterPrint (MSG_ReadString ());
			break;

		case qwsvc_setangle:
			for (i = 0; i < 3; i++)
				cl.viewangles[i] = MSG_ReadAngle (0);
			break;

		case qwsvc_lightstyle:
			i = MSG_ReadByte ();
			if (i < MAX_LIGHTSTYLES)
				q_strlcpy (cl_lightstyle[i].map, MSG_ReadString (), MAX_STYLESTRING);
			else
				(void) MSG_ReadString ();
			break;

		case qwsvc_serverinfo:
			(void) MSG_ReadString ();	// key
			(void) MSG_ReadString ();	// value
			break;

		case qwsvc_cdtrack:
			cl.cdtrack = MSG_ReadByte ();
			break;

		case qwsvc_updatestat:
			i = MSG_ReadByte ();
			if (i >= 0 && i < MAX_CL_STATS)
				cl.stats[i] = MSG_ReadByte ();
			else
				(void) MSG_ReadByte ();
			break;

		case qwsvc_updatestatlong:
			i = MSG_ReadByte ();
			if (i >= 0 && i < MAX_CL_STATS)
				cl.stats[i] = MSG_ReadLong ();
			else
				(void) MSG_ReadLong ();
			break;

		case qwsvc_updateuserinfo:
			i = MSG_ReadByte ();		// slot
			(void) MSG_ReadLong ();		// userid
			s = MSG_ReadString ();		// userinfo
			if (i < QW_MAX_CLIENTS && cl.scores)
				CLQW_ProcessUserinfo (i, s);
			break;

		case qwsvc_setinfo:
			{
				char	key[64], val[64];

				i = MSG_ReadByte ();
				// MSG_ReadString reuses one buffer -- copy the key first
				q_strlcpy (key, MSG_ReadString (), sizeof(key));
				q_strlcpy (val, MSG_ReadString (), sizeof(val));
				if (i < QW_MAX_CLIENTS && cl.scores)
					CLQW_SetInfoKey (i, key, val);
			}
			break;

		case qwsvc_updatefrags:
			i = MSG_ReadByte ();
			if (i < QW_MAX_CLIENTS && cl.scores)
				cl.scores[i].frags = MSG_ReadShort ();
			else
				(void) MSG_ReadShort ();
			break;

		case qwsvc_updateping:
			(void) MSG_ReadByte ();
			(void) MSG_ReadShort ();
			break;

		case qwsvc_updatepl:
			(void) MSG_ReadByte ();
			(void) MSG_ReadByte ();
			break;

		case qwsvc_updateentertime:
			i = MSG_ReadByte ();
			if (i < QW_MAX_CLIENTS && cl.scores)
				cl.scores[i].entertime = realtime - MSG_ReadFloat ();
			else
				(void) MSG_ReadFloat ();
			break;

		case qwsvc_setpause:
			cl.paused = MSG_ReadByte ();
			break;

		// Per-player physics overrides (powerups, gravity zones). Feed the
		// prediction movevars so our own simulation matches the server.
		case qwsvc_maxspeed:
			qw_movevars.maxspeed = MSG_ReadFloat ();
			break;
		case qwsvc_entgravity:
			qw_movevars.entgravity = MSG_ReadFloat ();
			break;

		case qwsvc_download:
			{	// [short] size (-1 = no file), [byte] percent, [bytes] data
				int size = (short) MSG_ReadShort ();
				(void) MSG_ReadByte ();		// percent
				for (i = 0; i < size; i++)
					(void) MSG_ReadByte ();
			}
			break;

		default:
			// A protocol-28 server should never reach here; if it does the byte
			// stream is out of sync and the rest of the message is unreadable.
			Con_DPrintf ("[QW] svc %i not handled; stopping parse\n", cmd);
			return;
		}
	}
}

#endif	/* USE_QW_PROTOCOL */
