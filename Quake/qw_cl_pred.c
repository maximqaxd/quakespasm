/*
================================================================================
qw_cl_pred.c -- QuakeWorld client-side movement prediction

Server-authoritative movement has a full round trip of latency: press forward,
wait for the packet to reach the server and the snapshot to come back before the
view moves. Prediction hides it -- we replay our own not-yet-acknowledged
commands through the same physics the server runs (qw_pmove.c), starting from the
last snapshot the server confirmed, to get where we should be right now.

The command/state ring (qw_frames) is indexed by netchan sequence: qw_cl_input
fills .cmd when a command is sent, qw_cl_parse fills .playerstate from the
snapshot that acknowledges it. From the acked frame we simulate forward through
every command the server has not answered yet.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"

cvar_t	cl_nopred = {"cl_nopred", "0", CVAR_NONE};
cvar_t	cl_predict_players = {"cl_predict_players", "1", CVAR_NONE};
cvar_t	cl_solid_players = {"cl_solid_players", "1", CVAR_NONE};

qw_frame_t	qw_frames[QW_UPDATE_BACKUP];
int		qw_validsequence;	// netchan seq of the last good snapshot
vec3_t		qw_simorg;
vec3_t		qw_simvel;
vec3_t		qw_simangles;

// When a snapshot proves our prediction was slightly off, we don't snap the view
// to the corrected spot -- we keep it where prediction had it and glide the error
// out over QW_ERROR_DECAY seconds, so packet jitter reads as smooth motion.
#define	QW_ERROR_DECAY		0.10	// seconds to blend a correction away
#define	QW_ERROR_MAX		64.0	// bigger than this is a teleport, snap it
static vec3_t	qw_prediction_error;
static double	qw_prediction_error_time;

/*
==============
CLQW_CalcPredictionError -- called from the parser when a snapshot acknowledges
a command we already predicted: fold the miss (predicted - server) into the
error we glide out, unless it is large enough to be a teleport.
==============
*/
void CLQW_CalcPredictionError (const vec3_t predicted, const vec3_t server)
{
	vec3_t	delta;
	float	len;

	VectorSubtract (predicted, server, delta);
	len = VectorLength (delta);
	if (len > QW_ERROR_MAX || len < 0.01f)
	{	// teleport, respawn, or a perfect prediction -- nothing to smooth
		VectorCopy (vec3_origin, qw_prediction_error);
		return;
	}

	VectorCopy (delta, qw_prediction_error);
	qw_prediction_error_time = realtime;
}

/*
==============
QWPM_SetupWorld -- physent 0 is the world; a single-physent world is enough to
clip the local player (player-vs-player clipping is a later step).
==============
*/
static void QWPM_SetupWorld (void)
{
	qw_pmove.numphysent = 1;
	qw_pmove.physents[0].model = cl.worldmodel;
	VectorCopy (vec3_origin, qw_pmove.physents[0].origin);
	qw_pmove.physents[0].info = 0;
}

/*
==============
CLQW_SetSolidPlayers -- append every other live player as a solid box so our
predicted movement bumps into them the way the server's will.
==============
*/
static void CLQW_SetSolidPlayers (void)
{
	int	j;

	if (!cl_solid_players.value)
		return;

	for (j = 0; j < QW_MAX_CLIENTS; j++)
	{
		qw_player_render_t *pl = &qw_players[j];

		if (pl->messagenum != qw_parsecount)
			continue;		// not present this frame
		if (j == qwcl.playernum)
			continue;		// that's us
		if (!pl->modelindex)
			continue;
		if (pl->flags & QWPF_DEAD)
			continue;		// dead players aren't solid

		QWPM_AddBoxPhysent (pl->origin);
	}
}

