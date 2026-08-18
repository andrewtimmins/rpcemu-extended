/*
  RPCEmu - An Acorn system emulator

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

/*
 * rpcemu-netcap - the network frames a running machine sends and receives.
 *
 * The counterpart of rpcemu-run, which drives the guest's command line, and
 * rpcemu-debug, which drives the processor under it. This one watches the
 * wire. Like both of those it is a small socket client and does not link the
 * emulator.
 *
 *   rpcemu-netcap --status
 *   rpcemu-netcap --start /tmp/rpc.pcap        begin a capture file
 *   rpcemu-netcap --follow                     decoded frames, as they happen
 *   rpcemu-netcap --pcap - | wireshark -k -i - live, in real Wireshark
 *
 * ★ That last line is the reason the --pcap mode exists.
 *
 * Wireshark reads a pcap stream from a pipe, so handing it one gives every
 * dissector it has, live, on a machine that has no network interface of its
 * own to point it at. Nothing here needs to be as good as Wireshark; it needs
 * to get out of Wireshark's way.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define close_socket close
#endif

/* dirent is used on both platforms: finding which machine recorded an endpoint
   means reading the machine directories, and on Windows that is the only way
   to find a channel at all. mingw-w64 provides both of these. */
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
/* _setmode and _O_BINARY, for handing a pcap stream to stdout without the
   runtime translating line endings inside it. Missing these was a hard error
   under -Werror=implicit-function-declaration, so this tool had never been
   compiled for Windows at all. */
#include <fcntl.h>
#include <io.h>
#endif

#include "machine_lock.h"

#define DEFAULT_TCP_PORT	15592

/* machine_lock.c logs through the emulator's rpclog(); this tool has no log,
   and the only thing it uses from that file is the endpoint reader. */
void rpclog(const char *format, ...)
{
	(void) format;
}

static void
usage(void)
{
	fprintf(stderr,
	    "rpcemu-netcap - watch the network of a running RPCEmu machine\n"
	    "\n"
	    "Usage:\n"
	    "  rpcemu-netcap [where] [what]\n"
	    "\n"
	    "Where to connect (the running machine is found automatically if you\n"
	    "say nothing, and --machine picks one when several are running):\n"
	    "  --socket PATH        AF_UNIX socket to connect to\n"
	    "  --tcp HOST:PORT      TCP instead (the only option on Windows)\n"
	    "  --machine NAME       find the socket of this running machine\n"
	    "\n"
	    "What to do (--follow if you say nothing):\n"
	    "  --status             what is being captured, and how much\n"
	    "  --start PATH [--max-bytes N]\n"
	    "                       have the emulator write a pcap file\n"
	    "  --stop               close it\n"
	    "  --clear              forget the frames held in memory\n"
	    "  --tail [N]           the last N frames, decoded (default 50)\n"
	    "  --follow             decoded frames as they happen, until Ctrl-C\n"
	    "  --pcap FILE          write pcap instead; FILE of - means stdout\n"
	    "  --raw                with --follow, print the JSON rather than a table\n"
	    "\n"
	    "Examples:\n"
	    "  rpcemu-netcap --pcap - | wireshark -k -i -\n"
	    "  rpcemu-netcap --start /tmp/rpc.pcap --max-bytes 104857600\n"
	    "  rpcemu-netcap --tail 20\n");
}

/* ---- finding a running machine ----------------------------------------- */

/*
 * A machine records the socket it actually bound in its lock file, which is
 * the only way to find one whose configuration named a path of its own rather
 * than taking the default. Same approach as rpcemu-debug.
 */
