/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andy Timmins

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
 * arm_disasm.h - ARM instruction disassembler
 *
 * Decodes ARM (32-bit) instructions into human-readable assembly syntax,
 * covering the instruction sets of the cores this emulator implements -
 * ARMv3 (ARM610/710/7500) and ARMv4 (ARM810/StrongARM):
 *   - Data processing (AND, ORR, ADD, SUB, MOV, CMP, etc.)
 *   - Multiply and multiply-accumulate
 *   - Load/store (LDR, STR, LDRH, LDRSB, etc.)
 *   - Block transfer (LDM, STM)
 *   - Branch and branch-with-link
 *   - Status register access (MRS, MSR)
 *   - Software interrupt (SWI), named from the RISC OS SWI table
 *   - FPA10 floating point on coprocessors 1 and 2 (ADF, LDF, CMF, ...)
 *   - Generic coprocessor instructions for every other coprocessor
 *
 * Two entry points are offered. arm_disasm() renders an instruction as text;
 * arm_decode() fills in a description of what the instruction *does*, which is
 * what the debugger needs in order to step over a call, recognise a return, or
 * follow a branch.
 */

#ifndef ARM_DISASM_H
#define ARM_DISASM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How arm_disasm() renders what it decodes.
 *
 * Asked for in discussion #223, by somebody using the debugger against his own
 * assembler source: a disassembly that does not look like the code you are
 * comparing it against costs a translation step on every line. None of these
 * change what is decoded, only how it is written down.
 *
 * Held globally rather than passed per call, because the whole disassembler is
 * already a set of static tables and every caller wants the same answer. Both
 * the GUI thread (the inspector) and the emulator thread (the debug socket's
 * "dis") read it, and the GUI writes it when a checkbox moves; the fields are
 * single bytes and the worst a race can do is render one line with a mixture of
 * old and new settings.
 */
typedef struct {
	/** Immediates as RISC OS hex, "#&80" rather than "#128". */
	uint8_t hex_immediates;

	/**
	 * APCS register names: a1-a4, v1-v5, sb, sl, fp, ip, sp, lr, pc.
	 *
	 * Clear gives ARM names, R0-R14 and PC. Note that clear is not what this
	 * disassembler used to print: R13 and R14 came out as SP and LR while
	 * R0-R12 were numbered, which is the mixture the request complains about.
	 */
	uint8_t apcs_registers;

	/** Collapse LDM/STM lists to ranges: "{R0-R4, R7}" not "{R0, R1, ...}". */
	uint8_t collapse_reglists;

	/**
	 * Resolve a PC-relative load or store to the address it reaches, so
	 * "LDR R11, [PC, #-120]" reads "LDR R11, &00001C84".
	 *
	 * Only for a pre-indexed immediate offset off R15 with no writeback,
	 * which is the literal-pool form the compiler and the assembler emit.
	 * Anything else keeps its registers, because the address is not knowable
	 * from the instruction alone.
	 */
	uint8_t resolve_pc_relative;
} ArmDisasmOptions;

/** Replace the rendering options. NULL restores the defaults (all clear). */
void arm_disasm_set_options(const ArmDisasmOptions *opts);

/** Read the rendering options currently in force. */
void arm_disasm_get_options(ArmDisasmOptions *opts);

/** Broad category of an instruction, as identified by arm_decode(). */
typedef enum {
	ARM_CLASS_UNKNOWN = 0,	/**< Not a recognised encoding */
	ARM_CLASS_DATAPROC,	/**< AND/ORR/ADD/MOV/CMP/... */
	ARM_CLASS_MULTIPLY,	/**< MUL, MLA */
	ARM_CLASS_MULTIPLY_LONG,/**< UMULL, UMLAL, SMULL, SMLAL */
	ARM_CLASS_LDST,		/**< LDR, STR */
	ARM_CLASS_LDST_HALF,	/**< LDRH, STRH, LDRSB, LDRSH */
	ARM_CLASS_LDM_STM,	/**< LDM, STM */
	ARM_CLASS_BRANCH,	/**< B, BL */
	ARM_CLASS_BX,		/**< BX */
	ARM_CLASS_SWI,		/**< SWI */
	ARM_CLASS_SWAP,		/**< SWP, SWPB */
	ARM_CLASS_MRS,		/**< MRS */
	ARM_CLASS_MSR,		/**< MSR */
	ARM_CLASS_COPROC_LDST,	/**< LDC, STC */
	ARM_CLASS_COPROC_DATA,	/**< CDP */
	ARM_CLASS_COPROC_REG,	/**< MRC, MCR */
	ARM_CLASS_FPA_DATA,	/**< FPA arithmetic: ADF, MUF, MVF, ... */
	ARM_CLASS_FPA_REG,	/**< FPA register transfer: FLT, FIX, CMF, ... */
	ARM_CLASS_FPA_LDST	/**< FPA load/store: LDF, STF, LFM, SFM */
} ArmInsnClass;

