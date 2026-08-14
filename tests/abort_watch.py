#!/usr/bin/env python3
"""
Watch the emulated CPU for aborts while it boots, over the DebugCmd socket.

WHY THIS EXISTS. CI could tell that a boot reached the desktop and answered
over HostCmd. It could not tell whether the CPU took a hundred data aborts on
the way. Those are exactly the failures that do not show on a screenshot: an
MMU regression, a bad memory decode, a recompiled block jumping somewhere it
should not. The machine gets there anyway because RISC OS handles the abort,
and the screenshot looks perfect.

The debugger can already log exceptions without halting on them. This turns
that into a CI check.

THE THING THAT MAKES THIS HARD is that a normal, healthy RISC OS boot takes
aborts on purpose. It sizes memory and probes for hardware by reading addresses
that may not answer, and an abort is how it finds out. Any check that simply
fails on "an abort happened" would fail on every green build, so it would be
turned off within a week.

So aborts are split by WHEN they happened:

  - BOOT phase, up to the moment RISC OS answers over HostCmd. Aborts here are
    expected. They are reported, with the faulting instruction disassembled, and
    they do not fail the build. If a baseline of known sites is supplied, ones
    not in it are called out as new.

  - RUNNING phase, after the machine is up and idle. Aborts here are not
    expected and fail the build. Nothing should be probing anything; the guest
    is sitting at a prompt doing nothing.

That split is the whole design. It needs no baseline to be useful, cannot go
stale as the ROM changes, and encodes the actual distinction - "RISC OS is
still working out what hardware it has" versus "the machine is up and something
just faulted".

UNDEFINED INSTRUCTIONS ARE NOT ABORTS and never fail. On this machine they are
routine: the FPA10 in fpa.c implements the common operations in hardware and
raises an undefined-instruction exception for the rest (transcendentals, packed
decimal), exactly as the real one did, so RISC OS's floating point support code
can emulate them. A build with working floating point produces a stream of
them. They are counted so the number is visible, and that is all.
"""

import json
import socket
import time


class AbortWatchError(Exception):
    pass


# TraceEvent type values, from rpcemu.h
TRACE_EXCEPTION = 0

# TraceExceptionKind values, carried in arg0
KIND_UNDEFINED = 0
KIND_PREFETCH_ABORT = 1
KIND_DATA_ABORT = 2

KIND_NAMES = {
    KIND_UNDEFINED: "undefined instruction",
    KIND_PREFETCH_ABORT: "prefetch abort",
    KIND_DATA_ABORT: "data abort",
}

# The kinds that mean something went wrong. Undefined instructions are left out
# deliberately - see the note at the top of this file.
ABORT_KINDS = (KIND_PREFETCH_ABORT, KIND_DATA_ABORT)

# CP15 Fault Status codes, in FSR bits 0-3. This mirrors cp15_fault_status_name()
# in src/cp15.c; the two are separate because this file must run with no build.
#
# The distinction earns its keep: "translation" means nothing was mapped at that
# address, "permission" means something was and the MMU refused the access. They
# read identically in a log that prints only an address, and they send an
# investigation in opposite directions.
FAULT_NAMES = {
    0x5: "translation (section)",
    0x7: "translation (page)",
    0x9: "domain (section)",
    0xb: "domain (page)",
    0xd: "permission (section)",
    0xf: "permission (page)",
}


def fault_status_name(status: int) -> str:
    """Name an FSR value's fault code; codes we never generate stay unnamed."""
    return FAULT_NAMES.get(status & 0xf, "other")


class DebugCmd:
    """
    A client for the emulator's debugger socket.

    Newline-delimited: send one request line, read exactly one line of JSON
    back. Responses are read a byte at a time rather than assumed to arrive
    whole, because a long trace drain will not.
    """

    def __init__(self, spec: str, timeout: float = 10.0):
        self.spec = spec
        self.timeout = timeout
        self._buf = b""
        if spec.startswith("/") or "/" in spec:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(spec)
        else:
            host, _, port = spec.rpartition(":")
            self.sock = socket.create_connection(
                (host or "127.0.0.1", int(port)), timeout=timeout)
            self.sock.settimeout(timeout)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def cmd(self, line: str) -> dict:
        self.sock.sendall((line + "\n").encode("latin-1"))
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AbortWatchError(f"the debugger socket closed during {line!r}")
            self._buf += chunk
        raw, _, self._buf = self._buf.partition(b"\n")
        try:
            return json.loads(raw.decode("latin-1"))
        except ValueError as e:
            raise AbortWatchError(f"unparseable reply to {line!r}: {raw[:200]!r}") from e


