/*
================================================================================
qw_pmove.c -- QuakeWorld player physics (prediction backend)

The exact movement code the server runs, so the client can predict its own
position: given a start state and a usercmd, QWPM_PlayerMove reproduces the same
origin/velocity the server will compute. qw_cl_pred.c drives it. Merges id's
pmove.c (the movement) and pmovetst.c (the BSP hull traces); the on-disk 16-bit
clipnodes of the original are the engine's 32-bit mclipnode_t here.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"

qw_playermove_t	qw_pmove;
int		qw_onground;
int		qw_waterlevel;
int		qw_watertype;

static float	qw_pmframetime;
static vec3_t	qw_pm_forward, qw_pm_right, qw_pm_up;

static vec3_t	player_mins = {-16, -16, -24};
static vec3_t	player_maxs = { 16,  16,  32};

#define	STEPSIZE	18
#define	BUTTON_JUMP	2
#define	STOP_EPSILON	0.1
#define	MAX_CLIP_PLANES	5
#define	DIST_EPSILON	(0.03125)	// 1/32 epsilon to keep floating point happy

// ===========================================================================
// Hull tracing (from pmovetst.c)
// ===========================================================================

static hull_t		box_hull;
static mclipnode_t	box_clipnodes[6];
static mplane_t		box_planes[6];

/*
===================
QWPM_InitBoxHull -- set up the planes and clipnodes so a bounding box can be
stored out and get a proper hull_t.
===================
*/
static void QWPM_InitBoxHull (void)
{
	int	i, side;

	box_hull.clipnodes = box_clipnodes;
	box_hull.planes = box_planes;
	box_hull.firstclipnode = 0;
	box_hull.lastclipnode = 5;

	for (i = 0; i < 6; i++)
	{
		box_clipnodes[i].planenum = i;

		side = i & 1;
		box_clipnodes[i].children[side] = CONTENTS_EMPTY;
		if (i != 5)
			box_clipnodes[i].children[side^1] = i + 1;
		else
			box_clipnodes[i].children[side^1] = CONTENTS_SOLID;

		box_planes[i].type = i >> 1;
		box_planes[i].normal[i >> 1] = 1;
	}
}

void QWPM_Init (void)
{
	QWPM_InitBoxHull ();
}

/*
===================
QWPM_HullForBox -- bounding boxes are turned into small BSP trees so everything
goes through the same trace path.
===================
*/
static hull_t *QWPM_HullForBox (vec3_t mins, vec3_t maxs)
{
	box_planes[0].dist = maxs[0];
	box_planes[1].dist = mins[0];
	box_planes[2].dist = maxs[1];
	box_planes[3].dist = mins[1];
	box_planes[4].dist = maxs[2];
	box_planes[5].dist = mins[2];

	return &box_hull;
}