static int
find_socket(const char *machine, char *out, size_t out_len)
{
	/*
	 * ★ On Windows too.
	 *
	 * This returned nothing there, with the reason "Windows uses TCP; there is
	 * nothing to discover" - which held only while every machine used one fixed
	 * port. A machine now moves up to a free port when another already holds the
	 * one its configuration names, so the port is exactly what has to be
	 * discovered, and what it recorded in its lock file is the only place to
	 * find it. Nothing below is POSIX-only: reading the machine directories and
	 * their lock files works the same either way.
	 */
	const char *datadir = getenv("RPCEMU_DATADIR");
	char machines[600];
	char home_dir[600];
	DIR *dir;
	struct dirent *ent;
	char found[8][700];
	char names[8][128];
	int count = 0;

	if (datadir == NULL || datadir[0] == '\0') {
		const char *home = getenv("HOME");

		if (home == NULL) {
			return 0;
		}
		snprintf(home_dir, sizeof(home_dir), "%s/RPCEmu", home);
		datadir = home_dir;
	}
	snprintf(machines, sizeof(machines), "%.500s/machines", datadir);

	if (machine != NULL && machine[0] != '\0') {
		char d[700];

		snprintf(d, sizeof(d), "%.600s/%.64s/", machines, machine);
		return machine_lock_read_netcap_endpoint(d, out, out_len) &&
		    out[0] != '\0';
	}

	dir = opendir(machines);
	if (dir == NULL) {
		return 0;
	}
	while ((ent = readdir(dir)) != NULL && count < 8) {
		char d[700];
		char recorded[700];
		struct stat st;

		if (ent->d_name[0] == '.') {
			continue;
		}
		snprintf(d, sizeof(d), "%.600s/%.64s/", machines, ent->d_name);
		/* Either form of endpoint; a TCP one has no file to stat, and a path
		   that is gone means the machine is not really there. */
		if (machine_lock_read_netcap_endpoint(d, recorded, sizeof(recorded)) &&
		    recorded[0] != '\0' &&
		    (!machine_lock_endpoint_is_path(recorded) || stat(recorded, &st) == 0))
		{
			snprintf(found[count], sizeof(found[0]), "%s", recorded);
			snprintf(names[count], sizeof(names[0]), "%.127s", ent->d_name);
			count++;
		}
	}
	closedir(dir);

	if (count == 1) {
		snprintf(out, out_len, "%s", found[0]);
		return 1;
	}
	if (count > 1) {
		int i;

		/* Choosing one would be choosing wrong half the time. */
		fprintf(stderr, "rpcemu-netcap: several machines are running; say "
		    "--machine NAME:\n");
		for (i = 0; i < count; i++) {
			fprintf(stderr, "  %s\n", names[i]);
		}
	}
	return 0;
}

/* ---- connecting -------------------------------------------------------- */

static int
connect_unix(const char *path)
{
#ifdef _WIN32
	(void) path;
	fprintf(stderr, "rpcemu-netcap: this platform has no AF_UNIX sockets; "
	    "use --tcp\n");
	return -1;
#else
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "rpcemu-netcap: socket path too long: %s\n", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* The length was checked above; snprintf rather than strncpy so the
	   terminator is not something to reason about. */
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "rpcemu-netcap: could not connect to %s: %s\n", path,
		    strerror(errno));
		close_socket(fd);
		return -1;
	}
	return fd;
#endif
}

static int
connect_tcp(const char *hostport)
{
	struct sockaddr_in addr;
	char host[256];
	const char *colon;
	int port = DEFAULT_TCP_PORT;
	int fd;

	colon = strrchr(hostport, ':');
	if (colon != NULL) {
		const size_t n = (size_t) (colon - hostport);

		if (n >= sizeof(host)) {
			fprintf(stderr, "rpcemu-netcap: host name too long\n");
			return -1;
		}
		memcpy(host, hostport, n);
		host[n] = '\0';
		port = atoi(colon + 1);
	} else {
		snprintf(host, sizeof(host), "%s", hostport);
	}
	if (host[0] == '\0') {
		snprintf(host, sizeof(host), "127.0.0.1");
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short) port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "rpcemu-netcap: '%s' is not an address\n", host);
		close_socket(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "rpcemu-netcap: could not connect to %s:%d\n", host,
		    port);
		close_socket(fd);
		return -1;
	}
	return fd;
}

