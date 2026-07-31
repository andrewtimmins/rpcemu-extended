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
 * perfprof.c - a sampling profiler for the emulator thread.
 *
 * See perfprof.h for why this exists. The short version: the emulator thread is
 * pinned at 100% of a core, every performance question here is really "what is
 * it doing?", and the usual tools are not available on the development machine.
 *
 * How it works. timer_create() with CLOCK_THREAD_CPUTIME_ID and SIGEV_THREAD_ID
 * gives a timer that charges the calling thread's own CPU time and delivers its
 * signal to that thread, so no other thread's work is sampled and time spent
 * asleep is not counted. The handler reads the interrupted program counter out of
 * the signal's machine context and files it: inside the recompiler's code cache,
 * or not.
 *
 * The handler does nothing that is not async-signal-safe - it increments counters
 * in fixed-size arrays and returns. Symbolising happens in perfprof_report(),
 * outside the handler, where dladdr() and printf are allowed.
 */

#ifdef RPCEMU_PROFILE

#define _GNU_SOURCE	/* dladdr, REG_RIP */

#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

#include "rpcemu.h"
#include "perfprof.h"

/*
 * The code cache's bounds, so a sample inside recompiled code can be told from
 * one in C. Only the recompiler has a cache: an interpreter build has no such
 * range, and every sample is then attributed by symbol instead.
 */
#if defined(__x86_64__) || defined(__amd64__)
#include "codegen_amd64.h"
#elif defined(__aarch64__)
#include "codegen_arm64.h"
#endif

/**
 * Samples a second.
 *
 * Fast enough to say something useful about a ten-second window, slow enough
 * that the signal itself is not what is being measured: at 1kHz the handler runs
 * for a few hundred nanoseconds out of every millisecond.
 */
#define PROF_HZ		1000

/**
 * Distinct program counters remembered outside the code cache.
 *
 * An open-addressed table, because the handler cannot allocate. Anything that
 * does not fit is counted but not attributed, and the report says how much that
 * was - a profiler that quietly drops samples is worse than no profiler.
 */
#define PROF_SLOTS	4096

static struct {
	timer_t timer;
	int running;

	uintptr_t code_lo;	/* the recompiler's code cache */
	uintptr_t code_hi;

	unsigned long samples;
	unsigned long in_code;		/* inside recompiled blocks */
	unsigned long in_other;		/* everywhere else, attributed below */
	unsigned long overflowed;	/* no slot free, so not attributed */

	uintptr_t pc[PROF_SLOTS];
	unsigned long count[PROF_SLOTS];

	unsigned report_seconds;
	unsigned long report_at;
} prof;

/*
 * The interrupted program counter, per architecture.
 *
 * Only the two the recompiler targets are handled; anywhere else the profiler
 * reports no samples rather than guessing at a register layout.
 */
static uintptr_t
prof_pc(const ucontext_t *uc)
{
#if defined(__x86_64__)
	return (uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	return (uintptr_t) uc->uc_mcontext.pc;
#else
	(void) uc;
	return 0;
#endif
}

static void
prof_handler(int sig, siginfo_t *info, void *ucontext)
{
	const uintptr_t pc = prof_pc((const ucontext_t *) ucontext);
	size_t slot;

	(void) sig;
	(void) info;

	prof.samples++;

	if (pc >= prof.code_lo && pc < prof.code_hi) {
		prof.in_code++;
		return;
	}

	prof.in_other++;

	/* Round to sixteen bytes: it keeps a hot loop's samples together without
	   merging neighbouring functions. */
	slot = (size_t) ((pc >> 4) * 2654435761u) & (PROF_SLOTS - 1);
	for (size_t tries = 0; tries < 64; tries++) {
		const size_t s = (slot + tries) & (PROF_SLOTS - 1);

		if (prof.pc[s] == 0) {
			prof.pc[s] = pc;
			prof.count[s] = 1;
			return;
		}
		if ((prof.pc[s] >> 4) == (pc >> 4)) {
			prof.count[s]++;
			return;
		}
	}

	prof.overflowed++;
}

void
perfprof_start(unsigned report_seconds)
{
	struct sigaction sa;
	struct sigevent sev;
	struct itimerspec its;

	if (prof.running) {
		return;
	}

	memset(&prof, 0, sizeof(prof));
	prof.report_seconds = report_seconds ? report_seconds : 10;
	prof.report_at = (unsigned long) PROF_HZ * prof.report_seconds;

	/* The code cache, so a sample can be told from a C one. On this build it is
	   a plain array; on a MAP_JIT platform it is allocated, and either way the
	   bounds come from the same two expressions. */
#ifdef BLOCKS
	prof.code_lo = (uintptr_t) &rcodeblock[0][0];
	prof.code_hi = prof.code_lo + (uintptr_t) BLOCKS * sizeof(rcodeblock[0]);
#else
	prof.code_lo = prof.code_hi = 0;	/* no code cache on this backend */
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = prof_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPROF, &sa, NULL) != 0) {
		rpclog("perfprof: cannot install the handler\n");
		return;
	}

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = SIGPROF;
	sev._sigev_un._tid = (int) syscall(SYS_gettid);

	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &prof.timer) != 0) {
		rpclog("perfprof: cannot create the thread CPU timer\n");
		return;
	}

	memset(&its, 0, sizeof(its));
	its.it_interval.tv_nsec = 1000000000L / PROF_HZ;
	its.it_value = its.it_interval;
	if (timer_settime(prof.timer, 0, &its, NULL) != 0) {
		rpclog("perfprof: cannot start the timer\n");
		return;
	}

	prof.running = 1;
	rpclog("perfprof: sampling the emulator thread at %d Hz, reporting every "
	       "%u seconds; code cache %p to %p\n", PROF_HZ, prof.report_seconds,
	    (void *) prof.code_lo, (void *) prof.code_hi);
}

