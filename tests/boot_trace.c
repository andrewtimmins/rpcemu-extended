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
 * boot_trace - boot a real machine with no GUI and report how far it got.
 *
 * Why this exists. The recompiler backends are covered by differential tests
 * that build one block at a time in a synthetic machine, and they pass: on arm64
 * every JIT test passes and tens of millions of fuzzed cases find nothing. Yet
 * the arm64 recompiler does not boot RISC OS on real hardware (issue #30: the
 * window opens, the MIPS counter ticks, the screen stays blank). So the fault is
 * in something a synthetic single-block test cannot reach - the MMU and the soft
 * TLB, self-modifying guest code, mode changes and aborts, block linking across
 * a real instruction stream - and chasing it needs a real boot.
 *
 * A real boot normally needs the GUI, which needs wxWidgets, which is why this
 * has only ever been testable on the machine that has the fault. This links the
 * emulator core against the test stubs instead, so it boots the actual ROM with
 * no window, no sound and no video thread. It therefore cross-compiles for
 * aarch64 and runs under qemu-aarch64 on a developer's x86 machine, which is
 * where the arm64 backend can now be examined without owning a Pi.
 *
 * What it reports, deliberately, is not an instruction trace. Comparing traces
 * between an interpreter and a recompiler is awkward because they do not stop on
 * the same instruction boundaries. Instead it watches for the milestones a boot
 * must pass through and samples where the machine is spending its time:
 *
 *   - MMU turned on, first mode change, first abort, first IRQ
 *   - VIDC programmed with a mode, and the framestore actually written
 *   - a histogram of sampled PCs by 64K region, so a machine stuck in a loop
 *     says where it is stuck
 *
 * Run it on a working backend and a broken one and the first missing milestone,
 * plus where it is spinning, says which part of the machine to look at. Combined
 * with RPCEMU_JIT_DENY (see jit_deny_init() in arm_dynarec.c) that turns into a
 * bisection: deny half the opcode classes, see whether the milestone comes back.
 *
 *   boot_trace [chunks]
 *
 * RPCEMU_DATADIR selects the data directory as usual, and the ROM comes from
 * $RPCEMU_DATADIR/roms/ exactly as it does for the real emulator. A chunk is one
 * call to execrpcemu(), which is 20000 cycles, so the default is a few seconds
 * of guest time.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "vidc20.h"
#include "iomd.h"

/** One call to execrpcemu() is 20000 cycles. */
#define DEFAULT_CHUNKS	20000

/** PC samples are bucketed by 64K, over the 4G address space. */
#define REGIONS		65536
#define REGION_SHIFT	16

extern void execrpcemu(void);
extern void rpcemu_start(void);

static unsigned long pc_samples[REGIONS];

/**
 * Emulated time, in nanoseconds, derived from emulated cycles rather than from
 * the host clock.
 *
 * The real emulator reports host elapsed time here, which is right for it and
 * wrong for this. Under qemu the emulator runs perhaps twenty times slower than
 * native, so a host clock would race ahead of the guest and the machine would
 * see its timers fire far more often per instruction executed than it does on
 * amd64. The two runs being compared would then differ for a reason that has
 * nothing to do with the backend under test.
 *
 * Tying time to cycles removes the host's speed from the experiment altogether
 * and makes a boot reproducible: the same build and the same ROM take the same
 * path every time. A StrongARM RiscPC runs at 200MHz, so a cycle is 5ns.
 */
static uint64_t virtual_ns;

uint64_t
rpcemu_test_virtual_ns(void)
{
	return virtual_ns;
}

struct milestone {
	const char *name;
	int seen;
	unsigned long chunk;	/* when it was first seen */
};

static struct milestone milestones[] = {
	{ "MMU enabled",          0, 0 },
	{ "left SVC mode",        0, 0 },
	{ "first abort taken",    0, 0 },
	{ "first IRQ taken",      0, 0 },
	{ "VIDC given a mode",    0, 0 },
	{ "framestore written",   0, 0 },
	{ "reached user mode",    0, 0 },
};

enum {
	M_MMU, M_MODE, M_ABORT, M_IRQ, M_VIDC, M_FRAMESTORE, M_USER, M_COUNT
};

static void
reached(int m, unsigned long chunk)
{
	if (!milestones[m].seen) {
		milestones[m].seen = 1;
		milestones[m].chunk = chunk;
		printf("  [chunk %6lu] %s\n", chunk, milestones[m].name);
		fflush(stdout);
	}
}

/**
 * Has anything been drawn?
 *
 * A blank screen is the symptom being chased, so this asks the question
 * directly rather than trusting that a mode change implies output: it looks for
 * a non-zero byte anywhere in the first 256K of the framestore. Sampled rather
 * than exhaustive because it runs every chunk.
 */
