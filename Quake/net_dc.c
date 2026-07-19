/*
================================================================================
net_dc.c -- Dreamcast dial-up (PPP over the built-in 56k modem)

The Broadband Adapter and the W5500/LAN adapters are brought up automatically by
KOS at boot (INIT_NET + DHCP), so net_default_dev is already valid for them and
the rest of the UDP layer just works. The modem is the exception: it has to dial
a peer and negotiate PPP before any IP traffic can flow, which KOS does not do on
its own. This module adds that.

  dial [number]   -- dial and bring up PPP (uses the modem_* cvars; the optional
                     argument overrides modem_number for this one call)
  hangup          -- tear the link down and power the modem off

On a modem-only Dreamcast with no broadband device, NET_DC_ModemInit auto-dials
once at startup using the (DreamPi-friendly) cvar defaults so the common case
needs no configuration. Everything is a no-op on non-Dreamcast builds.
================================================================================
*/
#include "quakedef.h"

#if defined(PLATFORM_DREAMCAST)

#include <kos/net.h>
#include <dc/modem/modem.h>
#include <ppp/ppp.h>

extern void UDP_DC_RefreshLocal (void);		// net_udp.c

// DreamPi defaults: the dialed number is ignored by DreamPi and the login is
// "dream"/"cast"; a real ISP needs its own number and PPP credentials here.
cvar_t	modem_number   = {"modem_number",   "555",   CVAR_ARCHIVE};
cvar_t	modem_login    = {"modem_login",    "dream", CVAR_ARCHIVE};
cvar_t	modem_password = {"modem_password", "cast",  CVAR_ARCHIVE};
cvar_t	modem_blind    = {"modem_blind",    "0",     CVAR_ARCHIVE};	// 1 = don't wait for a dial tone
cvar_t	modem_autodial = {"modem_autodial", "1",     CVAR_ARCHIVE};	// dial at boot when no BBA/LAN is up

static qboolean	dc_dialed;

/*
==============
NET_DC_Dial -- returns true once the PPP link is up (net_default_dev valid).
==============
*/
static qboolean NET_DC_Dial (const char *number)
{
	int	rate = 0, err;

	if (dc_dialed)
	{
		Con_Printf ("modem: already connected -- hangup first\n");
		return true;
	}
	if (net_default_dev && (net_default_dev->flags & NETIF_RUNNING))
	{
		Con_Printf ("modem: a broadband adapter is already online; not dialing\n");
		return true;
	}

	Con_Printf ("modem: initializing hardware...\n");
	if (!modem_init ())
	{
		Con_Printf ("modem: no modem found\n");
		return false;
	}

	if (ppp_init () < 0)
	{
		Con_Printf ("modem: ppp_init failed\n");
		modem_shutdown ();
		return false;
	}
	ppp_set_login (modem_login.string, modem_password.string);

	Con_Printf ("modem: dialing %s...\n", number);
	err = ppp_modem_init (number, modem_blind.value ? 1 : 0, &rate);
	if (err != 0)
	{
		// -1 init, -2 no dial tone, -3 dial failed, -4 no carrier in 60s
		Con_Printf ("modem: dial failed (%d)\n", err);
		ppp_shutdown ();
		modem_shutdown ();
		return false;
	}
	Con_Printf ("modem: carrier at %d bps, negotiating PPP...\n", rate);

	err = ppp_connect ();		// blocks until the link is established
	if (err != 0)
	{
		Con_Printf ("modem: PPP negotiation failed (%d)\n", err);
		ppp_shutdown ();
		modem_shutdown ();
		return false;
	}

	// libppp registers its netif and makes it the default, but never raises
	// NETIF_RUNNING; the UDP layer keys "am I online" off that flag, so set it.
	if (net_default_dev)
		net_default_dev->flags |= NETIF_RUNNING;

	// KOS's boot-time net_init() bails out before initializing the socket layer
	// (fs_socket/UDP/TCP) when no device is present -- which is exactly the
	// modem-only case, since the modem isn't a netif until ppp_init() registers
	// one. That early return leaves net_initted clear, so run it again now that
	// the ppp device exists: this brings the sockets up (and it's a harmless
	// no-op when a BBA already initialized the stack at boot). ppp already has an
	// IP, so the DHCP step inside is skipped.
	net_init (0);

	dc_dialed = true;

	UDP_DC_RefreshLocal ();		// UDP_Init read loopback before the link existed

	if (net_default_dev)
		Con_Printf ("modem: online -- %d.%d.%d.%d\n",
			net_default_dev->ip_addr[0], net_default_dev->ip_addr[1],
			net_default_dev->ip_addr[2], net_default_dev->ip_addr[3]);
	return true;
}

static void NET_DC_Dial_f (void)
{
	const char *number = (Cmd_Argc () > 1) ? Cmd_Argv (1) : modem_number.string;
	NET_DC_Dial (number);
}

static void NET_DC_Hangup_f (void)
{
	if (!dc_dialed)
	{
		Con_Printf ("modem: not connected\n");
		return;
	}
	ppp_shutdown ();
	modem_shutdown ();
	dc_dialed = false;
	Con_Printf ("modem: hung up\n");
}

/*
==============
NET_DC_ModemInit -- called from NET_Init before the UDP driver comes up, so an
auto-dialed link is already the default device when UDP_Init reads its address.
==============
*/
void NET_DC_ModemInit (void)
{
	Cvar_RegisterVariable (&modem_number);
	Cvar_RegisterVariable (&modem_login);
	Cvar_RegisterVariable (&modem_password);
	Cvar_RegisterVariable (&modem_blind);
	Cvar_RegisterVariable (&modem_autodial);

	Cmd_AddCommand ("dial", NET_DC_Dial_f);
	Cmd_AddCommand ("hangup", NET_DC_Hangup_f);

	// Zero-config path: no broadband device detected, so try the modem. (Config
	// isn't loaded yet at this point, so this uses the DreamPi-friendly defaults;
	// ISP users dial manually after boot once their cvars are set.)
	if (modem_autodial.value && !net_default_dev)
		NET_DC_Dial (modem_number.string);
}

void NET_DC_ModemShutdown (void)
{
	if (dc_dialed)
	{
		ppp_shutdown ();
		modem_shutdown ();
		dc_dialed = false;
	}
}

#endif	/* PLATFORM_DREAMCAST */
