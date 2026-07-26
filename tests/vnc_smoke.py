#!/usr/bin/env python3
"""
Boot a machine headlessly and check that RISC OS actually got going.

Everything else in CI stops at "the binary starts and parses its arguments".
This drives a real boot instead: it starts the emulator with --headless, waits
for the built-in VNC server to accept a connection, captures the framebuffer
over RFB, and checks the guest drew something rather than leaving a blank or
frozen screen. That exercises the CPU, memory, VIDC, ROM loading and the VNC
server end to end, on whichever platform the job runs.

Note a fresh machine reaches a Supervisor prompt, not the desktop: the shipped
machines/<name>/hostfs/ holds only the HardDisc4 installer, which has to be run
from inside RISC OS to produce a !Boot. The check accounts for that - see
describe_screen().

The RFB handling mirrors tools/mcp/rpcemu_mcp.py (RFB 3.3, security None, Raw
encoding, 32bpp true colour) so there is one protocol implementation to reason
about, not two.

Usage:
  vnc_smoke.py --binary <path> [--machine Default] [--port 5900]
               [--boot-timeout 60] [--settle 20] [--save shot.png]

Exit status is 0 when the machine booted and drew to the screen, and 1
otherwise, with the reason on stderr.
"""

import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


class SmokeError(Exception):
    pass


# ---------------------------------------------------------------------------
# RFB client (subset of tools/mcp/rpcemu_mcp.py)
# ---------------------------------------------------------------------------


def _recvn(s: socket.socket, n: int, what: str) -> bytes:
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SmokeError(f"VNC connection closed during {what}")
        b += c
    return b


def vnc_capture(host: str, port: int) -> tuple[int, int, bytearray]:
    """Connect, grab one framebuffer update, return (width, height, RGBX bytes)."""
    s = socket.create_connection((host, port), timeout=15)
    s.settimeout(30)
    try:
        _recvn(s, 12, "handshake")  # server "RFB 003.00x\n"
        s.sendall(b"RFB 003.003\n")
        (sec,) = struct.unpack(">I", _recvn(s, 4, "handshake"))
        if sec == 0:
            (n,) = struct.unpack(">I", _recvn(s, 4, "handshake"))
            raise SmokeError("VNC rejected us: " + _recvn(s, n, "handshake").decode("latin-1"))
        if sec != 1:
            raise SmokeError(f"VNC wants authentication (security type {sec}); the test config must have no password")

        s.sendall(b"\x01")  # ClientInit, shared
        w, h = struct.unpack(">HH", _recvn(s, 4, "server init"))
        _recvn(s, 16, "pixel format")
        (nl,) = struct.unpack(">I", _recvn(s, 4, "desktop name"))
        _recvn(s, nl, "desktop name")

        if w == 0 or h == 0:
            raise SmokeError(f"VNC reported a {w}x{h} framebuffer")

        # 32bpp true colour, little-endian, shifts 0/8/16 so pixels arrive as
        # [R, G, B, x] - the order the PNG encoder below expects.
        spf = (
            struct.pack(">BBBB", 0, 0, 0, 0)
            + struct.pack(">BBBB", 32, 24, 0, 1)
            + struct.pack(">HHH", 255, 255, 255)
            + struct.pack(">BBB", 0, 8, 16)
            + b"\x00\x00\x00"
        )
        s.sendall(spf)
        s.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))  # SetEncodings: Raw
        s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))  # FramebufferUpdateRequest

        mt = _recvn(s, 1, "message type")[0]
        while mt != 0:  # skip anything that is not a FramebufferUpdate
            if mt == 1:
                _recvn(s, 5, "colour map")
            elif mt == 3:
                _recvn(s, 7, "cut text")
            else:
                raise SmokeError(f"unexpected VNC message type {mt}")
            mt = _recvn(s, 1, "message type")[0]

        _recvn(s, 1, "padding")
        (nrect,) = struct.unpack(">H", _recvn(s, 2, "rect count"))
        fb = bytearray(w * h * 4)
        for _ in range(nrect):
            x, y, rw, rh, enc = struct.unpack(">HHHHi", _recvn(s, 12, "rect header"))
            if enc != 0:
                raise SmokeError(f"VNC used encoding {enc}; only Raw is handled")
            data = _recvn(s, rw * rh * 4, "rect data")
            for row in range(rh):
                src = row * rw * 4
                dst = ((y + row) * w + x) * 4
                fb[dst : dst + rw * 4] = data[src : src + rw * 4]
        return w, h, fb
    finally:
        s.close()