/* ---- talking ----------------------------------------------------------- */

static int
send_line(int fd, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	size_t len;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	len = strlen(buf);
	buf[len++] = '\n';
	return (send(fd, buf, (int) len, 0) == (int) len) ? 0 : -1;
}

/**
 * Read one newline-terminated line.
 *
 * @return its length, 0 at end of stream, -1 on error
 */
static int
read_line(int fd, char *out, size_t max)
{
	size_t used = 0;

	while (used + 1 < max) {
		char c;
		const int n = (int) recv(fd, &c, 1, 0);

		if (n == 0) {
			break;
		}
		if (n < 0) {
			return -1;
		}
		if (c == '\n') {
			out[used] = '\0';
			return (int) used;
		}
		out[used++] = c;
	}
	out[used] = '\0';
	return (used > 0) ? (int) used : 0;
}

/* ---- a very small JSON reader ------------------------------------------ */

/*
 * Only enough to pull known keys out of the one-line objects the socket
 * sends. A parser is not wanted here: the shape is fixed, this is a client for
 * one server, and a dependency for it would be out of all proportion.
 */
static int
json_str(const char *json, const char *key, char *out, size_t max)
{
	char pattern[64];
	const char *p;
	size_t n = 0;

	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	p = strstr(json, pattern);
	if (p == NULL) {
		out[0] = '\0';
		return 0;
	}
	p += strlen(pattern);
	while (*p != '\0' && *p != '"' && n + 1 < max) {
		if (*p == '\\' && p[1] != '\0') {
			p++;
		}
		out[n++] = *p++;
	}
	out[n] = '\0';
	return 1;
}

static long long
json_num(const char *json, const char *key)
{
	char pattern[64];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	p = strstr(json, pattern);
	if (p == NULL) {
		return -1;
	}
	return strtoll(p + strlen(pattern), NULL, 10);
}

/* ---- output ------------------------------------------------------------ */

static void
print_header(void)
{
	printf("%-8s %-15s %-3s %-10s %-26s %-26s %s\n",
	    "No.", "Time", "Dir", "Protocol", "Source", "Destination", "Info");
	fflush(stdout);
}

static void
print_frame(const char *json)
{
	char dir[8], proto[32], src[128], dst[128], info[256];
	const long long serial = json_num(json, "serial");
	const long long sec = json_num(json, "sec");
	const long long usec = json_num(json, "usec");

	json_str(json, "dir", dir, sizeof(dir));
	json_str(json, "proto", proto, sizeof(proto));
	json_str(json, "src", src, sizeof(src));
	json_str(json, "dst", dst, sizeof(dst));
	json_str(json, "info", info, sizeof(info));

	{
		/*
		 * Shown in local time, because the first thing anybody does with a
		 * capture is line it up against something else that happened -
		 * a log line, a click - and those are in local time too.
		 */
		const time_t when = (time_t) sec;
		struct tm tmv;
		char clock[16];

#ifdef _WIN32
		localtime_s(&tmv, &when);
#else
		localtime_r(&when, &tmv);
#endif
		snprintf(clock, sizeof(clock), "%02d:%02d:%02d.%06lld",
		    tmv.tm_hour, tmv.tm_min, tmv.tm_sec, usec);
		printf("%-8lld %-15s %-3s %-10s %-26.26s %-26.26s %s\n",
		    serial, clock, dir, proto, src, dst, info);
	}
	fflush(stdout);
}

/* ---- modes ------------------------------------------------------------- */

