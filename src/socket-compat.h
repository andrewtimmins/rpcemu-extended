/*
  RPCEmu - An Acorn system emulator

  Cross-platform sockets compatibility layer.

  The core's networking code (hostcmd, debugcmd, broadcast relay, the TCP
  "modem" in peripheral_config) is written against the BSD/POSIX sockets API.
  Windows provides Winsock2, which differs in a handful of ways that matter:

    - headers: <winsock2.h>/<ws2tcpip.h> instead of <sys/socket.h> et al,
      and <winsock2.h> MUST be included before <windows.h>;
    - closing a socket is closesocket(), not close();
    - errors are reported via WSAGetLastError(), NOT errno;
    - poll() is spelled WSAPoll();
    - setsockopt()/getsockopt() take a char* option buffer;
    - there is no SIGPIPE, so MSG_NOSIGNAL is unnecessary.

  Include this header instead of the raw POSIX socket headers. It also brings
  in <windows.h> on Windows so callers get the rest of the Win32 API.
*/
#ifndef SOCKET_COMPAT_H
#define SOCKET_COMPAT_H

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* poll() -> WSAPoll(): identical struct pollfd / POLLIN shape. We only ever
   poll for readability, so WSAPoll's historical POLLOUT quirk does not bite. */
#define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0	/* Windows never raises SIGPIPE */
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0	/* our sockets are put in non-blocking mode explicitly */
#endif

/* Socket errors come from WSAGetLastError(), not errno. */
#define sock_errno()      WSAGetLastError()
#define SOCK_EWOULDBLOCK  WSAEWOULDBLOCK
#define SOCK_EAGAIN       WSAEWOULDBLOCK
#define SOCK_EINPROGRESS  WSAEWOULDBLOCK	/* non-blocking connect() in progress */
#define SOCK_EINTR        WSAEINTR

#else /* !_WIN32 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define closesocket(fd)   close(fd)
#define sock_errno()      errno
#define SOCK_EWOULDBLOCK  EWOULDBLOCK
#define SOCK_EAGAIN       EAGAIN
#define SOCK_EINPROGRESS  EINPROGRESS
#define SOCK_EINTR        EINTR

/* macOS/BSD have no MSG_NOSIGNAL send flag; SIGPIPE is suppressed per-socket
   via the SO_NOSIGPIPE option instead (set in socket_set_nonblocking below).
   Define the flag as a no-op so the shared send() call sites still compile. */
#if defined(__APPLE__) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

#endif /* _WIN32 */

/**
 * Put a socket into non-blocking mode.
 *
 * @param fd Socket descriptor
 * @return   0 on success, non-zero on error
 */
static inline int
socket_set_nonblocking(int fd)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(fd, FIONBIO, &mode);
#else
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl < 0) {
		return -1;
	}
#ifdef __APPLE__
	/* macOS has no MSG_NOSIGNAL; suppress SIGPIPE for this socket instead so a
	   write to a peer that has gone away returns EPIPE rather than killing us. */
	{
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	}
#endif
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

#endif /* SOCKET_COMPAT_H */
