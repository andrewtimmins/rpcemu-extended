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
 * arm_disasm.c - ARM instruction disassembler
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

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arm_disasm.h"

/* Condition code names */
static const char *cond_names[16] = {
	"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
	"HI", "LS", "GE", "LT", "GT", "LE", "",   "NV"
};

/* Register names */
static const char *reg_names[16] = {
	"R0",  "R1",  "R2",  "R3",  "R4",  "R5",  "R6",  "R7",
	"R8",  "R9",  "R10", "R11", "R12", "SP",  "LR",  "PC"
};

/* Data processing opcode names */
static const char *dp_names[16] = {
	"AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
	"TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"
};

/* Shift type names */
static const char *shift_names[4] = {
	"LSL", "LSR", "ASR", "ROR"
};

/* Coprocessor register names */
static const char *cp_reg_names[16] = {
	"C0",  "C1",  "C2",  "C3",  "C4",  "C5",  "C6",  "C7",
	"C8",  "C9",  "C10", "C11", "C12", "C13", "C14", "C15"
};

/**
 * Decode the shifter operand for data processing instructions.
 */
static int
decode_shifter(uint32_t opcode, char *buf, size_t buflen, int imm)
{
	if (imm) {
		/* Immediate value with rotation */
		uint32_t imm8 = opcode & 0xFF;
		uint32_t rot = ((opcode >> 8) & 0xF) * 2;
		uint32_t value = (imm8 >> rot) | (imm8 << (32 - rot));
		if (rot == 0) {
			return snprintf(buf, buflen, "#%u", imm8);
		} else {
			return snprintf(buf, buflen, "#0x%X", value);
		}
	} else {
		/* Register with optional shift */
		int rm = opcode & 0xF;
		int shift_type = (opcode >> 5) & 3;
		int reg_shift = (opcode >> 4) & 1;

		if (reg_shift) {
			/* Shift by register */
			int rs = (opcode >> 8) & 0xF;
			return snprintf(buf, buflen, "%s, %s %s",
			                reg_names[rm], shift_names[shift_type], reg_names[rs]);
		} else {
			/* Shift by immediate */
			int shift_amt = (opcode >> 7) & 0x1F;
			if (shift_amt == 0) {
				if (shift_type == 0) {
					/* No shift */
					return snprintf(buf, buflen, "%s", reg_names[rm]);
				} else if (shift_type == 3) {
					/* RRX */
					return snprintf(buf, buflen, "%s, RRX", reg_names[rm]);
				} else {
					/* LSR #32 or ASR #32 */
					return snprintf(buf, buflen, "%s, %s #32",
					                reg_names[rm], shift_names[shift_type]);
				}
			} else {
				return snprintf(buf, buflen, "%s, %s #%d",
				                reg_names[rm], shift_names[shift_type], shift_amt);
			}
		}
	}
}

/**
 * Decode data processing instructions.
 */
static int
disasm_data_processing(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int imm = (opcode >> 25) & 1;
	int op = (opcode >> 21) & 0xF;
	int s = (opcode >> 20) & 1;
	int rn = (opcode >> 16) & 0xF;
	int rd = (opcode >> 12) & 0xF;
	char shifter[64];
	int len;

	(void) address;

	decode_shifter(opcode, shifter, sizeof(shifter), imm);

	/* Test/Compare instructions (no Rd) */
	if (op >= 8 && op <= 11) {
		len = snprintf(buf, buflen, "%s%s %s, %s",
		               dp_names[op], cond_names[cond],
		               reg_names[rn], shifter);
	}
	/* MOV/MVN (no Rn) */
	else if (op == 13 || op == 15) {
		len = snprintf(buf, buflen, "%s%s%s %s, %s",
		               dp_names[op], cond_names[cond], s ? "S" : "",
		               reg_names[rd], shifter);
	}
	/* Standard 3-operand */
	else {
		len = snprintf(buf, buflen, "%s%s%s %s, %s, %s",
		               dp_names[op], cond_names[cond], s ? "S" : "",
		               reg_names[rd], reg_names[rn], shifter);
	}

	return len;
}

/**
 * Decode multiply instructions.
 */
static int
disasm_multiply(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int a = (opcode >> 21) & 1;
	int s = (opcode >> 20) & 1;
	int rd = (opcode >> 16) & 0xF;
	int rn = (opcode >> 12) & 0xF;
	int rs = (opcode >> 8) & 0xF;
	int rm = opcode & 0xF;

	(void) address;

	if (a) {
		return snprintf(buf, buflen, "MLA%s%s %s, %s, %s, %s",
		                cond_names[cond], s ? "S" : "",
		                reg_names[rd], reg_names[rm], reg_names[rs], reg_names[rn]);
	} else {
		return snprintf(buf, buflen, "MUL%s%s %s, %s, %s",
		                cond_names[cond], s ? "S" : "",
		                reg_names[rd], reg_names[rm], reg_names[rs]);
	}
}

/**
 * Decode long multiply instructions (UMULL, UMLAL, SMULL, SMLAL).
 */
static int
disasm_multiply_long(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int u = (opcode >> 22) & 1;
	int a = (opcode >> 21) & 1;
	int s = (opcode >> 20) & 1;
	int rdhi = (opcode >> 16) & 0xF;
	int rdlo = (opcode >> 12) & 0xF;
	int rs = (opcode >> 8) & 0xF;
	int rm = opcode & 0xF;
	const char *mnem;

	(void) address;

	if (u) {
		mnem = a ? "SMLAL" : "SMULL";
	} else {
		mnem = a ? "UMLAL" : "UMULL";
	}

	return snprintf(buf, buflen, "%s%s%s %s, %s, %s, %s",
	                mnem, cond_names[cond], s ? "S" : "",
	                reg_names[rdlo], reg_names[rdhi], reg_names[rm], reg_names[rs]);
}

/**
 * Decode single data transfer (LDR/STR).
 */
static int
disasm_single_transfer(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int imm = !((opcode >> 25) & 1); /* Note: inverted */
	int p = (opcode >> 24) & 1;
	int u = (opcode >> 23) & 1;
	int b = (opcode >> 22) & 1;
	int w = (opcode >> 21) & 1;
	int l = (opcode >> 20) & 1;
	int rn = (opcode >> 16) & 0xF;
	int rd = (opcode >> 12) & 0xF;
	char offset_str[64];
	char addr_str[96];
	const char *mnem = l ? "LDR" : "STR";
	const char *suffix = b ? "B" : "";
	const char *sign = u ? "" : "-";

	(void) address;

	if (imm) {
		/* Immediate offset */
		int offset = opcode & 0xFFF;
		if (offset == 0) {
			offset_str[0] = '\0';
		} else {
			snprintf(offset_str, sizeof(offset_str), ", #%s%d", sign, offset);
		}
	} else {
		/* Register offset with shift */
		int rm = opcode & 0xF;
		int shift_type = (opcode >> 5) & 3;
		int shift_amt = (opcode >> 7) & 0x1F;

		if (shift_amt == 0 && shift_type == 0) {
			snprintf(offset_str, sizeof(offset_str), ", %s%s", sign, reg_names[rm]);
		} else {
			snprintf(offset_str, sizeof(offset_str), ", %s%s, %s #%d",
			         sign, reg_names[rm], shift_names[shift_type], shift_amt);
		}
	}

	if (p) {
		/* Pre-indexed */
		snprintf(addr_str, sizeof(addr_str), "[%s%s]%s",
		         reg_names[rn], offset_str, w ? "!" : "");
	} else {
		/* Post-indexed */
		snprintf(addr_str, sizeof(addr_str), "[%s]%s",
		         reg_names[rn], offset_str);
	}

	return snprintf(buf, buflen, "%s%s%s %s, %s",
	                mnem, cond_names[cond], suffix, reg_names[rd], addr_str);
}

/**
 * Decode halfword/signed data transfer (LDRH/STRH/LDRSB/LDRSH).
 */
static int
disasm_halfword_transfer(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int p = (opcode >> 24) & 1;
	int u = (opcode >> 23) & 1;
	int imm = (opcode >> 22) & 1;
	int w = (opcode >> 21) & 1;
	int l = (opcode >> 20) & 1;
	int rn = (opcode >> 16) & 0xF;
	int rd = (opcode >> 12) & 0xF;
	int sh = (opcode >> 5) & 3;
	char offset_str[64];
	char addr_str[96];
	const char *mnem;
	const char *sign = u ? "" : "-";

	(void) address;

	/* Decode operation type */
	if (l) {
		switch (sh) {
		case 1: mnem = "LDRH"; break;
		case 2: mnem = "LDRSB"; break;
		case 3: mnem = "LDRSH"; break;
		default: mnem = "LDR?"; break;
		}
	} else {
		mnem = "STRH";
	}

	if (imm) {
		/* Immediate offset */
		int offset = ((opcode >> 4) & 0xF0) | (opcode & 0xF);
		if (offset == 0) {
			offset_str[0] = '\0';
		} else {
			snprintf(offset_str, sizeof(offset_str), ", #%s%d", sign, offset);
		}
	} else {
		/* Register offset */
		int rm = opcode & 0xF;
		snprintf(offset_str, sizeof(offset_str), ", %s%s", sign, reg_names[rm]);
	}

	if (p) {
		snprintf(addr_str, sizeof(addr_str), "[%s%s]%s",
		         reg_names[rn], offset_str, w ? "!" : "");
	} else {
		snprintf(addr_str, sizeof(addr_str), "[%s]%s",
		         reg_names[rn], offset_str);
	}

	return snprintf(buf, buflen, "%s%s %s, %s",
	                mnem, cond_names[cond], reg_names[rd], addr_str);
}

