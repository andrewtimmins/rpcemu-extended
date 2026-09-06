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

#include "emulator_snapshot.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "arm.h"
#include "arm_disasm.h"
#include "fdc.h"
#include "ide.h"
#include "iomd.h"
#include "mem.h"
#include "podules.h"
#include "rpcemu.h"
#include "accelerators.h"
#include "superio.h"
#include "vidc20.h"
}

namespace {

const char *
cpu_model_to_string(CPUModel cpu_model)
{
	switch (cpu_model) {
	case CPUModel_ARM610:     return "ARM610";
	case CPUModel_ARM710:     return "ARM710";
	case CPUModel_SA110:      return "StrongARM SA-110";
	case CPUModel_ARM7500:    return "ARM7500";
	case CPUModel_ARM7500FE:  return "ARM7500FE";
	case CPUModel_ARM810:     return "ARM810";
	default:                  return "Unknown";
	}
}

void
emulator_fill_snapshot(MachineSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));

	const Model_Details *details = nullptr;
	if (machine.model >= 0 && machine.model < Model_MAX) {
		details = &models[machine.model];
	}

	if (details != nullptr && details->name_gui != nullptr) {
		snprintf(snapshot->model_name, sizeof(snapshot->model_name), "%s", details->name_gui);
	} else {
		snprintf(snapshot->model_name, sizeof(snapshot->model_name), "%s", "Unknown");
	}

	const char *cpu_name = cpu_model_to_string(machine.cpu_model);
	snprintf(snapshot->cpu_name, sizeof(snapshot->cpu_name), "%s", cpu_name);
	snapshot->dynarec = arm_is_dynarec();

	for (int i = 0; i < 16; i++) {
		snapshot->regs[i] = arm.reg[i];
	}

	snapshot->cpsr = arm.reg[cpsr];

	/* Only the five exception modes bank an SPSR. Reading arm.spsr in User or
	   System mode would report whatever the last exception left there, which is
	   worse than reporting nothing. */
	switch (arm.mode & 0xf) {
	case FIQ:
	case IRQ:
	case SUPERVISOR:
	case ABORT:
	case UNDEFINED:
		snapshot->spsr = arm.spsr[arm.mode & 0xf];
		snapshot->spsr_valid = 1;
		break;
	default:
		snapshot->spsr = 0;
		snapshot->spsr_valid = 0;
		break;
	}
	snapshot->mode = arm.mode;

	const uint32_t pc = PC;
	snapshot->pc = pc;

	{
		FPADebugState fpa;

		fpa_get_state(&fpa);
		for (int i = 0; i < 8; i++) {
			snapshot->fpregs[i][0] = fpa.reg[i].w[0];
			snapshot->fpregs[i][1] = fpa.reg[i].w[1];
			snapshot->fpregs[i][2] = fpa.reg[i].w[2];
			snapshot->fpvalues[i] = fpa.reg[i].value;
		}
		snapshot->fpsr = fpa.fpsr;
		snapshot->fpcr = fpa.fpcr;
	}

	snapshot->debug_bank_count =
	    debugger_get_banked_registers(snapshot->debug_banks, DEBUG_BANK_COUNT);

	for (int i = 0; i < 8; i++) {
		const uint32_t addr = (pc + static_cast<uint32_t>(i * 4)) & arm.r15_mask;
		uint32_t word = 0;

		/* Side-effect-free, for the reason given in emulator_disassemble_at():
		   this runs on EVERY snapshot, so the live read fired watchpoints
		   around the PC continuously for as long as the inspector was open. */
		snapshot->pipeline_addr[i] = addr;
		snapshot->pipeline_data[i] = mem_debug_read(addr, 4, &word) ? word : 0;
	}

	snapshot->mmu_enabled = mmu;
	/* The mode is the bottom four bits; bit 4 only says 32-bit. Masking 0x1f
	   made every 32-bit User-mode machine report itself as privileged. */
	snapshot->privileged_mode = ((arm.mode & 0xf) != USER) ? 1 : 0;

	snapshot->iomd_irqa_status = iomd.irqa.status;
	snapshot->iomd_irqa_mask   = iomd.irqa.mask;
	snapshot->iomd_irqb_status = iomd.irqb.status;
	snapshot->iomd_irqb_mask   = iomd.irqb.mask;
	snapshot->iomd_fiq_status  = iomd.fiq.status;
	snapshot->iomd_fiq_mask    = iomd.fiq.mask;
	snapshot->iomd_dma_status  = iomd.irqdma.status;
	snapshot->iomd_dma_mask    = iomd.irqdma.mask;

	snapshot->iomd_timer0_counter   = static_cast<uint32_t>(iomd.t0.counter);
	snapshot->iomd_timer0_in_latch  = iomd.t0.in_latch;
	snapshot->iomd_timer0_out_latch = iomd.t0.out_latch;
	snapshot->iomd_timer1_counter   = static_cast<uint32_t>(iomd.t1.counter);
	snapshot->iomd_timer1_in_latch  = iomd.t1.in_latch;
	snapshot->iomd_timer1_out_latch = iomd.t1.out_latch;
	snapshot->iomd_sound_status     = iomd.sndstat;

	snapshot->floppy_motor_on = motoron;

	snapshot->perf_mips      = perf.mips;
	snapshot->perf_mhz       = perf.mhz;
	snapshot->perf_tlb_sec   = perf.tlb_sec;
	snapshot->perf_flush_sec = perf.flush_sec;

	snapshot->config_mem_size  = config.mem_size;
	snapshot->config_vram_size = config.vram_size;
	snapshot->network_type     = config.network_type;
	snapshot->cpu_idle_enabled = config.cpu_idle;

	DebuggerStatus debug_status;
	debugger_get_status(&debug_status);
	snapshot->debug_paused = debug_status.paused;
	snapshot->debug_pause_requested = debug_status.pause_requested;
	snapshot->debug_pause_reason = debug_status.reason;
	snapshot->debug_halt_pc = debug_status.halt_pc;
	snapshot->debug_halt_from_pc = debug_status.halt_from_pc;
	snapshot->debug_halt_opcode = debug_status.halt_opcode;
	snapshot->debug_last_pc = debug_status.last_pc;
	snapshot->debug_last_opcode = debug_status.last_opcode;
	snapshot->debug_hit_address = debug_status.hit_address;
	snapshot->debug_hit_value = debug_status.hit_value;
	snapshot->debug_hit_size = debug_status.hit_size;
	snapshot->debug_hit_is_write = debug_status.hit_is_write;
	snapshot->debug_step_active = debug_status.step_active;

	uint32_t bp_count = debug_status.breakpoint_count;
	if (bp_count > DEBUGGER_MAX_BREAKPOINTS) {
		bp_count = DEBUGGER_MAX_BREAKPOINTS;
	}
	snapshot->debug_breakpoint_count = bp_count;
	if (bp_count > 0) {
		memcpy(snapshot->debug_breakpoints,
		       debug_status.breakpoints,
		       bp_count * sizeof(DebugBreakpointInfo));
	}

	uint32_t wp_count = debug_status.watchpoint_count;
	if (wp_count > DEBUGGER_MAX_WATCHPOINTS) {
		wp_count = DEBUGGER_MAX_WATCHPOINTS;
	}
	snapshot->debug_watchpoint_count = wp_count;
	if (wp_count > 0) {
		memcpy(snapshot->debug_watchpoints,
		       debug_status.watchpoints,
		       wp_count * sizeof(DebugWatchpointInfo));
	}

	debugger_get_trace_config(&snapshot->debug_trace_config);
	snapshot->debug_trace_pending = debugger_trace_pending();
	snapshot->debug_trace_dropped = 0;

	vidc_get_snapshot(&snapshot->vidc);
	int double_x = 0;
	int double_y = 0;
	vidc_get_doublesize(&double_x, &double_y);
	snapshot->vidc_double_x = static_cast<uint8_t>(double_x);
	snapshot->vidc_double_y = static_cast<uint8_t>(double_y);
	superio_get_snapshot(&snapshot->superio);
	ide_get_snapshot(&snapshot->ide);
	podules_get_snapshot(&snapshot->podules);
	accel_get_stats(&snapshot->accel);
}

} // namespace

