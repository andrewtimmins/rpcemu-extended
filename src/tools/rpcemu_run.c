/*
  RPCEmu - An Acorn system emulator

  rpcemu-run / rpcemu-shell - host-side client for the HostCmd channel.

  Connects to the emulator's HostCmd socket and drives the guest RISC OS
  command line:
    rpcemu-run   [--socket PATH | --tcp host:port] -- <command...>
    rpcemu-shell [--socket PATH | --tcp host:port]

  One-shot mode (rpcemu-run) sends one command, prints its output and exits
  with the guest's return code. Interactive mode (rpcemu-shell) is a simple
  line shell: type a command, see its output, repeat.

  Wire protocol: send one '\n'-terminated command line; read length-prefixed
  frames [type:1][len:u32 BE][payload]: 'O' output -> stdout, 'X' notice ->
  stderr, 'D' done (payload = 4-byte BE return code) -> ends the command.

  Copyright (C) 2025-2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
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
/* Matches HOSTCMD_DEFAULT_TCP_PORT in hostcmd.c (Windows has no AF_UNIX). */
#define RPCEMU_RUN_DEFAULT_TCP "127.0.0.1:15590"
#endif

#ifndef _WIN32
static const char *
default_socket_path(char *buf, size_t buflen)
{
	const char *datadir = getenv("RPCEMU_DATADIR");

	if (datadir != NULL && datadir[0] != '\0') {
		size_t n = strlen(datadir);
		const char *sep = (n > 0 && datadir[n - 1] == '/') ? "" : "/";

		snprintf(buf, buflen, "%s%shostcmd.sock", datadir, sep);
	} else {
		snprintf(buf, buflen, "hostcmd.sock");
	}
	return buf;
}
#endif /* !_WIN32 */

#ifndef _WIN32
static int
connect_unix(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "rpcemu-run: socket path too long: %s\n", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("rpcemu-run: socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "rpcemu-run: cannot connect to %s: %s\n",
		    path, strerror(errno));
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
		fprintf(stderr, "rpcemu-run: cannot resolve %s:%s\n", host, port);
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
		fprintf(stderr, "rpcemu-run: cannot connect to %s:%s\n", host, port);
	}
	return fd;
}

/* Read exactly n bytes; returns 0 on success, -1 on EOF/error. */
static int
read_exact(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n > 0) {
		ssize_t r = recv(fd, (char *) p, (int) n, 0);

		if (r <= 0) {
			return -1;
		}
		p += r;
		n -= (size_t) r;
	}
	return 0;
}

/*
 * Send a command line and stream the response until the 'D' frame.
 * Returns the guest return code, or -1 on a connection error.
 */
static int
run_one(int fd, const char *cmdline)
{
	size_t clen = strlen(cmdline);
	char *line = malloc(clen + 2);

	if (line == NULL) {
		return -1;
	}
	memcpy(line, cmdline, clen);
	line[clen] = '\n';
	line[clen + 1] = '\0';
	if (send(fd, line, (int) (clen + 1), 0) != (ssize_t) (clen + 1)) {
		free(line);
		fprintf(stderr, "rpcemu-run: write failed: %s\n", strerror(errno));
		return -1;
	}
	free(line);

	for (;;) {
		uint8_t hdr[5];
		uint32_t len;

		if (read_exact(fd, hdr, 5) != 0) {
			fprintf(stderr, "rpcemu-run: connection closed\n");
			return -1;
		}
		len = ((uint32_t) hdr[1] << 24) | ((uint32_t) hdr[2] << 16) |
		      ((uint32_t) hdr[3] << 8) | hdr[4];

		if (hdr[0] == 'D') {
			uint8_t rc[4];

			if (len != 4 || read_exact(fd, rc, 4) != 0) {
				return -1;
			}
			return (int) (((uint32_t) rc[0] << 24) | ((uint32_t) rc[1] << 16) |
			              ((uint32_t) rc[2] << 8) | rc[3]);
		}

		/* 'O' -> stdout, 'X' (and anything else) -> stderr. */
		{
			FILE *out = (hdr[0] == 'O') ? stdout : stderr;
			uint8_t buf[4096];

			while (len > 0) {
				size_t chunk = (len < sizeof(buf)) ? len : sizeof(buf);

				if (read_exact(fd, buf, chunk) != 0) {
					return -1;
				}
				fwrite(buf, 1, chunk, out);
				len -= (uint32_t) chunk;
			}
			fflush(out);
		}
	}
}