def connect(spec: str, deadline: float = 20.0) -> DebugCmd:
    """
    Connect to the debugger socket, waiting for the emulator to create it.

    The socket appears about a second into startup, so the wait is slack for a
    loaded runner rather than an expected delay.
    """
    end = time.time() + deadline
    last = None
    while time.time() < end:
        try:
            dbg = DebugCmd(spec)
            dbg.cmd("ping")
            return dbg
        except (OSError, AbortWatchError) as e:
            last = e
            time.sleep(0.25)
    raise AbortWatchError(f"cannot reach the debugger socket at {spec}: {last}")


class AbortWatcher:
    """
    Accumulates exception events, split by boot phase.

    Takes a callable rather than a socket so the classification can be tested
    without an emulator - which matters, because the logic here is what decides
    whether CI goes red, and it must not be the part that is only ever
    exercised by the thing it is meant to be checking.
    """

    def __init__(self, cmd):
        self.cmd = cmd
        self.events = []        # (phase, kind, pc)
        # (kind, pc) -> {(fault_address, fault_status): count}. Kept beside the
        # events rather than in them so the phase split, the baseline format and
        # the verdict are all unchanged by carrying the extra detail.
        self.faults = {}
        self.dropped = 0
        self.phase = "boot"

    def start(self) -> None:
        """
        Log exceptions without halting on them.

        Logging only: the traps stay off, so the machine runs at full speed and
        boots in the usual time. Turning a trap on would engage the debugger's
        per-instruction path and drop the recompiler to interpretation, which
        would make a CI boot take far longer than the job allows.
        """
        r = self.cmd("trace config log_exceptions=1 data_abort=0 "
                     "prefetch_abort=0 undefined=0 swi_log=0 swi_halt=0")
        if not r.get("ok"):
            raise AbortWatchError(f"cannot turn on exception logging: {r}")

    def mark_running(self) -> None:
        """RISC OS is up. Anything from here on is not hardware probing."""
        self.phase = "running"

    def drain(self) -> int:
        """
        Pull every pending event out of the emulator's ring.

        Drains until empty rather than once: the ring holds 4096 events and
        discards the oldest when it overflows, so a boot that produces more than
        one drain's worth between calls would silently lose the earliest - which
        are the most interesting ones.

        @return how many events were taken
        """
        taken = 0
        while True:
            r = self.cmd("trace 128")
            if not r.get("ok"):
                raise AbortWatchError(f"cannot drain the trace ring: {r}")
            self.dropped += int(r.get("dropped", 0) or 0)
            events = r.get("events") or []
            for ev in events:
                # `seq` and `type` come over as plain numbers; the address-like
                # fields come over as hex strings. See dc_cmd_trace() in
                # src/debugcmd.c, which is the authority on the shape.
                if int(ev.get("type", 0)) != TRACE_EXCEPTION:
                    continue
                kind = int(ev["arg0"], 16)
                pc = int(ev["pc"], 16)
                self.events.append((self.phase, kind, pc))
                # arg1/arg2 are the CP15 fault address and status, and are set
                # for a DATA ABORT only - see DebugTraceEvent in src/rpcemu.h.
                # Recording them for the other kinds would report a previous
                # fault's values as though they belonged to this one.
                if kind == KIND_DATA_ABORT:
                    detail = (int(ev.get("arg1", "0"), 16),
                              int(ev.get("arg2", "0"), 16))
                    seen = self.faults.setdefault((kind, pc), {})
                    seen[detail] = seen.get(detail, 0) + 1
            taken += len(events)
            if len(events) < 128:
                return taken

    # ---- reporting -------------------------------------------------------

    def sites(self, phase=None, kinds=ABORT_KINDS):
        """
        Distinct (kind, pc) sites and how often each was hit.

        Grouped by site because a hundred aborts from one instruction is one
        problem, not a hundred, and a list of raw events hides that.

        @return {(kind, pc): count}, ordered by first appearance
        """
        counts = {}
        for ev_phase, kind, pc in self.events:
            if phase is not None and ev_phase != phase:
                continue
            if kinds is not None and kind not in kinds:
                continue
            counts[(kind, pc)] = counts.get((kind, pc), 0) + 1
        return counts

    def count(self, phase=None, kinds=ABORT_KINDS) -> int:
        return sum(self.sites(phase, kinds).values())

    def describe_site(self, kind: int, pc: int) -> str:
        """
        One line naming a site, with the faulting instruction if it can be read.

        Disassembling it is the difference between "an abort at 0xfc03d870" and
        knowing it was an LDR through a register that was zero. The address is
        often not readable by the time the report is made - the guest may have
        remapped it - so a failure here is not one.
        """
        text = ""
        try:
            r = self.cmd(f"dis {pc:08x} 1")
            if r.get("ok") and r.get("lines"):
                line = r["lines"][0]
                _, _, rest = line.partition(":")
                text = "  " + rest.strip()
        except (AbortWatchError, OSError):
            pass
        return f"{KIND_NAMES.get(kind, '?')} at {pc:08x}{text}{self.describe_fault(kind, pc)}"

    def describe_fault(self, kind: int, pc: int) -> str:
        """
        Why the abort happened, for a site that has a fault status.

        One instruction can fault for more than one reason at different times -
        a page that was absent once and refused another time is two different
        problems - so every distinct (address, status) pair seen is named rather
        than only the first.
        """
        seen = self.faults.get((kind, pc))
        if not seen:
            return ""
        # A status of zero is not a fault code this emulator generates, so an
        # all-zero pair means the abort arrived without the CP15 registers being
        # set. Say nothing rather than dress that up as "fault code 0".
        seen = {k: v for k, v in seen.items() if k != (0, 0)}
        if not seen:
            return ""
        parts = []
        for (addr, status), n in sorted(seen.items(), key=lambda kv: -kv[1]):
            times = "" if len(seen) == 1 else f" x{n}"
            parts.append(f"{fault_status_name(status)} "
                         f"(status {status & 0xff:02x}) on {addr:08x}{times}")
        return "  [" + "; ".join(parts) + "]"

    def report(self, out) -> None:
        """Print everything seen, whether or not it fails the build."""
        undefined = self.count(kinds=(KIND_UNDEFINED,))
        if undefined:
            print(f"NOTE: {undefined} undefined-instruction exceptions "
                  f"(routine: the FPA traps the operations it does not "
                  f"implement to RISC OS's floating point support code)",
                  file=out, flush=True)

        if self.dropped:
            print(f"NOTE: the emulator dropped {self.dropped} trace events, so "
                  f"this report is incomplete", file=out, flush=True)

        for phase, label in (("boot", "while booting"),
                             ("running", "after the machine was up")):
            sites = self.sites(phase)
            if not sites:
                continue
            total = sum(sites.values())
            print(f"==> {total} abort(s) {label}, at {len(sites)} site(s):",
                  file=out, flush=True)
            for (kind, pc), n in sites.items():
                times = "once" if n == 1 else f"{n} times"
                print(f"      {self.describe_site(kind, pc)}  [{times}]",
                      file=out, flush=True)