static int QWPM_HullPointContents (hull_t *hull, int num, vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;

	while (num >= 0)
	{
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Sys_Error ("QWPM_HullPointContents: bad node number");

		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

static int QWPM_PointContents (vec3_t p)
{
	float		d;
	mclipnode_t	*node;
	mplane_t	*plane;
	hull_t		*hull;
	int		num;

	hull = &qw_pmove.physents[0].model->hulls[0];

	num = hull->firstclipnode;
	while (num >= 0)
	{
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Sys_Error ("QWPM_PointContents: bad node number");

		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;

		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}

	return num;
}

static qboolean QWPM_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, qw_pmtrace_t *trace)
{
	mclipnode_t	*node;
	mplane_t	*plane;
	float		t1, t2;
	float		frac;
	int		i;
	vec3_t		mid;
	int		side;
	float		midf;

// check for empty
	if (num < 0)
	{
		if (num != CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;
		return true;		// empty
	}

	if (num < hull->firstclipnode || num > hull->lastclipnode)
		Sys_Error ("QWPM_RecursiveHullCheck: bad node number");

// find the point distances
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

	if (t1 >= 0 && t2 >= 0)
		return QWPM_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if (t1 < 0 && t2 < 0)
		return QWPM_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);

// put the crosspoint DIST_EPSILON pixels on the near side
	if (t1 < 0)
		frac = (t1 + DIST_EPSILON) / (t1 - t2);
	else
		frac = (t1 - DIST_EPSILON) / (t1 - t2);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;

	midf = p1f + (p2f - p1f) * frac;
	for (i = 0; i < 3; i++)
		mid[i] = p1[i] + frac * (p2[i] - p1[i]);

	side = (t1 < 0);

// move up to the node
	if (!QWPM_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace))
		return false;

	if (QWPM_HullPointContents (hull, node->children[side^1], mid) != CONTENTS_SOLID)
// go past the node
		return QWPM_RecursiveHullCheck (hull, node->children[side^1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		VectorSubtract (vec3_origin, plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

	while (QWPM_HullPointContents (hull, hull->firstclipnode, mid) == CONTENTS_SOLID)
	{ // shouldn't really happen, but does occasionally
		frac -= 0.1;
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Con_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f) * frac;
		for (i = 0; i < 3; i++)
			mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}

/*
================
QWPM_TestPlayerPosition -- false if the position is inside solid.
================
*/
static qboolean QWPM_TestPlayerPosition (vec3_t pos)
{
	int		i;
	qw_physent_t	*pe;
	vec3_t		mins, maxs, test;
	hull_t		*hull;

	for (i = 0; i < qw_pmove.numphysent; i++)
	{
		pe = &qw_pmove.physents[i];
		if (pe->model)
			hull = &qw_pmove.physents[i].model->hulls[1];
		else
		{
			VectorSubtract (pe->mins, player_maxs, mins);
			VectorSubtract (pe->maxs, player_mins, maxs);
			hull = QWPM_HullForBox (mins, maxs);
		}

		VectorSubtract (pos, pe->origin, test);

		if (QWPM_HullPointContents (hull, hull->firstclipnode, test) == CONTENTS_SOLID)
			return false;
	}

	return true;
}

/*
================
QWPM_PlayerTrace -- trace the player box from start to end through every
physent, returning the closest impact.
================
*/
static qw_pmtrace_t QWPM_PlayerTrace (vec3_t start, vec3_t end)
{
	qw_pmtrace_t	trace, total;
	vec3_t		offset;
	vec3_t		start_l, end_l;
	hull_t		*hull;
	int		i;
	qw_physent_t	*pe;
	vec3_t		mins, maxs;

// fill in a default trace
	memset (&total, 0, sizeof(total));
	total.fraction = 1;
	total.ent = -1;
	VectorCopy (end, total.endpos);

	for (i = 0; i < qw_pmove.numphysent; i++)
	{
		pe = &qw_pmove.physents[i];
		if (pe->model)
			hull = &qw_pmove.physents[i].model->hulls[1];
		else
		{
			VectorSubtract (pe->mins, player_maxs, mins);
			VectorSubtract (pe->maxs, player_mins, maxs);
			hull = QWPM_HullForBox (mins, maxs);
		}

		VectorCopy (pe->origin, offset);
		VectorSubtract (start, offset, start_l);
		VectorSubtract (end, offset, end_l);

		memset (&trace, 0, sizeof(trace));
		trace.fraction = 1;
		trace.allsolid = true;
		VectorCopy (end, trace.endpos);

		QWPM_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start_l, end_l, &trace);

		if (trace.allsolid)
			trace.startsolid = true;
		if (trace.startsolid)
			trace.fraction = 0;

		if (trace.fraction < total.fraction)
		{	// fix trace up by the offset
			VectorAdd (trace.endpos, offset, trace.endpos);
			total = trace;
			total.ent = i;
		}
	}

	return total;
}

// ===========================================================================
// Movement (from pmove.c)
// ===========================================================================

/*
==================
PM_ClipVelocity -- slide off of the impacting object; returns the blocked flags
(1 = floor, 2 = step/wall).
==================
*/
static int PM_ClipVelocity (vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
	float	backoff, change;
	int	i, blocked;

	blocked = 0;
	if (normal[2] > 0)
		blocked |= 1;		// floor
	if (!normal[2])
		blocked |= 2;		// step

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++)
	{
		change = normal[i] * backoff;
		out[i] = in[i] - change;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}

	return blocked;
}

/*
============
PM_FlyMove -- the basic solid body movement clip that slides along multiple planes.
============
*/
static int PM_FlyMove (void)
{
	int		bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int		numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity, original_velocity;
	int		i, j;
	qw_pmtrace_t	trace;
	vec3_t		end;
	float		time_left;
	int		blocked;

	numbumps = 4;

	blocked = 0;
	VectorCopy (qw_pmove.velocity, original_velocity);
	VectorCopy (qw_pmove.velocity, primal_velocity);
	numplanes = 0;

	time_left = qw_pmframetime;

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		for (i = 0; i < 3; i++)
			end[i] = qw_pmove.origin[i] + time_left * qw_pmove.velocity[i];

		trace = QWPM_PlayerTrace (qw_pmove.origin, end);

		if (trace.startsolid || trace.allsolid)
		{	// entity is trapped in another solid
			VectorCopy (vec3_origin, qw_pmove.velocity);
			return 3;
		}

		if (trace.fraction > 0)
		{	// actually covered some distance
			VectorCopy (trace.endpos, qw_pmove.origin);
			numplanes = 0;
		}

		if (trace.fraction == 1)
			break;		// moved the entire distance

		// save entity for contact
		qw_pmove.touchindex[qw_pmove.numtouch] = trace.ent;
		qw_pmove.numtouch++;

		if (trace.plane.normal[2] > 0.7)
			blocked |= 1;		// floor
		if (!trace.plane.normal[2])
			blocked |= 2;		// step

		time_left -= time_left * trace.fraction;

		// clipped to another plane
		if (numplanes >= MAX_CLIP_PLANES)
		{	// this shouldn't really happen
			VectorCopy (vec3_origin, qw_pmove.velocity);
			break;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		// modify original_velocity so it parallels all of the clip planes
		for (i = 0; i < numplanes; i++)
		{
			PM_ClipVelocity (original_velocity, planes[i], qw_pmove.velocity, 1);
			for (j = 0; j < numplanes; j++)
				if (j != i)
				{
					if (DotProduct (qw_pmove.velocity, planes[j]) < 0)
						break;	// not ok
				}
			if (j == numplanes)
				break;
		}

		if (i != numplanes)
		{	// go along this plane
		}
		else
		{	// go along the crease
			if (numplanes != 2)
			{
				VectorCopy (vec3_origin, qw_pmove.velocity);
				break;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, qw_pmove.velocity);
			VectorScale (dir, d, qw_pmove.velocity);
		}

		// if velocity is against the original velocity, stop dead to avoid
		// tiny oscillations in sloping corners
		if (DotProduct (qw_pmove.velocity, primal_velocity) <= 0)
		{
			VectorCopy (vec3_origin, qw_pmove.velocity);
			break;
		}
	}

	if (qw_pmove.waterjumptime)
		VectorCopy (primal_velocity, qw_pmove.velocity);

	return blocked;
}

/*
=============
PM_GroundMove -- player is on ground, with no upwards velocity.
=============
*/
static void PM_GroundMove (void)
{
	vec3_t		dest;
	qw_pmtrace_t	trace;
	vec3_t		original, originalvel, down, up, downvel;
	float		downdist, updist;

	qw_pmove.velocity[2] = 0;
	if (!qw_pmove.velocity[0] && !qw_pmove.velocity[1] && !qw_pmove.velocity[2])
		return;

	// first try just moving to the destination
	dest[0] = qw_pmove.origin[0] + qw_pmove.velocity[0]*qw_pmframetime;
	dest[1] = qw_pmove.origin[1] + qw_pmove.velocity[1]*qw_pmframetime;
	dest[2] = qw_pmove.origin[2];

	trace = QWPM_PlayerTrace (qw_pmove.origin, dest);
	if (trace.fraction == 1)
	{
		VectorCopy (trace.endpos, qw_pmove.origin);
		return;
	}

	// try sliding forward both on ground and up 16 pixels; take the farthest
	VectorCopy (qw_pmove.origin, original);
	VectorCopy (qw_pmove.velocity, originalvel);

	// slide move
	PM_FlyMove ();

	VectorCopy (qw_pmove.origin, down);
	VectorCopy (qw_pmove.velocity, downvel);

	VectorCopy (original, qw_pmove.origin);
	VectorCopy (originalvel, qw_pmove.velocity);

	// move up a stair height
	VectorCopy (qw_pmove.origin, dest);
	dest[2] += STEPSIZE;
	trace = QWPM_PlayerTrace (qw_pmove.origin, dest);
	if (!trace.startsolid && !trace.allsolid)
		VectorCopy (trace.endpos, qw_pmove.origin);

	// slide move
	PM_FlyMove ();

	// press down the stepheight
	VectorCopy (qw_pmove.origin, dest);
	dest[2] -= STEPSIZE;
	trace = QWPM_PlayerTrace (qw_pmove.origin, dest);
	if (trace.plane.normal[2] < 0.7)
		goto usedown;
	if (!trace.startsolid && !trace.allsolid)
		VectorCopy (trace.endpos, qw_pmove.origin);
	VectorCopy (qw_pmove.origin, up);

	// decide which one went farther
	downdist = (down[0] - original[0])*(down[0] - original[0])
		+ (down[1] - original[1])*(down[1] - original[1]);
	updist = (up[0] - original[0])*(up[0] - original[0])
		+ (up[1] - original[1])*(up[1] - original[1]);

	if (downdist > updist)
	{
usedown:
		VectorCopy (down, qw_pmove.origin);
		VectorCopy (downvel, qw_pmove.velocity);
	}
	else // copy z value from slide move
		qw_pmove.velocity[2] = downvel[2];
}

/*
==================
PM_Friction -- handles both ground friction and water friction.
==================
*/
static void PM_Friction (void)
{
	float		*vel;
	float		speed, newspeed, control;
	float		friction;
	float		drop;
	vec3_t		start, stop;
	qw_pmtrace_t	trace;

	if (qw_pmove.waterjumptime)
		return;

	vel = qw_pmove.velocity;

	speed = sqrt (vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
	if (speed < 1)
	{
		vel[0] = 0;
		vel[1] = 0;
		return;
	}

	friction = qw_movevars.friction;

	// if the leading edge is over a dropoff, increase friction
	if (qw_onground != -1)
	{
		start[0] = stop[0] = qw_pmove.origin[0] + vel[0]/speed*16;
		start[1] = stop[1] = qw_pmove.origin[1] + vel[1]/speed*16;
		start[2] = qw_pmove.origin[2] + player_mins[2];
		stop[2] = start[2] - 34;

		trace = QWPM_PlayerTrace (start, stop);

		if (trace.fraction == 1)
			friction *= 2;
	}

	drop = 0;

	if (qw_waterlevel >= 2) // apply water friction
		drop += speed*qw_movevars.waterfriction*qw_waterlevel*qw_pmframetime;
	else if (qw_onground != -1) // apply ground friction
	{
		control = speed < qw_movevars.stopspeed ? qw_movevars.stopspeed : speed;
		drop += control*friction*qw_pmframetime;
	}

	// scale the velocity
	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	vel[0] = vel[0] * newspeed;
	vel[1] = vel[1] * newspeed;
	vel[2] = vel[2] * newspeed;
}

static void PM_Accelerate (vec3_t wishdir, float wishspeed, float accel)
{
	int	i;
	float	addspeed, accelspeed, currentspeed;

	if (qw_pmove.dead)
		return;
	if (qw_pmove.waterjumptime)
		return;

	currentspeed = DotProduct (qw_pmove.velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel*qw_pmframetime*wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i = 0; i < 3; i++)
		qw_pmove.velocity[i] += accelspeed*wishdir[i];
}

static void PM_AirAccelerate (vec3_t wishdir, float wishspeed, float accel)
{
	int	i;
	float	addspeed, accelspeed, currentspeed, wishspd = wishspeed;

	if (qw_pmove.dead)
		return;
	if (qw_pmove.waterjumptime)
		return;

	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (qw_pmove.velocity, wishdir);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel * wishspeed * qw_pmframetime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i = 0; i < 3; i++)
		qw_pmove.velocity[i] += accelspeed*wishdir[i];
}

/*
===================
PM_WaterMove
===================
*/
static void PM_WaterMove (void)
{
	int		i;
	vec3_t		wishvel;
	float		wishspeed;
	vec3_t		wishdir;
	vec3_t		start, dest;
	qw_pmtrace_t	trace;

	// user intentions
	for (i = 0; i < 3; i++)
		wishvel[i] = qw_pm_forward[i]*qw_pmove.cmd.forwardmove + qw_pm_right[i]*qw_pmove.cmd.sidemove;

	if (!qw_pmove.cmd.forwardmove && !qw_pmove.cmd.sidemove && !qw_pmove.cmd.upmove)
		wishvel[2] -= 60;		// drift towards bottom
	else
		wishvel[2] += qw_pmove.cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	if (wishspeed > qw_movevars.maxspeed)
	{
		VectorScale (wishvel, qw_movevars.maxspeed/wishspeed, wishvel);
		wishspeed = qw_movevars.maxspeed;
	}
	wishspeed *= 0.7;

	// water acceleration
	PM_Accelerate (wishdir, wishspeed, qw_movevars.wateraccelerate);

	// assume it is a stair or a slope, so press down from stepheight above
	VectorMA (qw_pmove.origin, qw_pmframetime, qw_pmove.velocity, dest);
	VectorCopy (dest, start);
	start[2] += STEPSIZE + 1;
	trace = QWPM_PlayerTrace (start, dest);
	if (!trace.startsolid && !trace.allsolid)	// walked up the step
	{
		VectorCopy (trace.endpos, qw_pmove.origin);
		return;
	}

	PM_FlyMove ();
}

/*
===================
PM_AirMove
===================
*/
static void PM_AirMove (void)
{
	int	i;
	vec3_t	wishvel;
	float	fmove, smove;
	vec3_t	wishdir;
	float	wishspeed;

	fmove = qw_pmove.cmd.forwardmove;
	smove = qw_pmove.cmd.sidemove;

	qw_pm_forward[2] = 0;
	qw_pm_right[2] = 0;
	VectorNormalize (qw_pm_forward);
	VectorNormalize (qw_pm_right);

	for (i = 0; i < 2; i++)
		wishvel[i] = qw_pm_forward[i]*fmove + qw_pm_right[i]*smove;
	wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	// clamp to server defined max speed
	if (wishspeed > qw_movevars.maxspeed)
	{
		VectorScale (wishvel, qw_movevars.maxspeed/wishspeed, wishvel);
		wishspeed = qw_movevars.maxspeed;
	}

	if (qw_onground != -1)
	{
		qw_pmove.velocity[2] = 0;
		PM_Accelerate (wishdir, wishspeed, qw_movevars.accelerate);
		qw_pmove.velocity[2] -= qw_movevars.entgravity * qw_movevars.gravity * qw_pmframetime;
		PM_GroundMove ();
	}
	else
	{	// not on ground, so little effect on velocity
		PM_AirAccelerate (wishdir, wishspeed, qw_movevars.accelerate);
		qw_pmove.velocity[2] -= qw_movevars.entgravity * qw_movevars.gravity * qw_pmframetime;
		PM_FlyMove ();
	}
}

/*
=============
PM_CatagorizePosition
=============
*/
static void PM_CatagorizePosition (void)
{
	vec3_t		point;
	int		cont;
	qw_pmtrace_t	tr;

// if the player hull point one unit down is solid, the player is on ground

	// see if standing on something solid
	point[0] = qw_pmove.origin[0];
	point[1] = qw_pmove.origin[1];
	point[2] = qw_pmove.origin[2] - 1;
	if (qw_pmove.velocity[2] > 180)
	{
		qw_onground = -1;
	}
	else
	{
		tr = QWPM_PlayerTrace (qw_pmove.origin, point);
		if (tr.plane.normal[2] < 0.7)
			qw_onground = -1;	// too steep
		else
			qw_onground = tr.ent;
		if (qw_onground != -1)
		{
			qw_pmove.waterjumptime = 0;
			if (!tr.startsolid && !tr.allsolid)
				VectorCopy (tr.endpos, qw_pmove.origin);
		}

		// standing on an entity other than the world
		if (tr.ent > 0)
		{
			qw_pmove.touchindex[qw_pmove.numtouch] = tr.ent;
			qw_pmove.numtouch++;
		}
	}

	// get waterlevel
	qw_waterlevel = 0;
	qw_watertype = CONTENTS_EMPTY;

	point[2] = qw_pmove.origin[2] + player_mins[2] + 1;
	cont = QWPM_PointContents (point);

	if (cont <= CONTENTS_WATER)
	{
		qw_watertype = cont;
		qw_waterlevel = 1;
		point[2] = qw_pmove.origin[2] + (player_mins[2] + player_maxs[2])*0.5;
		cont = QWPM_PointContents (point);
		if (cont <= CONTENTS_WATER)
		{
			qw_waterlevel = 2;
			point[2] = qw_pmove.origin[2] + 22;
			cont = QWPM_PointContents (point);
			if (cont <= CONTENTS_WATER)
				qw_waterlevel = 3;
		}
	}
}

/*
=============
JumpButton
=============
*/
static void JumpButton (void)
{
	if (qw_pmove.dead)
	{
		qw_pmove.oldbuttons |= BUTTON_JUMP;	// don't jump again until released
		return;
	}

	if (qw_pmove.waterjumptime)
	{
		qw_pmove.waterjumptime -= qw_pmframetime;
		if (qw_pmove.waterjumptime < 0)
			qw_pmove.waterjumptime = 0;
		return;
	}

	if (qw_waterlevel >= 2)
	{	// swimming, not jumping
		qw_onground = -1;

		if (qw_watertype == CONTENTS_WATER)
			qw_pmove.velocity[2] = 100;
		else if (qw_watertype == CONTENTS_SLIME)
			qw_pmove.velocity[2] = 80;
		else
			qw_pmove.velocity[2] = 50;
		return;
	}

	if (qw_onground == -1)
		return;		// in air, so no effect

	if (qw_pmove.oldbuttons & BUTTON_JUMP)
		return;		// don't pogo stick

	qw_onground = -1;
	qw_pmove.velocity[2] += 270;

	qw_pmove.oldbuttons |= BUTTON_JUMP;	// don't jump again until released
}

/*
=============
CheckWaterJump
=============
*/
static void CheckWaterJump (void)
{
	vec3_t	spot;
	int	cont;
	vec3_t	flatforward;

	if (qw_pmove.waterjumptime)
		return;

	// don't hop out if we just jumped in
	if (qw_pmove.velocity[2] < -180)
		return; // only hop out if we are moving up

	// see if near an edge
	flatforward[0] = qw_pm_forward[0];
	flatforward[1] = qw_pm_forward[1];
	flatforward[2] = 0;
	VectorNormalize (flatforward);

	VectorMA (qw_pmove.origin, 24, flatforward, spot);
	spot[2] += 8;
	cont = QWPM_PointContents (spot);
	if (cont != CONTENTS_SOLID)
		return;
	spot[2] += 24;
	cont = QWPM_PointContents (spot);
	if (cont != CONTENTS_EMPTY)
		return;
	// jump out of water
	VectorScale (flatforward, 50, qw_pmove.velocity);
	qw_pmove.velocity[2] = 310;
	qw_pmove.waterjumptime = 2;	// safety net
	qw_pmove.oldbuttons |= BUTTON_JUMP;	// don't jump again until released
}

/*
=================
NudgePosition -- if the origin is in solid, nudge on each axis to allow for the
cut precision of the net coordinates.
=================
*/
static void NudgePosition (void)
{
	vec3_t		base;
	int		x, y, z;
	int		i;
	static int	sign[3] = {0, -1, 1};

	VectorCopy (qw_pmove.origin, base);

	for (i = 0; i < 3; i++)
		qw_pmove.origin[i] = ((int)(qw_pmove.origin[i]*8)) * 0.125;

	for (z = 0; z <= 2; z++)
	{
		for (x = 0; x <= 2; x++)
		{
			for (y = 0; y <= 2; y++)
			{
				qw_pmove.origin[0] = base[0] + (sign[x] * 1.0/8);
				qw_pmove.origin[1] = base[1] + (sign[y] * 1.0/8);
				qw_pmove.origin[2] = base[2] + (sign[z] * 1.0/8);
				if (QWPM_TestPlayerPosition (qw_pmove.origin))
					return;
			}
		}
	}
	VectorCopy (base, qw_pmove.origin);
}

/*
===============
SpectatorMove
===============
*/
static void SpectatorMove (void)
{
	float	speed, drop, friction, control, newspeed;
	float	currentspeed, addspeed, accelspeed;
	int	i;
	vec3_t	wishvel;
	float	fmove, smove;
	vec3_t	wishdir;
	float	wishspeed;

	// friction
	speed = VectorLength (qw_pmove.velocity);
	if (speed < 1)
	{
		VectorCopy (vec3_origin, qw_pmove.velocity);
	}
	else
	{
		drop = 0;

		friction = qw_movevars.friction*1.5;	// extra friction
		control = speed < qw_movevars.stopspeed ? qw_movevars.stopspeed : speed;
		drop += control*friction*qw_pmframetime;

		// scale the velocity
		newspeed = speed - drop;
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;

		VectorScale (qw_pmove.velocity, newspeed, qw_pmove.velocity);
	}

	// accelerate
	fmove = qw_pmove.cmd.forwardmove;
	smove = qw_pmove.cmd.sidemove;

	VectorNormalize (qw_pm_forward);
	VectorNormalize (qw_pm_right);

	for (i = 0; i < 3; i++)
		wishvel[i] = qw_pm_forward[i]*fmove + qw_pm_right[i]*smove;
	wishvel[2] += qw_pmove.cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);

	// clamp to server defined max speed
	if (wishspeed > qw_movevars.spectatormaxspeed)
	{
		VectorScale (wishvel, qw_movevars.spectatormaxspeed/wishspeed, wishvel);
		wishspeed = qw_movevars.spectatormaxspeed;
	}

	currentspeed = DotProduct (qw_pmove.velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = qw_movevars.accelerate*qw_pmframetime*wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i = 0; i < 3; i++)
		qw_pmove.velocity[i] += accelspeed*wishdir[i];

	// move
	VectorMA (qw_pmove.origin, qw_pmframetime, qw_pmove.velocity, qw_pmove.origin);
}

