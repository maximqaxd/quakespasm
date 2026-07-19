/*
================================================================================
qw_cl_ents.c -- QuakeWorld entity snapshots (phase 3b)

QW does not resend the whole world each frame: a snapshot carries only the
entities that changed since an earlier frame the client still holds, plus a list
of removals. We rebuild the full entity set by copying that older snapshot
forward and applying the deltas, then stamp the engine's persistent cl_entities[]
and push pointers into cl_visedicts[] so the renderer draws them.

Entity numbers 1..QW_MAX_CLIENTS are players (delivered separately as
playerinfo); packet entities are everything else -- items, projectiles, gibs,
doors. New entities delta against their spawn baseline; existing ones against
their previous state.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

qw_entity_state_t	*qw_baselines;		// [QW_MAX_EDICTS]
qw_player_render_t	qw_players[QW_MAX_CLIENTS];
int			qw_parsecount;

int			qw_playerindex = -1;
int			qw_spikeindex = -1;
int			qw_flagindex = -1;

// Colored player skins (shirt/pants): the engine keeps one translated texture
// per scoreboard slot; rebuild it only when a player's model or skin changes.
extern gltexture_t	*playertextures[MAX_SCOREBOARD];
static int		qw_translate_model[QW_MAX_CLIENTS];
static int		qw_translate_skin[QW_MAX_CLIENTS];

// Snapshot ring keyed by the server packet sequence, so a delta can copy any of
// the recently received frames forward. Hunk-allocated (~208 KB) rather than
// living in BSS -- see CLQW_ClearEntities.
static qw_packet_entities_t	*qw_snapshots;	// [QW_UPDATE_BACKUP]
static int			qw_snap_seq;	// sequence of the newest snapshot
static qboolean			qw_have_snap;	// a full frame is available to link

// Nail projectiles (svc_nails): resent in full each frame as packed 48-bit
// entities, so they are cleared and refilled per server message.
#define	QW_MAX_PROJECTILES	32
typedef struct
{
	int	modelindex;
	vec3_t	origin;
	vec3_t	angles;
} qw_projectile_t;
static qw_projectile_t	qw_projectiles[QW_MAX_PROJECTILES];
static int		qw_num_projectiles;
static entity_t		qw_proj_ents[QW_MAX_PROJECTILES];	// scratch render entities

void CLQW_ClearProjectiles (void)
{
	qw_num_projectiles = 0;
}

/*
==============
CLQW_Dlight -- (re)allocate a keyed dynamic light. Re-created every frame for a
persistent glow (same key reuses the slot); QuakeSpasm dlights are uncolored, so
these are white.
==============
*/
void CLQW_Dlight (int key, const vec3_t org, float radius, float time)
{
	dlight_t	*dl = CL_AllocDlight (key);

	VectorCopy (org, dl->origin);
	dl->radius = radius;
	dl->die = cl.time + time;
	dl->minlight = 32;
}

/*
==============
CLQW_ClearEntities -- wipe the snapshot/baseline/player state at level change.
==============
*/
void CLQW_ClearEntities (void)
{
	// The snapshot ring (~208 KB) and baselines (~26 KB) are the port's biggest
	// buffers. Rather than sit in always-resident BSS, allocate them from the
	// hunk here -- this runs from serverdata parsing, right after CL_ClearState
	// wiped the hunk, so they cost nothing outside QuakeWorld and are reclaimed
	// on the next level change. Hunk_AllocName zeroes, so no memset is needed.
	qw_baselines = (qw_entity_state_t *) Hunk_AllocName (QW_MAX_EDICTS * sizeof(*qw_baselines), "qwbaseline");
	qw_snapshots = (qw_packet_entities_t *) Hunk_AllocName (QW_UPDATE_BACKUP * sizeof(*qw_snapshots), "qwsnapshot");

	memset (qw_players, 0, sizeof(qw_players));
	qw_have_snap = false;
	qw_snap_seq = 0;
	qw_validsequence = 0;		// re-arm the "wait for first player frame" view gate
	qw_parsecount = 0;
	qw_playerindex = qw_spikeindex = qw_flagindex = -1;
	memset (qw_translate_model, -1, sizeof(qw_translate_model));
	memset (qw_translate_skin, -1, sizeof(qw_translate_skin));
	CLQW_ClearProjectiles ();
}

