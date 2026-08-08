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
 * debugexpr.c - expressions over the emulated CPU's state
 *
 * A recursive-descent parser that evaluates as it goes. There is no syntax
 * tree: expressions are short, evaluated at most once per breakpoint hit, and
 * building a tree would mean allocating on a path that runs between two
 * instructions of the guest.
 *
 * Two things are load-bearing here.
 *
 * It cannot allocate and it cannot fault. Evaluation happens on the emulator
 * thread with the machine stopped between instructions, so anything that could
 * block, allocate or take a signal would show up as a hang or a crash in the
 * emulator rather than as a bad expression. Recursion is bounded (see
 * DEPTH_LIMIT) so a pathological input cannot take the stack out.
 *
 * It must be side-effect free. Reads go through the caller's environment,
 * which reaches memory by the debugger's no-abort, no-watchpoint path. An
 * expression that could fault would change the behaviour of the program being
 * debugged, which is the one thing a debugger must not do.
 *
 * Malformed input is rejected with a static message rather than guessed at.
 * "Stop when R0 is 5" silently becoming "stop always" is much worse than being
 * told the expression is wrong.
 */

#include <stdint.h>
#include <string.h>

#include "debugexpr.h"

/* Deep enough for any expression a person writes, shallow enough that the
   recursion cannot exhaust the emulator thread's stack. */
#define DEPTH_LIMIT 32

typedef struct {
	const char *p;			/**< Current position */
	const DebugExprEnv *env;	/**< NULL when only checking syntax */
	const char *error;		/**< First error, static string */
	int depth;
} Parser;

static int parse_expr(Parser *ps, uint32_t *out);

/**
 * Record the first error. Later errors are usually consequences of the first.
 */
static int
fail(Parser *ps, const char *message)
{
	if (ps->error == NULL) {
		ps->error = message;
	}
	return 0;
}

static void
skip_space(Parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t') {
		ps->p++;
	}
}

/**
 * Consume `text` if it is next, after whitespace.
 */
static int
accept(Parser *ps, const char *text)
{
	size_t len = strlen(text);

	skip_space(ps);
	if (strncmp(ps->p, text, len) != 0) {
		return 0;
	}
	ps->p += len;
	return 1;
}

/**
 * Consume `text` only when it is not the prefix of a longer operator.
 *
 * Without this, "&&" would parse as bitwise-and followed by a stray "&", and
 * "<=" as less-than followed by nothing. Every operator that is a prefix of
 * another has to be matched this way.
 */
static int
accept_exact(Parser *ps, const char *text, const char *not_followed_by)
{
	const char *save = ps->p;
	size_t len = strlen(text);

	skip_space(ps);
	if (strncmp(ps->p, text, len) != 0) {
		ps->p = save;
		return 0;
	}
	if (strchr(not_followed_by, ps->p[len]) != NULL && ps->p[len] != '\0') {
		ps->p = save;
		return 0;
	}
	ps->p += len;
	return 1;
}

static int
is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int
is_ident(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       is_digit(c) || c == '_';
}

static char
lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/**
 * Parse a number: 0x-prefixed hex, &-prefixed hex (the RISC OS spelling), or
 * decimal. Overflow past 32 bits wraps, as everything else here does.
 */
static int
parse_number(Parser *ps, uint32_t *out)
{
	uint32_t value = 0;
	int digits = 0;
	int base = 10;

	if (ps->p[0] == '&') {
		base = 16;
		ps->p++;
	} else if (ps->p[0] == '0' && lower(ps->p[1]) == 'x') {
		base = 16;
		ps->p += 2;
	}

	for (;;) {
		char c = lower(*ps->p);
		uint32_t digit;

		if (is_digit(c)) {
			digit = (uint32_t) (c - '0');
		} else if (base == 16 && c >= 'a' && c <= 'f') {
			digit = (uint32_t) (c - 'a' + 10);
		} else {
			break;
		}

		value = value * (uint32_t) base + digit;
		digits++;
		ps->p++;
	}

	if (digits == 0) {
		return fail(ps, "expected a number");
	}

	*out = value;
	return 1;
}

