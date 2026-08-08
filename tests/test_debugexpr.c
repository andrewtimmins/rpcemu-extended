/*
 * Breakpoint condition expressions.
 *
 * WHY THIS EXISTS. A conditional breakpoint is trusted in a way an ordinary one
 * is not. "Stop at 0x8000" either works or obviously does not. "Stop at 0x8000
 * when R0 is 5" that silently evaluates wrongly does something much worse than
 * fail: it stops at the wrong moment, or never stops, and the person debugging
 * draws a conclusion about their program from an answer the debugger got wrong.
 * They have no reason to suspect the tool.
 *
 * So the cases here lean on the ways a small expression parser is usually
 * wrong:
 *
 *   - PRECEDENCE. Getting `a + b * c` right is table stakes; the ones that bite
 *     are `&` versus `&&`, and comparison binding tighter than bitwise-and, so
 *     that `r0 & 0xff == 0` does not silently mean `r0 & (0xff == 0)`.
 *
 *   - PREFIX OPERATORS. `<`, `<<` and `<=` all start the same way, as do `&`
 *     and `&&`, `|` and `||`, `!` and `!=`. A parser that takes the short one
 *     first turns `a <= b` into `a < (= b)` and fails, or worse, parses `a & &b`
 *     as something plausible.
 *
 *   - MALFORMED INPUT ACCEPTED. The dangerous failure is not rejecting a good
 *     expression, it is accepting a bad one and evaluating it to something.
 *     `r0 ==` and `r0 5` and `r0 == 5 junk` must all be refused, because a
 *     condition that quietly means something else is the whole problem.
 *
 *   - UNBOUNDED RECURSION. The parser runs on the emulator thread between two
 *     guest instructions. Deep nesting must hit a limit and return an error,
 *     not take out the stack.
 *
 * The fake environment below returns known register values and a small window
 * of memory, so evaluation can be checked exactly. It also refuses reads
 * outside that window, which is how the "condition could not be evaluated"
 * path gets exercised - that path exists because a dereference of an address
 * that is not currently mapped is the normal case, not an exceptional one.
 */

#include <stdio.h>
#include <string.h>

#include "debugexpr.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* ---- a fake machine ------------------------------------------------------ */

/* R0..R15. R15 is the raw register, PSR bits and all, as the real one is. */
static uint32_t fake_regs[16] = {
	0,          5,          0xff,       0x1234,
	0x80000000, 100,        200,        0,
	0,          0,          0,          0x00030000, /* R11 = frame pointer */
	0,          0x00040000, 0x00008024, 0x00008000  /* SP, LR, PC */
};

/* N and C set, Z and V clear, plus supervisor mode bits. */
static uint32_t fake_cpsr = 0xA0000013;

#define MEM_BASE	0x00010000u
#define MEM_WORDS	4
static uint32_t fake_mem[MEM_WORDS] = {
	0xDEADBEEF, 0x00000000, 0x12345678, 0x000000FF
};

static uint32_t
fake_read_reg(int reg, void *ctx)
{
	(void) ctx;
	return fake_regs[reg & 15];
}

static uint32_t
fake_read_cpsr(void *ctx)
{
	(void) ctx;
	return fake_cpsr;
}

static int
fake_read_mem(uint32_t addr, uint32_t size, uint32_t *out, void *ctx)
{
	uint32_t word;
	uint32_t value;

	(void) ctx;

	/* Anything outside the window is "not mapped", the same as a real
	   unmapped page - that is the case worth exercising. */
	if (addr < MEM_BASE || addr + size > MEM_BASE + MEM_WORDS * 4) {
		return 0;
	}

	word = (addr - MEM_BASE) / 4;
	value = fake_mem[word] >> (((addr - MEM_BASE) % 4) * 8);

	switch (size) {
	case 1:  *out = value & 0xff;	break;
	case 2:  *out = value & 0xffff;	break;
	default: *out = value;		break;
	}
	return 1;
}

static const DebugExprEnv env = {
	fake_read_reg, fake_read_cpsr, fake_read_mem, NULL
};

/* ---- helpers ------------------------------------------------------------- */

/**
 * The expression must evaluate, and to exactly this value.
 */
static void
eval_is(const char *expr, uint32_t expected)
{
	uint32_t got = 0;
	const char *error = NULL;
	char what[160];

	if (!debugexpr_eval(expr, &env, &got, &error)) {
		snprintf(what, sizeof(what), "\"%s\" evaluates", expr);
		check(what, 0);
		printf("      rejected: %s\n", error ? error : "(no reason)");
		return;
	}

	snprintf(what, sizeof(what), "\"%s\" == 0x%X", expr, expected);
	check(what, got == expected);
	if (got != expected) {
		printf("      got 0x%X\n", got);
	}
}

/**
 * The expression must be refused, at parse time or evaluation time.
 */