/**
 * Decode block data transfer (LDM/STM).
 */
static int
disasm_block_transfer(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int p = (opcode >> 24) & 1;
	int u = (opcode >> 23) & 1;
	int s = (opcode >> 22) & 1;
	int w = (opcode >> 21) & 1;
	int l = (opcode >> 20) & 1;
	int rn = (opcode >> 16) & 0xF;
	uint16_t reglist = opcode & 0xFFFF;
	char reglist_str[128];
	char *ptr = reglist_str;
	int first = 1;
	int i;
	const char *mnem;
	const char *suffix;

	(void) address;

	/* Mnemonic based on direction and indexing */
	if (l) {
		if (p && u) suffix = "IB";       /* Increment Before */
		else if (!p && u) suffix = "IA"; /* Increment After */
		else if (p && !u) suffix = "DB"; /* Decrement Before */
		else suffix = "DA";              /* Decrement After */
		mnem = "LDM";
	} else {
		if (p && u) suffix = "IB";
		else if (!p && u) suffix = "IA";
		else if (p && !u) suffix = "DB";
		else suffix = "DA";
		mnem = "STM";
	}

	/* Build register list */
	*ptr++ = '{';
	for (i = 0; i < 16; i++) {
		if (reglist & (1 << i)) {
			if (!first) {
				*ptr++ = ',';
				*ptr++ = ' ';
			}
			ptr += sprintf(ptr, "%s", reg_names[i]);
			first = 0;
		}
	}
	*ptr++ = '}';
	if (s) {
		*ptr++ = '^';
	}
	*ptr = '\0';

	return snprintf(buf, buflen, "%s%s%s %s%s, %s",
	                mnem, cond_names[cond], suffix,
	                reg_names[rn], w ? "!" : "", reglist_str);
}

/**
 * Decode branch instructions.
 */
static int
disasm_branch(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int l = (opcode >> 24) & 1;
	int32_t offset = opcode & 0x00FFFFFF;

	/* Sign extend 24-bit offset */
	if (offset & 0x00800000) {
		offset |= 0xFF000000;
	}

	/* Calculate target: PC + 8 + (offset << 2) */
	uint32_t target = address + 8 + ((uint32_t) offset << 2);

	return snprintf(buf, buflen, "%s%s 0x%08X",
	                l ? "BL" : "B", cond_names[cond], target);
}

/**
 * RISC OS SWI name lookup table.
 * SWI numbers are structured as:
 *   Bits 17-6: Module chunk number
 *   Bits 5-0:  SWI within chunk
 *   Bit 17 set: X form (returns errors via R0)
 */
typedef struct {
	uint32_t number;
	const char *name;
} swi_entry_t;