/*
==================
CLQW_ParseProjectiles -- svc_nails: decode the packed nail list (6 bytes each:
3 axes at 13 bits + 2 angle bytes) into the projectile array.
==================
*/
void CLQW_ParseProjectiles (void)
{
	int		i, c, j;
	byte		bits[6];
	qw_projectile_t	*pr;

	c = MSG_ReadByte ();
	for (i = 0; i < c; i++)
	{
		for (j = 0; j < 6; j++)
			bits[j] = MSG_ReadByte ();

		if (qw_num_projectiles == QW_MAX_PROJECTILES)
			continue;

		pr = &qw_projectiles[qw_num_projectiles++];
		pr->modelindex = qw_spikeindex;
		pr->origin[0] = (( bits[0] + ((bits[1]&15)<<8) ) <<1) - 4096;
		pr->origin[1] = (( (bits[1]>>4) + (bits[2]<<4) ) <<1) - 4096;
		pr->origin[2] = (( bits[3] + ((bits[4]&15)<<8) ) <<1) - 4096;
		pr->angles[0] = 360.0f * (bits[4]>>4) / 16.0f;
		pr->angles[1] = 360.0f * bits[5] / 256.0f;
		pr->angles[2] = 0;
	}
}

/*
==============
CLQW_FindModelNumbers -- once the model list is loaded, cache the precache
indices QW references by hardcoded model (player skin default, nails, CTF flag).
==============
*/
void CLQW_FindModelNumbers (void)
{
	int		i;
	qmodel_t	*m;

	qw_playerindex = qw_spikeindex = qw_flagindex = -1;

	for (i = 1; i < MAX_MODELS; i++)
	{
		m = cl.model_precache[i];
		if (!m)
			continue;
		if (!strcmp (m->name, "progs/player.mdl"))	qw_playerindex = i;
		else if (!strcmp (m->name, "progs/spike.mdl"))	qw_spikeindex = i;
		else if (!strcmp (m->name, "progs/flag.mdl"))	qw_flagindex = i;
	}
}

/*
==============
CLQW_ParseBaseline -- svc_spawnbaseline: the default state a numbered entity
delta-compresses against when it first appears.
==============
*/
void CLQW_ParseBaseline (int num)
{
	qw_entity_state_t	scratch;
	qw_entity_state_t	*es;
	int			i;

	es = (num >= 0 && num < QW_MAX_EDICTS) ? &qw_baselines[num] : &scratch;
	memset (es, 0, sizeof(*es));

	es->modelindex = MSG_ReadByte ();
	es->frame      = MSG_ReadByte ();
	es->colormap   = MSG_ReadByte ();
	es->skinnum    = MSG_ReadByte ();
	for (i = 0; i < 3; i++)
	{
		es->origin[i] = MSG_ReadCoord (0);
		es->angles[i] = MSG_ReadAngle (0);
	}
	es->number = num;
}

/*
==================
CLQW_ParseDelta -- apply one entity's changed fields on top of a source state
(either its baseline or its previous snapshot entry).
==================
*/
static void CLQW_ParseDelta (qw_entity_state_t *from, qw_entity_state_t *to, int bits)
{
	*to = *from;

	to->number = bits & 511;
	bits &= ~511;

	if (bits & QWU_MOREBITS)
		bits |= MSG_ReadByte ();
	to->flags = bits;

	if (bits & QWU_MODEL)    to->modelindex = MSG_ReadByte ();
	if (bits & QWU_FRAME)    to->frame      = MSG_ReadByte ();
	if (bits & QWU_COLORMAP) to->colormap   = MSG_ReadByte ();
	if (bits & QWU_SKIN)     to->skinnum    = MSG_ReadByte ();
	if (bits & QWU_EFFECTS)  to->effects    = MSG_ReadByte ();
	if (bits & QWU_ORIGIN1)  to->origin[0]  = MSG_ReadCoord (0);
	if (bits & QWU_ANGLE1)   to->angles[0]  = MSG_ReadAngle (0);
	if (bits & QWU_ORIGIN2)  to->origin[1]  = MSG_ReadCoord (0);
	if (bits & QWU_ANGLE2)   to->angles[1]  = MSG_ReadAngle (0);
	if (bits & QWU_ORIGIN3)  to->origin[2]  = MSG_ReadCoord (0);
	if (bits & QWU_ANGLE3)   to->angles[2]  = MSG_ReadAngle (0);
	// QWU_SOLID carries no data
}

/*
==================
CLQW_DeltaSequence -- the newest snapshot we can safely ask the server to delta
against (sent as clc_delta with each move), or -1 to request a full update.
==================
*/
int CLQW_DeltaSequence (void)
{
	if (!qw_have_snap)
		return -1;
	if (cls.netchan.incoming_sequence - qw_snap_seq >= QW_UPDATE_BACKUP - 1)
		return -1;	// so old its ring slot may have been reused
	return qw_snap_seq;
}