static void
eval_fails(const char *expr)
{
	uint32_t got = 0;
	const char *error = NULL;
	char what[160];

	snprintf(what, sizeof(what), "\"%s\" is refused", expr);
	check(what, !debugexpr_eval(expr, &env, &got, &error));
	check("  ... with a reason given", error != NULL);
}

/**
 * The expression must be refused by the syntax check alone, with no machine.
 */
static void
check_fails(const char *expr)
{
	const char *error = NULL;
	char what[160];

	snprintf(what, sizeof(what), "\"%s\" fails the syntax check", expr);
	check(what, !debugexpr_check(expr, &error));
}

/* ---- tests --------------------------------------------------------------- */

static void
test_literals(void)
{
	printf("Literals\n");

	eval_is("0", 0);
	eval_is("42", 42);
	eval_is("0x10", 0x10);
	eval_is("0X10", 0x10);
	eval_is("0xdeadbeef", 0xdeadbeef);
	eval_is("0xDEADBEEF", 0xdeadbeef);

	/* & is how RISC OS spells a hex prefix, and the people using this will
	   type it out of habit */
	eval_is("&1000", 0x1000);
	eval_is("&ff", 0xff);

	/* Whitespace is not significant */
	eval_is("  7  ", 7);
}

static void
test_registers_and_flags(void)
{
	printf("Registers and flags\n");

	eval_is("r0", 0);
	eval_is("r1", 5);
	eval_is("r2", 0xff);
	eval_is("r15", 0x00008000);
	eval_is("R1", 5);		/* case insensitive */

	eval_is("pc", 0x00008000);
	eval_is("sp", 0x00040000);
	eval_is("lr", 0x00008024);
	eval_is("cpsr", 0xA0000013);

	/* Flags come out of the CPSR as 0 or 1 */
	eval_is("n", 1);
	eval_is("z", 0);
	eval_is("c", 1);
	eval_is("v", 0);

	/* r16 and beyond do not exist */
	eval_fails("r16");
	eval_fails("r99");
	eval_fails("banana");
}

static void
test_arithmetic(void)
{
	printf("Arithmetic\n");

	eval_is("1 + 2", 3);
	eval_is("10 - 3", 7);
	eval_is("6 * 7", 42);
	eval_is("100 / 7", 14);
	eval_is("100 % 7", 2);

	/* Multiplication binds tighter than addition */
	eval_is("2 + 3 * 4", 14);
	eval_is("(2 + 3) * 4", 20);

	/* Values are 32-bit and wrap */
	eval_is("0xffffffff + 1", 0);
	eval_is("0 - 1", 0xffffffff);

	/* Unary minus */
	eval_is("-1", 0xffffffff);
	eval_is("5 + -3", 2);

	eval_fails("1 / 0");
	eval_fails("1 % 0");
}

static void
test_bitwise(void)
{
	printf("Bitwise\n");

	eval_is("0xf0 & 0x3c", 0x30);
	eval_is("0xf0 | 0x0f", 0xff);
	eval_is("0xff ^ 0x0f", 0xf0);
	eval_is("~0", 0xffffffff);
	eval_is("~0xff", 0xffffff00);

	eval_is("1 << 4", 0x10);
	eval_is("0x100 >> 4", 0x10);

	/* A shift of 32 or more is undefined in C; here it is defined as 0 */
	eval_is("1 << 32", 0);
	eval_is("1 << 100", 0);
	eval_is("0xffffffff >> 32", 0);

	/* & binds tighter than ^, which binds tighter than | */
	eval_is("1 | 2 & 2", 3);
	eval_is("0xff ^ 0x0f & 0x0f", 0xf0);
}

static void
test_comparison(void)
{
	printf("Comparison\n");

	eval_is("1 == 1", 1);
	eval_is("1 == 2", 0);
	eval_is("1 != 2", 1);
	eval_is("1 < 2", 1);
	eval_is("2 < 1", 0);
	eval_is("2 > 1", 1);
	eval_is("1 <= 1", 1);
	eval_is("1 >= 2", 0);

	/* Comparisons are unsigned, as documented: 0x80000000 is a large
	   number here, not a negative one */
	eval_is("r4 > 1", 1);
	eval_is("0x80000000 > 0x7fffffff", 1);

	/* Conditions people will actually write */
	eval_is("r1 == 5", 1);
	eval_is("r0 == 0", 1);
	eval_is("r1 != 5", 0);
	eval_is("pc >= 0x8000", 1);
}