/* Common RISC OS SWIs - sorted by number for binary search */
static const swi_entry_t swi_table[] = {
	/* OS SWIs (0x00-0x3F) */
	{ 0x00, "OS_WriteC" },
	{ 0x01, "OS_WriteS" },
	{ 0x02, "OS_Write0" },
	{ 0x03, "OS_NewLine" },
	{ 0x04, "OS_ReadC" },
	{ 0x05, "OS_CLI" },
	{ 0x06, "OS_Byte" },
	{ 0x07, "OS_Word" },
	{ 0x08, "OS_File" },
	{ 0x09, "OS_Args" },
	{ 0x0A, "OS_BGet" },
	{ 0x0B, "OS_BPut" },
	{ 0x0C, "OS_GBPB" },
	{ 0x0D, "OS_Find" },
	{ 0x0E, "OS_ReadLine" },
	{ 0x0F, "OS_Control" },
	{ 0x10, "OS_GetEnv" },
	{ 0x11, "OS_Exit" },
	{ 0x12, "OS_SetEnv" },
	{ 0x13, "OS_IntOn" },
	{ 0x14, "OS_IntOff" },
	{ 0x15, "OS_CallBack" },
	{ 0x16, "OS_EnterOS" },
	{ 0x17, "OS_BreakPt" },
	{ 0x18, "OS_BreakCtrl" },
	{ 0x19, "OS_UnusedSWI" },
	{ 0x1A, "OS_UpdateMEMC" },
	{ 0x1B, "OS_SetCallBack" },
	{ 0x1C, "OS_Mouse" },
	{ 0x1D, "OS_Heap" },
	{ 0x1E, "OS_Module" },
	{ 0x1F, "OS_Claim" },
	{ 0x20, "OS_Release" },
	{ 0x21, "OS_ReadUnsigned" },
	{ 0x22, "OS_GenerateEvent" },
	{ 0x23, "OS_ReadVarVal" },
	{ 0x24, "OS_SetVarVal" },
	{ 0x25, "OS_GSInit" },
	{ 0x26, "OS_GSRead" },
	{ 0x27, "OS_GSTrans" },
	{ 0x28, "OS_BinaryToDecimal" },
	{ 0x29, "OS_FSControl" },
	{ 0x2A, "OS_ChangeDynamicArea" },
	{ 0x2B, "OS_GenerateError" },
	{ 0x2C, "OS_ReadEscapeState" },
	{ 0x2D, "OS_EvaluateExpression" },
	{ 0x2E, "OS_SpriteOp" },
	{ 0x2F, "OS_ReadPalette" },
	{ 0x30, "OS_ServiceCall" },
	{ 0x31, "OS_ReadVduVariables" },
	{ 0x32, "OS_ReadPoint" },
	{ 0x33, "OS_UpCall" },
	{ 0x34, "OS_CallAVector" },
	{ 0x35, "OS_ReadModeVariable" },
	{ 0x36, "OS_RemoveCursors" },
	{ 0x37, "OS_RestoreCursors" },
	{ 0x38, "OS_SWINumberToString" },
	{ 0x39, "OS_SWINumberFromString" },
	{ 0x3A, "OS_ValidateAddress" },
	{ 0x3B, "OS_CallAfter" },
	{ 0x3C, "OS_CallEvery" },
	{ 0x3D, "OS_RemoveTickerEvent" },
	{ 0x3E, "OS_InstallKeyHandler" },
	{ 0x3F, "OS_CheckModeValid" },

	/* OS SWIs continued (0x40-0x7F) */
	{ 0x40, "OS_ChangeEnvironment" },
	{ 0x41, "OS_ClaimScreenMemory" },
	{ 0x42, "OS_ReadMonotonicTime" },
	{ 0x43, "OS_SubstituteArgs" },
	{ 0x44, "OS_PrettyPrint" },
	{ 0x45, "OS_Plot" },
	{ 0x46, "OS_WriteN" },
	{ 0x47, "OS_AddToVector" },
	{ 0x48, "OS_WriteEnv" },
	{ 0x49, "OS_ReadArgs" },
	{ 0x4A, "OS_ReadRAMFsLimits" },
	{ 0x4B, "OS_ClaimDeviceVector" },
	{ 0x4C, "OS_ReleaseDeviceVector" },
	{ 0x4D, "OS_DelinkApplication" },
	{ 0x4E, "OS_RelinkApplication" },
	{ 0x4F, "OS_HeapSort" },
	{ 0x50, "OS_ExitAndDie" },
	{ 0x51, "OS_ReadMemMapInfo" },
	{ 0x52, "OS_ReadMemMapEntries" },
	{ 0x53, "OS_SetMemMapEntries" },
	{ 0x54, "OS_AddCallBack" },
	{ 0x55, "OS_ReadDefaultHandler" },
	{ 0x56, "OS_SetECFOrigin" },
	{ 0x57, "OS_SerialOp" },
	{ 0x58, "OS_ReadSysInfo" },
	{ 0x59, "OS_Confirm" },
	{ 0x5A, "OS_ChangedBox" },
	{ 0x5B, "OS_CRC" },
	{ 0x5C, "OS_ReadDynamicArea" },
	{ 0x5D, "OS_PrintChar" },
	{ 0x5E, "OS_ChangeRedirection" },
	{ 0x5F, "OS_RemoveCallBack" },
	{ 0x60, "OS_FindMemMapEntries" },
	{ 0x61, "OS_SetColour" },
	{ 0x64, "OS_Pointer" },
	{ 0x65, "OS_ScreenMode" },
	{ 0x66, "OS_DynamicArea" },
	{ 0x68, "OS_Memory" },
	{ 0x69, "OS_ClaimProcessorVector" },
	{ 0x6A, "OS_Reset" },
	{ 0x6B, "OS_MMUControl" },
	{ 0x6C, "OS_ResyncTime" },
	{ 0x6D, "OS_PlatformFeatures" },
	{ 0x6E, "OS_SynchroniseCodeAreas" },
	{ 0x6F, "OS_CallASWI" },
	{ 0x70, "OS_AMBControl" },
	{ 0x71, "OS_CallASWIR12" },
	{ 0x73, "OS_LeaveOS" },
	{ 0x75, "OS_IICOp" },
	{ 0x77, "OS_Hardware" },

	/* OS Conversion SWIs (0xC0-0xEC) */
	{ 0xC0, "OS_ConvertStandardDateAndTime" },
	{ 0xC1, "OS_ConvertDateAndTime" },
	{ 0xD0, "OS_ConvertHex1" },
	{ 0xD1, "OS_ConvertHex2" },
	{ 0xD2, "OS_ConvertHex4" },
	{ 0xD3, "OS_ConvertHex6" },
	{ 0xD4, "OS_ConvertHex8" },
	{ 0xD5, "OS_ConvertCardinal1" },
	{ 0xD6, "OS_ConvertCardinal2" },
	{ 0xD7, "OS_ConvertCardinal3" },
	{ 0xD8, "OS_ConvertCardinal4" },
	{ 0xD9, "OS_ConvertInteger1" },
	{ 0xDA, "OS_ConvertInteger2" },
	{ 0xDB, "OS_ConvertInteger3" },
	{ 0xDC, "OS_ConvertInteger4" },
	{ 0xDD, "OS_ConvertBinary1" },
	{ 0xDE, "OS_ConvertBinary2" },
	{ 0xDF, "OS_ConvertBinary3" },
	{ 0xE0, "OS_ConvertBinary4" },
	{ 0xE1, "OS_ConvertSpacedCardinal1" },
	{ 0xE2, "OS_ConvertSpacedCardinal2" },
	{ 0xE3, "OS_ConvertSpacedCardinal3" },
	{ 0xE4, "OS_ConvertSpacedCardinal4" },
	{ 0xE5, "OS_ConvertSpacedInteger1" },
	{ 0xE6, "OS_ConvertSpacedInteger2" },
	{ 0xE7, "OS_ConvertSpacedInteger3" },
	{ 0xE8, "OS_ConvertSpacedInteger4" },
	{ 0xE9, "OS_ConvertFixedNetStation" },
	{ 0xEA, "OS_ConvertNetStation" },
	{ 0xEB, "OS_ConvertFixedFileSize" },
	{ 0xEC, "OS_ConvertFileSize" },

	/* IIC SWI (0x240) */
	{ 0x240, "IIC_Control" },

	/* Cache SWIs (0x280-0x284) */
	{ 0x280, "Cache_Control" },
	{ 0x281, "Cache_Cacheable" },
	{ 0x282, "Cache_Updateable" },
	{ 0x283, "Cache_Disruptive" },
	{ 0x284, "Cache_Flush" },

	/* Econet SWIs (0x40000-0x40021) */
	{ 0x40000, "Econet_CreateReceive" },
	{ 0x40001, "Econet_ExamineReceive" },
	{ 0x40002, "Econet_ReadReceive" },
	{ 0x40003, "Econet_AbandonReceive" },
	{ 0x40004, "Econet_WaitForReception" },
	{ 0x40005, "Econet_EnumerateReceive" },
	{ 0x40006, "Econet_StartTransmit" },
	{ 0x40007, "Econet_PollTransmit" },
	{ 0x40008, "Econet_AbandonTransmit" },
	{ 0x40009, "Econet_DoTransmit" },
	{ 0x4000A, "Econet_ReadLocalStationAndNet" },
	{ 0x4000B, "Econet_ConvertStatusToString" },
	{ 0x4000C, "Econet_ConvertStatusToError" },
	{ 0x4000D, "Econet_ReadProtection" },
	{ 0x4000E, "Econet_SetProtection" },
	{ 0x4000F, "Econet_ReadStationNumber" },
	{ 0x40010, "Econet_PrintBanner" },
	{ 0x40011, "Econet_ReadTransportType" },
	{ 0x40012, "Econet_ReleasePort" },
	{ 0x40013, "Econet_AllocatePort" },
	{ 0x40014, "Econet_DeAllocatePort" },
	{ 0x40015, "Econet_ClaimPort" },
	{ 0x40016, "Econet_StartImmediate" },
	{ 0x40017, "Econet_DoImmediate" },
	{ 0x40018, "Econet_AbandonAndReadReceive" },
	{ 0x40019, "Econet_Version" },
	{ 0x4001A, "Econet_NetworkState" },
	{ 0x4001B, "Econet_PacketSize" },
	{ 0x4001C, "Econet_ReadTransportName" },
	{ 0x4001D, "Econet_InetRxDirect" },
	{ 0x4001E, "Econet_EnumerateMap" },
	{ 0x4001F, "Econet_EnumerateTransmit" },
	{ 0x40020, "Econet_HardwareAddresses" },
	{ 0x40021, "Econet_NetworkParameters" },

	/* NetFS SWIs (0x40040-0x40051) */
	{ 0x40040, "NetFS_ReadFSNumber" },
	{ 0x40041, "NetFS_SetFSNumber" },
	{ 0x40042, "NetFS_ReadFSName" },
	{ 0x40043, "NetFS_SetFSName" },
	{ 0x40044, "NetFS_ReadCurrentContext" },
	{ 0x40045, "NetFS_SetCurrentContext" },
	{ 0x40046, "NetFS_ReadFSTimeouts" },
	{ 0x40047, "NetFS_SetFSTimeouts" },
	{ 0x40048, "NetFS_DoFSOp" },
	{ 0x40049, "NetFS_EnumerateFSList" },
	{ 0x4004A, "NetFS_EnumerateFS" },
	{ 0x4004B, "NetFS_ConvertDate" },
	{ 0x4004C, "NetFS_DoFSOpToGivenFS" },
	{ 0x4004D, "NetFS_UpdateFSList" },
	{ 0x4004E, "NetFS_EnumerateFSContexts" },
	{ 0x4004F, "NetFS_ReadUserId" },
	{ 0x40050, "NetFS_GetObjectUID" },
	{ 0x40051, "NetFS_EnableCache" },

	/* Wimp SWIs (0x400C0-0x400FF) */
	{ 0x400C0, "Wimp_Initialise" },
	{ 0x400C1, "Wimp_CreateWindow" },
	{ 0x400C2, "Wimp_CreateIcon" },
	{ 0x400C3, "Wimp_DeleteWindow" },
	{ 0x400C4, "Wimp_DeleteIcon" },
	{ 0x400C5, "Wimp_OpenWindow" },
	{ 0x400C6, "Wimp_CloseWindow" },
	{ 0x400C7, "Wimp_Poll" },
	{ 0x400C8, "Wimp_RedrawWindow" },
	{ 0x400C9, "Wimp_UpdateWindow" },
	{ 0x400CA, "Wimp_GetRectangle" },
	{ 0x400CB, "Wimp_GetWindowState" },
	{ 0x400CC, "Wimp_GetWindowInfo" },
	{ 0x400CD, "Wimp_SetIconState" },
	{ 0x400CE, "Wimp_GetIconState" },
	{ 0x400CF, "Wimp_GetPointerInfo" },
	{ 0x400D0, "Wimp_DragBox" },
	{ 0x400D1, "Wimp_ForceRedraw" },
	{ 0x400D2, "Wimp_SetCaretPosition" },
	{ 0x400D3, "Wimp_GetCaretPosition" },
	{ 0x400D4, "Wimp_CreateMenu" },
	{ 0x400D5, "Wimp_DecodeMenu" },
	{ 0x400D6, "Wimp_WhichIcon" },
	{ 0x400D7, "Wimp_SetExtent" },
	{ 0x400D8, "Wimp_SetPointerShape" },
	{ 0x400D9, "Wimp_OpenTemplate" },
	{ 0x400DA, "Wimp_CloseTemplate" },
	{ 0x400DB, "Wimp_LoadTemplate" },
	{ 0x400DC, "Wimp_ProcessKey" },
	{ 0x400DD, "Wimp_CloseDown" },
	{ 0x400DE, "Wimp_StartTask" },
	{ 0x400DF, "Wimp_ReportError" },
	{ 0x400E0, "Wimp_GetWindowOutline" },
	{ 0x400E1, "Wimp_PollIdle" },
	{ 0x400E2, "Wimp_PlotIcon" },
	{ 0x400E3, "Wimp_SetMode" },
	{ 0x400E4, "Wimp_SetPalette" },
	{ 0x400E5, "Wimp_ReadPalette" },
	{ 0x400E6, "Wimp_SetColour" },
	{ 0x400E7, "Wimp_SendMessage" },
	{ 0x400E8, "Wimp_CreateSubMenu" },
	{ 0x400E9, "Wimp_SpriteOp" },
	{ 0x400EA, "Wimp_BaseOfSprites" },
	{ 0x400EB, "Wimp_BlockCopy" },
	{ 0x400EC, "Wimp_SlotSize" },
	{ 0x400ED, "Wimp_ReadPixTrans" },
	{ 0x400EE, "Wimp_ClaimFreeMemory" },
	{ 0x400EF, "Wimp_CommandWindow" },
	{ 0x400F0, "Wimp_TextColour" },
	{ 0x400F1, "Wimp_TransferBlock" },
	{ 0x400F2, "Wimp_ReadSysInfo" },
	{ 0x400F3, "Wimp_SetFontColours" },
	{ 0x400F4, "Wimp_GetMenuState" },
	{ 0x400F5, "Wimp_RegisterFilter" },
	{ 0x400F6, "Wimp_AddMessages" },
	{ 0x400F7, "Wimp_RemoveMessages" },
	{ 0x400F8, "Wimp_SetColourMapping" },
	{ 0x400F9, "Wimp_TextOp" },
	{ 0x400FA, "Wimp_SetWatchdogState" },
	{ 0x400FB, "Wimp_Extend" },
	{ 0x400FC, "Wimp_ResizeIcon" },
	{ 0x400FD, "Wimp_AutoScroll" },

	/* Font SWIs (0x40080-0x400BF) */
	{ 0x40080, "Font_CacheAddr" },
	{ 0x40081, "Font_FindFont" },
	{ 0x40082, "Font_LoseFont" },
	{ 0x40083, "Font_ReadDefn" },
	{ 0x40084, "Font_ReadInfo" },
	{ 0x40085, "Font_StringWidth" },
	{ 0x40086, "Font_Paint" },
	{ 0x40087, "Font_Caret" },
	{ 0x40088, "Font_ConverttoOS" },
	{ 0x40089, "Font_Converttopoints" },
	{ 0x4008A, "Font_SetFont" },
	{ 0x4008B, "Font_CurrentFont" },
	{ 0x4008C, "Font_FutureFont" },
	{ 0x4008D, "Font_FindCaret" },
	{ 0x4008E, "Font_CharBBox" },
	{ 0x4008F, "Font_ReadScaleFactor" },
	{ 0x40090, "Font_SetScaleFactor" },
	{ 0x40091, "Font_ListFonts" },
	{ 0x40092, "Font_SetFontColours" },
	{ 0x40093, "Font_SetPalette" },
	{ 0x40094, "Font_ReadThresholds" },
	{ 0x40095, "Font_SetThresholds" },
	{ 0x40096, "Font_FindCaretJ" },
	{ 0x40097, "Font_StringBBox" },
	{ 0x40098, "Font_ReadColourTable" },
	{ 0x40099, "Font_MakeBitmap" },
	{ 0x4009A, "Font_UnCacheFile" },
	{ 0x4009B, "Font_SetFontMax" },
	{ 0x4009C, "Font_ReadFontMax" },
	{ 0x4009D, "Font_ReadFontPrefix" },
	{ 0x4009E, "Font_SwitchOutputToBuffer" },
	{ 0x4009F, "Font_ReadFontMetrics" },
	{ 0x400A0, "Font_DecodeMenu" },
	{ 0x400A1, "Font_ScanString" },
	{ 0x400A2, "Font_SetColourTable" },
	{ 0x400A3, "Font_CurrentRGB" },
	{ 0x400A4, "Font_FutureRGB" },
	{ 0x400A5, "Font_ReadEncodingFilename" },
	{ 0x400A6, "Font_FindField" },
	{ 0x400A7, "Font_ApplyFields" },
	{ 0x400A8, "Font_LookupFont" },

	/* Hourglass SWIs (0x406C0-0x406C7) */
	{ 0x406C0, "Hourglass_On" },
	{ 0x406C1, "Hourglass_Off" },
	{ 0x406C2, "Hourglass_Smash" },
	{ 0x406C3, "Hourglass_Start" },
	{ 0x406C4, "Hourglass_Percentage" },
	{ 0x406C5, "Hourglass_LEDs" },
	{ 0x406C6, "Hourglass_Colours" },

	/* ColourTrans SWIs (0x40740-0x4075F) */
	{ 0x40740, "ColourTrans_SelectTable" },
	{ 0x40741, "ColourTrans_SelectGCOLTable" },
	{ 0x40742, "ColourTrans_ReturnGCOL" },
	{ 0x40743, "ColourTrans_SetGCOL" },
	{ 0x40744, "ColourTrans_ReturnColourNumber" },
	{ 0x40745, "ColourTrans_ReturnGCOLForMode" },
	{ 0x40746, "ColourTrans_ReturnColourNumberForMode" },
	{ 0x40747, "ColourTrans_ReturnOppGCOL" },
	{ 0x40748, "ColourTrans_SetOppGCOL" },
	{ 0x40749, "ColourTrans_ReturnOppColourNumber" },
	{ 0x4074A, "ColourTrans_ReturnOppGCOLForMode" },
	{ 0x4074B, "ColourTrans_ReturnOppColourNumberForMode" },
	{ 0x4074C, "ColourTrans_GCOLToColourNumber" },
	{ 0x4074D, "ColourTrans_ColourNumberToGCOL" },
	{ 0x4074E, "ColourTrans_ReturnFontColours" },
	{ 0x4074F, "ColourTrans_SetFontColours" },
	{ 0x40750, "ColourTrans_InvalidateCache" },
	{ 0x40751, "ColourTrans_SetCalibration" },
	{ 0x40752, "ColourTrans_ReadCalibration" },
	{ 0x40753, "ColourTrans_ConvertDeviceColour" },
	{ 0x40754, "ColourTrans_ConvertDevicePalette" },
	{ 0x40755, "ColourTrans_ConvertRGBToCIE" },
	{ 0x40756, "ColourTrans_ConvertCIEToRGB" },
	{ 0x40757, "ColourTrans_WriteCalibrationToFile" },
	{ 0x40758, "ColourTrans_ConvertRGBToHSV" },
	{ 0x40759, "ColourTrans_ConvertHSVToRGB" },
	{ 0x4075A, "ColourTrans_ConvertRGBToCMYK" },
	{ 0x4075B, "ColourTrans_ConvertCMYKToRGB" },
	{ 0x4075C, "ColourTrans_ReadPalette" },
	{ 0x4075D, "ColourTrans_WritePalette" },
	{ 0x4075E, "ColourTrans_SetColour" },
	{ 0x4075F, "ColourTrans_MiscOp" },
	{ 0x40760, "ColourTrans_WriteLoadingsToFile" },
	{ 0x40761, "ColourTrans_SetTextColour" },
	{ 0x40762, "ColourTrans_SetOppTextColour" },
	{ 0x40763, "ColourTrans_GenerateTable" },

	/* MessageTrans SWIs (0x41500-0x4150A) */
	{ 0x41500, "MessageTrans_FileInfo" },
	{ 0x41501, "MessageTrans_OpenFile" },
	{ 0x41502, "MessageTrans_Lookup" },
	{ 0x41503, "MessageTrans_MakeMenus" },
	{ 0x41504, "MessageTrans_CloseFile" },
	{ 0x41505, "MessageTrans_EnumerateTokens" },
	{ 0x41506, "MessageTrans_ErrorLookup" },
	{ 0x41507, "MessageTrans_GSLookup" },
	{ 0x41508, "MessageTrans_CopyError" },
	{ 0x41509, "MessageTrans_Dictionary" },

	/* PDumper SWIs (0x41B00-0x41B09) */
	{ 0x41B00, "PDumper_Info" },
	{ 0x41B01, "PDumper_Claim" },
	{ 0x41B02, "PDumper_Free" },
	{ 0x41B03, "PDumper_Find" },
	{ 0x41B04, "PDumper_StartJob" },
	{ 0x41B05, "PDumper_TidyJob" },
	{ 0x41B06, "PDumper_SetColour" },
	{ 0x41B07, "PDumper_PrepareStrip" },
	{ 0x41B08, "PDumper_LookupError" },
	{ 0x41B09, "PDumper_CopyFilename" },

	/* ResourceFS SWIs (0x41B40-0x41B41) */
	{ 0x41B40, "ResourceFS_RegisterFiles" },
	{ 0x41B41, "ResourceFS_DeregisterFiles" },

	/* CDFS SWIs (0x41E80-0x41E88) */
	{ 0x41E80, "CDFS_ConvertDriveToDevice" },
	{ 0x41E81, "CDFS_SetBufferSize" },
	{ 0x41E82, "CDFS_GetBufferSize" },
	{ 0x41E83, "CDFS_SetNumberOfDrives" },
	{ 0x41E84, "CDFS_GetNumberOfDrives" },
	{ 0x41E85, "CDFS_GiveFileType" },
	{ 0x41E86, "CDFS_DescribeDisc" },
	{ 0x41E87, "CDFS_WhereIsFile" },
	{ 0x41E88, "CDFS_Truncation" },

	/* DragASprite SWIs (0x42400-0x42401) */
	{ 0x42400, "DragASprite_Start" },
	{ 0x42401, "DragASprite_Stop" },

	/* Filter SWIs (0x42640-0x42643) */
	{ 0x42640, "Filter_RegisterPreFilter" },
	{ 0x42641, "Filter_RegisterPostFilter" },
	{ 0x42642, "Filter_DeRegisterPreFilter" },
	{ 0x42643, "Filter_DeRegisterPostFilter" },

	/* Territory SWIs (0x43040-0x4305F) */
	{ 0x43040, "Territory_Number" },
	{ 0x43041, "Territory_Register" },
	{ 0x43042, "Territory_Deregister" },
	{ 0x43043, "Territory_NumberToName" },
	{ 0x43044, "Territory_Exists" },
	{ 0x43045, "Territory_AlphabetNumberToName" },
	{ 0x43046, "Territory_SelectAlphabet" },
	{ 0x43047, "Territory_SetTime" },
	{ 0x43048, "Territory_ReadCurrentTimeZone" },
	{ 0x43049, "Territory_ConvertTimeToUTCOrdinals" },
	{ 0x4304A, "Territory_ReadTimeZones" },
	{ 0x4304B, "Territory_ConvertDateAndTime" },
	{ 0x4304C, "Territory_ConvertStandardDateAndTime" },
	{ 0x4304D, "Territory_ConvertStandardDate" },
	{ 0x4304E, "Territory_ConvertStandardTime" },
	{ 0x4304F, "Territory_ConvertTimeToOrdinals" },
	{ 0x43050, "Territory_ConvertTimeStringToOrdinals" },
	{ 0x43051, "Territory_ConvertOrdinalsToTime" },
	{ 0x43052, "Territory_Alphabet" },
	{ 0x43053, "Territory_AlphabetIdentifier" },
	{ 0x43054, "Territory_SelectKeyboardHandler" },
	{ 0x43055, "Territory_WriteDirection" },
	{ 0x43056, "Territory_CharacterPropertyTable" },
	{ 0x43057, "Territory_LowerCaseTable" },
	{ 0x43058, "Territory_UpperCaseTable" },
	{ 0x43059, "Territory_ControlTable" },
	{ 0x4305A, "Territory_PlainTable" },
	{ 0x4305B, "Territory_ValueTable" },
	{ 0x4305C, "Territory_RepresentationTable" },
	{ 0x4305D, "Territory_Collate" },
	{ 0x4305E, "Territory_ReadSymbols" },
	{ 0x4305F, "Territory_ReadCalendarInformation" },
	{ 0x43060, "Territory_NameToNumber" },
	{ 0x43061, "Territory_TransformString" },
	{ 0x43063, "Territory_ConvertTextToString" },
	{ 0x43064, "Territory_DaylightRules" },

	/* TaskManager SWIs (0x42680-0x42683) */
	{ 0x42680, "TaskManager_TaskNameFromHandle" },
	{ 0x42681, "TaskManager_EnumerateTasks" },
	{ 0x42682, "TaskManager_Shutdown" },
	{ 0x42683, "TaskManager_StartTask" },

	/* Squash SWIs (0x42700-0x42701) */
	{ 0x42700, "Squash_Compress" },
	{ 0x42701, "Squash_Decompress" },

	/* DeviceFS SWIs (0x42740-0x42747) */
	{ 0x42740, "DeviceFS_Register" },
	{ 0x42741, "DeviceFS_Deregister" },
	{ 0x42742, "DeviceFS_RegisterObjects" },
	{ 0x42743, "DeviceFS_DeregisterObjects" },
	{ 0x42744, "DeviceFS_CallDevice" },
	{ 0x42745, "DeviceFS_Threshold" },
	{ 0x42746, "DeviceFS_ReceivedCharacter" },
	{ 0x42747, "DeviceFS_TransmitCharacter" },

	/* BASICTrans SWIs (0x42C80-0x42C82) */
	{ 0x42C80, "BASICTrans_HELP" },
	{ 0x42C81, "BASICTrans_Error" },
	{ 0x42C82, "BASICTrans_Message" },

	/* Parallel SWIs (0x42EC0-0x42EC1) */
	{ 0x42EC0, "Parallel_HardwareAddress" },
	{ 0x42EC1, "Parallel_Op" },

	/* Portable SWIs (0x42FC0-0x42FC4) */
	{ 0x42FC0, "Portable_Speed" },
	{ 0x42FC1, "Portable_Control" },
	{ 0x42FC2, "Portable_ReadBMUVariable" },
	{ 0x42FC3, "Portable_WriteBMUVariable" },
	{ 0x42FC4, "Portable_CommandBMU" },

	/* ScreenBlanker SWI (0x43100) */
	{ 0x43100, "ScreenBlanker_Control" },

	/* TaskWindow SWIs (0x43380-0x43384) */
	{ 0x43380, "TaskWindow_TaskInfo" },

	/* Joystick SWIs (0x43F40-0x43F42) */
	{ 0x43F40, "Joystick_Read" },
	{ 0x43F41, "Joystick_CalibrateTopRight" },
	{ 0x43F42, "Joystick_CalibrateBottomLeft" },

	/* FSLock SWIs (0x44780-0x44782) */
	{ 0x44780, "FSLock_Version" },
	{ 0x44781, "FSLock_Status" },
	{ 0x44782, "FSLock_ChangeStatus" },

	/* DOSFS SWIs (0x44B00-0x44B01) */
	{ 0x44B00, "DOSFS_DiscFormat" },
	{ 0x44B01, "DOSFS_LayoutStructure" },

	/* Toolbox SWIs (0x44EC0-0x44EFF) */
	{ 0x44EC0, "Toolbox_CreateObject" },
	{ 0x44EC1, "Toolbox_DeleteObject" },
	{ 0x44EC2, "Toolbox_CopyObject" },
	{ 0x44EC3, "Toolbox_ShowObject" },
	{ 0x44EC4, "Toolbox_HideObject" },
	{ 0x44EC5, "Toolbox_GetObjectState" },
	{ 0x44EC6, "Toolbox_ObjectMiscOp" },
	{ 0x44EC7, "Toolbox_SetClientHandle" },
	{ 0x44EC8, "Toolbox_GetClientHandle" },
	{ 0x44EC9, "Toolbox_GetObjectClass" },
	{ 0x44ECA, "Toolbox_GetParent" },
	{ 0x44ECB, "Toolbox_GetAncestor" },
	{ 0x44ECC, "Toolbox_GetTemplateName" },
	{ 0x44ECD, "Toolbox_RaiseToolboxEvent" },
	{ 0x44ECE, "Toolbox_GetSysInfo" },
	{ 0x44ECF, "Toolbox_Initialise" },
	{ 0x44ED0, "Toolbox_LoadResources" },
	{ 0x44ED1, "Toolbox_DeRegisterObjectModule" },
	{ 0x44ED2, "Toolbox_TemplateLookUp" },
	{ 0x44ED3, "Toolbox_GetInternalHandle" },
	{ 0x44ED4, "Toolbox_RegisterObjectModule" },
	{ 0x44ED5, "Toolbox_RegisterPreFilter" },
	{ 0x44ED6, "Toolbox_RegisterPostFilter" },

	/* DrawFile SWIs (0x45540-0x45542) */
	{ 0x45540, "DrawFile_Render" },
	{ 0x45541, "DrawFile_BBox" },
	{ 0x45542, "DrawFile_DeclareFonts" },

	/* DMA SWIs (0x46140-0x46146) */
	{ 0x46140, "DMA_RegisterChannel" },
	{ 0x46141, "DMA_DeregisterChannel" },
	{ 0x46142, "DMA_QueueTransfer" },
	{ 0x46143, "DMA_TerminateTransfer" },
	{ 0x46144, "DMA_SuspendTransfer" },
	{ 0x46145, "DMA_ResumeTransfer" },
	{ 0x46146, "DMA_ExamineTransfer" },

	/* ColourPicker SWIs (0x47700-0x47708) */
	{ 0x47700, "ColourPicker_RegisterModel" },
	{ 0x47701, "ColourPicker_DeregisterModel" },
	{ 0x47702, "ColourPicker_OpenDialogue" },
	{ 0x47703, "ColourPicker_CloseDialogue" },
	{ 0x47704, "ColourPicker_UpdateDialogue" },
	{ 0x47705, "ColourPicker_ReadDialogue" },
	{ 0x47706, "ColourPicker_SetColour" },
	{ 0x47707, "ColourPicker_HelpReply" },
	{ 0x47708, "ColourPicker_ModelSWI" },

	/* Freeway SWIs (0x47A80-0x47A84) */
	{ 0x47A80, "Freeway_Register" },
	{ 0x47A81, "Freeway_Write" },
	{ 0x47A82, "Freeway_Read" },
	{ 0x47A83, "Freeway_Enumerate" },
	{ 0x47A84, "Freeway_Status" },

	/* ShareFS SWIs (0x47AC0-0x47AC2) */
	{ 0x47AC0, "ShareFS_CreateShare" },
	{ 0x47AC1, "ShareFS_StopShare" },
	{ 0x47AC2, "ShareFS_EnumerateShares" },

	/* ScreenModes SWI (0x487C0) */
	{ 0x487C0, "ScreenModes_ReadInfo" },

	/* JPEG SWIs (0x49980-0x49986) */
	{ 0x49980, "JPEG_Info" },
	{ 0x49981, "JPEG_FileInfo" },
	{ 0x49982, "JPEG_PlotScaled" },
	{ 0x49983, "JPEG_PlotFileScaled" },
	{ 0x49984, "JPEG_PlotTransformed" },
	{ 0x49985, "JPEG_PlotFileTransformed" },
	{ 0x49986, "JPEG_PDriverIntercept" },

	/* DragAnObject SWIs (0x49C40-0x49C41) */
	{ 0x49C40, "DragAnObject_Start" },
	{ 0x49C41, "DragAnObject_Stop" },

	/* CompressJPEG SWIs (0x4A500-0x4A502) */
	{ 0x4A500, "CompressJPEG_Start" },
	{ 0x4A501, "CompressJPEG_WriteLine" },
	{ 0x4A502, "CompressJPEG_Finish" },

	/* Mbuf SWIs (0x4A580-0x4A584) */
	{ 0x4A580, "Mbuf_OpenSession" },
	{ 0x4A581, "Mbuf_CloseSession" },
	{ 0x4A582, "Mbuf_Memory" },
	{ 0x4A583, "Mbuf_Statistic" },
	{ 0x4A584, "Mbuf_Control" },

	/* ATAPI SWI (0x4A740) */
	{ 0x4A740, "ATAPI_GetDrives" },

	/* PDriver SWIs (0x80140-0x8015D) */
	{ 0x80140, "PDriver_Info" },
	{ 0x80141, "PDriver_SetInfo" },
	{ 0x80142, "PDriver_CheckFeatures" },
	{ 0x80143, "PDriver_PageSize" },
	{ 0x80144, "PDriver_SetPageSize" },
	{ 0x80145, "PDriver_SelectJob" },
	{ 0x80146, "PDriver_CurrentJob" },
	{ 0x80147, "PDriver_FontSWI" },
	{ 0x80148, "PDriver_EndJob" },
	{ 0x80149, "PDriver_AbortJob" },
	{ 0x8014A, "PDriver_Reset" },
	{ 0x8014B, "PDriver_GiveRectangle" },
	{ 0x8014C, "PDriver_DrawPage" },
	{ 0x8014D, "PDriver_GetRectangle" },
	{ 0x8014E, "PDriver_CancelJob" },
	{ 0x8014F, "PDriver_ScreenDump" },
	{ 0x80150, "PDriver_EnumerateJobs" },
	{ 0x80151, "PDriver_SetPrinter" },
	{ 0x80152, "PDriver_CancelJobWithError" },
	{ 0x80153, "PDriver_SelectIllustration" },
	{ 0x80154, "PDriver_InsertIllustration" },
	{ 0x80155, "PDriver_DeclareFont" },
	{ 0x80156, "PDriver_DeclareDriver" },
	{ 0x80157, "PDriver_RemoveDriver" },
	{ 0x80158, "PDriver_SelectDriver" },
	{ 0x80159, "PDriver_EnumerateDrivers" },
	{ 0x8015A, "PDriver_MiscOp" },
	{ 0x8015B, "PDriver_MiscOpForDriver" },
	{ 0x8015C, "PDriver_SetDriver" },
	{ 0x8015D, "PDriver_JTABLE" },

	/* Window SWIs (0x82880-0x828BF) */
	{ 0x82880, "Window_ClassSWI" },
	{ 0x82881, "Window_PostFilter" },
	{ 0x82882, "Window_PreFilter" },
	{ 0x82883, "Window_GetPointerInfo" },
	{ 0x82884, "Window_WimpToToolbox" },
	{ 0x828C0, "Window_RegisterExternal" },
	{ 0x828C1, "Window_DeregisterExternal" },
	{ 0x828C2, "Window_ExtractGadgetInfo" },
	{ 0x828C3, "Window_PlotGadget" },

	/* Iconbar SWI (0x82900) */
	{ 0x82900, "Iconbar_ClassSWI" },

	/* Menu SWI (0x82940) */
	{ 0x82940, "Menu_ClassSWI" },

	/* ColourDbox SWI (0x829C0) */
	{ 0x829C0, "ColourDbox_ClassSWI" },

	/* ColourMenu SWI (0x82980) */
	{ 0x82980, "ColourMenu_ClassSWI" },

	/* DCS SWI (0x82A80) */
	{ 0x82A80, "DCS_ClassSWI" },

	/* FileInfo SWI (0x82AC0) */
	{ 0x82AC0, "FileInfo_ClassSWI" },

	/* FontDbox SWI (0x82A00) */
	{ 0x82A00, "FontDbox_ClassSWI" },

	/* FontMenu SWI (0x82A40) */
	{ 0x82A40, "FontMenu_ClassSWI" },

	/* PrintDbox SWI (0x82B00) */
	{ 0x82B00, "PrintDbox_ClassSWI" },

	/* ProgInfo SWI (0x82B40) */
	{ 0x82B40, "ProgInfo_ClassSWI" },

	/* SaveAs SWI (0x82BC0) */
	{ 0x82BC0, "SaveAs_ClassSWI" },

	/* Scale SWI (0x82C00) */
	{ 0x82C00, "Scale_ClassSWI" },

	/* Quit SWI (0x82A80) - shares with DCS */

	/* FPEmulator SWIs (0x40480-0x40481) */
	{ 0x40480, "FPEmulator_Version" },
	{ 0x40481, "FPEmulator_DeactivateContext" },
	{ 0x40482, "FPEmulator_ActivateContext" },
	{ 0x40483, "FPEmulator_ChangedContext" },
	{ 0x40484, "FPEmulator_ContextLength" },
	{ 0x40485, "FPEmulator_InitContext" },
	{ 0x40486, "FPEmulator_ExceptionDump" },
	{ 0x40487, "FPEmulator_Abort" },
	{ 0x40488, "FPEmulator_LoadContext" },
	{ 0x40489, "FPEmulator_SaveContext" },

	/* SharedCLibrary SWIs (0x80680-0x80681) */
	{ 0x80680, "SharedCLibrary_LibInitAPCS_A" },
	{ 0x80681, "SharedCLibrary_LibInitAPCS_R" },
	{ 0x80682, "SharedCLibrary_LibInitModule" },

	/* FileCore SWIs (0x40540-0x4055F) */
	{ 0x40540, "FileCore_DiscOp" },
	{ 0x40541, "FileCore_Create" },
	{ 0x40542, "FileCore_Drives" },
	{ 0x40543, "FileCore_FreeSpace" },
	{ 0x40544, "FileCore_FloppyStructure" },
	{ 0x40545, "FileCore_DescribeDisc" },
	{ 0x40546, "FileCore_DiscardReadSectorsCache" },
	{ 0x40547, "FileCore_DiscFormat" },
	{ 0x40548, "FileCore_LayoutStructure" },
	{ 0x40549, "FileCore_MiscOp" },
	{ 0x4054A, "FileCore_SectorDiscOp" },
	{ 0x4054B, "FileCore_FreeSpace64" },
	{ 0x4054C, "FileCore_DiscOp64" },
	{ 0x4054D, "FileCore_Features" },

	/* Shell SWIs (0x405C0-0x405C1) */
	{ 0x405C0, "Shell_Create" },
	{ 0x405C1, "Shell_Destroy" },

	/* RamFS SWIs (0x40780-0x40785) */
	{ 0x40780, "RamFS_DiscOp" },
	{ 0x40782, "RamFS_Drives" },
	{ 0x40783, "RamFS_FreeSpace" },
	{ 0x40785, "RamFS_DescribeDisc" },

	/* ADFS SWIs (0x40240-0x4025F) */
	{ 0x40240, "ADFS_DiscOp" },
	{ 0x40241, "ADFS_HDC" },
	{ 0x40242, "ADFS_Drives" },
	{ 0x40243, "ADFS_FreeSpace" },
	{ 0x40244, "ADFS_Retries" },
	{ 0x40245, "ADFS_DescribeDisc" },
	{ 0x40246, "ADFS_VetFormat" },
	{ 0x40247, "ADFS_FlpProcessDCB" },
	{ 0x40248, "ADFS_ControllerType" },
	{ 0x40249, "ADFS_PowerControl" },
	{ 0x4024A, "ADFS_SetIDEController" },
	{ 0x4024B, "ADFS_IDEUserOp" },
	{ 0x4024C, "ADFS_MiscOp" },
	{ 0x4024D, "ADFS_SectorDiscOp" },
	{ 0x4024E, "ADFS_NOP2" },
	{ 0x4024F, "ADFS_NOP3" },
	{ 0x40250, "ADFS_ECCSAndRetries" },
	{ 0x40251, "ADFS_LockIDE" },
	{ 0x40252, "ADFS_FreeSpace64" },
	{ 0x40253, "ADFS_IDEDeviceInfo" },
	{ 0x40254, "ADFS_DiscOp64" },
	{ 0x40255, "ADFS_ATAPIOp" },

	/* Podule SWIs (0x40280-0x4028E) */
	{ 0x40280, "Podule_ReadID" },
	{ 0x40281, "Podule_ReadHeader" },
	{ 0x40282, "Podule_EnumerateChunks" },
	{ 0x40283, "Podule_ReadChunk" },
	{ 0x40284, "Podule_ReadBytes" },
	{ 0x40285, "Podule_WriteBytes" },
	{ 0x40286, "Podule_CallLoader" },
	{ 0x40287, "Podule_RawRead" },
	{ 0x40288, "Podule_RawWrite" },
	{ 0x40289, "Podule_HardwareAddress" },
	{ 0x4028A, "Podule_EnumerateChunksWithInfo" },
	{ 0x4028B, "Podule_HardwareAddresses" },
	{ 0x4028C, "Podule_ReturnNumber" },
	{ 0x4028D, "Podule_ReadInfo" },
	{ 0x4028E, "Podule_SetSpeed" },

	/* Debugger SWI (0x40380) */
	{ 0x40380, "Debugger_Disassemble" },

	/* Sound SWIs (0x40140-0x4017F) */
	{ 0x40140, "Sound_Configure" },
	{ 0x40141, "Sound_Enable" },
	{ 0x40142, "Sound_Stereo" },
	{ 0x40143, "Sound_Speaker" },
	{ 0x40144, "Sound_Mode" },
	{ 0x40145, "Sound_LinearHandler" },
	{ 0x40146, "Sound_SampleRate" },

	/* NetPrint SWIs (0x40200-0x40208) */
	{ 0x40200, "NetPrint_ReadPSNumber" },
	{ 0x40201, "NetPrint_SetPSNumber" },
	{ 0x40202, "NetPrint_ReadPSName" },
	{ 0x40203, "NetPrint_SetPSName" },
	{ 0x40204, "NetPrint_ReadPSTimeouts" },
	{ 0x40205, "NetPrint_SetPSTimeouts" },
	{ 0x40206, "NetPrint_BindPSName" },
	{ 0x40207, "NetPrint_ListServers" },
	{ 0x40208, "NetPrint_ConvertStatusToString" },

	{ 0x40180, "Sound_Volume" },
	{ 0x40181, "Sound_SoundLog" },
	{ 0x40182, "Sound_LogScale" },
	{ 0x40183, "Sound_InstallVoice" },
	{ 0x40184, "Sound_RemoveVoice" },
	{ 0x40185, "Sound_AttachVoice" },
	{ 0x40186, "Sound_ControlPacked" },
	{ 0x40187, "Sound_Tuning" },
	{ 0x40188, "Sound_Pitch" },
	{ 0x40189, "Sound_Control" },
	{ 0x4018A, "Sound_AttachNamedVoice" },
	{ 0x4018B, "Sound_ReadControlBlock" },
	{ 0x4018C, "Sound_WriteControlBlock" },
	{ 0x401C0, "Sound_QInit" },
	{ 0x401C1, "Sound_QSchedule" },
	{ 0x401C2, "Sound_QRemove" },
	{ 0x401C3, "Sound_QFree" },
	{ 0x401C4, "Sound_QSDispatch" },
	{ 0x401C5, "Sound_QTempo" },
	{ 0x401C6, "Sound_QBeat" },
	{ 0x401C7, "Sound_QInterface" },

	/* Draw SWIs (0x40700-0x4070F) */
	{ 0x40700, "Draw_ProcessPath" },
	{ 0x40701, "Draw_ProcessPathFP" },
	{ 0x40702, "Draw_Fill" },
	{ 0x40703, "Draw_FillFP" },
	{ 0x40704, "Draw_Stroke" },
	{ 0x40705, "Draw_StrokeFP" },
	{ 0x40706, "Draw_StrokePath" },
	{ 0x40707, "Draw_StrokePathFP" },
	{ 0x40708, "Draw_FlattenPath" },
	{ 0x40709, "Draw_FlattenPathFP" },
	{ 0x4070A, "Draw_TransformPath" },
	{ 0x4070B, "Draw_TransformPathFP" },

	/* Socket/Internet SWIs (0x41200-0x41219) */
	{ 0x41200, "Socket_Creat" },
	{ 0x41201, "Socket_Bind" },
	{ 0x41202, "Socket_Listen" },
	{ 0x41203, "Socket_Accept" },
	{ 0x41204, "Socket_Connect" },
	{ 0x41205, "Socket_Recv" },
	{ 0x41206, "Socket_Recvfrom" },
	{ 0x41207, "Socket_Recvmsg" },
	{ 0x41208, "Socket_Send" },
	{ 0x41209, "Socket_Sendto" },
	{ 0x4120A, "Socket_Sendmsg" },
	{ 0x4120B, "Socket_Shutdown" },
	{ 0x4120C, "Socket_Setsockopt" },
	{ 0x4120D, "Socket_Getsockopt" },
	{ 0x4120E, "Socket_Getpeername" },
	{ 0x4120F, "Socket_Getsockname" },
	{ 0x41210, "Socket_Close" },
	{ 0x41211, "Socket_Select" },
	{ 0x41212, "Socket_Ioctl" },
	{ 0x41213, "Socket_Read" },
	{ 0x41214, "Socket_Write" },
	{ 0x41215, "Socket_Stat" },
	{ 0x41216, "Socket_Readv" },
	{ 0x41217, "Socket_Writev" },
	{ 0x41218, "Socket_Gettsize" },
	{ 0x41219, "Socket_Sendtosm" },

	/* CD SWIs (0x41240-0x4126A) */
	{ 0x41240, "CD_Version" },
	{ 0x41241, "CD_ReadData" },
	{ 0x41242, "CD_SeekTo" },
	{ 0x41243, "CD_DriveStatus" },
	{ 0x41244, "CD_DriveReady" },
	{ 0x41245, "CD_GetParameters" },
	{ 0x41246, "CD_SetParameters" },
	{ 0x41247, "CD_OpenDrawer" },
	{ 0x41248, "CD_EjectButton" },
	{ 0x41249, "CD_EnquireAddress" },
	{ 0x4124A, "CD_EnquireDataMode" },
	{ 0x4124B, "CD_PlayAudio" },
	{ 0x4124C, "CD_PlayTrack" },
	{ 0x4124D, "CD_AudioPause" },
	{ 0x4124E, "CD_EnquireTrack" },
	{ 0x4124F, "CD_ReadSubChannel" },
	{ 0x41250, "CD_CheckDrive" },
	{ 0x41251, "CD_DiscChanged" },
	{ 0x41252, "CD_StopDisc" },
	{ 0x41253, "CD_DiscUsed" },
	{ 0x41254, "CD_AudioStatus" },
	{ 0x41255, "CD_Inquiry" },
	{ 0x41256, "CD_DiscHasChanged" },
	{ 0x41257, "CD_Control" },
	{ 0x41258, "CD_Supported" },
	{ 0x41259, "CD_Prefetch" },
	{ 0x4125A, "CD_Reset" },
	{ 0x4125B, "CD_CloseDrawer" },
	{ 0x4125C, "CD_IsDrawerLocked" },
	{ 0x4125D, "CD_AudioControl" },
	{ 0x4125E, "CD_LastError" },
	{ 0x4125F, "CD_AudioLevel" },
	{ 0x41260, "CD_Register" },
	{ 0x41261, "CD_Unregister" },
	{ 0x41262, "CD_ByteCopy" },
	{ 0x41263, "CD_Identify" },
	{ 0x41264, "CD_ConvertToLBA" },
	{ 0x41265, "CD_ConvertToMSF" },
	{ 0x41266, "CD_ReadAudio" },
	{ 0x41267, "CD_ReadUserData" },
	{ 0x41268, "CD_SeekUserData" },
	{ 0x41269, "CD_GetAudioParms" },
	{ 0x4126A, "CD_SetAudioParms" },

	/* SharedSound SWIs (0x4B440-0x4B44F) */
	{ 0x4B440, "SharedSound_InstallHandler" },
	{ 0x4B441, "SharedSound_RemoveHandler" },
	{ 0x4B442, "SharedSound_HandlerInfo" },
	{ 0x4B443, "SharedSound_HandlerVolume" },
	{ 0x4B444, "SharedSound_HandlerSampleType" },
	{ 0x4B445, "SharedSound_HandlerPause" },
	{ 0x4B446, "SharedSound_SampleRate" },
	{ 0x4B447, "SharedSound_InstallDriver" },
	{ 0x4B448, "SharedSound_RemoveDriver" },
	{ 0x4B449, "SharedSound_DriverInfo" },
	{ 0x4B44A, "SharedSound_DriverMixer" },
	{ 0x4B44B, "SharedSound_CheckDriver" },

	/* Free SWIs (0x444C0-0x444C2) */
	{ 0x444C0, "Free_Register" },
	{ 0x444C1, "Free_DeRegister" },

	/* Buffer SWIs (0x42940-0x4294F) */
	{ 0x42940, "Buffer_Create" },
	{ 0x42941, "Buffer_Remove" },
	{ 0x42942, "Buffer_Register" },
	{ 0x42943, "Buffer_Deregister" },
	{ 0x42944, "Buffer_ModifyFlags" },
	{ 0x42945, "Buffer_LinkDevice" },
	{ 0x42946, "Buffer_UnlinkDevice" },
	{ 0x42947, "Buffer_GetInfo" },
	{ 0x42948, "Buffer_Threshold" },
	{ 0x42949, "Buffer_InternalInfo" },
};

