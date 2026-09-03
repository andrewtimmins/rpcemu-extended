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

## A breakpoint in your own source

`SWI &FFFFFF` — the instruction word `&EFFFFFFF` — halts the machine in the
debugger. The emulator swallows it, so RISC OS never sees a SWI it does not
know, no error is raised, and execution resumes at the instruction after it.
With the debugger disabled (`debug_enabled=0`) it is still swallowed and nothing
else happens, so code carrying one runs unchanged.

The pause reason reads *breakpoint SWI* and the halt PC is the SWI's own
address. It needs nothing switched on first: putting the instruction in the
source is the request.

It exists because an address breakpoint is awkward for module code, where the
address moves every time the module is rebuilt and reloaded. Suggested in
discussion #223 by somebody who had been getting the same effect by trapping a
SWI in his own module's handler.

The number is in the unallocated range and is matched on the whole 24-bit
comment field, not on the masked SWI number `opSWI()` works with — several
ordinary SWIs share that masked value and must not be caught by it.

## A reserved CPU mode

The mode field of the CPSR has sixteen possible values and the architecture
defines seven of them. Writing one of the other nine is UNPREDICTABLE on real
hardware, so it is always a fault in the guest, and there is no checkbox for it:
the machine halts whenever it happens, with the pause reason given as *reserved
CPU mode written* and the halt PC naming the instruction that wrote it. The mode
itself is in the Trace tab entry beside it.

The emulator used to report this with a fatal error and exit, which took the
registers, the memory and the disassembly with it at exactly the moment somebody
wanted to read them (issue #227). The halted machine can be inspected and then
stepped or resumed like any other. Register banking in a reserved mode follows
User and System mode, so what the inspector shows is defined rather than
whatever happened to be in the banks.

The one case that still ends the emulator is a machine with the debugger turned
off (`debug_enabled=0`), where there is nothing to halt into: neither the
inspector nor the debug socket is available, so a halt would be indistinguishable
from a hang.

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