/*
=================
CLQW_FlushEntityPacket -- the delta references a frame we no longer hold: read
the update to keep the stream in sync but throw it away, and stop requesting
deltas so the server sends a fresh full update.
=================
*/
static void CLQW_FlushEntityPacket (void)
{
	int			word;
	qw_entity_state_t	olde, newe;

	Con_DPrintf ("[QW] FlushEntityPacket\n");
	memset (&olde, 0, sizeof(olde));
	qw_have_snap = false;

	for (;;)
	{
		word = (unsigned short) MSG_ReadShort ();
		if (msg_badread)
		{
			Con_Printf ("[QW] bad packetentities\n");
			CL_Disconnect ();
			return;
		}
		if (!word)
			break;
		CLQW_ParseDelta (&olde, &newe, word);
	}
}

/*
==================
CLQW_ParsePacketEntities -- merge the incoming delta against the previous frame
into a fresh snapshot. Entities are transmitted in ascending number order, which
lets a single pass copy-forward unchanged ones, insert new ones from baseline,
and drop removals.
==================
*/
void CLQW_ParsePacketEntities (qboolean delta)
{
	int			newpacket, oldpacket;
	qw_packet_entities_t	*oldp, *newp, dummy;
	int			oldindex, newindex;
	int			word, newnum, oldnum;
	qboolean		full;

	newpacket = cls.netchan.incoming_sequence & QW_UPDATE_MASK;
	newp = &qw_snapshots[newpacket];

	if (delta)
	{
		int	from = MSG_ReadByte ();

		// the byte echoes the low bits of the frame we asked to delta from;
		// rebuild the full sequence relative to the current one and make sure
		// that frame is still in the ring
		oldpacket = cls.netchan.incoming_sequence -
			((cls.netchan.incoming_sequence - from) & 0xff);
		oldp = &qw_snapshots[oldpacket & QW_UPDATE_MASK];

		if (oldp->sequence != oldpacket ||
			cls.netchan.incoming_sequence - oldpacket >= QW_UPDATE_BACKUP - 1)
		{
			CLQW_FlushEntityPacket ();
			return;
		}
		full = false;
	}
	else
	{	// a full update: start delta compressing from here on
		oldp = &dummy;
		dummy.num_entities = 0;
		full = true;
	}

	oldindex = 0;
	newindex = 0;
	newp->num_entities = 0;

	for (;;)
	{
		word = (unsigned short) MSG_ReadShort ();
		if (msg_badread)
		{
			Con_Printf ("[QW] bad packetentities\n");
			CL_Disconnect ();
			return;
		}

		if (!word)
		{	// end of list: copy any remaining old entities forward unchanged
			while (oldindex < oldp->num_entities)
			{
				if (newindex >= QW_MAX_PACKET_ENTITIES)
					break;
				newp->entities[newindex++] = oldp->entities[oldindex++];
			}
			break;
		}

		newnum = word & 511;
		oldnum = (oldindex >= oldp->num_entities) ? 9999 : oldp->entities[oldindex].number;

		while (newnum > oldnum)
		{	// copy forward entities the delta didn't mention
			if (full || newindex >= QW_MAX_PACKET_ENTITIES)
				break;
			newp->entities[newindex++] = oldp->entities[oldindex++];
			oldnum = (oldindex >= oldp->num_entities) ? 9999 : oldp->entities[oldindex].number;
		}

		if (newnum < oldnum)
		{	// a new entity, delta'd from its baseline
			if (word & QWU_REMOVE)
				continue;
			if (newindex >= QW_MAX_PACKET_ENTITIES)
				continue;
			CLQW_ParseDelta (&qw_baselines[newnum], &newp->entities[newindex], word);
			newindex++;
			continue;
		}

		// newnum == oldnum: delta from the previous frame, or a removal
		if (word & QWU_REMOVE)
		{
			oldindex++;
			continue;
		}
		CLQW_ParseDelta (&oldp->entities[oldindex], &newp->entities[newindex], word);
		newindex++;
		oldindex++;
	}

	newp->num_entities = newindex;
	newp->sequence = cls.netchan.incoming_sequence;
	qw_snap_seq = cls.netchan.incoming_sequence;
	qw_have_snap = true;
}