#define SWI_TABLE_SIZE (sizeof(swi_table) / sizeof(swi_table[0]))

/**
 * Look up a SWI name by number.
 * Returns the name if found, or NULL if not in table.
 * Uses linear search since the table is grouped by module rather than sorted.
 */
static const char *
lookup_swi_name(uint32_t swi_num)
{
	/* Strip the X bit for lookup */
	uint32_t base_swi = swi_num & 0x1FFFF;
	size_t i;

	for (i = 0; i < SWI_TABLE_SIZE; i++) {
		if (swi_table[i].number == base_swi) {
			return swi_table[i].name;
		}
	}

	return NULL;
}

/**
 * Decode SWI instruction.
 */
static int
disasm_swi(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	uint32_t imm = opcode & 0x00FFFFFF;
	const char *name;
	int is_x;

	(void) address;

	/* Check for X form (error-returning) */
	is_x = (imm & 0x20000) != 0;
	name = lookup_swi_name(imm);

	if (name) {
		/* Known SWI - show name */
		if (is_x) {
			return snprintf(buf, buflen, "SWI%s X%s",
			                cond_names[cond], name);
		} else {
			return snprintf(buf, buflen, "SWI%s %s",
			                cond_names[cond], name);
		}
	} else {
		/* Unknown SWI - show number */
		if (imm < 0x200) {
			return snprintf(buf, buflen, "SWI%s 0x%X", cond_names[cond], imm);
		} else {
			return snprintf(buf, buflen, "SWI%s 0x%06X", cond_names[cond], imm);
		}
	}
}