/** CPSR bit positions of the condition flags. */
#define FLAG_N 31
#define FLAG_Z 30
#define FLAG_C 29
#define FLAG_V 28

/**
 * Parse a register, flag or other named value.
 */
static int
parse_identifier(Parser *ps, uint32_t *out)
{
	char name[8];
	size_t len = 0;
	int reg = -1;

	while (is_ident(*ps->p) && len < sizeof(name) - 1) {
		name[len++] = lower(*ps->p);
		ps->p++;
	}
	name[len] = '\0';

	if (len == 0) {
		return fail(ps, "expected a value");
	}
	if (is_ident(*ps->p)) {
		return fail(ps, "unknown name");
	}

	if (strcmp(name, "pc") == 0) {
		reg = 15;
	} else if (strcmp(name, "sp") == 0) {
		reg = 13;
	} else if (strcmp(name, "lr") == 0) {
		reg = 14;
	} else if (name[0] == 'r' && len >= 2 && len <= 3) {
		int n = 0;
		size_t i;

		for (i = 1; i < len; i++) {
			if (!is_digit(name[i])) {
				return fail(ps, "unknown name");
			}
			n = n * 10 + (name[i] - '0');
		}
		if (n > 15) {
			return fail(ps, "no such register");
		}
		reg = n;
	}

	if (reg >= 0) {
		*out = ps->env ? ps->env->read_reg(reg, ps->env->ctx) : 0;
		return 1;
	}

	if (strcmp(name, "cpsr") == 0) {
		*out = ps->env ? ps->env->read_cpsr(ps->env->ctx) : 0;
		return 1;
	}

	if (len == 1 && strchr("nzcv", name[0]) != NULL) {
		uint32_t cpsr = ps->env ? ps->env->read_cpsr(ps->env->ctx) : 0;
		int bit;

		switch (name[0]) {
		case 'n':  bit = FLAG_N; break;
		case 'z':  bit = FLAG_Z; break;
		case 'c':  bit = FLAG_C; break;
		default:   bit = FLAG_V; break;
		}
		*out = (cpsr >> bit) & 1;
		return 1;
	}

	return fail(ps, "unknown name");
}

/**
 * Parse a memory reference: [addr], or [addr]:size for a byte or halfword.
 */
static int
parse_memory(Parser *ps, uint32_t *out)
{
	uint32_t addr;
	uint32_t size = 4;

	if (!parse_expr(ps, &addr)) {
		return 0;
	}
	if (!accept(ps, "]")) {
		return fail(ps, "expected ]");
	}

	if (accept(ps, ":")) {
		skip_space(ps);
		if (!parse_number(ps, &size)) {
			return 0;
		}
		if (size != 1 && size != 2 && size != 4) {
			return fail(ps, "access size must be 1, 2 or 4");
		}
	}

	if (ps->env == NULL) {
		*out = 0;	/* syntax check only */
		return 1;
	}

	if (!ps->env->read_mem(addr, size, out, ps->env->ctx)) {
		return fail(ps, "address is not readable");
	}
	return 1;
}

static int
parse_primary(Parser *ps, uint32_t *out)
{
	skip_space(ps);

	if (*ps->p == '\0') {
		return fail(ps, "expression ends early");
	}

	if (accept(ps, "(")) {
		if (!parse_expr(ps, out)) {
			return 0;
		}
		if (!accept(ps, ")")) {
			return fail(ps, "expected )");
		}
		return 1;
	}

	if (accept(ps, "[")) {
		return parse_memory(ps, out);
	}

	if (is_digit(*ps->p) || *ps->p == '&') {
		return parse_number(ps, out);
	}

	return parse_identifier(ps, out);
}

