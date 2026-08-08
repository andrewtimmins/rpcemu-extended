/*
  RPCEmu - An Acorn system emulator

  rpcemu-debug - host-side client for the DebugCmd channel.

  Connects to the emulator's debugger socket and drives the emulated ARM:
    rpcemu-debug [--socket PATH | --tcp host:port] <command...>   (one-shot)
    rpcemu-debug [--socket PATH | --tcp host:port]                (interactive)

  This is the counterpart of rpcemu-run, which drives the guest's RISC OS
  command line. That one talks to the operating system inside the machine;
  this one talks to the processor underneath it, and the two are separate
  sockets speaking different protocols for that reason.

  Wire protocol: send one '\n'-terminated request line, read exactly one line
  of JSON back. See docs/debugcmd.md for the verb reference.

  The JSON is printed as it arrives rather than being parsed. A debugger client
  that reformatted its answers would be one more place for the truth to change
  shape between the emulator and the person reading it, and the responses are
  already short enough to read. Anything that wants structure should use the
  MCP server or speak to the socket directly.

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../socket-compat.h"
#ifndef _WIN32
#include <sys/un.h>
#endif

#ifdef _WIN32
/* Matches DEBUGCMD_DEFAULT_TCP_PORT in debugcmd.c (Windows has no AF_UNIX). */
#define RPCEMU_DEBUG_DEFAULT_TCP "127.0.0.1:15591"
#endif

#define LINE_MAX_LEN 65536

static const char *progname = "rpcemu-debug";

#ifndef _WIN32
/**
 * Where the emulator puts its debugger socket by default.
 */
static const char *
default_socket_path(char *buf, size_t buflen)
{
	const char *datadir = getenv("RPCEMU_DATADIR");

	if (datadir != NULL && datadir[0] != '\0') {
		size_t n = strlen(datadir);
		const char *sep = (n > 0 && datadir[n - 1] == '/') ? "" : "/";

		snprintf(buf, buflen, "%s%srpcemu-debug.sock", datadir, sep);
	} else {
		snprintf(buf, buflen, "rpcemu-debug.sock");
	}
	return buf;
}

static int
connect_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "%s: socket path too long: %s\n", progname, path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("rpcemu-debug: socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* The length was checked above, so this fits with room for the
	   terminator the memset already put there. memcpy rather than strncpy
	   for the reason given in rpcemu_run.c. */
	memcpy(addr.sun_path, path, strlen(path));
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "%s: cannot connect to %s: %s\n",
		    progname, path, strerror(errno));
		closesocket(fd);
		return -1;
	}
	return fd;
}
#endif /* !_WIN32 */

static int
connect_tcp(const char *hostport)
{
	char host[256];
	const char *colon = strrchr(hostport, ':');
	const char *port;
	struct addrinfo hints, *res, *rp;
	int fd = -1;

	if (colon != NULL) {
		size_t n = (size_t) (colon - hostport);

		if (n >= sizeof(host)) {
			n = sizeof(host) - 1;
		}
		memcpy(host, hostport, n);
		host[n] = '\0';
		port = colon + 1;
	} else {
		snprintf(host, sizeof(host), "127.0.0.1");
		port = hostport;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		fprintf(stderr, "%s: cannot resolve %s:%s\n", progname, host, port);
		return -1;
	}
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			break;
		}
		closesocket(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot connect to %s:%s\n", progname, host, port);
	}
	return fd;
}

/**
 * Send one request line.
 *
 * @return 0 on success, -1 on error
 */
static int
send_line(int fd, const char *line)
{
	size_t len = strlen(line);
	char *buf = malloc(len + 2);
	size_t sent = 0;
	int result = 0;

	if (buf == NULL) {
		return -1;
	}
	memcpy(buf, line, len);
	buf[len] = '\n';
	len++;

	while (sent < len) {
		int n = (int) send(fd, buf + sent, (int) (len - sent), 0);

		if (n <= 0) {
			result = -1;
			break;
		}
		sent += (size_t) n;
	}

	free(buf);
	return result;
}

/* Bytes read from the socket but not yet consumed as a complete line. */
static char pending[LINE_MAX_LEN];
static size_t pending_len;

/**
 * Read one response line, without its newline.
 *
 * Responses are read a byte at a time into a buffer rather than assumed to
 * arrive whole: a large disassembly or a long backtrace can easily be split
 * across several packets, and a client that assumed otherwise would work
 * perfectly until the day the answer got big.
 *
 * @return 0 on success, -1 on EOF or error
 */
static int
read_line(int fd, char *out, size_t outlen)
{
	for (;;) {
		char *nl = memchr(pending, '\n', pending_len);
		int n;

		if (nl != NULL) {
			size_t len = (size_t) (nl - pending);
			size_t copy = (len < outlen - 1) ? len : outlen - 1;

			memcpy(out, pending, copy);
			out[copy] = '\0';

			pending_len -= len + 1;
			memmove(pending, nl + 1, pending_len);
			return 0;
		}

		if (pending_len >= sizeof(pending)) {
			fprintf(stderr, "%s: response line too long\n", progname);
			pending_len = 0;
			return -1;
		}

		n = (int) recv(fd, pending + pending_len,
		    (int) (sizeof(pending) - pending_len), 0);
		if (n <= 0) {
			return -1;
		}
		pending_len += (size_t) n;
	}
}

