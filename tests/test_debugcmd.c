/*
 * The debugger's wire protocol, driven over a real socket.
 *
 * WHY THIS EXISTS. Everything else in the debugger is reached through C
 * functions with types on them. This layer is a hand-written parser over
 * strtok, and it is the layer every external client actually touches - the MCP
 * server, rpcemu-debug, and anything a person writes themselves. A mistake
 * here is not caught by a compiler and is not visible from inside: the command
 * is simply understood as something other than what was sent.
 *
 * That failure has a particular shape worth naming. `bp add 8000 if r0 == 0`
 * is one line of text that has to survive being split on spaces, having its
 * options recognised in any order, and having the condition - which itself
 * contains spaces - kept whole to the end of the line. A parser that gets that
 * subtly wrong does not report an error. It sets an unconditional breakpoint,
 * or one with a condition that means something else, and the person debugging
 * believes they asked for something they did not.
 *
 * So this connects to the real socket, sends real request lines, and reads the
 * real JSON back. The checks lean on:
 *
 *   - the arguments surviving the trip, especially conditions containing
 *     spaces and operators;
 *   - malformed requests being refused with ok:false rather than accepted and
 *     quietly doing something else;
 *   - unknown verbs not being mistaken for known ones;
 *   - the responses actually being reported, since a client that cannot see
 *     the state cannot act on it.
 *
 * The emulator is not running here - no thread, no ROM. debugcmd_poll() is
 * called from this test in place of the emulator thread, which is exactly the
 * arrangement the real code uses while the CPU is paused.
 *
 * TRANSPORT. The protocol is the same everywhere; only the socket underneath it
 * differs, exactly as it does for a real client. Unix gets AF_UNIX, Windows -
 * which has no useful AF_UNIX - gets TCP on the loopback, which is what
 * debugcmd.c itself falls back to there. The tests below are the same either
 * way: what is being checked is the parser, and it never sees the difference.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket-compat.h"
#ifndef _WIN32
#include <sys/un.h>
#endif

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "debugcmd.h"

/* The lowest port tried on Windows. Probed upwards rather than fixed: a test
   that fails because something else on the machine holds a port has said
   nothing about the parser. */
#define TEST_TCP_BASE_PORT	15600
#define TEST_TCP_PORT_TRIES	20

static int failures;
static int client_fd = -1;

/**
 * Wait a millisecond between polls. usleep() is POSIX; Windows spells it Sleep().
 */
static void
settle(void)
{
#ifdef _WIN32
	Sleep(1);
#else
	usleep(1000);
#endif
}

#ifndef _WIN32
/* Where the AF_UNIX socket was bound, so it can be removed afterwards. Nothing
   to remove on Windows: a TCP listener leaves nothing behind. */
static char sock_path[512];
#endif

/**
 * Remove the socket file, where the transport has one.
 */
static void
remove_socket_file(void)
{
#ifndef _WIN32
	if (sock_path[0] != '\0') {
		unlink(sock_path);
	}
#endif
}

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/**
 * Send a request and return the response line.
 *
 * debugcmd_poll() is pumped rather than waited on: it is non-blocking, and in
 * the real emulator it is called from the same thread on every pass round the
 * main loop.
 */
static const char *
request(const char *line)
{
	static char response[65536];
	char buf[1024];
	size_t got = 0;
	int spins;

	snprintf(buf, sizeof(buf), "%s\n", line);
	if (send(client_fd, buf, strlen(buf), 0) < 0) {
		return "";
	}

	for (spins = 0; spins < 200; spins++) {
		ssize_t n;

		debugcmd_poll();

		n = recv(client_fd, response + got, sizeof(response) - got - 1,
		    MSG_DONTWAIT);
		if (n > 0) {
			got += (size_t) n;
			response[got] = '\0';
			if (memchr(response, '\n', got) != NULL) {
				*strchr(response, '\n') = '\0';
				return response;
			}
		}
		settle();
	}

	response[got] = '\0';
	return response;
}

/**
 * The response must report success and contain a given fragment.
 */
static void
ok_with(const char *line, const char *fragment)
{
	const char *r = request(line);
	char what[200];

	snprintf(what, sizeof(what), "\"%s\" -> %s", line, fragment);
	check(what, strstr(r, "\"ok\":true") != NULL && strstr(r, fragment) != NULL);
	if (strstr(r, "\"ok\":true") == NULL || strstr(r, fragment) == NULL) {
		printf("      got %.200s\n", r);
	}
}

