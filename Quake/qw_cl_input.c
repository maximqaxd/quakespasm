/*
================================================================================
qw_cl_input.c -- QuakeWorld movement output (clc_move)

Each frame the client builds a usercmd from the input state (reusing NetQuake's
CL_BaseMove for the movement values and view angles) and sends the last three
commands, delta-compressed, in a clc_move packet with the sequence checksum the
server validates. Without this the server ignores our input and eventually times
us out. Server-authoritative movement (no client prediction yet -- that's the
next phase); our position comes back via svc_playerinfo.
================================================================================
*/
#include "quakedef.h"

#if defined(USE_QW_PROTOCOL)

#include "qw_local.h"
#include "qw_net.h"

extern kbutton_t	in_attack, in_jump;	// cl_input.c
extern int		in_impulse;
extern double		host_frametime;		// host.c

static qw_usercmd_t	qw_cmds[QW_UPDATE_BACKUP];
static qw_usercmd_t	qw_nullcmd;		// zero baseline for the first delta

/*
==============
CLQW_ButtonBits -- QuakeWorld button flags (attack=1, jump=2)
==============
*/
static int CLQW_ButtonBits (void)
{
	int	bits = 0;

	if (in_attack.state & 3)
		bits |= 1;
	in_attack.state &= ~2;

	if (in_jump.state & 3)
		bits |= 2;
	in_jump.state &= ~2;

	return bits;
}

/*
==============
CLQW_WriteDeltaUsercmd -- delta a usercmd against the previous one
==============
*/
static void CLQW_WriteDeltaUsercmd (sizebuf_t *buf, const qw_usercmd_t *from, const qw_usercmd_t *cmd)
{
	int	bits = 0;

	if (cmd->angles[0] != from->angles[0]) bits |= QWCM_ANGLE1;
	if (cmd->angles[1] != from->angles[1]) bits |= QWCM_ANGLE2;
	if (cmd->angles[2] != from->angles[2]) bits |= QWCM_ANGLE3;
	if (cmd->forwardmove != from->forwardmove) bits |= QWCM_FORWARD;
	if (cmd->sidemove != from->sidemove) bits |= QWCM_SIDE;
	if (cmd->upmove != from->upmove) bits |= QWCM_UP;
	if (cmd->buttons != from->buttons) bits |= QWCM_BUTTONS;
	if (cmd->impulse != from->impulse) bits |= QWCM_IMPULSE;

	MSG_WriteByte (buf, bits);

	if (bits & QWCM_ANGLE1)  MSG_WriteAngle16 (buf, cmd->angles[0], 0);
	if (bits & QWCM_ANGLE2)  MSG_WriteAngle16 (buf, cmd->angles[1], 0);
	if (bits & QWCM_ANGLE3)  MSG_WriteAngle16 (buf, cmd->angles[2], 0);
	if (bits & QWCM_FORWARD) MSG_WriteShort (buf, cmd->forwardmove);
	if (bits & QWCM_SIDE)    MSG_WriteShort (buf, cmd->sidemove);
	if (bits & QWCM_UP)      MSG_WriteShort (buf, cmd->upmove);
	if (bits & QWCM_BUTTONS) MSG_WriteByte (buf, cmd->buttons);
	if (bits & QWCM_IMPULSE) MSG_WriteByte (buf, cmd->impulse);

	MSG_WriteByte (buf, cmd->msec);
}

/*
==============
CLQW_SendMove -- build the current usercmd and transmit clc_move
==============
*/
void CLQW_SendMove (void)
{
	usercmd_t	nqcmd;		// NetQuake movement (reuses CL_BaseMove)
	qw_usercmd_t	*cmd, *oldcmd;
	sizebuf_t	buf;
	byte		data[128];
	int		seq, i, ms, checksumIndex;

	seq = cls.netchan.outgoing_sequence;
	cmd = &qw_cmds[seq & QW_UPDATE_MASK];
	memset (cmd, 0, sizeof(*cmd));

	// movement + view angles from the shared input path. CL_BaseMove pulls
	// keyboard movement/turning; IN_Move layers on the mouse and analog-stick
	// look (which is what writes cl.viewangles on the Dreamcast pad) -- the same
	// two-step NetQuake's CL_SendCmd runs.
	memset (&nqcmd, 0, sizeof(nqcmd));
	CL_BaseMove (&nqcmd);
	IN_Move (&nqcmd);
	VectorCopy (cl.viewangles, cmd->angles);
	cmd->forwardmove = (short) nqcmd.forwardmove;
	cmd->sidemove    = (short) nqcmd.sidemove;
	cmd->upmove      = (short) nqcmd.upmove;
	cmd->buttons     = (byte) CLQW_ButtonBits ();
	cmd->impulse     = (byte) in_impulse;
	in_impulse = 0;

	ms = (int)(host_frametime * 1000.0 + 0.5);
	if (ms > 250) ms = 100;		// clamp long hitches
	if (ms < 1)   ms = 1;
	cmd->msec = (byte) ms;

	// record this command for prediction, keyed by its outgoing sequence
	qw_frames[seq & QW_UPDATE_MASK].cmd = *cmd;
	qw_frames[seq & QW_UPDATE_MASK].senttime = realtime;
	qw_frames[seq & QW_UPDATE_MASK].playervalid = false;

	// assemble the clc_move packet
	buf.data = data;
	buf.maxsize = sizeof(data);
	buf.cursize = 0;
	buf.allowoverflow = false;
	buf.overflowed = false;

	MSG_WriteByte (&buf, qwclc_move);
	checksumIndex = buf.cursize;
	MSG_WriteByte (&buf, 0);		// checksum, filled below
	MSG_WriteByte (&buf, 0);		// packet loss (0 -- not tracked yet)

	// the three most recent commands, delta'd for redundancy vs packet loss
	oldcmd = &qw_nullcmd;
	i = (seq - 2) & QW_UPDATE_MASK;
	CLQW_WriteDeltaUsercmd (&buf, oldcmd, &qw_cmds[i]);
	oldcmd = &qw_cmds[i];
	i = (seq - 1) & QW_UPDATE_MASK;
	CLQW_WriteDeltaUsercmd (&buf, oldcmd, &qw_cmds[i]);
	oldcmd = &qw_cmds[i];
	CLQW_WriteDeltaUsercmd (&buf, oldcmd, cmd);

	// checksum over everything after the checksum byte
	buf.data[checksumIndex] = COM_BlockSequenceCRCByte (
		buf.data + checksumIndex + 1, buf.cursize - checksumIndex - 1, seq);

	QWNetchan_Transmit (&cls.netchan, buf.cursize, buf.data);
}

#endif	/* USE_QW_PROTOCOL */