def encode_png(w: int, h: int, fb: bytearray) -> bytes:
    """Minimal RGB PNG, so a failing run can upload what it saw."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        row = fb[y * w * 4 : (y + 1) * w * 4]
        for x in range(w):
            raw += row[x * 4 : x * 4 + 3]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )


# ---------------------------------------------------------------------------
# Screen plausibility
# ---------------------------------------------------------------------------


def describe_screen(w: int, h: int, fb: bytearray) -> tuple[bool, str]:
    """
    Decide whether the guest reached the desktop.

    The test machine is seeded with a !Boot that runs Desktop (see
    seed_boot_file), so a healthy run shows the RISC OS desktop: a coloured
    backdrop with an icon bar, not the Supervisor prompt a fresh machine stops
    at.

    Two things are asked. The screen must not be blank or frozen, measured as
    the share of pixels differing from the dominant colour. And it must have
    the range of colour a desktop has and a text screen does not - which is
    what makes this stronger than "something was drawn": a boot that dies after
    printing an error still lights up pixels, but stays at two colours.

    Deliberately loose beyond that: no reference image, so a cosmetic RISC OS,
    ROM or mode change cannot fail the build.
    """
    if w == 0 or h == 0:
        return False, "empty framebuffer"

    # Count how much of the screen differs from the most common colour. Text on
    # a plain background lights up a small but far from negligible fraction;
    # a blank or frozen screen lights up almost none.
    step = max(1, (w * h) // 40000)
    colours: dict[tuple[int, int, int], int] = {}
    total = 0
    for i in range(0, w * h, step):
        px = (fb[i * 4], fb[i * 4 + 1], fb[i * 4 + 2])
        colours[px] = colours.get(px, 0) + 1
        total += 1

    if total == 0:
        return False, "no pixels sampled"

    bg_px, bg_n = max(colours.items(), key=lambda kv: kv[1])
    foreground = 1.0 - (bg_n / total)

    summary = (
        f"{w}x{h}, {len(colours)} distinct colours in {total} sampled pixels, "
        f"background {bg_px} covering {bg_n / total:.1%}, "
        f"foreground {foreground:.2%}"
    )

    if len(colours) < 2:
        return False, f"screen is a single flat colour, nothing was drawn ({summary})"
    if foreground < 0.0005:
        return False, f"screen is effectively blank, the guest drew almost nothing ({summary})"
    # A two-colour screen is a text mode: either the Supervisor prompt, meaning
    # !Boot did not run, or an error message. Both mean the desktop was not
    # reached. A real desktop has a backdrop, icons and window furniture, so it
    # clears this by a wide margin even in 16 colours.
    if len(colours) < 8:
        return False, (
            f"screen looks like a text mode, not the desktop - !Boot may not have "
            f"run ({summary})"
        )
    return True, summary


# ---------------------------------------------------------------------------
# Driving the emulator
# ---------------------------------------------------------------------------


def make_test_config(src_cfg: str, dst_cfg: str, name: str, port: int) -> None:
    """
    Copy the machine config with the VNC server switched on, under a new name.

    The shipped Default.cfg has vnc_enabled=0, and headless mode refuses to
    start without it. The tracked config is left alone: CI should not depend on
    mutating a file that is also a user-facing default.

    The "name" field matters as much as the filename - config_load() derives the
    machine data directory from it, so leaving it as "Default" would point the
    test at machines/Default/ and write into the real machine's HostFS.
    """
    with open(src_cfg, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    def set_key(t: str, key: str, value: str) -> str:
        pat = re.compile(rf"^{re.escape(key)}=.*$", re.MULTILINE)
        if pat.search(t):
            return pat.sub(f"{key}={value}", t)
        return t.rstrip("\n") + f"\n{key}={value}\n"

    text = set_key(text, "name", name)
    text = set_key(text, "vnc_enabled", "1")
    text = set_key(text, "vnc_port", str(port))
    text = set_key(text, "vnc_password", "")

    with open(dst_cfg, "w", encoding="utf-8") as f:
        f.write(text)


def seed_boot_file(hostfs: str) -> None:
    """
    Give the test machine a !Boot that starts the desktop.

    The shipped HostFS holds only HardDisc4.5.30.util, an installer that has to
    be run from inside RISC OS, so a fresh machine stops at a Supervisor prompt.
    RISC OS runs $.!Boot at startup if it is there, so a one-line Obey file
    (filetype &feb) containing "Desktop" is enough to reach the desktop - which
    makes this a far stronger check than "some text appeared". No leading "*":
    the lines of an Obey file are commands already.
    """
    os.makedirs(hostfs, exist_ok=True)
    # HostFS encodes the RISC OS filetype in the host filename suffix: ,feb is
    # Obey. Without it the file is plain Text and RISC OS will not run it.
    with open(os.path.join(hostfs, "!Boot,feb"), "w", encoding="ascii", newline="\n") as f:
        f.write("Desktop\n")


def wait_for_port(host: str, port: int, proc: subprocess.Popen, timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SmokeError(f"the emulator exited early with status {proc.returncode}")
        try:
            with socket.create_connection((host, port), timeout=2):
                return
        except OSError:
            time.sleep(1)
    raise SmokeError(f"the VNC server did not accept a connection within {timeout:.0f}s")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="emulator to run")
    ap.add_argument("--machine", default="Default", help="machine config name")
    ap.add_argument("--port", type=int, default=5900, help="VNC port to use")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--boot-timeout", type=float, default=60.0,
                    help="seconds to wait for the VNC server to come up "
                         "(it listens about a second in, so this is slack for a "
                         "loaded runner, not an expected wait)")
    ap.add_argument("--settle", type=float, default=20.0,
                    help="seconds to let RISC OS reach the desktop after VNC "
                         "accepts (a boot takes about ten)")
    ap.add_argument("--datadir",
                    help="directory holding configs/, roms/ and machines/; "
                         "found automatically when it is beside the binary or "
                         "in a .app's Contents/Resources")
    ap.add_argument("--save", help="write the captured screen here as a PNG")
    args = ap.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.isfile(binary):
        print(f"error: no such binary: {binary}", file=sys.stderr)
        return 1

    # Locate the data directory. A staged Linux/Windows release keeps configs/,
    # roms/ and machines/ beside the executable, but a macOS .app puts the
    # read-only payload in Contents/Resources while the binary sits in
    # Contents/MacOS with nothing next to it. Rather than encode either layout,
    # look for the directory that actually holds configs/ and then pin it with
    # RPCEMU_DATADIR, which the emulator honours on every platform.
    workdir = os.path.dirname(binary)
    datadir = args.datadir
    if not datadir:
        candidates = [
            workdir,                                              # staged release
            os.path.join(os.path.dirname(workdir), "Resources"),  # .app bundle
        ]
        for c in candidates:
            if os.path.isdir(os.path.join(c, "configs")):
                datadir = c
                break
        if not datadir:
            print("error: no 'configs' directory found near the binary; looked in:",
                  file=sys.stderr)
            for c in candidates:
                print(f"  {c}", file=sys.stderr)
            print("Pass --datadir to say where the data lives.", file=sys.stderr)
            return 1

    datadir = os.path.abspath(datadir)
    configs = os.path.join(datadir, "configs")
    src_cfg = os.path.join(configs, f"{args.machine}.cfg")
    if not os.path.isfile(src_cfg):
        print(f"error: no config at {src_cfg}", file=sys.stderr)
        return 1

    if not os.access(datadir, os.W_OK):
        print(f"error: {datadir} is not writable; the test needs to add a config there",
              file=sys.stderr)
        return 1

    test_name = f"{args.machine}-vncsmoke"
    test_cfg = os.path.join(configs, f"{test_name}.cfg")
    make_test_config(src_cfg, test_cfg, test_name, args.port)

    # The machine directory is keyed by the config's "name" field, which
    # make_test_config() has just set to test_name - so the test gets its own
    # directory, seeded from the real one for the same CMOS starting point, and
    # the real machine's HostFS is never touched.
    machines = os.path.join(datadir, "machines")
    src_machine = os.path.join(machines, args.machine)
    dst_machine = os.path.join(machines, test_name)
    if os.path.isdir(dst_machine):
        shutil.rmtree(dst_machine)  # start from a known state on a re-run
    if os.path.isdir(src_machine):
        shutil.copytree(src_machine, dst_machine)

    seed_boot_file(os.path.join(dst_machine, "hostfs"))

    env = dict(os.environ)
    env["RPCEMU_NO_GUI_MESSAGES"] = "1"
    # Pin the data location so the emulator uses the tree we just prepared,
    # rather than re-deriving it (a bundle would otherwise seed and use
    # ~/RPCEmu, where our test config does not exist).
    env["RPCEMU_DATADIR"] = datadir

    log = tempfile.NamedTemporaryFile(prefix="vnc-smoke-", suffix=".log", delete=False)
    print(f"==> starting {binary} --headless --machine {test_name}", flush=True)
    proc = subprocess.Popen(
        [binary, "--headless", "--machine", test_name],
        cwd=datadir, env=env, stdout=log, stderr=subprocess.STDOUT,
    )

    rc = 1
    try:
        wait_for_port(args.host, args.port, proc, args.boot_timeout)
        print(f"==> VNC accepted on {args.host}:{args.port}; "
              f"letting RISC OS settle for {args.settle:.0f}s", flush=True)
        # The server accepts as soon as it is listening, which is well before
        # the desktop is drawn. Give the guest time to get there.
        time.sleep(args.settle)

        w, h, fb = vnc_capture(args.host, args.port)
        if args.save:
            with open(args.save, "wb") as f:
                f.write(encode_png(w, h, fb))
            print(f"==> wrote {args.save}", flush=True)

        ok, summary = describe_screen(w, h, fb)
        if ok:
            print(f"PASS: {summary}", flush=True)
            rc = 0
        else:
            print(f"FAIL: {summary}", file=sys.stderr, flush=True)
    except SmokeError as e:
        print(f"FAIL: {e}", file=sys.stderr, flush=True)
    except OSError as e:
        print(f"FAIL: {e}", file=sys.stderr, flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()  # SIGTERM: headless saves CMOS and discs on this
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        log.close()
        with open(log.name, "r", encoding="utf-8", errors="replace") as f:
            output = f.read().strip()
        if output:
            print("--- emulator output ---", flush=True)
            print(output, flush=True)
        os.unlink(log.name)

    return rc


if __name__ == "__main__":
    sys.exit(main())