/**
 * Decode coprocessor data transfer (LDC/STC).
 */
static int
disasm_coproc_transfer(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int p = (opcode >> 24) & 1;
	int u = (opcode >> 23) & 1;
	int n = (opcode >> 22) & 1;
	int w = (opcode >> 21) & 1;
	int l = (opcode >> 20) & 1;
	int rn = (opcode >> 16) & 0xF;
	int crd = (opcode >> 12) & 0xF;
	int cpn = (opcode >> 8) & 0xF;
	int offset = (opcode & 0xFF) << 2;
	const char *sign = u ? "" : "-";

	(void) address;

	if (p) {
		return snprintf(buf, buflen, "%s%s%s P%d, %s, [%s, #%s%d]%s",
		                l ? "LDC" : "STC", cond_names[cond], n ? "L" : "",
		                cpn, cp_reg_names[crd], reg_names[rn], sign, offset,
		                w ? "!" : "");
	} else {
		return snprintf(buf, buflen, "%s%s%s P%d, %s, [%s], #%s%d",
		                l ? "LDC" : "STC", cond_names[cond], n ? "L" : "",
		                cpn, cp_reg_names[crd], reg_names[rn], sign, offset);
	}
}

/**
 * Decode coprocessor data operation (CDP).
 */
static int
disasm_coproc_data_op(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int op1 = (opcode >> 20) & 0xF;
	int crn = (opcode >> 16) & 0xF;
	int crd = (opcode >> 12) & 0xF;
	int cpn = (opcode >> 8) & 0xF;
	int op2 = (opcode >> 5) & 7;
	int crm = opcode & 0xF;

	(void) address;

	return snprintf(buf, buflen, "CDP%s P%d, %d, %s, %s, %s, %d",
	                cond_names[cond], cpn, op1,
	                cp_reg_names[crd], cp_reg_names[crn], cp_reg_names[crm], op2);
}