/**
 * The request must be refused. This is the direction that matters: a bad
 * request accepted is a bad request acted on.
 */
static void
refused(const char *line)
{
	const char *r = request(line);
	char what[200];

	snprintf(what, sizeof(what), "\"%s\" is refused", line);
	check(what, strstr(r, "\"ok\":false") != NULL);
	if (strstr(r, "\"ok\":false") == NULL) {
		printf("      got %.200s\n", r);
	}
}

/* ---- tests --------------------------------------------------------------- */

static void
test_basics(void)
{
	printf("Basics\n");

	ok_with("ping", "\"paused\"");
	ok_with("status", "\"breakpoints\"");
	ok_with("regs", "\"regs\"");
	ok_with("help", "\"commands\"");

	/* An unknown verb must be refused, not quietly matched to something */
	refused("nosuchverb");
	refused("bpx add 8000");

	/* A blank line is not a request and draws no reply. What matters is
	   that it does not put the connection out of step - the next request
	   must still get its own answer, not the previous one's. */
	request("");
	ok_with("ping", "\"paused\"");
}

static void
test_breakpoints(void)
{
	printf("Breakpoints\n");

	ok_with("bp clear", "\"ok\":true");
	ok_with("bp add 8000", "\"address\":\"00008000\"");
	ok_with("status", "\"address\":\"00008000\"");

	/* A new breakpoint is armed, unconditional, and not a one-shot */
	ok_with("status", "\"enabled\":true");
	ok_with("status", "\"condition\":null");
	ok_with("status", "\"one_shot\":false");

	ok_with("bp disable 8000", "\"enabled\":false");
	ok_with("status", "\"enabled\":false");
	ok_with("bp enable 8000", "\"enabled\":true");

	/* Disabling something that is not there must say so rather than
	   silently succeeding */
	refused("bp disable 12340000");

	ok_with("bp del 8000", "\"address\":\"00008000\"");
	ok_with("bp clear", "\"ok\":true");

	refused("bp");
	refused("bp add");
	refused("bp frobnicate 8000");
}

static void
test_conditions(void)
{
	printf("Conditions on the wire\n");

	ok_with("bp clear", "\"ok\":true");

	/* The condition contains spaces and operators and must arrive whole */
	ok_with("bp add 8000 if r0 == 0", "\"address\":\"00008000\"");
	ok_with("status", "\"condition\":\"r0 == 0\"");

	/* Options in combination, with the condition last */
	ok_with("bp add 8100 once count 5 if r1 != 0x10", "\"address\":\"00008100\"");
	ok_with("status", "\"condition\":\"r1 != 0x10\"");
	ok_with("status", "\"ignore_count\":5");
	ok_with("status", "\"one_shot\":true");

	/* Order of the options before "if" must not matter */
	ok_with("bp add 8200 count 3 once if z", "\"address\":\"00008200\"");
	ok_with("status", "\"condition\":\"z\"");

	/* A condition full of things that could confuse a tokeniser */
	ok_with("bp add 8300 if [sp + 4] == 0 && r0 != 1", "\"ok\":true");
	ok_with("status", "[sp + 4] == 0 && r0 != 1");

	/* Re-adding replaces rather than duplicating */
	ok_with("bp add 8000 if r2 == 7", "\"ok\":true");
	ok_with("status", "\"condition\":\"r2 == 7\"");

	/* A malformed condition must be refused when the breakpoint is set.
	   Accepting it here means it fails silently later, at the one moment
	   the person is relying on it. */
	refused("bp add 8400 if r0 ==");
	refused("bp add 8400 if banana");
	refused("bp add 8400 if r0 == 5 junk");
	refused("bp add 8400 if");

	/* ... and having been refused, it must not have been created */
	{
		const char *r = request("status");

		check("a refused condition creates no breakpoint",
		      strstr(r, "\"00008400\"") == NULL);
	}

	refused("bp add 8500 count");
	refused("bp add 8500 wibble");

	ok_with("bp clear", "\"ok\":true");
}