def load_baseline(path: str):
    """
    Read a baseline of known boot-phase abort sites.

    Format is one site per line, "<kind> <hex pc>", where kind is "data" or
    "prefetch"; blank lines and # comments are skipped.
    """
    kinds = {"data": KIND_DATA_ABORT, "prefetch": KIND_PREFETCH_ABORT}
    sites = set()
    with open(path, "r", encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2 or parts[0] not in kinds:
                raise AbortWatchError(
                    f"{path}:{n}: expected '<data|prefetch> <hex pc>', got {line!r}")
            sites.add((kinds[parts[0]], int(parts[1], 16)))
    return sites


def format_baseline(sites) -> str:
    """Render sites in the baseline file's format, sorted so it diffs cleanly."""
    names = {KIND_DATA_ABORT: "data", KIND_PREFETCH_ABORT: "prefetch"}
    lines = sorted(f"{names[kind]} {pc:08x}" for kind, pc in sites)
    return "\n".join(lines) + ("\n" if lines else "")


def verdict(watcher, baseline=None):
    """
    Decide whether what was seen should fail the build.

    Two rules, and only the first is unconditional:

      1. Any abort after the machine was up fails. Nothing legitimate is
         probing hardware at that point.
      2. If a baseline was supplied, a boot-phase site that is not in it fails.
         Without one, boot-phase aborts are reported and accepted, because a
         healthy RISC OS boot genuinely takes them.

    @return (ok, [reasons])
    """
    reasons = []

    running = watcher.sites("running")
    if running:
        total = sum(running.values())
        reasons.append(
            f"{total} abort(s) after the machine was up, at "
            f"{len(running)} site(s) - nothing should be faulting once RISC OS "
            f"is idle")

    if baseline is not None:
        new = [site for site in watcher.sites("boot") if site not in baseline]
        if new:
            reasons.append(
                f"{len(new)} boot-phase abort site(s) not in the baseline: " +
                ", ".join(f"{KIND_NAMES[k]} at {pc:08x}" for k, pc in new))

    return (not reasons), reasons