/**
 * Decode coprocessor register transfer (MCR/MRC).
 */
static int
disasm_coproc_reg_transfer(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int op1 = (opcode >> 21) & 7;
	int l = (opcode >> 20) & 1;
	int crn = (opcode >> 16) & 0xF;
	int rd = (opcode >> 12) & 0xF;
	int cpn = (opcode >> 8) & 0xF;
	int op2 = (opcode >> 5) & 7;
	int crm = opcode & 0xF;

	(void) address;

	return snprintf(buf, buflen, "%s%s P%d, %d, %s, %s, %s, %d",
	                l ? "MRC" : "MCR", cond_names[cond], cpn, op1,
	                reg_names[rd], cp_reg_names[crn], cp_reg_names[crm], op2);
}

/**
 * Decode swap instructions (SWP/SWPB).
 */
static int
disasm_swap(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int b = (opcode >> 22) & 1;
	int rn = (opcode >> 16) & 0xF;
	int rd = (opcode >> 12) & 0xF;
	int rm = opcode & 0xF;

	(void) address;

	return snprintf(buf, buflen, "SWP%s%s %s, %s, [%s]",
	                cond_names[cond], b ? "B" : "",
	                reg_names[rd], reg_names[rm], reg_names[rn]);
}

/**
 * Decode MRS instruction.
 */