static int
do_status(int fd)
{
	char line[8192];
	char file[600];

	if (send_line(fd, "status") != 0 || read_line(fd, line, sizeof(line)) <= 0) {
		fprintf(stderr, "rpcemu-netcap: no answer\n");
		return 1;
	}
	json_str(line, "file", file, sizeof(file));
	printf("Frames seen      %lld (%lld sent, %lld received)\n",
	    json_num(line, "frames"), json_num(line, "tx"), json_num(line, "rx"));
	printf("Bytes            %lld\n", json_num(line, "bytes"));
	printf("Capture file     %s\n", (file[0] != '\0') ? file : "none");
	if (file[0] != '\0') {
		printf("  written        %lld bytes\n", json_num(line, "file_bytes"));
	}
	if (strstr(line, "\"file_stopped_full\":true") != NULL) {
		printf("  NOTE           it stopped itself on reaching its size limit\n");
	}
	printf("Frames in memory %s\n",
	    (strstr(line, "\"ring\":true") != NULL) ? "yes" : "no");
	if (json_num(line, "ring_dropped") > 0) {
		printf("  overwritten    %lld before anything read them\n",
		    json_num(line, "ring_dropped"));
	}
	return 0;
}

static int
do_simple(int fd, const char *cmd)
{
	char line[8192];

	if (send_line(fd, "%s", cmd) != 0 || read_line(fd, line, sizeof(line)) <= 0) {
		fprintf(stderr, "rpcemu-netcap: no answer\n");
		return 1;
	}
	if (strstr(line, "\"ok\":true") == NULL) {
		char err[256];

		json_str(line, "error", err, sizeof(err));
		fprintf(stderr, "rpcemu-netcap: %s\n",
		    (err[0] != '\0') ? err : "refused");
		return 1;
	}
	return 0;
}

static int
do_tail(int fd, const char *count, int raw)
{
	char line[65536];
	int shown = 0;

	if (send_line(fd, "tail %s", (count != NULL) ? count : "50") != 0) {
		return 1;
	}
	for (;;) {
		const int n = read_line(fd, line, sizeof(line));

		if (n <= 0 || strcmp(line, "end") == 0) {
			break;
		}
		if (strstr(line, "\"ok\":false") != NULL) {
			char err[256];

			json_str(line, "error", err, sizeof(err));
			fprintf(stderr, "rpcemu-netcap: %s\n", err);
			return 1;
		}
		if (raw) {
			printf("%s\n", line);
		} else {
			if (shown == 0) {
				print_header();
			}
			print_frame(line);
		}
		shown++;
	}
	if (shown == 0) {
		fprintf(stderr, "rpcemu-netcap: no frames held. Frames are only kept "
		    "once something asks for them; try --follow, or run --tail again "
		    "in a moment.\n");
	}
	return 0;
}

static int
do_follow(int fd, int raw)
{
	char line[65536];
	int shown = 0;

	if (send_line(fd, "follow") != 0) {
		return 1;
	}
	for (;;) {
		const int n = read_line(fd, line, sizeof(line));

		if (n <= 0) {
			break;
		}
		if (strstr(line, "\"following\":true") != NULL) {
			continue;
		}
		if (strstr(line, "\"missed\":") != NULL) {
			fprintf(stderr, "rpcemu-netcap: missed %lld frames (this client "
			    "is reading slower than the machine is sending)\n",
			    json_num(line, "missed"));
			continue;
		}
		if (raw) {
			printf("%s\n", line);
			fflush(stdout);
		} else {
			if (shown == 0) {
				print_header();
			}
			print_frame(line);
		}
		shown++;
	}
	return 0;
}

static int
do_pcap(int fd, const char *path)
{
	FILE *out;
	char buf[65536];
	int n;

	if (strcmp(path, "-") == 0) {
		out = stdout;
#ifdef _WIN32
		_setmode(_fileno(stdout), _O_BINARY);
#endif
	} else {
		out = fopen(path, "wb");
		if (out == NULL) {
			fprintf(stderr, "rpcemu-netcap: could not write %s: %s\n", path,
			    strerror(errno));
			return 1;
		}
	}

	if (send_line(fd, "pcap") != 0) {
		return 1;
	}
	/*
	 * Flushed per read rather than per buffer: the whole point is that
	 * something at the other end of the pipe is displaying frames as they
	 * happen, and stdio would otherwise hold them back until it had 4KB.
	 */
	while ((n = (int) recv(fd, buf, sizeof(buf), 0)) > 0) {
		if (fwrite(buf, 1, (size_t) n, out) != (size_t) n) {
			break;
		}
		fflush(out);
	}
	if (out != stdout) {
		fclose(out);
	}
	return 0;
}