static int
parse_unary(Parser *ps, uint32_t *out)
{
	if (++ps->depth > DEPTH_LIMIT) {
		ps->depth--;
		return fail(ps, "expression nested too deeply");
	}

	skip_space(ps);

	if (accept_exact(ps, "!", "=")) {
		uint32_t v;
		int ok = parse_unary(ps, &v);

		ps->depth--;
		if (!ok) {
			return 0;
		}
		*out = v ? 0 : 1;
		return 1;
	}
	if (accept(ps, "~")) {
		uint32_t v;
		int ok = parse_unary(ps, &v);

		ps->depth--;
		if (!ok) {
			return 0;
		}
		*out = ~v;
		return 1;
	}
	if (accept(ps, "-")) {
		uint32_t v;
		int ok = parse_unary(ps, &v);

		ps->depth--;
		if (!ok) {
			return 0;
		}
		*out = (uint32_t) (0u - v);
		return 1;
	}

	{
		int ok = parse_primary(ps, out);

		ps->depth--;
		return ok;
	}
}

static int
parse_mul(Parser *ps, uint32_t *out)
{
	if (!parse_unary(ps, out)) {
		return 0;
	}

	for (;;) {
		uint32_t rhs;

		if (accept(ps, "*")) {
			if (!parse_unary(ps, &rhs)) {
				return 0;
			}
			*out = *out * rhs;
		} else if (accept(ps, "/")) {
			if (!parse_unary(ps, &rhs)) {
				return 0;
			}
			if (rhs == 0) {
				return fail(ps, "division by zero");
			}
			*out = *out / rhs;
		} else if (accept(ps, "%")) {
			if (!parse_unary(ps, &rhs)) {
				return 0;
			}
			if (rhs == 0) {
				return fail(ps, "division by zero");
			}
			*out = *out % rhs;
		} else {
			return 1;
		}
	}
}

static int
parse_add(Parser *ps, uint32_t *out)
{
	if (!parse_mul(ps, out)) {
		return 0;
	}

	for (;;) {
		uint32_t rhs;

		if (accept(ps, "+")) {
			if (!parse_mul(ps, &rhs)) {
				return 0;
			}
			*out = *out + rhs;
		} else if (accept(ps, "-")) {
			if (!parse_mul(ps, &rhs)) {
				return 0;
			}
			*out = *out - rhs;
		} else {
			return 1;
		}
	}
}

static int
parse_shift(Parser *ps, uint32_t *out)
{
	if (!parse_add(ps, out)) {
		return 0;
	}

	for (;;) {
		uint32_t rhs;
		int left;

		if (accept(ps, "<<")) {
			left = 1;
		} else if (accept(ps, ">>")) {
			left = 0;
		} else {
			return 1;
		}

		if (!parse_add(ps, &rhs)) {
			return 0;
		}
		/* A shift of 32 or more is undefined in C; define it as zero,
		   which is what the reader of the expression expects. */
		if (rhs >= 32) {
			*out = 0;
		} else {
			*out = left ? (*out << rhs) : (*out >> rhs);
		}
	}
}

static int
parse_relational(Parser *ps, uint32_t *out)
{
	if (!parse_shift(ps, out)) {
		return 0;
	}

	for (;;) {
		uint32_t rhs;

		if (accept(ps, "<=")) {
			if (!parse_shift(ps, &rhs)) {
				return 0;
			}
			*out = (*out <= rhs) ? 1 : 0;
		} else if (accept(ps, ">=")) {
			if (!parse_shift(ps, &rhs)) {
				return 0;
			}
			*out = (*out >= rhs) ? 1 : 0;
		} else if (accept_exact(ps, "<", "<=")) {
			if (!parse_shift(ps, &rhs)) {
				return 0;
			}
			*out = (*out < rhs) ? 1 : 0;
		} else if (accept_exact(ps, ">", ">=")) {
			if (!parse_shift(ps, &rhs)) {
				return 0;
			}
			*out = (*out > rhs) ? 1 : 0;
		} else {
			return 1;
		}
	}
}

