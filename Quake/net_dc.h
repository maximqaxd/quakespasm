/*
 * net_dc.h -- Dreamcast/KallistiOS networking compatibility shim.
 *
 * KOS provides BSD sockets (lwIP) and takes the PLATFORM_UNIX code paths, but a
 * few POSIX niceties net_udp.c expects are missing or behave differently:
 *   - ioctl() is aliased to fs_ioctl() (filesystem), so ioctlsocket(FIONBIO)
 *     cannot set a socket non-blocking -- translate it to fcntl() instead.
 *   - FIONBIO/FIONREAD are not defined by KOS headers.
 *   - gethostbyaddr() (reverse DNS) and hstrerror() are not provided.
 *
 * Included from net_sys.h for PLATFORM_DREAMCAST, after the UNIX block.
 *
 * NOTE: runtime networking behavior still needs on-target validation; this
 * header only makes the BSD-socket UDP driver build and set non-blocking mode
 * correctly. FIONREAD availability depends on the KOS socket fd handler.
 */
#ifndef NET_DC_H
#define NET_DC_H

#include <fcntl.h>
#include <string.h>
#include <errno.h>

#ifndef FIONBIO
#define FIONBIO		0x8004667eUL
#endif
#ifndef FIONREAD
#define FIONREAD	0x4004667fUL
#endif

#ifndef hstrerror
#define hstrerror(x)	strerror((x))
#endif
#ifndef h_errno
#define h_errno		errno
#endif

/* KOS has gethostbyname()/gethostbyname2() but no gethostbyaddr(); reverse DNS
   of a peer address is cosmetic (used only for display), so stub it out. */
#define gethostbyaddr(addr, len, type)	((struct hostent *)0)

/* Translate the socket ioctls net_udp.c uses onto KOS-friendly primitives. */
static inline int dc_ioctlsocket (int s, unsigned long cmd, int *argp)
{
	if (cmd == FIONBIO)
	{
		int flags = fcntl (s, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		if (argp && *argp)
			flags |= O_NONBLOCK;
		else
			flags &= ~O_NONBLOCK;
		return fcntl (s, F_SETFL, flags);
	}
	/* FIONREAD and anything else: defer to the fd's ioctl handler. */
	return ioctl (s, cmd, argp);
}

#undef ioctlsocket
#define ioctlsocket	dc_ioctlsocket

/* KOS declares gethostname() but provides no implementation; supply a stub so
   UDP_Init() can name the local host. */
static inline int dc_gethostname (char *name, size_t len)
{
	if (name && len)
	{
		strncpy (name, "dreamcast", len);
		name[len - 1] = '\0';
	}
	return 0;
}
#undef gethostname
#define gethostname	dc_gethostname

#endif /* NET_DC_H */