static int
disasm_mrs(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int ps = (opcode >> 22) & 1;
	int rd = (opcode >> 12) & 0xF;

	(void) address;

	return snprintf(buf, buflen, "MRS%s %s, %s",
	                cond_names[cond], reg_names[rd], ps ? "SPSR" : "CPSR");
}

/**
 * Decode MSR instruction.
 */
static int
disasm_msr(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int imm = (opcode >> 25) & 1;
	int pd = (opcode >> 22) & 1;
	int mask = (opcode >> 16) & 0xF;
	char fields[5];
	char *fp = fields;
	char operand[64];

	(void) address;

	/* Build field mask string */
	if (mask & 1) *fp++ = 'c';
	if (mask & 2) *fp++ = 'x';
	if (mask & 4) *fp++ = 's';
	if (mask & 8) *fp++ = 'f';
	*fp = '\0';

	if (imm) {
		uint32_t imm8 = opcode & 0xFF;
		uint32_t rot = ((opcode >> 8) & 0xF) * 2;
		uint32_t value = (imm8 >> rot) | (imm8 << (32 - rot));
		snprintf(operand, sizeof(operand), "#0x%X", value);
	} else {
		int rm = opcode & 0xF;
		snprintf(operand, sizeof(operand), "%s", reg_names[rm]);
	}

	return snprintf(buf, buflen, "MSR%s %s_%s, %s",
	                cond_names[cond], pd ? "SPSR" : "CPSR", fields, operand);
}