/*
=============
QWPM_PlayerMove -- returns with origin, angles and velocity modified in place.
Numtouch/touchindex are set if any physents were contacted during the move.
=============
*/
void QWPM_PlayerMove (void)
{
	qw_pmframetime = qw_pmove.cmd.msec * 0.001;
	qw_pmove.numtouch = 0;

	AngleVectors (qw_pmove.angles, qw_pm_forward, qw_pm_right, qw_pm_up);

	if (qw_pmove.spectator)
	{
		SpectatorMove ();
		return;
	}

	NudgePosition ();

	// take angles directly from command
	VectorCopy (qw_pmove.cmd.angles, qw_pmove.angles);

	// set onground, watertype, and waterlevel
	PM_CatagorizePosition ();

	if (qw_waterlevel == 2)
		CheckWaterJump ();

	if (qw_pmove.velocity[2] < 0)
		qw_pmove.waterjumptime = 0;

	if (qw_pmove.cmd.buttons & BUTTON_JUMP)
		JumpButton ();
	else
		qw_pmove.oldbuttons &= ~BUTTON_JUMP;

	PM_Friction ();

	if (qw_waterlevel >= 2)
		PM_WaterMove ();
	else
		PM_AirMove ();

	// set onground, watertype, and waterlevel for final spot
	PM_CatagorizePosition ();
}

#endif	/* USE_QW_PROTOCOL */
