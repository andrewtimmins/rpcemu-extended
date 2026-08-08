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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "debugcmd.h"

static int failures;
static int client_fd = -1;

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
		usleep(1000);
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

int
main(int argc, char *argv[])
{
	struct sockaddr_un addr;
	const char *dir = (argc > 1) ? argv[1] : ".";
	char sock_path[512];

	printf("Debugger wire protocol\n\n");

	mem_init();
	mem_reset(16, 2);	/* megabytes, not bytes - see mem_reset() */

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
	debugcmd_poll();	/* accept */

	/* The server greets a new client, so drain that before asking anything */
	{
		char greeting[4096];
		int spins;

		for (spins = 0; spins < 50; spins++) {
			debugcmd_poll();
			if (recv(client_fd, greeting, sizeof(greeting), MSG_DONTWAIT) > 0) {
				break;
			}
			usleep(1000);
		}
	}

	setenv("RPCEMU_TEST_DIR", dir, 1);

	test_basics();
	test_breakpoints();
	test_conditions();
	test_stepping_and_backtrace();
	test_symbols();
	test_memory_and_disassembly();

	close(client_fd);
	debugcmd_close();
	unlink(sock_path);

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