static int
parse_equality(Parser *ps, uint32_t *out)
{
	if (!parse_relational(ps, out)) {
		return 0;
	}

	for (;;) {
		uint32_t rhs;

		if (accept(ps, "==")) {
			if (!parse_relational(ps, &rhs)) {
				return 0;
			}
			*out = (*out == rhs) ? 1 : 0;
		} else if (accept(ps, "!=")) {
			if (!parse_relational(ps, &rhs)) {
				return 0;
			}
			*out = (*out != rhs) ? 1 : 0;
		} else {
			return 1;
		}
	}
}

static int
parse_bitand(Parser *ps, uint32_t *out)
{
	if (!parse_equality(ps, out)) {
		return 0;
	}

	while (accept_exact(ps, "&", "&")) {
		uint32_t rhs;

		if (!parse_equality(ps, &rhs)) {
			return 0;
		}
		*out = *out & rhs;
	}
	return 1;
}

static int
parse_bitxor(Parser *ps, uint32_t *out)
{
	if (!parse_bitand(ps, out)) {
		return 0;
	}

	while (accept(ps, "^")) {
		uint32_t rhs;

		if (!parse_bitand(ps, &rhs)) {
			return 0;
		}
		*out = *out ^ rhs;
	}
	return 1;
}

static int
parse_bitor(Parser *ps, uint32_t *out)
{
	if (!parse_bitxor(ps, out)) {
		return 0;
	}

	while (accept_exact(ps, "|", "|")) {
		uint32_t rhs;

		if (!parse_bitxor(ps, &rhs)) {
			return 0;
		}
		*out = *out | rhs;
	}
	return 1;
}

static int
parse_and(Parser *ps, uint32_t *out)
{
	if (!parse_bitor(ps, out)) {
		return 0;
	}

	while (accept(ps, "&&")) {
		uint32_t rhs;

		/* Both sides are always evaluated. Short-circuiting would mean a
		   condition's cost depended on its left half, and there is
		   nothing here whose evaluation can be observed anyway. */
		if (!parse_bitor(ps, &rhs)) {
			return 0;
		}
		*out = (*out && rhs) ? 1 : 0;
	}
	return 1;
}

static int
parse_expr(Parser *ps, uint32_t *out)
{
	if (++ps->depth > DEPTH_LIMIT) {
		ps->depth--;
		return fail(ps, "expression nested too deeply");
	}

	if (!parse_and(ps, out)) {
		ps->depth--;
		return 0;
	}

	while (accept(ps, "||")) {
		uint32_t rhs;

		if (!parse_and(ps, &rhs)) {
			ps->depth--;
			return 0;
		}
		*out = (*out || rhs) ? 1 : 0;
	}

	ps->depth--;
	return 1;
}

/**
 * Parse, and optionally evaluate, a complete expression.
 */
static int
run(const char *expr, const DebugExprEnv *env, uint32_t *result,
	const char **error)
{
	Parser ps;
	uint32_t value = 0;

	if (error != NULL) {
		*error = NULL;
	}
	if (expr == NULL) {
		if (error != NULL) {
			*error = "no expression";
		}
		return 0;
	}

	ps.p = expr;
	ps.env = env;
	ps.error = NULL;
	ps.depth = 0;

	if (!parse_expr(&ps, &value)) {
		if (error != NULL) {
			*error = ps.error ? ps.error : "bad expression";
		}
		return 0;
	}

	/* Trailing junk means the expression was not understood the way it was
	   written, so it must not be treated as valid. */
	skip_space(&ps);
	if (*ps.p != '\0') {
		if (error != NULL) {
			*error = "unexpected text after expression";
		}
		return 0;
	}

	if (result != NULL) {
		*result = value;
	}
	return 1;
}

int
debugexpr_eval(const char *expr, const DebugExprEnv *env, uint32_t *result,
	const char **error)
{
	if (env == NULL) {
		if (error != NULL) {
			*error = "no environment";
		}
		return 0;
	}
	return run(expr, env, result, error);
}

int
debugexpr_check(const char *expr, const char **error)
{
	return run(expr, NULL, NULL, error);
}