static void
test_stepping_and_backtrace(void)
{
	printf("Stepping and backtrace\n");

	/* Not paused, so the run-the-machine verbs must refuse rather than
	   start something */
	refused("step over");
	refused("step out");
	refused("runto 8000");

	/* Plain stepping is always accepted, and "into" is a synonym */
	ok_with("step", "\"mode\":\"into\"");
	ok_with("step 4", "\"stepped\":4");
	ok_with("step into 3", "\"stepped\":3");

	ok_with("resume", "\"paused\":false");

	/* A backtrace always answers, even with nothing sensible to walk */
	ok_with("bt", "\"frames\"");
	ok_with("bt", "\"truncated\"");
	ok_with("bt 4", "\"level\":0");
	ok_with("backtrace", "\"frames\"");

	refused("runto");
}

static void
test_symbols(void)
{
	FILE *f;
	char path[512];
	const char *dir = getenv("RPCEMU_TEST_DIR");

	printf("Symbols\n");

	snprintf(path, sizeof(path), "%s/wire.sym", dir ? dir : ".");
	f = fopen(path, "w");
	if (f == NULL) {
		check("can write a symbol file", 0);
		return;
	}
	fputs("00008000 main\n00008100 parse\n", f);
	fclose(f);

	ok_with("sym clear", "\"count\":0");

	{
		char line[600];

		snprintf(line, sizeof(line), "sym load %s", path);
		ok_with(line, "\"count\":2");
	}

	ok_with("sym lookup 8004", "\"symbol\":\"main\"");
	ok_with("sym lookup 8004", "\"offset\":4");
	ok_with("sym find parse", "\"address\":\"00008100\"");

	/* An address outside the table is unnamed, not attributed to the
	   nearest symbol below it */
	ok_with("sym lookup 1000", "\"symbol\":null");

	/* A name is accepted wherever an address is */
	ok_with("bp add main", "\"address\":\"00008000\"");
	ok_with("status", "\"pc_symbol\"");
	ok_with("bp clear", "\"ok\":true");

	/* And a name that is not in the table is refused rather than being
	   quietly read as the address zero */
	refused("bp add nosuchsymbol");

	refused("sym find nosuchsymbol");
	refused("sym load /nonexistent/nowhere.sym");
	refused("sym load");
	refused("sym wibble");

	ok_with("sym clear", "\"count\":0");

	/* With no symbols loaded, a lookup answers rather than failing */
	ok_with("sym lookup 8004", "\"symbol\":null");
}

/*
 * The FPA's registers over the wire.
 *
 * The values are put there by executing real FPA instructions rather than by
 * reaching into fpa.c, because what is being checked is the whole path a
 * client sees: the chip's register file, the accessor, and the JSON. The
 * opcodes are hand-assembled from the operation tables in fpa.c, exactly as
 * the FPA cases in test_arm_disasm.c are.
 *
 * The extended layout is asserted rather than described. 1.0 is exponent
 * 0x3fff (bias 16383) with the integer bit set and no fraction, so a register
 * holding it reads 00003fff 80000000 00000000, and those are the words LDFE
 * and STFE move. Get the layout wrong and the value would still look right.
 */
static void
test_fpregs(void)
{
	printf("Floating point registers\n");

	resetfpa();

	/* The system ID byte the FPEmulator support code reads to decide it has a
	   chip to talk to. If this is ever not 0x81, RISC OS stops using the
	   register file these tests are about. */
	ok_with("fpregs", "\"sysid\":\"81\"");
	ok_with("fpregs", "\"fpsr\":\"81000000\"");
	ok_with("fpregs",
	    "{\"raw\":[\"00000000\",\"00000000\",\"00000000\"],\"value\":\"0\"}");

	/* MVFS F0, #1.0 */
	fpaopcode(0xee008109);
	ok_with("fpregs",
	    "{\"raw\":[\"00003fff\",\"80000000\",\"00000000\"],\"value\":\"1\"}");

	/* ADFS F1, F0, F0 - a second register, and a value that had to be computed
	   rather than loaded from the constant table. */
	fpaopcode(0xee001100);
	ok_with("fpregs",
	    "{\"raw\":[\"00004000\",\"80000000\",\"00000000\"],\"value\":\"2\"}");

	/* MNFS F0, F0: the sign is in the top bit of the first word, and a
	   negative value must not come back as its magnitude. */
	fpaopcode(0xee108100);
	ok_with("fpregs",
	    "{\"raw\":[\"80003fff\",\"80000000\",\"00000000\"],\"value\":\"-1\"}");

	/* Reading is all that is offered, deliberately: the support code holds its
	   own copy of the register file around a trapped operation. */
	refused("fpreg set 0 1");
}

