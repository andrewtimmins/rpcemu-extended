/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef MACHINE_SNAPSHOT_H
#define MACHINE_SNAPSHOT_H

#include <stdint.h>

#include "rpcemu.h"
#include "peripheral_snapshot.h"

/**
 * Lightweight view of the emulator state captured on the emulator thread
 * and marshalled to the GUI. All fields are POD types to simplify copying
 * between threads.
 */
typedef struct MachineSnapshot {
	char model_name[64];
	char cpu_name[64];
	int dynarec;

	uint32_t regs[16];
	uint32_t cpsr;

	/* The saved PSR of the mode the machine is in, and whether that mode has
	   one. User and System do not; the five exception modes each have their
	   own. Asked for in discussion #223 by somebody tracing an ADFFS fault
	   through 36 places that write SPSR and 14 that return through it - the
	   mode a return is about to restore is not visible any other way. */
	uint32_t spsr;
	int spsr_valid;
	uint32_t mode;
	uint32_t pc;

	/* The FPA10's state, in the chip's own 80-bit extended format plus the
	   value each register stands for. Asked for in discussion #257: the
	   debugger disassembled FPA instructions and then had nothing to say about
	   what they did. See FPADebugReg in rpcemu.h for why both forms are kept. */
	uint32_t fpregs[8][3];
	double fpvalues[8];
	uint32_t fpsr;
	uint32_t fpcr;

	uint32_t pipeline_addr[8];
	uint32_t pipeline_data[8];

	int mmu_enabled;
	int privileged_mode;

	uint32_t iomd_irqa_status;
	uint32_t iomd_irqa_mask;
	uint32_t iomd_irqb_status;
	uint32_t iomd_irqb_mask;
	uint32_t iomd_fiq_status;
	uint32_t iomd_fiq_mask;
	uint32_t iomd_dma_status;
	uint32_t iomd_dma_mask;

	uint32_t iomd_timer0_counter;
	uint32_t iomd_timer0_in_latch;
	uint32_t iomd_timer0_out_latch;
	uint32_t iomd_timer1_counter;
	uint32_t iomd_timer1_in_latch;
	uint32_t iomd_timer1_out_latch;
	uint8_t iomd_sound_status;

	int floppy_motor_on;

	float perf_mips;
	float perf_mhz;
	float perf_tlb_sec;
	float perf_flush_sec;

	uint32_t config_mem_size;
	uint32_t config_vram_size;
	NetworkType network_type;
	int cpu_idle_enabled;

	int debug_paused;
	int debug_pause_requested;
	DebugPauseReason debug_pause_reason;
	uint32_t debug_halt_pc;
	uint32_t debug_halt_opcode;
	uint32_t debug_last_pc;
	uint32_t debug_last_opcode;
	uint32_t debug_hit_address;
	uint32_t debug_hit_value;
	uint8_t debug_hit_size;
	uint8_t debug_hit_is_write;
	uint8_t debug_step_active;
	uint8_t debug_reserved0;
	uint32_t debug_breakpoint_count;
	DebugBreakpointInfo debug_breakpoints[DEBUGGER_MAX_BREAKPOINTS];
	uint32_t debug_watchpoint_count;
	DebugWatchpointInfo debug_watchpoints[DEBUGGER_MAX_WATCHPOINTS];

	DebugTraceConfig debug_trace_config;
	uint32_t debug_trace_dropped;
	uint32_t debug_trace_pending;

	VIDCStateSnapshot vidc;
	SuperIOStateSnapshot superio;
	IDEStateSnapshot ide;
	PodulesStateSnapshot podules;
	uint8_t vidc_double_x;
	uint8_t vidc_double_y;
	uint8_t reserved_peripherals[2];
} MachineSnapshot;

#endif