/** Order for the report: heaviest first. */
static int
prof_worse(const void *a, const void *b)
{
	const size_t ia = *(const size_t *) a, ib = *(const size_t *) b;

	if (prof.count[ia] < prof.count[ib]) {
		return 1;
	}
	if (prof.count[ia] > prof.count[ib]) {
		return -1;
	}
	return 0;
}

void
perfprof_report(void)
{
	size_t order[PROF_SLOTS];
	size_t used = 0;
	size_t i;
	const unsigned long total = prof.samples;

	if (!prof.running || total == 0) {
		return;
	}

	rpclog("perfprof: %lu samples: recompiled code %lu (%.1f%%), other %lu "
	       "(%.1f%%)%s\n", total, prof.in_code,
	    100.0 * (double) prof.in_code / (double) total, prof.in_other,
	    100.0 * (double) prof.in_other / (double) total,
	    prof.overflowed ? " [table full, some unattributed]" : "");

	for (i = 0; i < PROF_SLOTS; i++) {
		if (prof.pc[i] != 0) {
			order[used++] = i;
		}
	}
	qsort(order, used, sizeof(order[0]), prof_worse);

	for (i = 0; i < used && i < 20; i++) {
		const size_t s = order[i];
		Dl_info info;
		const char *name = "?";
		unsigned long off = 0;
		unsigned long file_off = 0;

		/*
		 * Outside the handler, so this may be as unsafe as it likes.
		 *
		 * dli_sname is usually empty here: a normal build exports only its
		 * dynamic symbols, and everything interesting in this program is local.
		 * What is always available is dli_fbase, the address the executable was
		 * loaded at, so the offset within the file is reported as well - and
		 * that is what resolves a name:
		 *
		 *   addr2line -fpe build/bin/rpcemu-recompiler <offset>
		 */
		if (dladdr((void *) prof.pc[s], &info) != 0) {
			if (info.dli_sname != NULL) {
				name = info.dli_sname;
				off = (unsigned long) (prof.pc[s] -
				    (uintptr_t) info.dli_saddr);
			}
			file_off = (unsigned long) (prof.pc[s] -
			    (uintptr_t) info.dli_fbase);
		}
		rpclog("perfprof:   %5.1f%%  file+0x%-8lx  %s+%lu\n",
		    100.0 * (double) prof.count[s] / (double) total, file_off, name,
		    off);
	}

	/* A fresh window, so a report describes the seconds before it rather than
	   everything since the machine started. */
	prof.samples = prof.in_code = prof.in_other = prof.overflowed = 0;
	memset(prof.pc, 0, sizeof(prof.pc));
	memset(prof.count, 0, sizeof(prof.count));
}

/**
 * Called from the emulator's own loop, which is where the report is due.
 *
 * The handler cannot log, so somebody has to notice that the window is up.
 */
void
perfprof_poll(void)
{
	if (prof.running && prof.samples >= prof.report_at) {
		perfprof_report();
	}
}

void
perfprof_stop(void)
{
	if (!prof.running) {
		return;
	}
	perfprof_report();
	timer_delete(prof.timer);
	prof.running = 0;
}

#endif /* RPCEMU_PROFILE */