/** Condition code value meaning "always"; see ArmInsnInfo.cond. */
#define ARM_COND_AL 14

/**
 * What an instruction is and what it touches.
 *
 * The control-flow fields are the point of this structure: is_call and
 * is_return are what let the debugger implement step-over and step-out, and
 * branch_target is what lets a disassembly view be navigable.
 */
typedef struct {
	uint32_t opcode;	/**< The instruction word decoded */
	uint32_t address;	/**< Address it was decoded at */

	ArmInsnClass cls;
	uint8_t cond;		/**< Condition field, 0-15 */
	uint8_t is_conditional;	/**< Condition is neither AL nor NV */

	uint8_t is_branch;	/**< Transfers control (explicitly or by writing PC) */
	uint8_t is_call;	/**< A subroutine call returning to address + 4 */
	uint8_t is_return;	/**< A subroutine return */
	uint8_t writes_pc;	/**< Writes R15 */

	uint8_t branch_target_known; /**< branch_target holds a computed address */
	uint32_t branch_target;

	uint16_t regs_read;	/**< Bitmask of R0-R15 read */
	uint16_t regs_written;	/**< Bitmask of R0-R15 written */

	uint8_t is_swi;
	uint32_t swi_number;	/**< 24-bit SWI comment field, valid if is_swi */
	const char *swi_name;	/**< Static string, or NULL if the SWI is unknown */
} ArmInsnInfo;

/**
 * Resolve an address to a symbol name.
 *
 * @param addr    Address to look up
 * @param offset  Set to the offset of addr past the symbol's base
 * @param ctx     Opaque caller context
 * @return Symbol name, or NULL if the address is not within a known symbol
 */
typedef const char *(*ArmSymbolLookup)(uint32_t addr, uint32_t *offset, void *ctx);

/**
 * Describe an ARM instruction without rendering it.
 *
 * Always fills in `out` - an unrecognised encoding yields ARM_CLASS_UNKNOWN
 * with the control-flow and register fields zeroed, which callers can treat as
 * "assume nothing".
 *
 * @param opcode  The 32-bit ARM instruction word
 * @param address The address of the instruction (used for PC-relative targets)
 * @param out     Filled in with the instruction's description
 * @return Non-zero if the encoding was recognised, zero otherwise
 */
int arm_decode(uint32_t opcode, uint32_t address, ArmInsnInfo *out);

/**
 * Disassemble an ARM instruction.
 *
 * @param opcode  The 32-bit ARM instruction word
 * @param address The address of the instruction (used for PC-relative calculations)
 * @param buffer  Output buffer for the disassembled string
 * @param buflen  Size of the output buffer
 * @return Pointer to buffer on success, or NULL on failure
 */
const char *arm_disasm(uint32_t opcode, uint32_t address, char *buffer, size_t buflen);

/**
 * Disassemble an ARM instruction, annotating a known branch target with its
 * symbol as a trailing "; name+offset" comment.
 *
 * Identical to arm_disasm() when `lookup` is NULL or resolves nothing.
 *
 * @param opcode  The 32-bit ARM instruction word
 * @param address The address of the instruction
 * @param buffer  Output buffer for the disassembled string
 * @param buflen  Size of the output buffer
 * @param lookup  Symbol resolver, or NULL for no annotation
 * @param ctx     Opaque context passed to `lookup`
 * @return Pointer to buffer on success, or NULL on failure
 */
const char *arm_disasm_sym(uint32_t opcode, uint32_t address, char *buffer,
                           size_t buflen, ArmSymbolLookup lookup, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ARM_DISASM_H */