/**
 * Decode BX instruction.
 */
static int
disasm_bx(uint32_t opcode, uint32_t address, char *buf, size_t buflen)
{
	int cond = (opcode >> 28) & 0xF;
	int rm = opcode & 0xF;

	(void) address;

	return snprintf(buf, buflen, "BX%s %s", cond_names[cond], reg_names[rm]);
}

/**
 * Main disassembly entry point.
 */
const char *
arm_disasm(uint32_t opcode, uint32_t address, char *buffer, size_t buflen)
{
	if (buffer == NULL || buflen == 0) {
		return NULL;
	}

	/* Check condition - NV condition is undefined in ARMv4 */
	int cond = (opcode >> 28) & 0xF;
	if (cond == 0xF) {
		snprintf(buffer, buflen, "DCD 0x%08X", opcode);
		return buffer;
	}

	/* Decode based on bits [27:25] and other fields */
	uint32_t bits_27_25 = (opcode >> 25) & 7;
	uint32_t bits_7_4 = (opcode >> 4) & 0xF;

	switch (bits_27_25) {
	case 0: /* Data processing / Multiply / Misc */
		if ((opcode & 0x0FFFFFD0) == 0x012FFF10) {
			/* BX */
			disasm_bx(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0FBF0FFF) == 0x010F0000) {
			/* MRS */
			disasm_mrs(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0DB0F000) == 0x0120F000) {
			/* MSR */
			disasm_msr(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0FB00FF0) == 0x01000090) {
			/* SWP/SWPB */
			disasm_swap(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0F8000F0) == 0x00800090) {
			/* Long multiply */
			disasm_multiply_long(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0FC000F0) == 0x00000090) {
			/* Multiply */
			disasm_multiply(opcode, address, buffer, buflen);
		} else if ((bits_7_4 & 0x9) == 0x9) {
			/* Halfword transfer or SWP */
			if ((opcode & 0x0E400F90) == 0x00000090) {
				/* Multiply already handled above */
				disasm_data_processing(opcode, address, buffer, buflen);
			} else {
				disasm_halfword_transfer(opcode, address, buffer, buflen);
			}
		} else {
			disasm_data_processing(opcode, address, buffer, buflen);
		}
		break;

	case 1: /* Data processing immediate */
		if ((opcode & 0x0FBF0FFF) == 0x010F0000) {
			disasm_mrs(opcode, address, buffer, buflen);
		} else if ((opcode & 0x0DB0F000) == 0x0120F000) {
			disasm_msr(opcode, address, buffer, buflen);
		} else {
			disasm_data_processing(opcode, address, buffer, buflen);
		}
		break;

	case 2: /* Load/Store immediate offset */
	case 3: /* Load/Store register offset */
		disasm_single_transfer(opcode, address, buffer, buflen);
		break;

	case 4: /* Load/Store multiple */
		disasm_block_transfer(opcode, address, buffer, buflen);
		break;

	case 5: /* Branch */
		disasm_branch(opcode, address, buffer, buflen);
		break;

	case 6: /* Coprocessor load/store */
		disasm_coproc_transfer(opcode, address, buffer, buflen);
		break;

	case 7: /* Coprocessor data/register transfer or SWI */
		if (opcode & (1 << 24)) {
			disasm_swi(opcode, address, buffer, buflen);
		} else if (opcode & (1 << 4)) {
			disasm_coproc_reg_transfer(opcode, address, buffer, buflen);
		} else {
			disasm_coproc_data_op(opcode, address, buffer, buflen);
		}
		break;

	default:
		snprintf(buffer, buflen, "DCD 0x%08X", opcode);
		break;
	}

	return buffer;
}
