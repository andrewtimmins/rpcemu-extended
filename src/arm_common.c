/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

/* arm_common.c - sections of code that are shared between the interpreted and dynarec builds */

#include <stdint.h>

#include "rpcemu.h"

#include "arm.h"
#include "arm_common.h"
#include "mem.h"
#include "keyboard.h"
#include "hostfs.h"
#include "hostcmd.h"
#include "savestate.h"

#ifdef RPCEMU_NETWORKING
#include "network.h"
#endif

#define SWI_OS_Byte		0x6
#define SWI_OS_Word		0x7
#define SWI_OS_Mouse		0x1c
#define SWI_OS_CallASWI		0x6f
#define SWI_OS_CallASWIR12	0x71

#define SWI_Portable_ReadFeatures	0x42fc5
#define SWI_Portable_Idle		0x42fc6

/**
 * Perform a Store Halfword.
 *
 * This is part of the Load/Store Extensions added in ARMv4.
 *
 * On a real Risc PC, this can't always work reliably due to the memory system
 * not supporting 16-bit data transfers. Rather than try to identify exactly
 * when it could work, we implement it as always working correctly.
 *
 * @param opcode Opcode of instruction being emulated
 */
void
arm_strh(uint32_t opcode)
{
	uint32_t addr, data, offset;

	addr = GETADDR(RN);

	// Calculate offset
	if (opcode & (1u << 22)) {
		offset = ((opcode >> 4) & 0xf0) | (opcode & 0xf);
	} else {
		offset = arm.reg[RM];
	}
	if (!(opcode & (1u << 23))) {
		offset = -offset;
	}

	// Pre-indexed
	if (opcode & (1u << 24)) {
		addr += offset;
	}

	// Store
	data = GETREG(RD);
	mem_write8(addr & ~1, (uint8_t) data);
	mem_write8(addr | 1, (uint8_t) (data >> 8));

	// Check for Abort
	if (arm.event & 0x40) {
		return;
	}

	if (!(opcode & (1u << 24))) {
		// Post-indexed
		arm.reg[RN] = addr + offset;
	} else if (opcode & (1u << 21)) {
		// Pre-indexed with Writeback
		arm.reg[RN] = addr;
	}
}

/**
 * Perform a Load Halfword.
 *
 * This is part of the Load/Store Extensions added in ARMv4.
 *
 * On a real Risc PC, this can't always work reliably due to the memory system
 * not supporting 16-bit data transfers. Rather than try to identify exactly
 * when it could work, we implement it as always working correctly.
 *
 * @param opcode Opcode of instruction being emulated
 */
void
arm_ldrh(uint32_t opcode)
{
	uint32_t addr, data, offset;

	addr = GETADDR(RN);

	// Calculate offset
	if (opcode & (1u << 22)) {
		offset = ((opcode >> 4) & 0xf0) | (opcode & 0xf);
	} else {
		offset = arm.reg[RM];
	}
	if (!(opcode & (1u << 23))) {
		offset = -offset;
	}

	// Pre-indexed
	if (opcode & (1u << 24)) {
		addr += offset;
	}

	// Load
	data = mem_read32(addr & ~3u);
	if (addr & 2) {
		data >>= 16;
	} else {
		data &= 0xffff;
	}

	// Check for Abort
	if (arm.event & 0x40) {
		return;
	}

	if (!(opcode & (1u << 24))) {
		// Post-indexed
		arm.reg[RN] = addr + offset;
	} else if (opcode & (1u << 21)) {
		// Pre-indexed with Writeback
		arm.reg[RN] = addr;
	}

	// Write Rd
	LOADREG(RD, data);
}

/**
 * Perform a Load Signed Halfword.
 *
 * This is part of the Load/Store Extensions added in ARMv4.
 *
 * On a real Risc PC, this can't always work reliably due to the memory system
 * not supporting 16-bit data transfers. Rather than try to identify exactly
 * when it could work, we implement it as always working correctly.
 *
 * @param opcode Opcode of instruction being emulated
 */
