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
 * debugexpr.h - expressions over the emulated CPU's state
 *
 * Small integer expressions evaluated against the ARM's registers, flags and
 * memory, so a breakpoint can say "stop here when R0 is zero" rather than
 * "stop here", and the person debugging is not left holding the continue
 * button until the interesting case comes round.
 *
 * Grammar, lowest precedence first:
 *
 *   ||  &&  |  ^  &  == !=  < > <= >=  << >>  + -  * / %  unary ! ~ -
 *
 * Operands:
 *
 *   r0 .. r15, pc, sp, lr    ARM registers (r15/pc is the raw register)
 *   cpsr                     status register
 *   n, z, c, v               condition flags, 0 or 1
 *   0x1234, 1234, &1234      literals; & is the RISC OS hex prefix
 *   [expr]                   read a word of guest memory
 *   [expr]:1  :2  :4         read a byte, halfword or word
 *   (expr)                   grouping
 *
 * Values are 32-bit and unsigned throughout, including comparisons: the usual
 * comparand is an address, and signed address comparison is wrong more often
 * than it is right.
 *
 * Reads go through the caller-supplied environment, which is expected to reach
 * memory by a side-effect-free path. Evaluating a condition must never perturb
 * the machine being debugged - it happens between instructions, and an
 * expression that faulted or tripped a watchpoint would change the behaviour
 * of the program under inspection.
 */

#ifndef DEBUGEXPR_H
#define DEBUGEXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Where an expression reads its values from.
 *
 * All three are required. read_mem returns zero for an address that cannot be
 * read, which fails the whole evaluation rather than yielding a made-up value.
 */
typedef struct {
	uint32_t (*read_reg)(int reg, void *ctx);	/**< reg is 0-15 */
	uint32_t (*read_cpsr)(void *ctx);
	int (*read_mem)(uint32_t addr, uint32_t size, uint32_t *out, void *ctx);
	void *ctx;
} DebugExprEnv;

/**
 * Evaluate an expression.
 *
 * @param expr  Expression text
 * @param env   Where to read registers, flags and memory from
 * @param[out] result  The value, untouched on failure
 * @param[out] error   Set to a static description on failure, may be NULL
 * @return Non-zero on success, zero if the expression is malformed or a read
 *         failed
 */
int debugexpr_eval(const char *expr, const DebugExprEnv *env, uint32_t *result,
                   const char **error);

/**
 * Check an expression parses, without needing a machine to evaluate it
 * against.
 *
 * Used to reject a bad condition when a breakpoint is set, rather than
 * silently at some later moment when it is reached and cannot be evaluated.
 *
 * @param expr  Expression text
 * @param[out] error  Set to a static description on failure, may be NULL
 * @return Non-zero if the expression is well formed
 */
int debugexpr_check(const char *expr, const char **error);

#ifdef __cplusplus
}
#endif

#endif /* DEBUGEXPR_H */