static void
usage(const char *argv0)
{
	fprintf(stderr,
	    "Usage:\n"
	    "  %s [--socket PATH | --tcp host:port] -- <command...>   (one-shot)\n"
	    "  rpcemu-shell [--socket PATH | --tcp host:port]         (interactive)\n"
	    "\n"
	    "Drives the guest RISC OS command line over the emulator's HostCmd\n"
	    "socket. Default socket: $RPCEMU_DATADIR/hostcmd.sock (or ./hostcmd.sock).\n",
	    argv0);
}

int
main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/');
	int interactive;
	const char *sock_path = NULL;
	const char *tcp = NULL;
	int i;
	int fd;

#ifdef _WIN32
	{
		WSADATA wsadata;

		if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
			fprintf(stderr, "rpcemu-run: WSAStartup failed\n");
			return 1;
		}
	}
	/* argv[0] may use backslashes on Windows. */
	{
		const char *bslash = strrchr(argv[0], '\\');

		if (bslash != NULL && (base == NULL || bslash > base)) {
			base = bslash;
		}
	}
#endif

	base = (base != NULL) ? base + 1 : argv[0];
	interactive = (strstr(base, "shell") != NULL);

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			sock_path = argv[++i];
		} else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
			tcp = argv[++i];
		} else if (strcmp(argv[i], "--shell") == 0) {
			interactive = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "rpcemu-run: unknown option %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		} else {
			break;	/* first non-option: start of the command */
		}
	}

	if (tcp != NULL) {
		fd = connect_tcp(tcp);
#ifdef _WIN32
	} else {
		/* No AF_UNIX on Windows: default to the HostCmd TCP loopback port. */
		if (sock_path != NULL) {
			fprintf(stderr, "rpcemu-run: --socket is unsupported on Windows; "
			    "use --tcp host:port (defaulting to %s)\n",
			    RPCEMU_RUN_DEFAULT_TCP);
		}
		fd = connect_tcp(RPCEMU_RUN_DEFAULT_TCP);
	}
#else
	} else {
		char pathbuf[512];
		const char *path = sock_path ? sock_path
		                             : default_socket_path(pathbuf, sizeof(pathbuf));

		fd = connect_unix(path);
	}
#endif
	if (fd < 0) {
		return 1;
	}

	if (!interactive && i < argc) {
		/* One-shot: join the remaining args into a single command line. */
		char cmd[1024];
		size_t off = 0;
		int rc;

		cmd[0] = '\0';
		for (; i < argc; i++) {
			int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s",
			    (off > 0) ? " " : "", argv[i]);

			if (n < 0 || (size_t) n >= sizeof(cmd) - off) {
				fprintf(stderr, "rpcemu-run: command too long\n");
				closesocket(fd);
				return 2;
			}
			off += (size_t) n;
		}
		rc = run_one(fd, cmd);
		closesocket(fd);
		return (rc < 0) ? 1 : rc;
	}

	/* Interactive line shell. */
	{
		char line[1024];

		/* End-of-file is Ctrl-D on Unix but Ctrl-Z (then Return) on Windows,
		   where Ctrl-D is not EOF at all - it just enters ASCII 4, which would
		   be sent to the guest as a command. Name the key that works here. */
#ifdef _WIN32
		fprintf(stderr, "rpcemu-shell: connected. Ctrl-Z then Return to exit.\n");
#else
		fprintf(stderr, "rpcemu-shell: connected. Ctrl-D to exit.\n");
#endif
		for (;;) {
			int rc;

			fputs("* ", stdout);
			fflush(stdout);
			if (fgets(line, sizeof(line), stdin) == NULL) {
				break;	/* EOF */
			}
			line[strcspn(line, "\n")] = '\0';
			if (line[0] == '\0') {
				continue;
			}
			rc = run_one(fd, line);
			if (rc < 0) {
				break;
			}
			if (rc != 0) {
				fprintf(stderr, "[return code %d]\n", rc);
			}
		}
		closesocket(fd);
	}
	return 0;
}
