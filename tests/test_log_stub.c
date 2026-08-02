/*
 * rpclog() for tests that compile one unit directly instead of linking
 * rpcemu_core.
 *
 * Most units in src/ log, and that is their only tie to the rest of the
 * emulator; supplying it here is what lets a test compile such a unit on its own
 * rather than dragging in the core and, through it, SDL and wxWidgets.
 *
 * Silent by default so a passing test's output stays readable. Set
 * RPCEMU_TEST_LOG in the environment to see what the unit under test is saying,
 * which is worth doing when one of them fails.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void
rpclog(const char *format, ...)
{
	static int checked;
	static int enabled;
	va_list ap;

	if (!checked) {
		enabled = (getenv("RPCEMU_TEST_LOG") != NULL);
		checked = 1;
	}
	if (!enabled) {
		return;
	}

	fputs("      log: ", stdout);
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	fflush(stdout);
}
