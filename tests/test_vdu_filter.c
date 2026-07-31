/*
 * Turning a RISC OS VDU stream into text a host shell can use.
 *
 * The cases that matter are the ones that make the obvious implementation wrong:
 * a VDU code whose parameter bytes happen to be 10 or 13, and a sequence split
 * across two reads. Both corrupt a line invisibly rather than failing loudly, so
 * they are worth pinning down.
 */

#include <stdio.h>
#include <string.h>

#include "vdu_filter.h"

static int failures;

static void
expect(const char *what, const char *in, size_t in_len, const char *want)
{
	VduFilter f;
	uint8_t out[256];
	size_t w;

	vdu_filter_init(&f);
	w = vdu_filter(&f, (const uint8_t *) in, in_len, out);
	w += vdu_filter_finish(&f, out + w);

	if (w != strlen(want) || memcmp(out, want, w) != 0) {
		printf("  %-46s FAIL (got \"%.*s\" [%zu], wanted \"%s\")\n",
		       what, (int) w, (const char *) out, w, want);
		failures++;
		return;
	}
	printf("  %-46s ok\n", what);
}

/* The same input, delivered in two calls, must give the same answer: a socket
   read boundary can fall anywhere, including inside a VDU sequence. */
static void
expect_split(const char *what, const char *in, size_t in_len, size_t at,
             const char *want)
{
	VduFilter f;
	uint8_t out[256];
	size_t w;

	vdu_filter_init(&f);
	w = vdu_filter(&f, (const uint8_t *) in, at, out);
	w += vdu_filter(&f, (const uint8_t *) in + at, in_len - at, out + w);
	w += vdu_filter_finish(&f, out + w);

	if (w != strlen(want) || memcmp(out, want, w) != 0) {
		printf("  %-46s FAIL (got \"%.*s\" [%zu], wanted \"%s\")\n",
		       what, (int) w, (const char *) out, w, want);
		failures++;
		return;
	}
	printf("  %-46s ok\n", what);
}

int
main(void)
{
	printf("line endings\n");
	/* What RISC OS actually sends, confirmed by hexdump of *Echo output. */
	expect("RISC OS order, LF then CR", "Test\n\r", 6, "Test\n");
	expect("DOS order, CR then LF", "Test\r\n", 6, "Test\n");
	expect("bare LF", "Test\n", 5, "Test\n");
	expect("bare CR", "Test\r", 5, "Test\n");
	expect("two lines", "a\n\rb\n\r", 6, "a\nb\n");
	expect("no terminator at all", "Test", 4, "Test");
	expect("blank line", "a\n\r\n\rb", 6, "a\n\nb");

	printf("\ncontrol codes\n");
	/* *Status emits this one. */
	expect("VDU 14, paged mode, no parameters", "a\016b", 3, "ab");
	expect("VDU 17, one parameter", "a\021\007b", 4, "ab");
	expect("VDU 22, one parameter", "a\026\033b", 4, "ab");

	printf("\nparameters that look like text or line endings\n");
	/* The whole reason for the parameter table. VDU 19 takes five bytes; here
	   two of them are 10 and 13. Dropping only the 19 would leave "AB" and two
	   spurious newlines in the middle of the line. */
	expect("VDU 19 with LF and CR among its parameters",
	       "x\023\001\012\101\015\102y", 9, "xy");
	/* VDU 23 takes nine, the longest. */
	expect("VDU 23 swallows all nine parameters",
	       "x\027ABCDEFGHIy", 12, "xy");
	/* VDU 31 tab-to, two parameters, both printable. */
	expect("VDU 31 with printable parameters", "x\037\040\041y", 5, "xy");

	printf("\nsplit across two reads\n");
	expect_split("split inside a VDU 19 sequence",
	             "x\023\001\002\003\004\005y", 8, 3, "xy");
	expect_split("split between LF and CR", "Test\n\r", 6, 5, "Test\n");
	expect_split("split between CR and LF", "Test\r\n", 6, 5, "Test\n");
	expect_split("split immediately after a VDU introducer",
	             "x\021\007y", 4, 2, "xy");

	printf("\nordinary text is untouched\n");
	expect("plain text", "Hello, world", 12, "Hello, world");
	/* *Echo prints the quotes it was given; that is RISC OS behaviour and not
	   something to normalise away. */
	expect("quotes from *Echo survive", "\"Test\"\n\r", 8, "\"Test\"\n");
	expect("high-bit bytes survive", "caf\xe9", 4, "caf\xe9");

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
