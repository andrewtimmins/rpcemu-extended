/*
 * A HostCmd command that nothing is there to collect must be answered.
 *
 * WHY THIS EXISTS. A command from a client sits in cmd_pending until the guest
 * module polls for it. If no module is running - the machine is still booting,
 * its !Boot never loads one, or its HostFS disc is empty - that poll never
 * comes. The command used to wait there in silence, and the client wore out its
 * own timeout with no idea whether the command had run, had failed, or was
 * still going. "The tool just hangs" is the least useful failure a tool has.
 *
 * That is the ordinary state of a machine that has not finished booting, not an
 * exotic one, so the guest's silence is turned into an answer here: a return
 * code and a reason, in about a second rather than never.
 *
 * The two waits are deliberately different lengths and that difference is the
 * interesting part, so it is asserted rather than assumed. "Never announced
 * itself" is unambiguous - nothing is listening - and gets the short limit.
 * "Announced itself and has since gone quiet" is not: the guest module polls
 * from a ticker, and a machine busy with a redraw or sitting in a modal error
 * box legitimately stops polling for a while. Holding that to the same second
 * would report a working gateway as broken.
 *
 * No emulator runs here - no thread, no ROM. hostcmd_poll() is called from this
 * test in place of the emulator thread, and the guest half is imitated by
 * calling the SWI handler, which is the only thing that tells the host side a
 * module exists.
 *
 * TRANSPORT. As for the debugger: AF_UNIX where it is useful, TCP loopback on
 * Windows, which is what hostcmd.c itself does there. What is being tested is
 * unaffected either way.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#endif

#include "socket-compat.h"
#ifndef _WIN32
#include <sys/un.h>
#endif

#include "rpcemu.h"
#include "hostcmd.h"

/* Frame types on the wire, from the protocol in hostcmd.c. */
#define FRAME_OUTPUT	'O'
#define FRAME_DONE	'D'
#define FRAME_NOTICE	'X'

static int failures;

static void
check(int cond, const char *what)
{
	printf("%s: %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) {
		failures++;
	}
}

static void
sleep_ms(int ms)
{
#ifdef _WIN32
	Sleep((DWORD) ms);
#else
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long) (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
#endif
}

static int64_t
now_ms(void)
{
#ifdef _WIN32
	return (int64_t) GetTickCount64();
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ---- the guest half --------------------------------------------------- */

/*
 * Imitate the guest module polling for work. This is the only thing that tells
 * the host side a module exists, so a test that never calls it is a machine
 * whose module never loaded - which is the case being tested.
 */
static void
guest_polls(void)
{
	/* ARMul_State holds a pointer to the register file, not the registers
	   themselves, so the array has to be supplied. */
	uint32_t regs[16];
	ARMul_State state;

	memset(regs, 0, sizeof(regs));
	state.Reg = regs;

	regs[9] = 0;	/* HC_OP_POLL */
	regs[0] = 0;	/* no buffer, so nothing is collected: only the poll
			   itself matters, which is what proves the module is
			   alive */
	regs[1] = 0;
	hostcmd(&state);
}

/* ---- the client half -------------------------------------------------- */

static void
set_nonblock(int fd)
{
#ifdef _WIN32
	u_long on = 1;

	ioctlsocket((SOCKET) fd, FIONBIO, &on);
#else
	int fl = fcntl(fd, F_GETFL, 0);

	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Non-blocking, because the loop below drives the server between reads: a
   blocking recv() would stop hostcmd_poll() being called and deadlock the
   very thing under test. */
static int
client_connect(const char *spec)
{
	int fd;

#ifdef _WIN32
	struct sockaddr_in sa;
	int port = atoi(spec);

	fd = (int) socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short) port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
		closesocket(fd);
		return -1;
	}
#else
	struct sockaddr_un sa;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, spec, sizeof(sa.sun_path) - 1);
	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
#endif
	set_nonblock(fd);
	return fd;
}

/*
 * Drive the server and collect frames until a DONE arrives or the deadline
 * passes. Returns the milliseconds waited, or -1 if no DONE came.
 *
 * guest_alive: call the guest's poll each time round, so the module looks like
 * one that is running but has not taken the command.
 */