/*
==============
CLQW_PredictUsercmd -- advance one command's worth of physics from 'from' to 'to'.
==============
*/
void CLQW_PredictUsercmd (qw_playerstate_t *from, qw_playerstate_t *to, qw_usercmd_t *u, qboolean spectator)
{
	// split up very long moves so a hitch doesn't tunnel through geometry
	if (u->msec > 50)
	{
		qw_playerstate_t	temp;
		qw_usercmd_t		split;

		split = *u;
		split.msec /= 2;

		CLQW_PredictUsercmd (from, &temp, &split, spectator);
		CLQW_PredictUsercmd (&temp, to, &split, spectator);
		return;
	}

	VectorCopy (from->origin, qw_pmove.origin);
	VectorCopy (u->angles, qw_pmove.angles);
	VectorCopy (from->velocity, qw_pmove.velocity);

	qw_pmove.oldbuttons = from->oldbuttons;
	qw_pmove.waterjumptime = from->waterjumptime;
	qw_pmove.dead = (cl.stats[STAT_HEALTH] <= 0);
	qw_pmove.spectator = spectator;

	qw_pmove.cmd = *u;

	QWPM_PlayerMove ();

	to->waterjumptime = qw_pmove.waterjumptime;
	to->oldbuttons = qw_pmove.cmd.buttons;
	VectorCopy (qw_pmove.origin, to->origin);
	VectorCopy (qw_pmove.angles, to->viewangles);
	VectorCopy (qw_pmove.velocity, to->velocity);
	to->onground = qw_onground;
	to->weaponframe = from->weaponframe;
}

/*
==============
CLQW_PredictMove -- from the last acked snapshot, replay every unacknowledged
command to produce qw_simorg, and move the view entity there.
==============
*/
void CLQW_PredictMove (void)
{
	int			i;
	qw_playerstate_t	*from, *to;
	entity_t		*ent;

	if (!CLQW_IsConnected ())
		return;
	if (!qw_validsequence)
		return;		// no authoritative snapshot yet
	if (cl.paused)
		return;

	VectorCopy (cl.viewangles, qw_simangles);

	// the last frame the server confirmed
	from = &qw_frames[qw_validsequence & QW_UPDATE_MASK].playerstate;
	to = from;

	if (!cl_nopred.value && cl.worldmodel)
	{
		QWPM_SetupWorld ();
		CLQW_SetSolidPlayers ();

		// simulate forward through the commands the server has not answered
		for (i = qw_validsequence + 1;
			 i < cls.netchan.outgoing_sequence && i <= qw_validsequence + (QW_UPDATE_BACKUP - 1);
			 i++)
		{
			qw_frame_t *f = &qw_frames[i & QW_UPDATE_MASK];
			CLQW_PredictUsercmd (from, &f->playerstate, &f->cmd, qwcl.spectator);
			to = &f->playerstate;
			from = to;
		}
	}

	VectorCopy (to->origin, qw_simorg);
	VectorCopy (to->velocity, qw_simvel);

	// feed the on-ground state to the view code: without it the stair-step
	// camera smoothing never runs and every step reads as a vertical snap
	cl.onground = (to->onground != -1);

	// glide out any leftover prediction error instead of snapping to it
	{
		float	frac = 1.0f - (realtime - qw_prediction_error_time) / QW_ERROR_DECAY;
		if (frac > 0 && frac <= 1.0f)
			VectorMA (qw_simorg, frac, qw_prediction_error, qw_simorg);
	}

	// drive the view from the predicted origin
	if (cl.viewentity)
	{
		ent = CL_EntityNum (cl.viewentity);
		VectorCopy (qw_simorg, ent->origin);
	}
}

/*
==============
CLQW_InitPrediction -- register the prediction cvar and prime the box hull.
==============
*/
void CLQW_InitPrediction (void)
{
	Cvar_RegisterVariable (&cl_nopred);
	Cvar_RegisterVariable (&cl_predict_players);
	Cvar_RegisterVariable (&cl_solid_players);
	QWPM_Init ();
}

#endif	/* USE_QW_PROTOCOL */