/*
===============
CLQW_LinkPacketEntities -- turn the current snapshot into renderable entities and
add automatic particle trails for rockets, grenades and gibs.
===============
*/
static void CLQW_LinkPacketEntities (void)
{
	qw_packet_entities_t	*pack;
	qw_entity_state_t	*s1;
	entity_t		*ent;
	qmodel_t		*model;
	float			autorotate;
	vec3_t			old_origin;
	int			pnum, i;

	pack = &qw_snapshots[qw_snap_seq & QW_UPDATE_MASK];
	autorotate = anglemod (100 * cl.time);

	for (pnum = 0; pnum < pack->num_entities; pnum++)
	{
		s1 = &pack->entities[pnum];

		if (!s1->modelindex || s1->modelindex >= MAX_MODELS)
			continue;
		model = cl.model_precache[s1->modelindex];
		if (!model)
			continue;
		if (cl_numvisedicts >= MAX_VISEDICTS)
			break;

		ent = CL_EntityNum (s1->number);
		VectorCopy (ent->origin, old_origin);	// last frame's origin, for trails

		ent->model = model;
		ent->frame = s1->frame;
		ent->skinnum = s1->skinnum;
		ent->alpha = 0;			// ENTALPHA_DEFAULT -> opaque
		ent->scale = ENTSCALE_DEFAULT;	// CL_EntityNum only primes baseline.scale
		ent->colormap = vid.colormap;	// no player-color translation
		ent->effects = s1->effects;

		// rotate bonus items in place; everything else takes server angles
		if (model->flags & EF_ROTATE)
		{
			ent->angles[0] = 0;
			ent->angles[1] = autorotate;
			ent->angles[2] = 0;
		}
		else
			VectorCopy (s1->angles, ent->angles);

		VectorCopy (s1->origin, ent->origin);

		cl_visedicts[cl_numvisedicts++] = ent;

		// glowing items / powerups cast a dynamic light
		if (s1->effects & EF_BRIGHTLIGHT)
		{
			vec3_t p;
			VectorCopy (ent->origin, p);
			p[2] += 16;
			CLQW_Dlight (s1->number, p, 400 + (rand() & 31), 0.1);
		}
		else if (s1->effects & EF_DIMLIGHT)
			CLQW_Dlight (s1->number, ent->origin, 200 + (rand() & 31), 0.1);

		// automatic particle trails
		if (!model->flags)
			continue;
		for (i = 0; i < 3; i++)
			if (fabs(old_origin[i] - ent->origin[i]) > 128)
			{	// teleported or first sighting: no trail across the gap
				VectorCopy (ent->origin, old_origin);
				break;
			}

		if (model->flags & EF_ROCKET)
		{
			R_RocketTrail (old_origin, ent->origin, 0);
			CLQW_Dlight (s1->number, ent->origin, 200, 0.01);	// rocket lights the walls
		}
		else if (model->flags & EF_GRENADE) R_RocketTrail (old_origin, ent->origin, 1);
		else if (model->flags & EF_GIB)     R_RocketTrail (old_origin, ent->origin, 2);
		else if (model->flags & EF_ZOMGIB)  R_RocketTrail (old_origin, ent->origin, 4);
		else if (model->flags & EF_TRACER)  R_RocketTrail (old_origin, ent->origin, 3);
		else if (model->flags & EF_TRACER2) R_RocketTrail (old_origin, ent->origin, 5);
		else if (model->flags & EF_TRACER3) R_RocketTrail (old_origin, ent->origin, 6);
	}
}