void
arm_ldrsh(uint32_t opcode)
{
	uint32_t addr, data, offset;

	addr = GETADDR(RN);

	// Calculate offset
	if (opcode & (1u << 22)) {
		offset = ((opcode >> 4) & 0xf0) | (opcode & 0xf);
	} else {
		offset = arm.reg[RM];
	}
	if (!(opcode & (1u << 23))) {
		offset = -offset;
	}

	// Pre-indexed
	if (opcode & (1u << 24)) {
		addr += offset;
	}

	// Load
	data = mem_read32(addr & ~3u);
	if (addr & 2) {
		data = (uint32_t) ((int32_t) data >> 16);
	} else {
		data = (uint32_t) (int32_t) (int16_t) data;
	}

	// Check for Abort
	if (arm.event & 0x40) {
		return;
	}

	if (!(opcode & (1u << 24))) {
		// Post-indexed
		arm.reg[RN] = addr + offset;
	} else if (opcode & (1u << 21)) {
		// Pre-indexed with Writeback
		arm.reg[RN] = addr;
	}

	// Write Rd
	LOADREG(RD, data);
}

/**
 * Perform a Load Signed Byte.
 *
 * This is part of the Load/Store Extensions added in ARMv4.
 *
 * @param opcode Opcode of instruction being emulated
 */
void
arm_ldrsb(uint32_t opcode)
{
	uint32_t addr, data, offset;

	addr = GETADDR(RN);

	// Calculate offset
	if (opcode & (1u << 22)) {
		offset = ((opcode >> 4) & 0xf0) | (opcode & 0xf);
	} else {
		offset = arm.reg[RM];
	}
	if (!(opcode & (1u << 23))) {
		offset = -offset;
	}

	// Pre-indexed
	if (opcode & (1u << 24)) {
		addr += offset;
	}

	// Load
	data = (uint32_t) (int32_t) (int8_t) mem_read8(addr);

	// Check for Abort
	if (arm.event & 0x40) {
		return;
	}

	if (!(opcode & (1u << 24))) {
		// Post-indexed
		arm.reg[RN] = addr + offset;
	} else if (opcode & (1u << 21)) {
		// Pre-indexed with Writeback
		arm.reg[RN] = addr;
	}

	// Write Rd
	LOADREG(RD, data);
}

/**
 * Perform a Store Multiple register operation when the S flag is clear.
 *
 * @param opcode    Opcode of instruction being emulated
 * @param address   The address to be used for the first transfer
 * @param writeback The value to be written to the base register if Writeback
 *                  is requested
 */
