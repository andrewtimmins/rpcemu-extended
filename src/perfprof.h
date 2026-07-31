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
 * perfprof.h - where the emulator thread's time goes.
 *
 * A sampling profiler for the one thread that matters. The emulator thread runs
 * at 100% of a core by design, so "what is it doing?" is the question behind
 * every performance decision here - and it is a question this project kept
 * answering by reading code, because perf(1) and gdb are both unavailable on the
 * machine it is developed on.
 *
 * It samples the thread's own CPU clock rather than wall time, so a sample is a
 * unit of work rather than a unit of waiting, and it splits what it finds into
 * recompiled code and everything else. The "everything else" is symbolised, so
 * the answer arrives as function names rather than addresses.
 *
 * Off unless the build asks for it: -DRPCEMU_PROFILE=ON. It costs a signal a
 * millisecond when on, and nothing at all when off.
 */

#ifndef PERFPROF_H
#define PERFPROF_H

#ifdef RPCEMU_PROFILE

/**
 * Begin sampling, and report every @p report_seconds.
 *
 * Call from the thread to be profiled: the timer is bound to the calling
 * thread's CPU clock, so samples from any other thread are not collected at all.
 */
extern void perfprof_start(unsigned report_seconds);

/** Write what has been collected to the log, and start a fresh window. */
extern void perfprof_report(void);

/** Report if the window is up. Call from the loop being profiled. */
extern void perfprof_poll(void);

/** Stop sampling. Reports once more on the way out. */
extern void perfprof_stop(void);

#else

#define perfprof_start(report_seconds)	((void) 0)
#define perfprof_report()		((void) 0)
#define perfprof_poll()			((void) 0)
#define perfprof_stop()			((void) 0)

#endif /* RPCEMU_PROFILE */

#endif /* PERFPROF_H */