MachineSnapshot
emulator_take_snapshot()
{
	MachineSnapshot snapshot;
	emulator_fill_snapshot(&snapshot);
	return snapshot;
}

MemoryRead
emulator_read_memory(uint32_t address, uint32_t length, bool physical)
{
	if (length > 4096) {
		length = 4096;
	}

	MemoryRead result;
	result.data.reserve(length);
	result.mapped.reserve(length);

	/*
	 * Translated unless a physical read was asked for, which is what dc_read8()
	 * in debugcmd.c has always done. This read did not translate at all, so the
	 * hex view answered with whatever sat at that PHYSICAL address while every
	 * other address in the window - the PC, the registers, the disassembly,
	 * breakpoints - meant a virtual one. Same number, two different places, and
	 * that is issue #258.
	 *
	 * Byte at a time, and deliberately: a run of bytes can cross a page
	 * boundary into an unmapped page, so mapped-ness is per byte and not a
	 * property of the request.
	 */
	for (uint32_t i = 0; i < length; i++) {
		const uint32_t addr = address + i;
		uint32_t phys = addr;
		const int ok = physical ? 1 : mem_debug_translate(addr, &phys);

		result.mapped.push_back(ok ? 1u : 0u);
		result.data.push_back(ok
		    ? static_cast<uint8_t>(mem_phys_read8_debug(phys))
		    : static_cast<uint8_t>(0));
	}

	return result;
}

std::string
emulator_disassemble_at(uint32_t address, int count)
{
	if (count <= 0) {
		count = 1;
	}
	if (count > 256) {
		count = 256;
	}

	/*
	 * Where execution actually is, so the line about to run can be marked.
	 *
	 * R15 reads eight ahead of the instruction being executed, which is what
	 * PC in arm.h undoes. Read once rather than per line.
	 */
	const uint32_t pc = (arm.reg[15] - 8u) & arm.r15_mask;

	std::string result;
	char disasm_buf[128];

	for (int i = 0; i < count; i++) {
		const uint32_t addr = address + static_cast<uint32_t>(i * 4);
		uint32_t opcode = 0;

		/*
		 * mem_debug_read(), not mem_read32(). The live read calls
		 * debugger_memory_access(), so merely DISPLAYING code fired any
		 * watchpoint covering it, and an address on an I/O page went through
		 * readmemfl() with whatever side effect that carries. Looking at the
		 * machine must not change it.
		 */
		if (!mem_debug_read(addr, 4, &opcode)) {
			char line[256];

			snprintf(line, sizeof(line), "%08X%c %s  %s\n",
			         addr, addr == pc ? '<' : ':', "--------",
			         "<unmapped>");
			result += line;
			continue;
		}

		arm_disasm(opcode, addr, disasm_buf, sizeof(disasm_buf));

		/*
		 * Upper case, and "<" in place of ":" on the instruction about to
		 * execute - both asked for in discussion #223. The marker follows
		 * what the RISC OS disassembler does, so it reads the way somebody
		 * coming from *Memory expects, and it costs no column width.
		 */
		char line[256];
		snprintf(line, sizeof(line), "%08X%c %08X  %s\n",
		         addr, addr == pc ? '<' : ':', opcode, disasm_buf);
		result += line;
	}

	return result;
}