/*
 * "pc" and "15" name one register and do not mean one number.
 *
 * R15 reads eight ahead of the instruction being executed, so an address and
 * the register that points at it differ by a pipeline. Every value the
 * debugger REPORTS is the address - `regs.pc`, `status.halt_pc`, the
 * disassembly, the breakpoints - while `regs.regs[15]` is the raw register.
 * Writing had only one of those meanings, the raw one, under the name that
 * everywhere else has the other: "reg set pc 5000" resumed the machine at
 * 4ff8, two instructions into whatever preceded the address asked for, and
 * reading it back returned a different number from the one just written.
 *
 * Nothing warns. The machine runs perfectly well from the wrong place.
 */
static void
test_setting_the_pc(void)
{
	printf("Setting the PC\n");

	/* A register write is refused while the machine runs, so stop it the way
	   tests/test_stepping.c does - there is no emulator thread here to stop
	   itself. */
	arm.r15_mask = 0xfffffffcu;
	arm.reg[15] = 0x8000u + 8u;
	debugger_request_pause(DebugPauseReason_User);
	debugger_instruction_hook(0x8000u, 0xe1a00000u);

	/* The address given is where the machine will resume, and reading it back
	   gives the same number. R15 itself lands eight beyond it. */
	ok_with("reg set pc 5000", "\"r15\":\"00005008\"");
	ok_with("regs", "\"pc\":\"00005000\"");

	/* The raw register still means the raw register, which is the other half
	   of what `regs` reports. */
	ok_with("reg set 15 5000", "\"reg\":15");
	ok_with("regs", "\"pc\":\"00004ff8\"");

	/*
	 * In a 26-bit mode R15 carries the flags and the mode as well as the
	 * address, and a bare write would clear them - which is a fault nobody
	 * would connect to having set the PC. Only the address bits move.
	 */
	arm.r15_mask = 0x03fffffcu;
	arm.reg[15] = 0xf000800bu;
	ok_with("reg set pc 5000", "\"r15\":\"f000500b\"");
	ok_with("reg set pc 5000", "\"value\":\"00005000\"");

	arm.r15_mask = 0xfffffffcu;
	/* And still only while stopped: a write racing the emulator thread would
	   be overwritten by the next instruction anyway. */
	debugger_resume();
	refused("reg set pc 5000");
}

static void
test_memory_and_disassembly(void)
{
	printf("Memory and disassembly\n");

	/* MOV R0, R0 at a known address, so the disassembly can be checked
	   rather than merely observed to exist */
	mem_phys_write32(0x10030000, 0xE1A00000);

	ok_with("mem 10030000 4", "\"data\"");
	ok_with("dis 10030000 1", "MOV R0, R0");
	ok_with("dis 10030000 1", "10030000");

	/* FPA, which used to disassemble as a generic coprocessor operation */
	mem_phys_write32(0x10030010, 0xEE010102);
	ok_with("dis 10030010 1", "ADFS F0, F1, F2");

	refused("dis");
	refused("mem");
	refused("mem 10030000");
}

/**
 * Start the listener and connect a client to it.
 *
 * @param dir Writable directory, for the AF_UNIX socket
 * @return    0 on success, non-zero if no connection could be made
 */
