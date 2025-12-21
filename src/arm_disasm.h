/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

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
 * Decodes ARM (32-bit) instructions into human-readable assembly
 * syntax. Supports the ARMv4 instruction set including:
 *   - Data processing (AND, ORR, ADD, SUB, MOV, CMP, etc.)
 *   - Multiply and multiply-accumulate
 *   - Load/store (LDR, STR, LDRH, LDRSB, etc.)
 *   - Block transfer (LDM, STM)
 *   - Branch and branch-with-link
 *   - Coprocessor instructions
 *   - Status register access (MRS, MSR)
 *   - Software interrupt (SWI)
 */

#ifndef ARM_DISASM_H
#define ARM_DISASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* ARM_DISASM_H */