static void
test_precedence_traps(void)
{
	printf("Precedence traps\n");

	/* Comparison binds tighter than bitwise-and, as it does in C. That is a
	   famous trap rather than an obvious choice, so it is pinned down with
	   operands that tell the two readings apart: `1 & 2 == 2` is
	   `1 & (2 == 2)` = `1 & 1` = 1, where `(1 & 2) == 2` would be 0. */
	eval_is("r2 & 0xff", 0xff);
	eval_is("1 & 2 == 2", 1);
	eval_is("(1 & 2) == 2", 0);

	/* The same trap the other way up, so a parser that got it backwards
	   fails both rather than passing one by luck */
	eval_is("2 | 1 == 1", 3);	/* 2 | (1 == 1) */
	eval_is("(2 | 1) == 1", 0);

	/* && and || are lower than everything, and yield 0 or 1 */
	eval_is("1 && 1", 1);
	eval_is("1 && 0", 0);
	eval_is("0 || 1", 1);
	eval_is("0 || 0", 0);
	eval_is("5 && 3", 1);		/* not 1 */

	eval_is("r1 == 5 && r0 == 0", 1);
	eval_is("r1 == 5 && r0 == 1", 0);
	eval_is("r1 == 4 || r0 == 0", 1);

	/* && binds tighter than || */
	eval_is("1 || 0 && 0", 1);
	eval_is("(1 || 0) && 0", 0);

	/* Single versus double: these must not be confused for one another */
	eval_is("2 & 1", 0);
	eval_is("2 && 1", 1);
	eval_is("2 | 1", 3);
	eval_is("2 || 1", 1);

	/* ! and != */
	eval_is("!0", 1);
	eval_is("!5", 0);
	eval_is("!r0", 1);
	eval_is("1 != 0", 1);
	eval_is("!(r1 == 5)", 0);
}

static void
test_memory(void)
{
	printf("Memory\n");

	eval_is("[0x10000]", 0xDEADBEEF);
	eval_is("[0x10008]", 0x12345678);

	/* Sized accesses */
	eval_is("[0x10000]:4", 0xDEADBEEF);
	eval_is("[0x10000]:2", 0xBEEF);
	eval_is("[0x10000]:1", 0xEF);

	/* The address is itself an expression */
	eval_is("[0x10000 + 8]", 0x12345678);
	eval_is("[0xfffc + 4]", 0xDEADBEEF);

	/* And can be read through a register */
	eval_is("[r0 + 0x10000]", 0xDEADBEEF);

	/* Nested */
	eval_is("[[0x1000c] + 0xff01]", 0xDEADBEEF);

	/* Comparisons against memory, which is the point of the feature */
	eval_is("[0x10000] == 0xdeadbeef", 1);
	eval_is("[0x10004] == 0", 1);

	/* An unmapped address fails the whole evaluation rather than reading
	   as zero - a condition must never be decided on a made-up value */
	eval_fails("[0x99999999]");
	eval_fails("[0]");
	eval_fails("[0x10000] == 0 || [0x99999999] == 0");

	/* Bad sizes */
	eval_fails("[0x10000]:3");
	eval_fails("[0x10000]:0");
	eval_fails("[0x10000]:8");
}

static void
test_malformed(void)
{
	printf("Malformed input\n");

	/* The dangerous case is not rejecting these, it is accepting them */
	eval_fails("");
	eval_fails("   ");
	eval_fails("r0 ==");
	eval_fails("== 5");
	eval_fails("r0 5");
	eval_fails("r0 == 5 junk");
	eval_fails("(r0");
	eval_fails("r0)");
	eval_fails("[r0");
	eval_fails("1 +");
	eval_fails("* 5");
	eval_fails("r0 & & 5");
	eval_fails("&");
	eval_fails("0x");

	/* These are refusable without a machine at all */
	check_fails("r0 ==");
	check_fails("r0 == 5 junk");
	check_fails("(((");
	check_fails("banana");

	/* ... and a well-formed one must pass the syntax check */
	check("\"r0 == 5\" passes the syntax check", debugexpr_check("r0 == 5", NULL));
	check("\"[r1 + 4] != 0\" passes the syntax check",
	      debugexpr_check("[r1 + 4] != 0", NULL));

	/* A NULL expression is refused rather than crashing */
	check("NULL is refused", !debugexpr_check(NULL, NULL));
}

static void
test_depth_limit(void)
{
	char deep[512];
	size_t i;
	const char *error = NULL;

	printf("Recursion limit\n");

	/* Runs on the emulator thread between two guest instructions: deep
	   nesting has to come back with an error, not take the stack out */
	for (i = 0; i < sizeof(deep) - 2; i++) {
		deep[i] = '(';
	}
	deep[sizeof(deep) - 2] = '1';
	deep[sizeof(deep) - 1] = '\0';

	check("deeply nested input is refused", !debugexpr_check(deep, &error));
	check("  ... with a reason given", error != NULL);

	/* Modest nesting still works */
	eval_is("((((((1 + 1))))))", 2);
}

int
main(void)
{
	printf("Breakpoint condition expressions\n\n");

	test_literals();
	test_registers_and_flags();
	test_arithmetic();
	test_bitwise();
	test_comparison();
	test_precedence_traps();
	test_memory();
	test_malformed();
	test_depth_limit();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