/*
===============
CLQW_LinkPlayers -- draw a model for every other player present this frame at the
position/orientation from their last playerinfo (our own player is the view and
is never drawn).
===============
*/
static void CLQW_LinkPlayers (void)
{
	int			j, msec, oldphysent;
	double			playertime;
	qw_player_render_t	*pl;
	qw_playerstate_t	from, to;
	entity_t		*ent;
	qmodel_t		*model;

	// how far into the past the freshest player states are
	playertime = realtime - qw_latency + 0.02;
	if (playertime > realtime)
		playertime = realtime;

	// physent 0 must be the world for the short forward sims below
	qw_pmove.physents[0].model = cl.worldmodel;
	VectorCopy (vec3_origin, qw_pmove.physents[0].origin);

	for (j = 0; j < QW_MAX_CLIENTS; j++)
	{
		pl = &qw_players[j];

		if (pl->messagenum != qw_parsecount)
			continue;		// not present this frame
		if (j == qwcl.playernum)
			continue;		// that's us
		if (pl->modelindex <= 0 || pl->modelindex >= MAX_MODELS)
			continue;
		model = cl.model_precache[pl->modelindex];
		if (!model)
			continue;

		// quad/pentagram players glow (light flashes come even from a player we
		// can't currently see, so do this before the visible/vis-edict checks)
		if (pl->effects & EF_BRIGHTLIGHT)
		{
			vec3_t p;
			VectorCopy (pl->origin, p);
			p[2] += 16;
			CLQW_Dlight (j + 1, p, 400 + (rand() & 31), 0.1);
		}
		else if (pl->effects & EF_DIMLIGHT)
			CLQW_Dlight (j + 1, pl->origin, 200 + (rand() & 31), 0.1);

		if (cl_numvisedicts >= MAX_VISEDICTS)
			break;

		ent = CL_EntityNum (j + 1);	// players occupy entity slots 1..MAX_CLIENTS
		ent->model = model;
		ent->frame = pl->frame;
		ent->skinnum = pl->skinnum;
		ent->alpha = 0;
		ent->scale = ENTSCALE_DEFAULT;
		ent->effects = pl->effects;

		// shirt/pants colors: rebuild the translated skin when the model or
		// skin changes, then point colormap at the slot so the renderer swaps
		// in playertextures[]. Slots past MAX_SCOREBOARD keep the stock skin.
		if (j < MAX_SCOREBOARD && cl.scores)
		{
			if (qw_translate_model[j] != pl->modelindex || qw_translate_skin[j] != pl->skinnum)
			{
				qw_translate_model[j] = pl->modelindex;
				qw_translate_skin[j] = pl->skinnum;
				R_TranslateNewPlayerSkin (j);	// reads cl_entities[j+1] (= ent)
			}
			ent->colormap = playertextures[j] ? cl.scores[j].translations : vid.colormap;
		}
		else
			ent->colormap = vid.colormap;

		// lean the model from its own view pitch, bank it into turns
		ent->angles[0] = -pl->viewangles[0] / 3;
		ent->angles[1] = pl->viewangles[1];
		ent->angles[2] = 0;
		ent->angles[2] = V_CalcRoll (ent->angles, pl->velocity) * 4;

		// forward-predict: replay the player's last command for however stale
		// their state is (half of it, to limit overruns), so shooters line up
		// with their own projectiles instead of trailing them
		msec = (int)(500 * (playertime - pl->state_time));
		if (msec <= 0 || !cl_predict_players.value)
		{
			VectorCopy (pl->origin, ent->origin);
		}
		else
		{
			qw_usercmd_t	cmd;

			if (msec > 255)
				msec = 255;
			cmd = pl->cmd;
			cmd.msec = (byte) msec;

			memset (&from, 0, sizeof(from));
			VectorCopy (pl->origin, from.origin);
			VectorCopy (pl->velocity, from.velocity);

			oldphysent = qw_pmove.numphysent;
			qw_pmove.numphysent = 1;	// clip to the world only
			CLQW_PredictUsercmd (&from, &to, &cmd, false);
			qw_pmove.numphysent = oldphysent;

			VectorCopy (to.origin, ent->origin);
		}

		cl_visedicts[cl_numvisedicts++] = ent;
	}
}

/*
===============
CLQW_LinkProjectiles -- draw the nail projectiles decoded this frame. They have
no persistent edict, so a scratch entity pool backs the visedict pointers.
===============
*/
static void CLQW_LinkProjectiles (void)
{
	int		i;
	qw_projectile_t	*pr;
	entity_t	*ent;
	qmodel_t	*model;

	for (i = 0, pr = qw_projectiles; i < qw_num_projectiles; i++, pr++)
	{
		if (pr->modelindex <= 0 || pr->modelindex >= MAX_MODELS)
			continue;
		model = cl.model_precache[pr->modelindex];
		if (!model)
			continue;
		if (cl_numvisedicts >= MAX_VISEDICTS)
			break;

		ent = &qw_proj_ents[i];
		memset (ent, 0, sizeof(*ent));
		ent->model = model;
		ent->alpha = 0;
		ent->scale = ENTSCALE_DEFAULT;
		ent->colormap = vid.colormap;
		VectorCopy (pr->origin, ent->origin);
		VectorCopy (pr->angles, ent->angles);
		cl_visedicts[cl_numvisedicts++] = ent;
	}
}

/*
===============
CLQW_EmitEntities -- rebuild the visible-entity list for this frame.
===============
*/
void CLQW_EmitEntities (void)
{
	if (!CLQW_IsConnected ())
		return;

	cl_numvisedicts = 0;

	if (!qw_have_snap)
		return;

	CLQW_LinkPlayers ();
	CLQW_LinkPacketEntities ();
	CLQW_LinkProjectiles ();
	CL_UpdateTEnts ();		// animate + link active lightning beams
	// (dynamic lights are decayed by the host frame, once cl.time advances)
}

#endif	/* USE_QW_PROTOCOL */