static int64_t
wait_for_done(int fd, int guest_alive, int64_t limit_ms, char *notice,
              size_t notice_size)
{
	const int64_t start = now_ms();
	unsigned char buf[4096];
	size_t have = 0;

	notice[0] = '\0';

	while (now_ms() - start < limit_ms) {
		int n;

		hostcmd_poll();
		if (guest_alive) {
			guest_polls();
		}

		n = (int) recv(fd, (char *) buf + have, (int) (sizeof(buf) - have),
		               0);
		if (n > 0) {
			have += (size_t) n;
		}

		/* Frames are [type:1][len:u32 BE][payload]. */
		{
			size_t off = 0;

			while (have - off >= 5) {
				unsigned char type = buf[off];
				uint32_t len = ((uint32_t) buf[off + 1] << 24)
				             | ((uint32_t) buf[off + 2] << 16)
				             | ((uint32_t) buf[off + 3] << 8)
				             |  (uint32_t) buf[off + 4];

				if (have - off - 5 < len) {
					break;	/* incomplete */
				}
				if (type == FRAME_NOTICE && len > 0) {
					size_t copy = len < notice_size - 1
					            ? len : notice_size - 1;

					memcpy(notice, buf + off + 5, copy);
					notice[copy] = '\0';
				}
				if (type == FRAME_DONE) {
					return now_ms() - start;
				}
				off += 5 + len;
			}
			if (off > 0) {
				memmove(buf, buf + off, have - off);
				have -= off;
			}
		}

		sleep_ms(5);
	}

	return -1;
}

static void
client_send(int fd, const char *line)
{
	send(fd, line, (int) strlen(line), 0);
}

static void
client_close(int fd)
{
#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
}

int
main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : ".";
	char spec[512];
	int fd;
	int64_t waited;
	char notice[512];

	setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
	{
		WSADATA wsa;

		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
	/* A port unlikely to collide with anything else on a runner. */
	snprintf(spec, sizeof(spec), "15597");
#else
	snprintf(spec, sizeof(spec), "%s/test_hostcmd.sock", dir);
	remove(spec);
#endif

	memset(&config, 0, sizeof(config));
	config.hostcmd_enabled = 1;
	snprintf(config.hostcmd_socket, sizeof(config.hostcmd_socket), "%s", spec);

	hostcmd_init();

	/* ---- the guest module never loaded ------------------------------- */

	fd = client_connect(spec);
	check(fd >= 0, "a client can connect to the command socket");
	if (fd < 0) {
		return 1;
	}

	client_send(fd, "Cat\n");

	/* Generous limit: what is asserted is that an answer comes at all, and
	   roughly when, not the exact millisecond. */
	waited = wait_for_done(fd, 0, 5000, notice, sizeof(notice));

	check(waited >= 0,
	      "a command with no guest module is answered rather than left waiting");
	if (waited >= 0) {
		printf("    (answered after %d ms: %s)\n", (int) waited, notice);
		check(waited < 3000, "and answered promptly, not after a long wait");
		check(strstr(notice, "guest module") != NULL,
		      "with a reason naming the guest module");
	}

	client_close(fd);

	/* ---- the module is running, and is merely slow -------------------- */

	/*
	 * The same situation except that the guest is polling. It never takes
	 * the command (the poll above passes no buffer), so this is a module
	 * that is alive but not collecting - which must NOT be given up on at
	 * the short limit, or a busy desktop gets reported as a broken gateway.
	 */
	fd = client_connect(spec);
	check(fd >= 0, "a second client can connect");
	if (fd < 0) {
		return 1;
	}

	client_send(fd, "Cat\n");
	waited = wait_for_done(fd, 1, 3000, notice, sizeof(notice));

	check(waited < 0,
	      "a command is NOT given up on while the guest module is still polling");
	if (waited >= 0) {
		printf("    (gave up after %d ms: %s)\n", (int) waited, notice);
	}

	client_close(fd);
	hostcmd_close();

#ifndef _WIN32
	remove(spec);
#endif

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