/**
 * Send one command and print its reply.
 *
 * @return 0 if the emulator reported success, 1 if it reported an error, -1 on
 *         a connection failure
 */
static int
do_command(int fd, const char *command)
{
	char response[LINE_MAX_LEN];

	if (send_line(fd, command) < 0) {
		fprintf(stderr, "%s: connection closed\n", progname);
		return -1;
	}
	if (read_line(fd, response, sizeof(response)) < 0) {
		fprintf(stderr, "%s: connection closed\n", progname);
		return -1;
	}

	puts(response);
	fflush(stdout);

	/* The protocol puts "ok" first in every response, so this is a reliable
	   test without parsing the JSON - which is deliberate, since the point
	   of this tool is to show what the emulator actually said. */
	return strstr(response, "\"ok\":true") != NULL ? 0 : 1;
}

/**
 * Read commands from stdin until it ends.
 */
static int
interactive(int fd)
{
	char line[1024];
	int last = 0;

	fprintf(stderr,
	    "rpcemu-debug: connected. One command per line, 'help' lists them, "
	    "Ctrl-D to quit.\n");

	for (;;) {
		char *p;

		fputs("(rpcemu) ", stderr);
		fflush(stderr);

		if (fgets(line, sizeof(line), stdin) == NULL) {
			fputc('\n', stderr);
			break;
		}

		p = strpbrk(line, "\r\n");
		if (p != NULL) {
			*p = '\0';
		}

		/* Blank lines are for spacing the transcript, not requests */
		p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			continue;
		}

		if (strcmp(p, "quit") == 0 || strcmp(p, "exit") == 0) {
			break;
		}

		last = do_command(fd, p);
		if (last < 0) {
			return 1;
		}
	}

	return last > 0 ? 1 : 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "Drive the emulated ARM over RPCEmu's debugger socket.\n"
	    "\n"
	    "Usage:\n"
	    "  rpcemu-debug [options] <command...>   one-shot\n"
	    "  rpcemu-debug [options]                interactive\n"
	    "\n"
	    "Options:\n"
	    "  --socket PATH     AF_UNIX socket to connect to\n"
	    "  --tcp host:port   connect over TCP instead\n"
	    "  --help            this message\n"
	    "\n"
	    "The socket defaults to $RPCEMU_DEBUG_SOCKET, then to\n"
	    "$RPCEMU_DATADIR/rpcemu-debug.sock.\n"
	    "\n"
	    "Examples:\n"
	    "  rpcemu-debug pause\n"
	    "  rpcemu-debug bt\n"
	    "  rpcemu-debug dis 8000 20\n"
	    "  rpcemu-debug bp add 8000 if r0 == 0\n"
	    "  rpcemu-debug step over\n"
	    "\n"
	    "Exits 0 if the emulator reported success, 1 if it reported an error,\n"
	    "2 if it could not be reached. See docs/debugcmd.md.\n");
}

int
main(int argc, char *argv[])
{
	const char *sock_path = NULL;
	const char *tcp_target = NULL;
	int fd;
	int i;
	int first_command = argc;
	int result;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			sock_path = argv[++i];
		} else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
			tcp_target = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		} else if (strcmp(argv[i], "--") == 0) {
			first_command = i + 1;
			break;
		} else {
			first_command = i;
			break;
		}
	}

#ifdef _WIN32
	{
		WSADATA wsa;

		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			fprintf(stderr, "%s: cannot start Winsock\n", progname);
			return 2;
		}
	}
#endif

	if (tcp_target == NULL && sock_path == NULL) {
		const char *env = getenv("RPCEMU_DEBUG_SOCKET");

		/* A bare port number, or host:port, means TCP - the same
		   spelling the emulator's debug_socket setting accepts, so one
		   value can be shared between the two. */
		if (env != NULL && env[0] != '\0') {
			if (env[0] != '/' && strchr(env, '/') == NULL) {
				tcp_target = env;
			} else {
				sock_path = env;
			}
		}
	}

	if (tcp_target != NULL) {
		fd = connect_tcp(tcp_target);
	} else {
#ifdef _WIN32
		fd = connect_tcp(sock_path ? sock_path : RPCEMU_DEBUG_DEFAULT_TCP);
#else
		char buf[512];

		if (sock_path == NULL) {
			sock_path = default_socket_path(buf, sizeof(buf));
		}
		fd = connect_unix(sock_path);
#endif
	}

	if (fd < 0) {
		fprintf(stderr,
		    "%s: is the emulator running with debug_enabled=1?\n", progname);
		return 2;
	}

	if (first_command < argc) {
		/* Join the remaining arguments into one request line, so a
		   condition can be written without quoting:
		     rpcemu-debug bp add 8000 if r0 == 0 */
		char command[1024];
		size_t n = 0;

		command[0] = '\0';
		for (i = first_command; i < argc; i++) {
			int written = snprintf(command + n, sizeof(command) - n,
			    "%s%s", (i > first_command) ? " " : "", argv[i]);

			if (written < 0 || (size_t) written >= sizeof(command) - n) {
				fprintf(stderr, "%s: command too long\n", progname);
				closesocket(fd);
				return 2;
			}
			n += (size_t) written;
		}
		result = do_command(fd, command);
		result = (result < 0) ? 2 : result;
	} else {
		result = interactive(fd);
	}

	closesocket(fd);
#ifdef _WIN32
	WSACleanup();
#endif
	return result;
}
