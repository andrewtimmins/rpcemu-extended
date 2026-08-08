#!/usr/bin/env python3
"""
The CI abort watcher's classification and verdict.

WHY THIS EXISTS. abort_watch.py decides whether CI goes red. Everything it
depends on - a ROM, a real boot, a machine that takes real aborts - is exactly
what is not available when running the unit suite, so without this file the
only thing that ever exercises the logic is the thing it is meant to be
checking. If it were wrong, the symptom would be a boot test that passes
forever, which is indistinguishable from a boot test that works.

The two failures worth guarding against pull in opposite directions:

  - TOO STRICT. A healthy RISC OS boot takes data aborts on purpose, sizing
    memory and probing for hardware by reading addresses that may not answer,
    and the FPA raises undefined-instruction exceptions for every operation it
    does not implement in silicon. A check that failed on any of that would go
    red on every green build and be switched off within the week. So: boot-phase
    aborts must pass, and undefined instructions must never fail at all.

  - TOO LAX. The reason for having this is a regression - an MMU fault, a bad
    memory decode, a recompiled block branching somewhere it should not - that
    a screenshot cannot show, because RISC OS handles the abort and the desktop
    is drawn perfectly anyway. So: an abort once the machine is up must fail,
    and a boot-phase site that is not in a supplied baseline must fail.

The fake below answers the DebugCmd protocol, so the phase split, the drain
loop and the verdict are all checked without an emulator.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import abort_watch
from abort_watch import (KIND_DATA_ABORT, KIND_PREFETCH_ABORT, KIND_UNDEFINED,
                         TRACE_EXCEPTION)

failures = 0


def check(what, ok):
    global failures
    print(f"  {what:<66} {'ok' if ok else 'FAIL'}")
    if not ok:
        failures += 1


# ---------------------------------------------------------------------------
# A fake emulator
# ---------------------------------------------------------------------------


class FakeEmulator:
    """
    Answers the handful of DebugCmd verbs the watcher uses.

    Events are handed out 128 at a time, the same cap the real `trace` command
    applies, so the drain loop is exercised rather than assumed.
    """

    def __init__(self, events=(), dropped=0):
        self.pending = list(events)
        self.dropped = dropped
        self.sent = []

    def queue(self, kind, pc, count=1, type_=TRACE_EXCEPTION):
        for _ in range(count):
            # Matches dc_cmd_trace() exactly: seq and type are plain numbers,
            # the address-like fields are quoted hex strings. A fake that got
            # this wrong would test the watcher against a protocol the emulator
            # does not speak.
            self.pending.append({
                "seq": len(self.pending),
                "type": type_,
                "pc": f"{pc:08x}",
                "opcode": "e5910000",
                "arg0": f"{kind:08x}",
                "arg1": f"{pc:08x}",
                "arg2": "00000000",
            })

    def cmd(self, line):
        self.sent.append(line)
        if line.startswith("trace config"):
            return {"ok": True}
        if line.startswith("trace"):
            batch, self.pending = self.pending[:128], self.pending[128:]
            dropped, self.dropped = self.dropped, 0
            return {"ok": True, "dropped": dropped, "events": batch}
        if line.startswith("dis"):
            return {"ok": True, "lines": ["fc03d870: e5910000  LDR R0, [R1]"]}
        return {"ok": False, "error": f"unknown: {line}"}


def watcher_with(boot=(), running=(), dropped=0):
    """Build a watcher that saw `boot` events, then came up, then `running`."""
    fake = FakeEmulator(dropped=dropped)
    w = abort_watch.AbortWatcher(fake.cmd)
    w.start()
    for kind, pc, n in boot:
        fake.queue(kind, pc, n)
    w.drain()
    w.mark_running()
    for kind, pc, n in running:
        fake.queue(kind, pc, n)
    w.drain()
    return w, fake


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_logging_is_not_trapping():
    print("Turning logging on")

    fake = FakeEmulator()
    w = abort_watch.AbortWatcher(fake.cmd)
    w.start()

    config = [c for c in fake.sent if c.startswith("trace config")]
    check("exception logging is turned on", any("log_exceptions=1" in c for c in config))

    # Trapping would engage the debugger's per-instruction path and drop the
    # recompiler to interpretation, so a CI boot would take far longer than the
    # job allows. Logging is deliberately the only thing switched on.
    joined = " ".join(config)
    check("halting on data aborts is left off", "data_abort=0" in joined)
    check("halting on prefetch aborts is left off", "prefetch_abort=0" in joined)
    check("halting on undefined instructions is left off", "undefined=0" in joined)
    check("halting on SWIs is left off", "swi_halt=0" in joined)


def test_phase_split():
    print("Which phase an abort happened in")

    w, _ = watcher_with(
        boot=[(KIND_DATA_ABORT, 0xFC001000, 3)],
        running=[(KIND_DATA_ABORT, 0xFC002000, 1)],
    )

    check("boot-phase aborts are counted as such", w.count("boot") == 3)
    check("running-phase aborts are counted as such", w.count("running") == 1)
    check("both are counted together", w.count() == 4)
    check("the boot site is attributed to the boot phase",
          list(w.sites("boot")) == [(KIND_DATA_ABORT, 0xFC001000)])
    check("the running site is attributed to the running phase",
          list(w.sites("running")) == [(KIND_DATA_ABORT, 0xFC002000)])


def test_undefined_never_counts():
    print("Undefined instructions")

    # The FPA raises these for every operation it does not implement in
    # silicon, so RISC OS's support code can emulate it. A working machine
    # produces a stream of them, in both phases.
    w, _ = watcher_with(
        boot=[(KIND_UNDEFINED, 0xFC00A000, 500)],
        running=[(KIND_UNDEFINED, 0xFC00A000, 500)],
    )

    check("undefined instructions are not counted as aborts", w.count() == 0)
    check("... in the running phase either", w.count("running") == 0)
    check("... but they are counted for reporting",
          w.count(kinds=(KIND_UNDEFINED,)) == 1000)

    ok, reasons = abort_watch.verdict(w)
    check("a thousand undefined instructions do not fail the build", ok)
    check("... with nothing to report as a reason", reasons == [])


def test_boot_aborts_pass_without_a_baseline():
    print("Aborts while booting")

    # This is the case that decides whether the check survives contact with a
    # real ROM: RISC OS probes for hardware by reading addresses that abort.
    w, _ = watcher_with(boot=[
        (KIND_DATA_ABORT, 0xFC001000, 12),
        (KIND_DATA_ABORT, 0xFC001004, 4),
        (KIND_PREFETCH_ABORT, 0xFC003000, 1),
    ])

    ok, reasons = abort_watch.verdict(w)
    check("aborts while booting do not fail the build", ok)
    check("... with nothing to report as a reason", reasons == [])
    check("... but they are still counted", w.count("boot") == 17)
    check("... and grouped by site, not left as raw events",
          len(w.sites("boot")) == 3)


def test_running_aborts_fail():
    print("Aborts once the machine is up")

    w, _ = watcher_with(
        boot=[(KIND_DATA_ABORT, 0xFC001000, 50)],
        running=[(KIND_DATA_ABORT, 0xFC009000, 1)],
    )

    ok, reasons = abort_watch.verdict(w)
    check("a single abort after boot fails the build", not ok)
    check("... with a reason given", len(reasons) == 1)
    check("... naming how many and where", "1 abort" in reasons[0])

    # A prefetch abort counts the same: branching into unmapped memory is not
    # something a healthy idle machine does either.
    w, _ = watcher_with(running=[(KIND_PREFETCH_ABORT, 0xFC009000, 1)])
    ok, _ = abort_watch.verdict(w)
    check("a prefetch abort after boot fails too", not ok)

    # And a clean run passes
    w, _ = watcher_with(boot=[(KIND_DATA_ABORT, 0xFC001000, 5)])
    ok, _ = abort_watch.verdict(w)
    check("a run with nothing after boot passes", ok)


def test_baseline():
    print("Baselines")

    sites = [(KIND_DATA_ABORT, 0xFC001000, 1), (KIND_DATA_ABORT, 0xFC001004, 1)]
    w, _ = watcher_with(boot=sites)

    known = {(KIND_DATA_ABORT, 0xFC001000), (KIND_DATA_ABORT, 0xFC001004)}
    ok, reasons = abort_watch.verdict(w, known)
    check("a boot matching its baseline passes", ok)

    # A site the baseline does not know is the interesting signal: the same ROM
    # doing the same probing should abort in the same places every time, so a
    # new one means something changed underneath it.
    partial = {(KIND_DATA_ABORT, 0xFC001000)}
    ok, reasons = abort_watch.verdict(w, partial)
    check("a new boot-phase site fails against a baseline", not ok)
    check("... naming the site", "fc001004" in reasons[0])

    # A baseline entry that did not happen is not a failure: probing depends on
    # what hardware is configured, and demanding every known abort occur would
    # make the check fail on a machine with less of it.
    extra = known | {(KIND_DATA_ABORT, 0xFC00BEEF)}
    ok, _ = abort_watch.verdict(w, extra)
    check("a baseline site that did not occur is not a failure", ok)

    # Without a baseline, nothing about the boot phase can fail
    ok, _ = abort_watch.verdict(w, None)
    check("no baseline means no boot-phase failure", ok)


def test_baseline_file():
    print("Baseline files")

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "baseline.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("# probing for a podule that is not fitted\n"
                    "\n"
                    "data     fc001000\n"
                    "data     FC001004   # case should not matter\n"
                    "prefetch fc003000\n")

        sites = abort_watch.load_baseline(path)
        check("every entry is read", len(sites) == 3)
        check("data aborts are read", (KIND_DATA_ABORT, 0xFC001000) in sites)
        check("uppercase hex is read", (KIND_DATA_ABORT, 0xFC001004) in sites)
        check("prefetch aborts are read", (KIND_PREFETCH_ABORT, 0xFC003000) in sites)

        # A malformed baseline must be refused rather than silently read as
        # empty, which would turn the check off without saying so
        for bad in ("wibble fc001000\n", "data\n", "data fc001000 extra\n"):
            with open(path, "w", encoding="utf-8") as f:
                f.write(bad)
            try:
                abort_watch.load_baseline(path)
                check(f"{bad.strip()!r} is refused", False)
            except abort_watch.AbortWatchError:
                check(f"{bad.strip()!r} is refused", True)

        # What is written must be readable again, or recording a baseline and
        # using it would not round-trip
        original = {(KIND_DATA_ABORT, 0xFC001000), (KIND_PREFETCH_ABORT, 0xFC003000)}
        with open(path, "w", encoding="utf-8") as f:
            f.write(abort_watch.format_baseline(original))
        check("a written baseline reads back identically",
              abort_watch.load_baseline(path) == original)

        # Sorted, so a regenerated baseline diffs cleanly rather than churning
        text = abort_watch.format_baseline(
            {(KIND_DATA_ABORT, 0xFC009000), (KIND_DATA_ABORT, 0xFC001000)})
        check("baselines are written in a stable order",
              text.index("fc001000") < text.index("fc009000"))

        with open(path, "w", encoding="utf-8") as f:
            f.write(abort_watch.format_baseline(set()))
        check("an empty baseline is valid", abort_watch.load_baseline(path) == set())


def test_drain_pulls_everything():
    print("Draining the ring")

    # The emulator hands out at most 128 events per call and discards the
    # oldest when its ring overflows, so a drain that stopped after one batch
    # would quietly lose the earliest aborts - the ones worth having.
    fake = FakeEmulator()
    w = abort_watch.AbortWatcher(fake.cmd)
    w.start()
    fake.queue(KIND_DATA_ABORT, 0xFC001000, 300)
    w.drain()

    check("everything is drained, not just the first batch", w.count() == 300)
    check("... taking more than one call",
          len([c for c in fake.sent if c.startswith("trace ") ]) >= 3)
    check("... and stopping once the ring is empty", fake.pending == [])

    # Draining an empty ring must not spin
    before = len(fake.sent)
    w.drain()
    check("draining an empty ring costs one call", len(fake.sent) - before == 1)


def test_dropped_events_are_surfaced():
    print("Dropped events")

    # If the emulator dropped events the report is incomplete, and saying so
    # matters more than the count: "no aborts after boot" means much less when
    # some were thrown away.
    w, _ = watcher_with(boot=[(KIND_DATA_ABORT, 0xFC001000, 1)], dropped=42)
    check("dropped events are recorded", w.dropped == 42)


def test_non_exception_events_ignored():
    print("Other trace events")

    # The ring also carries SWI and watchpoint events. Reading one as an
    # exception would invent an abort out of a SWI number.
    fake = FakeEmulator()
    w = abort_watch.AbortWatcher(fake.cmd)
    w.start()
    fake.queue(KIND_DATA_ABORT, 0xFC001000, 1, type_=1)   # a SWI
    fake.queue(KIND_DATA_ABORT, 0xFC001000, 1, type_=2)   # a watchpoint
    w.drain()
    check("SWI and watchpoint events are not read as aborts", w.count() == 0)


def test_report_is_readable():
    print("The report")

    import io

    w, _ = watcher_with(
        boot=[(KIND_DATA_ABORT, 0xFC001000, 3), (KIND_UNDEFINED, 0xFC00A000, 7)],
        running=[(KIND_PREFETCH_ABORT, 0xFC009000, 1)],
    )
    out = io.StringIO()
    w.report(out)
    text = out.getvalue()

    check("the boot phase is reported", "while booting" in text)
    check("the running phase is reported", "after the machine was up" in text)
    check("sites are named", "fc001000" in text)
    check("repeat counts are given", "3 times" in text)
    check("a single hit reads as 'once'", "once" in text)
    check("undefined instructions are mentioned but not alarming",
          "undefined-instruction" in text and "routine" in text)

    # The faulting instruction is disassembled: the difference between an
    # address and knowing what actually faulted
    check("the faulting instruction is disassembled", "LDR R0, [R1]" in text)


def main():
    print("CI abort watcher\n")

    test_logging_is_not_trapping()
    test_phase_split()
    test_undefined_never_counts()
    test_boot_aborts_pass_without_a_baseline()
    test_running_aborts_fail()
    test_baseline()
    test_baseline_file()
    test_drain_pulls_everything()
    test_dropped_events_are_surfaced()
    test_non_exception_events_ignored()
    test_report_is_readable()

    print(f"\n{'FAILED' if failures else 'All tests passed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