void
arm_store_multiple(uint32_t opcode, uint32_t address, uint32_t writeback)
{
	uint32_t orig_base, addr, mask;
	int c;

	orig_base = arm.reg[RN];

	addr = address & ~3;

	/* Store first register */
	mask = 1;
	for (c = 0; c < 15; c++) {
		if (opcode & mask) {
			mem_write32(addr, arm.reg[c]);
			addr += 4;
			break;
		}
		mask <<= 1;
	}
	mask <<= 1;
	c++;

	/* Perform Writeback (if requested) at end of 2nd cycle */
	if (!arm.stm_writeback_at_end && (opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	/* Check for Abort from first Store */
	if (arm.event & 0x40) {
		goto data_abort;
	}

	/* Store remaining registers up to R14 */
	for ( ; c < 15; c++) {
		if (opcode & mask) {
			mem_write32(addr, arm.reg[c]);
			if (arm.event & 0x40) {
				goto data_abort;
			}
			addr += 4;
		}
		mask <<= 1;
	}

	/* Store R15 (if requested) */
	if (opcode & (1 << 15)) {
		mem_write32(addr, arm.reg[15] + arm.r15_diff);
		if (arm.event & 0x40) {
			goto data_abort;
		}
	}

	/* Perform Writeback (if requested) at end of instruction (SA110) */
	if (arm.stm_writeback_at_end && (opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	/* No Data Abort */
	return;

	/* A Data Abort occurred, restore the Base Register to the value it
	   had before the instruction */
data_abort:
	if (arm.abort_base_restored && (opcode & (1u << 21)) && (RN != 15)) {
		arm.reg[RN] = orig_base;
	}
}

/**
 * Perform a Store Multiple register operation when the S flag is set.
 *
 * The registers to be stored will be taken from the User bank instead of the
 * current bank.
 *
 * @param opcode    Opcode of instruction being emulated
 * @param address   The address to be used for the first transfer
 * @param writeback The value to be written to the base register if Writeback
 *                  is requested
 */
void
arm_store_multiple_s(uint32_t opcode, uint32_t address, uint32_t writeback)
{
	uint32_t orig_base, addr, mask;
	int c;

	orig_base = arm.reg[RN];

	addr = address & ~3;

	/* Store first register */
	mask = 1;
	for (c = 0; c < 15; c++) {
		if (opcode & mask) {
			mem_write32(addr, *usrregs[c]);
			addr += 4;
			break;
		}
		mask <<= 1;
	}
	mask <<= 1;
	c++;

	/* Perform Writeback (if requested) at end of 2nd cycle */
	if (!arm.stm_writeback_at_end && (opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	/* Check for Abort from first Store */
	if (arm.event & 0x40) {
		goto data_abort;
	}

	/* Store remaining registers up to R14 */
	for ( ; c < 15; c++) {
		if (opcode & mask) {
			mem_write32(addr, *usrregs[c]);
			if (arm.event & 0x40) {
				goto data_abort;
			}
			addr += 4;
		}
		mask <<= 1;
	}

	/* Store R15 (if requested) */
	if (opcode & (1 << 15)) {
		mem_write32(addr, arm.reg[15] + arm.r15_diff);
		if (arm.event & 0x40) {
			goto data_abort;
		}
	}

	/* Perform Writeback (if requested) at end of instruction (SA110) */
	if (arm.stm_writeback_at_end && (opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	/* No Data Abort */
	return;

	/* A Data Abort occurred, restore the Base Register to the value it
	   had before the instruction */
data_abort:
	if (arm.abort_base_restored && (opcode & (1u << 21)) && (RN != 15)) {
		arm.reg[RN] = orig_base;
	}
}

/**
 * Perform a Load Multiple register operation when the S flag is clear.
 *
 * @param opcode    Opcode of instruction being emulated
 * @param address   The address to be used for the first transfer
 * @param writeback The value to be written to the base register if Writeback
 *                  is requested
 */
void
arm_load_multiple(uint32_t opcode, uint32_t address, uint32_t writeback)
{
	uint32_t orig_base, addr, mask, temp;
	int c;

	orig_base = arm.reg[RN];

	addr = address & ~3;

	/* Perform Writeback (if requested) */
	if ((opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	/* Load registers up to R14 */
	mask = 1;
	for (c = 0; c < 15; c++) {
		if (opcode & mask) {
			temp = mem_read32(addr);
			if (arm.event & 0x40) {
				goto data_abort;
			}
			arm.reg[c] = temp;
			addr += 4;
		}
		mask <<= 1;
	}

	/* Load R15 (if requested) */
	if (opcode & (1 << 15)) {
		temp = mem_read32(addr);
		if (arm.event & 0x40) {
			goto data_abort;
		}
		/* Only update R15 if no Data Abort occurred */
		arm.reg[15] = (arm.reg[15] & ~arm.r15_mask) |
		              ((temp + 4) & arm.r15_mask);
	}

	/* No Data Abort */
	return;

	/* A Data Abort occurred, modify the Base Register */
data_abort:
	if (!arm.abort_base_restored && (opcode & (1u << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	} else {
		arm.reg[RN] = orig_base;
	}
}

/**
 * Perform a Load Multiple register operation when the S flag is set.
 *
 * If R15 is in the list of registers to be loaded, the PSR flags will be
 * updated as well, subject to the current privilege level.
 *
 * If R15 is not in the list of registers to be loaded, the values will be
 * loaded into the User bank instead of the current bank.
 *
 * @param opcode    Opcode of instruction being emulated
 * @param address   The address to be used for the first transfer
 * @param writeback The value to be written to the base register if Writeback
 *                  is requested
 */
void
arm_load_multiple_s(uint32_t opcode, uint32_t address, uint32_t writeback)
{
	uint32_t orig_base, addr, mask, temp;
	int c;

	orig_base = arm.reg[RN];

	addr = address & ~3;

	/* Perform Writeback (if requested) */
	if ((opcode & (1 << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	}

	mask = 1;
	/* Is R15 in the list of registers to be loaded? */
	if (opcode & (1 << 15)) {
		/* R15 in list - Load registers up to R14 */
		for (c = 0; c < 15; c++) {
			if (opcode & mask) {
				temp = mem_read32(addr);
				if (arm.event & 0x40) {
					goto data_abort;
				}
				arm.reg[c] = temp;
				addr += 4;
			}
			mask <<= 1;
		}

		/* Perform load of R15 and update CPSR/flags */
		temp = mem_read32(addr);
		if (arm.event & 0x40) {
			goto data_abort;
		}
		arm_write_r15(opcode, temp);

	} else {
		/* R15 not in list - Perform load into User Bank */
		for (c = 0; c < 15; c++) {
			if (opcode & mask) {
				temp = mem_read32(addr);
				if (arm.event & 0x40) {
					goto data_abort;
				}
				*usrregs[c] = temp;
				addr += 4;
			}
			mask <<= 1;
		}
	}

	/* No Data Abort */
	return;

	/* A Data Abort occurred, modify the Base Register */
data_abort:
	if (!arm.abort_base_restored && (opcode & (1u << 21)) && (RN != 15)) {
		arm.reg[RN] = writeback;
	} else {
		arm.reg[RN] = orig_base;
	}
}

#ifndef TEST
/**
 * Handler for SWI instructions; includes all the emulator specific SWIs as
 * well as the standard SWI interface of raising an exception.
 *
 * Called from dynarec and interpreted code modes.
 *
 * @param opcode Opcode of instruction being emulated
 * @return 0
 */
int
opSWI(uint32_t opcode)
{
	uint32_t swinum = opcode & 0xdffff;

	/* Get actual SWI number from OS_CallASWI and OS_CallASWIR12 */
	if (swinum == SWI_OS_CallASWI) {
		swinum = arm.reg[10] & 0xdffff;
	} else if (swinum == SWI_OS_CallASWIR12) {
		swinum = arm.reg[12] & 0xdffff;
	}

	/* Debugger: trace/halt on SWI (gated to stay free when tracing is off) */
	if (debugger_swi_trace_active) {
		debugger_swi_hook(swinum, opcode);
	}

	/* Intercept RISC OS Portable SWIs to enable RPCEmu to sleep when
	   RISC OS is idle */
	if (config.cpu_idle) {
		switch (swinum) {
		case SWI_Portable_ReadFeatures:
			arm.reg[1] = (1u << 4);	/* Idle supported flag */
			arm.reg[cpsr] &= ~VFLAG;
			return 0;
		case SWI_Portable_Idle:
			rpcemu_idle();
			arm.reg[cpsr] &= ~VFLAG;
			return 0;
		}
	}

	/* This is called regardless of whether or not we're in mousehack
	   as it allows 'fullscreen' or 'mouse capture mode' risc os mode changes
	   to have their boxes cached, allowing mousehack to work when you change
	   back to it */
	if (swinum == SWI_OS_Word && arm.reg[0] == 21 && mem_read8(arm.reg[1]) == 1) {
			/* OS_Word 21, 1 Define Mouse Coordinate bounding box */
			mouse_hack_osword_21_1(arm.reg[1]);
			return 0;
	}
	
	if (mousehack && swinum == SWI_OS_Word && arm.reg[0] == 21) {
		if (mem_read8(arm.reg[1]) == 4) {
			/* OS_Word 21, 4 Read unbuffered mouse position */
			mouse_hack_osword_21_4(arm.reg[1]);
			return 0;
		} else if (mem_read8(arm.reg[1]) == 3) {
			/* OS_Word 21, 3 Move mouse */
			mouse_hack_osword_21_3(arm.reg[1]);
			return 0;
		} else {
			goto realswi;
		}

	} else if (mousehack && swinum == SWI_OS_Mouse) {
		/* OS_Mouse */
		mouse_hack_osmouse();
		arm.reg[cpsr] &= ~VFLAG;

	} else if (swinum == ARCEM_SWI_HOSTFS) {
		ARMul_State state;

		state.Reg = arm.reg;
		hostfs(&state);
		hostfs_activity_increment();

	}
#ifdef RPCEMU_NETWORKING
	else if (swinum == ARCEM_SWI_NETWORK) {
		if (config.network_type != NetworkType_Off) {
			network_swi(arm.reg[0], arm.reg[1], arm.reg[2], arm.reg[3],
			            arm.reg[4], arm.reg[5], &arm.reg[0], &arm.reg[1]);
			network_activity_increment();
		}
	}
#endif
	else if (swinum == ARCEM_SWI_HOSTCMD) {
		if (config.hostcmd_enabled) {
			ARMul_State state;

			state.Reg = arm.reg;
			hostcmd(&state);
			arm.reg[cpsr] &= ~VFLAG;	/* X-form: report success */
		} else {
			goto realswi;	/* disabled -> Unknown SWI; guest detects absence */
		}
	}
	else {
realswi:
		if (mousehack && swinum == SWI_OS_Word && arm.reg[0] == 21 &&
		    mem_read8(arm.reg[1]) == 0)
		{
			/* OS_Word 21, 0 Define pointer size, shape and active point */
			mouse_hack_osword_21_0(arm.reg[1]);
		}
		if (mousehack && swinum == SWI_OS_Byte && arm.reg[0] == 106) {
			/* OS_Byte 106 Select pointer / activate mouse */
			mouse_hack_osbyte_106(arm.reg[1]);
		}
		exception(SUPERVISOR, 0xc, 4);
	}

	return 0;
}
/**
 * Write the CPU state to a suspend snapshot.
 *
 * Shared between the interpreter and dynarec builds: both keep the complete
 * architectural state in the ARMState struct between arm_exec() calls (the
 * dynarec's lazy flags and compiled blocks are only live inside a block),
 * so a snapshot is portable between the two CPU cores.
 */
void
arm_savestate(FILE *f)
{
	int c;

	for (c = 0; c < 17; c++) {
		savestate_write_u32(f, arm.reg[c]);
	}
	savestate_write_u32(f, arm.mode);
	savestate_write_u32(f, arm.event);
	for (c = 0; c < 15; c++) {
		savestate_write_u32(f, arm.user_reg[c]);
	}
	for (c = 0; c < 15; c++) {
		savestate_write_u32(f, arm.fiq_reg[c]);
	}
	for (c = 0; c < 2; c++) {
		savestate_write_u32(f, arm.irq_reg[c]);
	}
	for (c = 0; c < 2; c++) {
		savestate_write_u32(f, arm.super_reg[c]);
	}
	for (c = 0; c < 2; c++) {
		savestate_write_u32(f, arm.abort_reg[c]);
	}
	for (c = 0; c < 2; c++) {
		savestate_write_u32(f, arm.undef_reg[c]);
	}
	for (c = 0; c < 16; c++) {
		savestate_write_u32(f, arm.spsr[c]);
	}
	savestate_write_u32(f, arm.r15_diff);
	savestate_write_u8(f, arm.abort_base_restored);
	savestate_write_u8(f, arm.stm_writeback_at_end);
	savestate_write_u8(f, arm.arch_v4);
}

/**
 * Restore the CPU state from a suspend snapshot.
 *
 * mmask, r15_mask, prog32, memmode, cpsr/pcpsr and the usrregs bank
 * pointers are all derived from the restored mode by updatemode().
 */
void
arm_loadstate(FILE *f)
{
	ARMState saved;
	int c;

	for (c = 0; c < 17; c++) {
		arm.reg[c] = savestate_read_u32(f);
	}
	arm.mode = savestate_read_u32(f);
	arm.event = savestate_read_u32(f);
	for (c = 0; c < 15; c++) {
		arm.user_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 15; c++) {
		arm.fiq_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 2; c++) {
		arm.irq_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 2; c++) {
		arm.super_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 2; c++) {
		arm.abort_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 2; c++) {
		arm.undef_reg[c] = savestate_read_u32(f);
	}
	for (c = 0; c < 16; c++) {
		arm.spsr[c] = savestate_read_u32(f);
	}
	arm.r15_diff = savestate_read_u32(f);
	arm.abort_base_restored = savestate_read_u8(f);
	arm.stm_writeback_at_end = savestate_read_u8(f);
	arm.arch_v4 = savestate_read_u8(f);

	/* updatemode() starts by writing the active registers back into the
	   banked copies for the current mode, which would clobber the
	   restored bank contents; put them back afterwards so the restored
	   CPU state is exactly the saved one */
	saved = arm;
	updatemode(arm.mode);
	memcpy(arm.user_reg, saved.user_reg, sizeof(arm.user_reg));
	memcpy(arm.fiq_reg, saved.fiq_reg, sizeof(arm.fiq_reg));
	memcpy(arm.irq_reg, saved.irq_reg, sizeof(arm.irq_reg));
	memcpy(arm.super_reg, saved.super_reg, sizeof(arm.super_reg));
	memcpy(arm.abort_reg, saved.abort_reg, sizeof(arm.abort_reg));
	memcpy(arm.undef_reg, saved.undef_reg, sizeof(arm.undef_reg));

	/* Invalidate the instruction-fetch fast path; it holds a host
	   pointer into the previous session's memory */
	pccache = 0xffffffff;
	blockend = 0;
}

#endif /* ifndef TEST */