/* ---- main -------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	const char *sock_path = NULL;
	const char *tcp = NULL;
	const char *machine = NULL;
	const char *start_path = NULL;
	const char *pcap_path = NULL;
	const char *tail_count = NULL;
	const char *max_bytes = "0";
	int want_status = 0, want_stop = 0, want_clear = 0;
	int want_tail = 0, want_follow = 0, raw = 0;
	int fd;
	int rc = 0;
	int i;

#ifdef _WIN32
	{
		WSADATA wsa;

		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
#endif

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			sock_path = argv[++i];
		} else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
			tcp = argv[++i];
		} else if (strcmp(argv[i], "--machine") == 0 && i + 1 < argc) {
			machine = argv[++i];
		} else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
			start_path = argv[++i];
		} else if (strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc) {
			max_bytes = argv[++i];
		} else if (strcmp(argv[i], "--pcap") == 0 && i + 1 < argc) {
			pcap_path = argv[++i];
		} else if (strcmp(argv[i], "--tail") == 0) {
			want_tail = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				tail_count = argv[++i];
			}
		} else if (strcmp(argv[i], "--status") == 0) {
			want_status = 1;
		} else if (strcmp(argv[i], "--stop") == 0) {
			want_stop = 1;
		} else if (strcmp(argv[i], "--clear") == 0) {
			want_clear = 1;
		} else if (strcmp(argv[i], "--follow") == 0) {
			want_follow = 1;
		} else if (strcmp(argv[i], "--raw") == 0) {
			raw = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		} else {
			fprintf(stderr, "rpcemu-netcap: unknown option '%s'\n", argv[i]);
			usage();
			return 2;
		}
	}

	/*
	 * Nothing said about where: ask the running machines. This is the same
	 * trick rpcemu-debug uses - a machine records the sockets it is listening
	 * on in its lock file, which is the only way to find one whose
	 * configuration named a path of its own.
	 */
	if (sock_path == NULL && tcp == NULL) {
		static char found[700];

		if (find_socket(machine, found, sizeof(found))) {
			sock_path = found;
		} else {
			fprintf(stderr, "rpcemu-netcap: no running machine found. Say "
			    "--socket PATH, or --machine NAME if several are running.\n");
			return 1;
		}
	}

	/* A discovered endpoint may be a path or a host:port - see
	   machine_lock_endpoint_is_path(). */
	if (tcp != NULL) {
		fd = connect_tcp(tcp);
	} else if (machine_lock_endpoint_is_path(sock_path)) {
		fd = connect_unix(sock_path);
	} else {
		fd = connect_tcp(sock_path);
	}
	if (fd < 0) {
		return 1;
	}

	/* Actions, in the order somebody would want them to happen. */
	if (start_path != NULL) {
		char cmd[1024];

		snprintf(cmd, sizeof(cmd), "start %s %s", start_path, max_bytes);
		rc |= do_simple(fd, cmd);
		if (rc == 0) {
			printf("Capturing to %s\n", start_path);
		}
	}
	if (want_stop) {
		rc |= do_simple(fd, "stop");
	}
	if (want_clear) {
		rc |= do_simple(fd, "clear");
	}
	if (want_status) {
		rc |= do_status(fd);
	}
	if (want_tail) {
		rc |= do_tail(fd, tail_count, raw);
	}
	if (pcap_path != NULL) {
		rc |= do_pcap(fd, pcap_path);
	} else if (want_follow ||
	    (!want_status && !want_stop && !want_clear && !want_tail &&
	     start_path == NULL)) {
		rc |= do_follow(fd, raw);
	}

	close_socket(fd);
	return rc;
}