static int
connect_to_debugger(const char *dir)
{
#ifdef _WIN32
	/* No useful AF_UNIX, so debugcmd.c listens on the loopback and so do we.
	   A bare port number in debug_socket is how that is asked for. */
	int try;

	(void) dir;

	for (try = 0; try < TEST_TCP_PORT_TRIES; try++) {
		int port = TEST_TCP_BASE_PORT + try;
		struct sockaddr_in addr;

		config.debug_enabled = 1;
		snprintf(config.debug_socket, sizeof(config.debug_socket), "%d", port);
		debugcmd_init();

		client_fd = (int) socket(AF_INET, SOCK_STREAM, 0);
		if (client_fd < 0) {
			printf("FAIL: cannot create a client socket\n");
			return 1;
		}
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short) port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		debugcmd_poll();	/* let the listener come up */
		if (connect(client_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0
		    && socket_set_nonblocking(client_fd) == 0)
		{
			/*
			 * ★ Connecting is not enough: ask, and require a debugger's
			 * answer.
			 *
			 * If something else on the machine already holds this port,
			 * our own bind() failed and connect() then succeeded - against
			 * the squatter. The port looks taken and the connection looks
			 * good, and every check afterwards fails on a timeout with
			 * nothing to say why. So the listener has to identify itself
			 * before it is believed. No greeting is sent on accept, so
			 * this asks the cheapest question there is.
			 */
			debugcmd_poll();	/* accept */
			if (strstr(request("ping"), "\"ok\"") != NULL) {
				printf("  (loopback port %d)\n\n", port);
				return 0;
			}
		}

		/* Not ours, or not reachable. Take both ends down, try the next. */
		closesocket(client_fd);
		client_fd = -1;
		debugcmd_close();
	}

	printf("FAIL: no free loopback port in %d..%d\n", TEST_TCP_BASE_PORT,
	    TEST_TCP_BASE_PORT + TEST_TCP_PORT_TRIES - 1);
	return 1;
#else
	struct sockaddr_un addr;

	/* Absolute, because debugcmd reads a socket spec that does not begin
	   with '/' as a TCP port number rather than a path. */
	if (dir[0] == '/') {
		snprintf(sock_path, sizeof(sock_path), "%s/test-debugcmd.sock", dir);
	} else {
		char cwd[256];

		if (getcwd(cwd, sizeof(cwd)) == NULL) {
			printf("FAIL: cannot determine the working directory\n");
			return 1;
		}
		snprintf(sock_path, sizeof(sock_path), "%.128s/%.128s/test-debugcmd.sock",
		    cwd, dir);
	}

	/* AF_UNIX paths are limited to about a hundred bytes by the kernel, and
	   a build directory nested deeply enough will exceed that. Fall back to
	   a short path rather than failing a test that is not about paths. */
	if (strlen(sock_path) >= sizeof(addr.sun_path)) {
		snprintf(sock_path, sizeof(sock_path),
		    "/tmp/rpcemu-test-debugcmd-%ld.sock", (long) getpid());
	}

	unlink(sock_path);

	config.debug_enabled = 1;
	snprintf(config.debug_socket, sizeof(config.debug_socket), "%s", sock_path);
	debugcmd_init();

	client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client_fd < 0) {
		printf("FAIL: cannot create a client socket\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, sock_path, strlen(sock_path));

	debugcmd_poll();	/* let the listener come up */
	if (connect(client_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		printf("FAIL: cannot connect to %s\n", sock_path);
		return 1;
	}
	return 0;
#endif
}

int
main(int argc, char *argv[])
{
	const char *dir;

	/* Required rather than defaulting to ".": this test writes a socket and
	   a symbol file, and a default of the working directory means running it
	   by hand from a source tree leaves them there. */
	if (argc < 2) {
		fprintf(stderr, "usage: test_debugcmd <writable-directory>\n");
		return 2;
	}
	dir = argv[1];

	printf("Debugger wire protocol\n\n");

#ifdef _WIN32
	{
		WSADATA wsadata;

		if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
			printf("FAIL: WSAStartup failed\n");
			return 1;
		}
	}
#endif

	mem_init();
	mem_reset(16, 2);	/* megabytes, not bytes - see mem_reset() */

	if (connect_to_debugger(dir) != 0) {
		return 1;
	}

	/* MSG_DONTWAIT does not exist on Windows, where it is defined away to 0
	   by socket-compat.h. The poll loop in request() would then block on a
	   socket that is still waiting to be accepted, so put the client into
	   non-blocking mode explicitly. Harmless where MSG_DONTWAIT is real. */
	if (socket_set_nonblocking(client_fd) != 0) {
		printf("FAIL: cannot put the client socket in non-blocking mode\n");
		return 1;
	}

	debugcmd_poll();	/* accept */

	/*
	 * There is no greeting to drain here. The code that used to spin fifty
	 * times waiting for one was reading a socket that says nothing until it
	 * is asked: dc_poll() accepts a client and resets its output ring, and
	 * sends nothing. The loop always ran out and always succeeded, which is
	 * why it never looked wrong.
	 */

#ifdef _WIN32
	_putenv_s("RPCEMU_TEST_DIR", dir);	/* no setenv() in the Windows CRT */
#else
	setenv("RPCEMU_TEST_DIR", dir, 1);
#endif

	test_basics();
	test_breakpoints();
	test_conditions();
	test_stepping_and_backtrace();
	test_symbols();
	test_memory_and_disassembly();
	test_fpregs();
	test_setting_the_pc();

	closesocket(client_fd);
	debugcmd_close();
	remove_socket_file();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