static int
framestore_dirty(void)
{
	unsigned i;

	if (vram == NULL) {
		return 0;
	}
	/* Sampled every 512 bytes over the first 256K: a mode with anything at all
	   on it has non-zero words all over that range, and an unwritten framestore
	   has none. */
	for (i = 0; i < (256 * 1024) / 4; i += 128) {
		if (vram[i] != 0) {
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	unsigned long chunks = (argc > 1) ? strtoul(argv[1], NULL, 0) : DEFAULT_CHUNKS;
	unsigned long chunk;
	unsigned long samples = 0;
	unsigned long event_chunks = 0;
	uint32_t event_seen = 0;
	uint32_t irqa_status_seen = 0, irqa_mask_seen = 0;
	unsigned long irqs_enabled_chunks = 0;
	uint32_t last_mode;
	int i;

	printf("boot_trace: %s backend, %lu chunks (%lu cycles)\n",
	    arm_is_dynarec() ? "recompiler" : "interpreter", chunks, chunks * 20000);
	if (getenv("RPCEMU_JIT_DENY") != NULL) {
		printf("boot_trace: RPCEMU_JIT_DENY=%s\n", getenv("RPCEMU_JIT_DENY"));
	}

	/* RPCEMU_DATADIR is honoured by the GUI front ends rather than by the core,
	   so with no GUI here it has to be applied by hand. */
	rpcemu_set_datadir(getenv("RPCEMU_DATADIR"));
	rpcemu_set_resourcedir(getenv("RPCEMU_DATADIR"));

	/* A machine directory holds the CMOS and the HostFS tree. Without one the
	   machine comes up with no settings at all, which is not the boot we want to
	   be studying. RPCEMU_MACHINE names it, as --machine does for the real
	   headless binary. */
	rpcemu_set_machine_datadir(getenv("RPCEMU_MACHINE") ? getenv("RPCEMU_MACHINE") : "Default");

	/* The same start-up the real emulator performs. config_load() is stubbed in
	   the test build, so the machine is described here instead: a plain RiscPC
	   with enough RAM and VRAM to reach the desktop. */
	memset(&config, 0, sizeof(config));
	config.model = Model_RPCSA110;
	config.mem_size = getenv("RPCEMU_MEM") ? (unsigned) atoi(getenv("RPCEMU_MEM")) : 256;
	config.vram_size = getenv("RPCEMU_VRAM") ? (unsigned) atoi(getenv("RPCEMU_VRAM")) : 8;
	config.refresh = 60;
	config.cpu_idle = 0;
	if (getenv("RPCEMU_ROM_DIR") != NULL) {
		snprintf(config.rom_dir, sizeof(config.rom_dir), "%s", getenv("RPCEMU_ROM_DIR"));
	}
	printf("boot_trace: data directory %s, machine %s, ROM directory \"%s\"\n",
	    rpcemu_get_datadir(), rpcemu_get_machine_datadir(), config.rom_dir);

	/* The GUI does this when the user picks a model; with no GUI the machine
	   struct would otherwise keep its defaults and the ROM would be rejected as
	   wrong for the CPU. */
	rpcemu_model_changed(config.model);

	rpcemu_start();

	/* PC trace mode. Instead of 20000-cycle chunks, run one dispatcher call at a
	   time and print the PC after each, deriving emulated time from the cycles
	   actually consumed so the hardware ticks stay in proportion. Two builds
	   traced this way diff to an exact divergence point, which chunk-granularity
	   milestones cannot give. */
	if (getenv("RPCEMU_PC_TRACE") != NULL) {
		const unsigned long n = strtoul(getenv("RPCEMU_PC_TRACE"), NULL, 0);
		unsigned long i;
		uint64_t next_iomd = 0, next_frame = 0;
		uint64_t cyc = 0;

		for (i = 0; i < n; i++) {
			/* One block per call on both backends. Without this the two run
			   different numbers of self-linked loop iterations per dispatcher
			   call, and the traces diverge on scheduling rather than on any
			   difference in result. */
			linecyc = 0;
			cyc += (uint64_t) arm_exec();
			virtual_ns = cyc * 5;		/* 200MHz: 5ns a cycle */

			if (virtual_ns >= next_iomd) {
				gentimerirq(virtual_ns);
				next_iomd = virtual_ns + 2000000;	/* 2ms */
			}
			if (virtual_ns >= next_frame) {
				drawscre++;
				iomd_flyback(1);
				next_frame = virtual_ns + 16666666;	/* 60Hz */
			}
			/* Timer 0 as the guest sees it. out_latch is what a read of
			   T0LOW/T0HIGH returns, and it only changes when the guest's write
			   to T0LATCH reaches the device. counter is the live count. If the
			   latch never moves while the counter does, the guest's write is
			   being lost, which is exactly what would hang the HAL's calibrated
			   delay loop for ever. */
			printf("%08x %u %08x t0=%08x/%08x", arm.reg[15] & arm.r15_mask,
			    arm.mode, arm.reg[16], iomd.t0.out_latch,
			    (uint32_t) iomd.t0.counter);
			for (int r = 0; r < 15; r++) {
				printf(" %08x", arm.reg[r]);
			}
			printf("\n");
		}
		return 0;
	}

	last_mode = arm.mode;
	printf("boot_trace: machine started, PC %08x mode %u\n", arm.reg[15], arm.mode);

	for (chunk = 0; chunk < chunks; chunk++) {
		execrpcemu();

		/* One chunk is 20000 cycles at 5ns each. */
		virtual_ns += 20000 * 5;

		/* The periodic hardware ticks. Both come from the GUI's timer thread in
		   the real emulator (EmulatorHost::ServiceTimers), so with no GUI nothing
		   generates them at all, and the machine waits for hardware that never
		   moves. The HAL's very first act is to program IOMD Timer 0 and spin on
		   IRQSTA until its bit sets, so without the 2ms tick a boot gets no
		   further than that loop.

		   A chunk is 100us of guest time: a 2ms IOMD tick is every 20 chunks and
		   a 60Hz frame every 167. gentimerirq() corrects the timers from the
		   absolute clock it is passed, so the virtual clock drives them directly. */
		if ((chunk % 20) == 0) {
			gentimerirq(virtual_ns);
		}
		if ((chunk % 167) == 0) {
			drawscre++;		/* one frame owed, as vblupdate() does */
			iomd_flyback(1);
		}

		/* One sample per chunk. Enough to find a spin, cheap enough to leave
		   on, and it cannot perturb the emulation.

		   r15_mask matters: in 26-bit mode r15 carries the PSR in its top six
		   bits and the mode in its bottom two, so sampling it raw buckets the
		   flags rather than the address and every sample lands in the same
		   fictitious region. */
		pc_samples[(arm.reg[15] & arm.r15_mask) >> REGION_SHIFT]++;
		samples++;

		/* Every event bit ever seen pending, and how often any was. If an IRQ is
		   never taken, this says whether it was never raised or was raised and
		   never serviced, which are faults in completely different places. */
		event_seen |= arm.event;
		if (arm.event != 0) {
			event_chunks++;
		}

		/* Persistent state, unlike arm.event which is set and consumed inside a
		   chunk. IRQA status and mask say whether the hardware ever asked for an
		   interrupt and whether the OS unmasked it; the CPSR I bit says whether
		   the CPU was ever willing to take one. Between them, a machine that
		   never takes an IRQ is either not being asked, or is refusing. */
		irqa_status_seen |= iomd.irqa.status;
		irqa_mask_seen |= iomd.irqa.mask;
		if ((arm.reg[16] & 0x80) == 0) {
			irqs_enabled_chunks++;
		}

		if (mmu) {
			reached(M_MMU, chunk);
		}
		if (arm.mode != last_mode) {
			reached(M_MODE, chunk);
			last_mode = arm.mode;
		}
		if ((arm.mode & 0xf) == ABORT) {
			reached(M_ABORT, chunk);
		}
		if ((arm.mode & 0xf) == IRQ) {
			reached(M_IRQ, chunk);
		}
		if ((arm.mode & 0xf) == USER) {
			reached(M_USER, chunk);
		}
		if (vidc_get_xsize() > 0) {
			reached(M_VIDC, chunk);
		}
		if (framestore_dirty()) {
			reached(M_FRAMESTORE, chunk);
		}
	}

	printf("\nboot_trace: milestones\n");
	for (i = 0; i < M_COUNT; i++) {
		printf("  %-22s %s", milestones[i].name,
		    milestones[i].seen ? "yes" : "NO");
		if (milestones[i].seen) {
			printf(" (chunk %lu)", milestones[i].chunk);
		}
		printf("\n");
	}

	printf("\nboot_trace: arm.event bits ever pending: 0x%02x (IRQ bit 0x40 %s), "
	    "pending on %lu of %lu samples\n", event_seen,
	    (event_seen & 0x40) ? "SEEN" : "never set", event_chunks, samples);

	printf("boot_trace: IOMD IRQA status bits seen 0x%02x, mask bits seen 0x%02x, "
	    "both together 0x%02x\n", irqa_status_seen, irqa_mask_seen,
	    irqa_status_seen & irqa_mask_seen);
	printf("boot_trace: CPSR had IRQs enabled on %lu of %lu samples\n",
	    irqs_enabled_chunks, samples);

	printf("\nboot_trace: where the machine was, by 64K region\n");
	for (int shown = 0;;) {
		unsigned long best = 0;
		int best_i = -1;

		for (i = 0; i < REGIONS; i++) {
			if (pc_samples[i] > best) {
				best = pc_samples[i];
				best_i = i;
			}
		}
		if (best_i < 0 || shown >= 12) {
			break;
		}
		shown++;
		printf("  %08x  %5.1f%%  (%lu samples)\n",
		    (uint32_t) best_i << REGION_SHIFT,
		    100.0 * (double) best / (double) samples, best);
		pc_samples[best_i] = 0;
	}

	printf("\nboot_trace: finished at PC %08x, mode %u, %s\n", arm.reg[15] & arm.r15_mask,
	    arm.mode, arm.r15_mask == 0xfffffffcu ? "32-bit" : "26-bit");
	printf("boot_trace: %s\n",
	    milestones[M_FRAMESTORE].seen ? "PASS - the machine drew something"
	                                  : "FAIL - nothing was ever drawn");

	return milestones[M_FRAMESTORE].seen ? 0 : 1;
}
