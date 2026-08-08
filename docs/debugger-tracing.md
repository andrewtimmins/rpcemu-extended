# Debugger: exception trapping, SWI tracing and logging watchpoints

> These are the *trace* features. For breakpoints with conditions, stepping over
> and out of calls, backtraces and symbols, see [debugcmd.md](debugcmd.md).

The Machine Inspector's debugger can, in addition to breakpoints and
watchpoints, trap CPU exceptions, trace operating-system calls (SWIs) and record
memory accesses to a running log. These are controlled from the **Trace** tab of
the inspector and share a single event log.

## Exception trapping

Three CPU exceptions can be trapped, each with its own checkbox:

- **Undefined instruction**
- **Prefetch abort**
- **Data abort**

When a trapped exception occurs the emulator pauses at the first instruction of
the exception handler, exactly as it would at a breakpoint, so you can inspect
registers and memory at the point of the fault. (IRQ and FIQ are ordinary
interrupts and are not trappable here; a SWI is caught by SWI tracing below.)

Exceptions can also be **logged** to the Trace tab without halting — useful for
seeing, for example, the harmless app-space probes RISC OS performs during boot.

## SWI tracing

SWI tracing records every operating-system call the guest makes. Options:

- **Enable** — emit a Trace event for each SWI.
- **Halt on SWI** — pause when a matching SWI is executed.
- **Filter** — restrict tracing/halting to an inclusive SWI-number range (the
  full range means all SWIs). SWI names are decoded from the built-in
  disassembler table.

## Logging watchpoints

An ordinary watchpoint halts the emulator when a chosen address is read or
written. Ticking **"Log only (don't halt)"** when adding a watchpoint instead
records each matching access to the Trace tab and keeps running, so you can watch
how a location changes over time without single-stepping.

## The Trace tab

The Trace tab shows the event log, newest activity appended as it happens:

- each row shows the sequence number, PC, event type and a decoded description
  (SWI name, exception kind, or watch address and value);
- a **dropped-count** indicator appears if events were produced faster than the
  GUI drained them (so you know the log is not complete);
- **Clear** empties the log and an autoscroll toggle controls following.

## How it works

For developers, the mechanism is a single-writer ring buffer in `rpcemu.c`. The
emulator thread pushes `DebugTraceEvent` records from three hooks — an exception
hook (called from both the interpreter and dynarec copies of `exception()`), a
SWI hook (called from the shared `opSWI()`), and the existing memory-access check
(for logging watchpoints). The GUI drains the ring on a timer via a synchronous
`DrainTraceEvents` command and renders it in the Trace tab.

The logging-only paths add no cost to the recompiler's fast path: SWI logging is
gated by a flag checked in `opSWI()` (always interpreted), and exception logging
lives in the cold `exception()` path. Halting (trapping or halt-on-SWI) engages
the per-instruction hooked execution path, the same cost model as a breakpoint.

Whether that path is engaged at all is decided by one flag, `debugger_hook_active`,
recomputed whenever the debugger's state changes and read once per instruction by
both the interpreter and the recompiler's dispatch. With nothing attached the
cost is a predictable load rather than a call. (The interpreter used to call the
hook unconditionally on every instruction, attached or not; the recompiler never
did.) Because a missed refresh would silently stop breakpoints firing, with no
error and no log line, `tests/test_debugger_gate.c` walks every route in and out
of "something is watching" and fails if the flag does not follow.
