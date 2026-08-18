#!/usr/bin/env python3
"""
RPCEmu MCP server — drive a RISC OS machine running under RPCEmu from an
MCP client (Claude Code, Claude Desktop, the API's MCP connector, ...).

Phase 1 tools (this file):
  - riscos_run         run a RISC OS CLI command, stream output, return exit code
  - riscos_write_file  write a file into the machine's HostFS drive (host-side)
  - riscos_read_file   read a file from the HostFS drive
  - riscos_list        list a directory on the HostFS drive
  - riscos_screenshot  grab the emulator screen as a PNG (via VNC)
  - riscos_send_text   type text at the keyboard (via VNC)
  - riscos_send_key    press a single key by X keysym (via VNC)
  - riscos_click       left-click at a pixel coordinate (via VNC)

It talks to interfaces RPCEmu already exposes:
  - HostCmd socket  (guest CLI + output + return code)   -> riscos_run
  - HostFS directory (host filesystem)                    -> file tools
  - VNC server       (framebuffer + input)                -> screen/input tools

Configuration (environment variables):
  RPCEMU_HOSTCMD_SOCKET  AF_UNIX path (or host:port for TCP) of the HostCmd socket.
  RPCEMU_HOSTFS_DIR      Host directory that backs the machine's HostFS drive.
                         (Guest sees it as HostFS::HostFS.$)
  RPCEMU_VNC_HOST        VNC host (default 127.0.0.1).
  RPCEMU_VNC_PORT        VNC port (default 5900).
  RPCEMU_VNC_PASSWORD    VNC control password (empty by default).

Run:  python rpcemu_mcp.py         (stdio transport, for Claude Code / Desktop)
"""

from __future__ import annotations

import os
import socket
import struct
import time
import zlib
from pathlib import Path
from typing import Optional

from mcp.server.fastmcp import FastMCP, Image
from Crypto.Cipher import DES

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

HOSTCMD_SOCKET = os.environ.get("RPCEMU_HOSTCMD_SOCKET", "")
HOSTFS_DIR = os.environ.get("RPCEMU_HOSTFS_DIR", "")
VNC_HOST = os.environ.get("RPCEMU_VNC_HOST", "127.0.0.1")
VNC_PORT = int(os.environ.get("RPCEMU_VNC_PORT", "5900"))
VNC_PASSWORD = os.environ.get("RPCEMU_VNC_PASSWORD", "")
DEBUG_SOCKET = os.environ.get("RPCEMU_DEBUG_SOCKET", "")
NETCAP_SOCKET = os.environ.get("RPCEMU_NETCAP_SOCKET", "")

mcp = FastMCP("rpcemu")


# --------------------------------------------------------------------------
# HostCmd client (persistent, auto-reconnecting)
#
# Wire protocol (client -> server): one command line terminated by '\n'.
# server -> client frames: [type:1][len:u32 BE][payload]
#   'O' output chunk, 'D' done (payload = 4-byte BE return code), 'X' notice.
# One command at a time; wait for 'D' before sending the next. The connection
# is kept open so the RISC OS session (current dir, system variables) persists
# across calls.
# --------------------------------------------------------------------------


class HostCmdError(RuntimeError):
    pass


class HostCmd:
    def __init__(self, spec: str):
        self.spec = spec
        self.sock: Optional[socket.socket] = None

    def _connect(self) -> None:
        if not self.spec:
            raise HostCmdError(
                "RPCEMU_HOSTCMD_SOCKET is not set — cannot reach the machine's "
                "HostCmd socket."
            )
        if ":" in self.spec and not self.spec.startswith("/"):
            host, _, port = self.spec.rpartition(":")
            s = socket.create_connection((host or "127.0.0.1", int(port)), timeout=5)
        else:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(self.spec)
        self.sock = s
        # Drain the connect banner ('X' notice), best effort.
        try:
            self._read_frame(deadline=time.monotonic() + 2)
        except (HostCmdError, socket.timeout):
            pass

    def _ensure(self) -> None:
        if self.sock is None:
            self._connect()

    def _drop(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def _recv_exact(self, n: int, deadline: float) -> bytes:
        assert self.sock is not None
        buf = b""
        while len(buf) < n:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise HostCmdError("HostCmd connection closed by the emulator")
            buf += chunk
        return buf

    def _read_frame(self, deadline: float):
        hdr = self._recv_exact(5, deadline)
        typ = chr(hdr[0])
        (length,) = struct.unpack(">I", hdr[1:5])
        payload = self._recv_exact(length, deadline) if length else b""
        return typ, payload

    def run(self, command: str, timeout_s: float) -> dict:
        command = command.rstrip("\r\n")
        # Try once; on a stale/broken connection, reconnect and retry once.
        for attempt in range(2):
            try:
                self._ensure()
                assert self.sock is not None
                self.sock.settimeout(5)
                self.sock.sendall((command + "\n").encode("latin-1", "replace"))
                deadline = time.monotonic() + timeout_s
                out = bytearray()
                notices = []
                while True:
                    typ, payload = self._read_frame(deadline)
                    if typ == "O":
                        out += payload
                    elif typ == "X":
                        note = payload.decode("latin-1").strip()
                        notices.append(note)
                        if "busy" in note.lower():
                            # A previous command is still in flight in the guest
                            # (often a hung memory-hungry/interactive command). No
                            # 'D' frame will follow this one — don't wait it out.
                            raise HostCmdError(
                                "the guest is busy with a previous command that "
                                "has not finished (it may be hung — e.g. an "
                                "interactive or memory-starved command). Reset the "
                                "machine or wait for it to complete."
                            )
                    elif typ == "D":
                        rc = struct.unpack(">i", payload)[0] if len(payload) == 4 else None
                        text = out.decode("latin-1")
                        return {
                            "return_code": rc,
                            "output": text,
                            "notices": notices,
                        }
                    else:
                        # Unknown frame; ignore.
                        pass
            except (HostCmdError, socket.timeout, OSError) as e:
                self._drop()
                if attempt == 0 and isinstance(e, (HostCmdError, OSError)):
                    continue  # reconnect and retry once
                if isinstance(e, socket.timeout):
                    raise HostCmdError(
                        f"Timed out after {timeout_s}s waiting for '{command}' to "
                        "finish. Interactive commands (bare BASIC, editors, Y/N "
                        "prompts) have no stdin and will hang — use non-interactive "
                        "invocations."
                    )
                raise
        raise HostCmdError("unreachable")


_hostcmd = HostCmd(HOSTCMD_SOCKET)


# --------------------------------------------------------------------------
# DebugCmd client (persistent, auto-reconnecting)
#
# Newline-delimited: send "<verb> [args]\n", read exactly one JSON object line.
# Runs against RPCEmu's host-side debugger, so it can inspect and control the
# emulated CPU directly.
# --------------------------------------------------------------------------


class DebugCmd:
    def __init__(self, spec: str):
        self.spec = spec
        self.sock: Optional[socket.socket] = None
        self.buf = b""

    def _connect(self) -> None:
        if not self.spec:
            raise HostCmdError(
                "RPCEMU_DEBUG_SOCKET is not set — the debugger socket is "
                "unavailable (is debug_enabled=1 for this machine?)."
            )
        if ":" in self.spec and not self.spec.startswith("/"):
            host, _, port = self.spec.rpartition(":")
            self.sock = socket.create_connection((host or "127.0.0.1", int(port)), timeout=5)
        else:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(self.spec)
            self.sock = s
        self.buf = b""

    def _drop(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None
        self.buf = b""

    def cmd(self, line: str) -> dict:
        for attempt in range(2):
            try:
                if self.sock is None:
                    self._connect()
                assert self.sock is not None
                self.sock.settimeout(5)
                self.sock.sendall((line + "\n").encode("latin-1", "replace"))
                deadline = time.monotonic() + 5
                while b"\n" not in self.buf:
                    self.sock.settimeout(max(0.05, deadline - time.monotonic()))
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        raise HostCmdError("debugger connection closed")
                    self.buf += chunk
                ln, _, self.buf = self.buf.partition(b"\n")
                import json

                return json.loads(ln.decode("latin-1"))
            except (HostCmdError, socket.timeout, OSError) as e:
                self._drop()
                if attempt == 0 and not isinstance(e, socket.timeout):
                    continue
                raise HostCmdError(f"debugger command {line!r} failed: {e}")
        raise HostCmdError("unreachable")


_debug = DebugCmd(DEBUG_SOCKET)


# --------------------------------------------------------------------------
# HostFS file helpers (host-side; the guest sees this dir as HostFS::HostFS.$)
#
# RISC OS filetypes are encoded on HostFS as a ",xxx" leaf suffix (xxx = 3 hex
# digits). Paths here use host-style '/' separators relative to the HostFS root.
# --------------------------------------------------------------------------


def _hostfs_root() -> Path:
    if not HOSTFS_DIR:
        raise HostCmdError(
            "RPCEMU_HOSTFS_DIR is not set — cannot access the HostFS drive."
        )
    return Path(HOSTFS_DIR).resolve()


def _safe_join(rel: str) -> Path:
    root = _hostfs_root()
    p = (root / rel.lstrip("/")).resolve()
    if p != root and root not in p.parents:
        raise HostCmdError(f"path escapes the HostFS root: {rel!r}")
    return p


# --------------------------------------------------------------------------
# Minimal VNC/RFB client (screenshot + input). RFB 3.3, None/VNC auth, Raw.
# --------------------------------------------------------------------------


def _vnc_auth_response(challenge: bytes, password: str) -> bytes:
    """Encrypt an RFB challenge with the protocol's bit-reversed DES key."""
    key = password.encode("utf-8")[:8].ljust(8, b"\0")
    key = bytes(int(f"{byte:08b}"[::-1], 2) for byte in key)
    return DES.new(key, DES.MODE_ECB).encrypt(challenge)


def _vnc_connect() -> tuple[socket.socket, int, int]:
    s = socket.create_connection((VNC_HOST, VNC_PORT), timeout=10)
    s.settimeout(15)

    def recvn(n: int) -> bytes:
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise HostCmdError("VNC connection closed during handshake")
            b += c
        return b

    recvn(12)  # server "RFB 003.00x\n"
    s.sendall(b"RFB 003.003\n")
    (sec,) = struct.unpack(">I", recvn(4))
    if sec == 0:
        (n,) = struct.unpack(">I", recvn(4))
        raise HostCmdError("VNC connection failed: " + recvn(n).decode("latin-1"))
    if sec == 2:
        if not VNC_PASSWORD:
            raise HostCmdError(
                "VNC server requires authentication; set RPCEMU_VNC_PASSWORD "
                "to its control password."
            )
        s.sendall(_vnc_auth_response(recvn(16), VNC_PASSWORD))
        (auth_result,) = struct.unpack(">I", recvn(4))
        if auth_result != 0:
            raise HostCmdError("VNC authentication failed")
    elif sec != 1:
        raise HostCmdError(
            f"VNC server offered unsupported security type {sec}."
        )
    s.sendall(b"\x01")  # ClientInit, shared
    w, h = struct.unpack(">HH", recvn(4))
    recvn(16)  # pixel format
    (nl,) = struct.unpack(">I", recvn(4))
    recvn(nl)  # desktop name
    return s, w, h


def _vnc_screenshot_png() -> bytes:
    s, w, h = _vnc_connect()

    def recvn(n: int) -> bytes:
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise HostCmdError("VNC connection closed mid-framebuffer")
            b += c
        return b

    # Force 32bpp true-colour, little-endian, red/green/blue shifts 0/8/16 so
    # each pixel arrives on the wire as bytes [R, G, B, x] — exactly what
    # _encode_png expects (it reads the first three bytes of every 4-byte pixel
    # as R, G, B). Requesting big-endian 16/8/0 instead makes the server emit
    # [x, R, G, B], which shifts every channel and yields visibly wrong colours.
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

    mt = recvn(1)[0]
    while mt != 0:  # skip non-FramebufferUpdate messages
        if mt == 1:
            recvn(5)
        elif mt == 3:
            recvn(7)
        mt = recvn(1)[0]
    recvn(1)
    (nrect,) = struct.unpack(">H", recvn(2))
    fb = bytearray(w * h * 4)
    for _ in range(nrect):
        x, y, rw, rh, enc = struct.unpack(">HHHHi", recvn(12))
        if enc != 0:
            raise HostCmdError(f"VNC server used unsupported encoding {enc}")
        data = recvn(rw * rh * 4)
        for row in range(rh):
            src = row * rw * 4
            dst = ((y + row) * w + x) * 4
            fb[dst : dst + rw * 4] = data[src : src + rw * 4]
    s.close()
    return _encode_png(w, h, fb)


def _encode_png(width: int, height: int, fb: bytes) -> bytes:
    """Encode the VNC framebuffer (4 bytes/pixel, first 3 = R,G,B) as an RGB PNG."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        row = y * width * 4
        for x in range(width):
            o = row + x * 4
            raw += bytes((fb[o], fb[o + 1], fb[o + 2]))

    def chunk(typ: bytes, data: bytes) -> bytes:
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        sig
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )


def _vnc_send_keys(syms: list[int]) -> None:
    s, _, _ = _vnc_connect()
    try:
        for sym in syms:
            # Deliberately unhurried: the emulator samples VNC input at the
            # keyboard-scan rate, and bursts faster than ~10 keys/s get dropped
            # (observed: "Desktop" arriving as "dkop" at 0.05s/key).
            s.sendall(struct.pack(">BBHI", 4, 1, 0, sym))  # KeyEvent down
            time.sleep(0.04)
            s.sendall(struct.pack(">BBHI", 4, 0, 0, sym))  # KeyEvent up
            time.sleep(0.07)
    finally:
        s.close()


# RISC OS is a three-button system and the buttons are not interchangeable:
# Select picks, Menu opens the menu for whatever is under the pointer, and
# Adjust does the "other" thing. Anything beyond dismissing a dialogue box needs
# Menu, so the button is a parameter rather than always the left one.
_BUTTONS = {"select": 1, "menu": 2, "adjust": 4}


def _vnc_button_mask(button: str) -> int:
    mask = _BUTTONS.get(button.strip().lower())
    if mask is None:
        raise ValueError(
            f"unknown button {button!r}: use select, menu or adjust"
        )
    return mask


def _vnc_pointer(s, x: int, y: int, mask: int) -> None:
    s.sendall(struct.pack(">BBHH", 5, mask, x, y))


def _vnc_move(x: int, y: int) -> None:
    s, _, _ = _vnc_connect()
    try:
        _vnc_pointer(s, x, y, 0)
        time.sleep(0.05)
    finally:
        s.close()


def _vnc_click(x: int, y: int, button: str = "select", count: int = 1) -> None:
    mask = _vnc_button_mask(button)
    s, _, _ = _vnc_connect()
    try:
        _vnc_pointer(s, x, y, 0)
        time.sleep(0.05)
        for n in range(count):
            if n:
                # Inside the double-click interval RISC OS allows, but not so
                # fast that the two presses arrive as one.
                time.sleep(0.12)
            _vnc_pointer(s, x, y, mask)
            time.sleep(0.08)
            _vnc_pointer(s, x, y, 0)
        time.sleep(0.05)
    finally:
        s.close()


def _vnc_drag(x1: int, y1: int, x2: int, y2: int, button: str = "select") -> None:
    mask = _vnc_button_mask(button)
    s, _, _ = _vnc_connect()
    try:
        _vnc_pointer(s, x1, y1, 0)
        time.sleep(0.05)
        _vnc_pointer(s, x1, y1, mask)
        time.sleep(0.10)
        # Stepped rather than jumped: the Wimp follows a drag by watching the
        # pointer move, and one leap can be missed entirely.
        steps = 8
        for n in range(1, steps + 1):
            _vnc_pointer(s, x1 + (x2 - x1) * n // steps,
                         y1 + (y2 - y1) * n // steps, mask)
            time.sleep(0.04)
        time.sleep(0.10)
        _vnc_pointer(s, x2, y2, 0)
        time.sleep(0.05)
    finally:
        s.close()


# --------------------------------------------------------------------------
# MCP tools
# --------------------------------------------------------------------------


@mcp.tool()
def riscos_run(command: str, timeout_s: float = 30.0) -> dict:
    """Run a RISC OS command-line command inside the emulated machine and return
    its output and return code.

    Use this to drive the guest OS: run star-commands, build tools (cc, objasm,
    link), BASIC programs, etc. Output written to the VDU stream is captured.
    The RISC OS session persists across calls — the current directory and system
    variables set by one command are visible to the next.

    IMPORTANT: this forwards whole command lines and captures output; it has no
    stdin. Commands that prompt for keyboard input (a bare `BASIC`, editors, Y/N
    confirmations) will hang until `timeout_s`. Prefer non-interactive forms
    (e.g. `BASIC -quit <file>`, batch flags). Files on the HostFS drive are
    reachable in the guest as `HostFS::HostFS.$.<name>`.

    Returns: {"return_code": int, "output": str, "notices": [str]}.
    """
    return _hostcmd.run(command, timeout_s)


@mcp.tool()
def riscos_write_file(path: str, content: str, filetype: str = "fff") -> str:
    """Write a text file onto the machine's HostFS drive (host-side write).

    `path` is relative to the HostFS root, using '/' as the separator (e.g.
    "work/hello"). `filetype` is a 3-hex-digit RISC OS filetype appended as a
    ",xxx" suffix on disk (default "fff" = Text; use "ffb" for tokenised BASIC,
    "ffa" for a module, etc.); pass an empty string for no filetype suffix.

    The guest sees the file at HostFS::HostFS.$.<path-with-dots>. Returns the
    host path written.
    """
    p = _safe_join(path)
    if filetype:
        p = p.with_name(p.name + "," + filetype.lower())
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding="latin-1")
    return str(p)


@mcp.tool()
def riscos_read_file(path: str) -> str:
    """Read a text file from the machine's HostFS drive.

    `path` is relative to the HostFS root ('/'-separated). The ",xxx" filetype
    suffix is optional — if "work/hello" isn't found, the first "work/hello,*"
    match is read. Returns the file contents.
    """
    p = _safe_join(path)
    if not p.exists():
        matches = sorted(p.parent.glob(p.name + ",*")) if p.parent.exists() else []
        if matches:
            p = matches[0]
        else:
            raise HostCmdError(f"no such file on the HostFS drive: {path!r}")
    return p.read_text(encoding="latin-1")


@mcp.tool()
def riscos_list(path: str = ".") -> list[str]:
    """List a directory on the machine's HostFS drive.

    `path` is relative to the HostFS root ('/'-separated; "." = root). Directory
    entries are suffixed with '/'. RISC OS filetype suffixes (",xxx") are shown
    as-is. Returns the sorted list of entries.
    """
    p = _safe_join(path)
    if not p.is_dir():
        raise HostCmdError(f"not a directory on the HostFS drive: {path!r}")
    out = []
    for child in sorted(p.iterdir()):
        out.append(child.name + ("/" if child.is_dir() else ""))
    return out


@mcp.tool()
def riscos_screenshot() -> Image:
    """Capture the emulated machine's screen as a PNG image (via VNC).

    Use this to see the current display — the desktop, an application window,
    text-mode output, an error box — so you can verify GUI results or read
    anything that isn't captured by riscos_run's text stream.
    """
    return Image(data=_vnc_screenshot_png(), format="png")


@mcp.tool()
def riscos_send_text(text: str, press_return: bool = True) -> str:
    """Type a string at the emulated keyboard (via VNC), by default followed by
    Return.

    Use this to interact with on-screen prompts or the desktop when a command
    can't be driven through riscos_run (e.g. typing into an application, or
    starting the desktop with `Desktop`). Printable ASCII plus Return only.

    Set press_return=False to fill a writable field without committing it — for
    a save box where the filename is typed and then the OK button is clicked.

    This types character by character at the emulated keyboard, so an
    application watching for keystrokes sees exactly that. To move a whole
    string in one go instead, use riscos_clipboard_set and paste it.
    """
    syms = [ord(c) for c in text]
    if press_return:
        syms.append(0xFF0D)  # Return
    _vnc_send_keys(syms)
    return f"typed {len(text)} chars" + (" + Return" if press_return else "")


@mcp.tool()
def riscos_send_key(keysym: int) -> str:
    """Press and release a single key by X11 keysym (via VNC).

    Useful for non-printing keys: Return=0xFF0D, Escape=0xFF1B, F12=0xFFC9,
    Tab=0xFF09, Backspace=0xFF08, cursor keys 0xFF51-0xFF54. Printable ASCII
    keysyms equal the character code.
    """
    _vnc_send_keys([keysym])
    return f"pressed keysym 0x{keysym:04x}"


@mcp.tool()
def riscos_click(x: int, y: int, button: str = "select", count: int = 1) -> str:
    """Click at pixel coordinate (x, y) on the emulated screen (via VNC).

    Coordinates are screen pixels from the top-left, matching riscos_screenshot
    output.

    button is "select" (left), "menu" (middle) or "adjust" (right). These are
    not interchangeable on RISC OS: **"menu" is how you open the menu** for
    whatever is under the pointer, which is how most of the desktop is reached,
    and there is no keyboard route to it.

    count=2 double-clicks, which is how a Filer window runs an application.
    """
    _vnc_click(int(x), int(y), button, int(count))
    return f"{button} click x{count} at ({x}, {y})"


@mcp.tool()
def riscos_mouse_move(x: int, y: int) -> str:
    """Move the pointer to (x, y) without pressing anything (via VNC).

    Worth doing before a screenshot when something reacts to the pointer being
    over it, and before opening a menu, so the menu belongs to the right thing.
    """
    _vnc_move(int(x), int(y))
    return f"pointer at ({x}, {y})"


@mcp.tool()
def riscos_drag(x1: int, y1: int, x2: int, y2: int, button: str = "select") -> str:
    """Drag from (x1, y1) to (x2, y2) with a button held (via VNC).

    How files are moved and copied on RISC OS, how windows are moved by their
    title bar, and how they are resized by the bottom-right corner. The pointer
    is stepped rather than jumped, because the Wimp follows a drag by watching
    it move and can miss a single leap.
    """
    _vnc_drag(int(x1), int(y1), int(x2), int(y2), button)
    return f"{button} drag ({x1}, {y1}) -> ({x2}, {y2})"


def _is_symbol_name(text: str) -> bool:
    """True if `text` can only be a symbol name, not a hex address.

    A bare hex number is always treated as an address, matching the emulator:
    loading a symbol table must not change what an existing call means.
    """
    try:
        int(text.strip().lstrip("&").removeprefix("0x").removeprefix("0X"), 16)
        return False
    except ValueError:
        return True


def _hexaddr(a: str) -> str:
    a = a.strip().lower()
    if a.startswith("0x"):
        a = a[2:]
    int(a, 16)  # validate
    return a


@mcp.tool()
def riscos_save_state(path: str) -> dict:
    """Snapshot the whole machine to a file: CPU, RAM, VRAM, devices, the
    graphics card, networking.

    This is an undo. Take one before anything that might wedge the guest or
    leave it in a state you cannot get out of, then riscos_load_state to put it
    back exactly as it was. Far quicker than rebooting RISC OS, and it restores
    things a reboot would not, such as open applications and unsaved work.

    The path is on the *host*, and the emulator writes it, so it must be
    somewhere the emulator can write.
    """
    return _debug.cmd(f"state save {path}")


@mcp.tool()
def riscos_load_state(path: str) -> dict:
    """Restore a machine snapshot taken by riscos_save_state.

    The snapshot has to belong to this machine - same model, RAM, VRAM, ROM and
    graphics card - and the mismatch is reported rather than half-applied. A
    snapshot that cannot be loaded leaves the machine running as it was, so a
    failed load costs nothing.

    Everything the guest was doing resumes from that instant. Give it a moment
    afterwards: the first riscos_run following a restore is often swallowed,
    exactly as the first one after a boot is, so send a throwaway command (or
    repeat) before trusting an empty result.
    """
    return _debug.cmd(f"state load {path}")


@mcp.tool()
def riscos_reset() -> dict:
    """Reset the emulated machine, as the reset button would.

    The way out when the guest is wedged and no longer answering riscos_run -
    which can happen if a command is interrupted part way through. Everything
    unsaved in the guest is lost, so prefer riscos_load_state if a snapshot
    exists.
    """
    return _debug.cmd("reset")


@mcp.tool()
def riscos_clipboard_set(text: str) -> dict:
    """Put text on the shared clipboard, ready for the guest to paste.

    A data channel, and a different thing from riscos_send_text: this moves a
    whole string in one go, however long, and nothing has to be watching the
    keyboard. Use it to get a file's worth of text into an editor - open the
    editor, then paste - where typing it would be slow and would be seen as
    keystrokes.

    Needs the shared clipboard turned on for the machine (Settings → Share
    Clipboard with RISC OS), and the guest's clipboard module running.
    """
    return _debug.cmd(f"clipboard set {text}")


@mcp.tool()
def riscos_clipboard_get() -> dict:
    """Read what is on the shared clipboard, e.g. after copying in the guest.

    Returns {"type": RISC OS filetype, "text": the text or null}. type 0xfff is
    text; 0xb60 (PNG) and 0xc85 (JPEG) are images, for which "text" is null -
    use riscos_screenshot to see a picture.

    A way to get a large amount of text *out* of the guest without reading it
    off the screen: select it in the application, copy, then call this.
    """
    return _debug.cmd("clipboard get")


@mcp.tool()
def riscos_debug_registers() -> dict:
    """Read the emulated ARM CPU registers.

    Returns {r0..r15 (hex), pc, cpsr, mode, flags ("NZCV"), paused}. This is the
    host-side view of the *emulated* CPU (not a RISC OS debugger) — it works
    whether the CPU is running or paused.
    """
    return _debug.cmd("regs")


@mcp.tool()
def riscos_debug_status() -> dict:
    """Read the debugger status: whether the CPU is paused and why, the last/halt
    PC and opcode, any watchpoint hit, and the current breakpoint and watchpoint
    lists. `reason`: 0=none 1=user 2=breakpoint 3=watchpoint 4=step 5=exception
    6=SWI.
    """
    return _debug.cmd("status")


@mcp.tool()
def riscos_debug_read_memory(address: str, length: int = 64, physical: bool = False) -> dict:
    """Read emulated memory (side-effect-free — never triggers watchpoints/aborts).

    `address` is hex (e.g. "fc03d870"). By default it is a **virtual** (MMU-
    translated) address, matching the CPU registers/PC; pass physical=True to
    read a raw physical address instead. `length` is capped at 4096. Returns
    {addr, physical, len, data} where data is a hex string (unmapped bytes read
    as 00).
    """
    return _debug.cmd(f"mem {_hexaddr(address)} {int(length)}{' phys' if physical else ''}")


@mcp.tool()
def riscos_debug_disassemble(address: str, count: int = 16, physical: bool = False) -> dict:
    """Disassemble emulated ARM instructions (side-effect-free).

    `address` is hex; virtual by default (use physical=True for a raw physical
    address). `count` is capped at 256. Returns {lines: ["<addr>: <opcode>
    <mnemonic>", ...]}. Disassemble at the `pc` from riscos_debug_registers to
    see what the CPU is about to execute.
    """
    return _debug.cmd(f"dis {_hexaddr(address)} {int(count)}{' phys' if physical else ''}")


@mcp.tool()
def riscos_debug_pause() -> dict:
    """Halt the emulated CPU. The pause is asynchronous — it takes effect at the
    next instruction, so poll riscos_debug_status until paused is true. WARNING:
    a paused CPU freezes the whole machine (guest OS, HostCmd, and the screen all
    stop) until riscos_debug_resume. Use for inspection, then resume.
    """
    return _debug.cmd("pause")


@mcp.tool()
def riscos_debug_resume() -> dict:
    """Resume the emulated CPU after a pause, breakpoint, watchpoint, or step."""
    return _debug.cmd("resume")


@mcp.tool()
def riscos_debug_step(count: int = 1) -> dict:
    """Single-step the emulated CPU `count` instructions, then pause again. The
    CPU must be paused (or about to be). Follow with riscos_debug_registers /
    riscos_debug_disassemble to observe the effect.
    """
    return _debug.cmd(f"step {int(count)}")


@mcp.tool()
def riscos_debug_breakpoint(action: str, address: str = "", condition: str = "",
                            ignore_count: int = 0, once: bool = False) -> dict:
    """Manage PC breakpoints (max 64). `action` is "add", "del", "enable",
    "disable", or "clear". `address` is hex, or the name of a loaded symbol
    (see riscos_debug_symbols).

    For "add":
      `condition`    an expression that must be true to halt, e.g. "r0 == 0",
                     "[sp + 4] != 0", "pc >= 0x8000 && z". Over registers
                     (r0-r15, pc, sp, lr, cpsr), flags (n z c v), literals
                     (0x10, &10, 16), memory ([addr], [addr]:1/:2/:4) and the
                     operators == != < > <= >= + - * / % & | ^ ~ ! << >> && ||.
                     Comparisons are UNSIGNED. A malformed condition is
                     refused here rather than silently never firing.
      `ignore_count` skip this many matches before halting.
      `once`         remove the breakpoint once it halts.

    Adding at an address that already has a breakpoint replaces its settings.
    When the CPU halts, riscos_debug_status shows reason=2; resume with
    riscos_debug_resume. If a breakpoint is not firing, riscos_debug_status
    reports its hit_count (times the address was reached) and eval_errors
    (times the condition could not be evaluated), which distinguishes "the
    condition is never true" from "this code never runs".
    """
    action = action.strip().lower()
    if action == "clear":
        return _debug.cmd("bp clear")
    if action in ("add", "del", "enable", "disable"):
        if not address:
            raise HostCmdError(f"bp {action} requires an address")
        # A symbol name is passed through as written; only a hex address is
        # normalised, so "main" reaches the emulator intact.
        target = address if _is_symbol_name(address) else _hexaddr(address)
        if action != "add":
            return _debug.cmd(f"bp {action} {target}")
        parts = [f"bp add {target}"]
        if once:
            parts.append("once")
        if ignore_count:
            parts.append(f"count {int(ignore_count)}")
        if condition:
            # "if" swallows the rest of the line, so it must come last
            parts.append(f"if {condition}")
        return _debug.cmd(" ".join(parts))
    raise HostCmdError('action must be "add", "del", "enable", "disable", or "clear"')


@mcp.tool()
def riscos_debug_watchpoint(action: str, address: str = "", size: int = 4,
                            access: str = "w", log_only: bool = False) -> dict:
    """Manage data watchpoints (max 32). `action` is "add", "del", or "clear".
    For add/del: `address` (hex), `size` in bytes, `access` one of "r"/"w"/"rw".
    If log_only=True (add), a matching access emits a trace event instead of
    halting (read with riscos_debug_trace). Otherwise a match pauses the CPU
    (riscos_debug_status shows reason=3).
    """
    action = action.strip().lower()
    if action == "clear":
        return _debug.cmd("wp clear")
    if action in ("add", "del"):
        if not address:
            raise HostCmdError(f"wp {action} requires an address")
        tail = " log" if (log_only and action == "add") else ""
        return _debug.cmd(f"wp {action} {_hexaddr(address)} {int(size)} {access}{tail}")
    raise HostCmdError('action must be "add", "del", or "clear"')


@mcp.tool()
def riscos_debug_backtrace(depth: int = 32) -> dict:
    """Walk the call stack of the emulated CPU.

    Returns {truncated, frames:[{level, pc, lr, sp, fp, symbol, offset}]},
    innermost frame first. Frame 0 comes from the live registers; the rest are
    recovered by following the APCS frame-pointer chain from R11.

    **Check `truncated`.** The frame chain is a compiler convention, and a great
    deal of RISC OS is hand-written assembler that does not keep one. A
    two-frame answer with truncated=false is the whole stack; the same answer
    with truncated=true is where the walk gave up, and the real caller is not
    shown. Treating the second as the first will send you reading the wrong
    code. `symbol` is null unless symbols have been loaded.
    """
    return _debug.cmd(f"bt {int(depth)}")


@mcp.tool()
def riscos_debug_step_over() -> dict:
    """Step one instruction, running any subroutine call to completion.

    The CPU must be paused. If the instruction at PC is a call (BL), the whole
    subroutine runs at full speed and the CPU stops at the following
    instruction; anything else is an ordinary single step. Returns immediately
    — poll riscos_debug_status to see where it stopped, as with pause.
    """
    return _debug.cmd("step over")


@mcp.tool()
def riscos_debug_step_out() -> dict:
    """Run until the current function returns.

    The CPU must be paused. The return address is taken from the frame chain
    where there is one, falling back to R14. Fails if neither yields a usable
    address, which happens in the outermost frame. Poll riscos_debug_status to
    see where it stopped.
    """
    return _debug.cmd("step out")


@mcp.tool()
def riscos_debug_run_to(address: str) -> dict:
    """Run until the emulated PC reaches an address, then pause.

    The CPU must be paused to start. `address` is hex, or the name of a loaded
    symbol. The target fires once and is then forgotten; it is separate from
    the breakpoint list, so this never disturbs a breakpoint you have set. It
    is also abandoned by any resume or step, so it cannot fire later out of
    nowhere. If the address is never reached the machine simply keeps running —
    poll riscos_debug_status.
    """
    target = address if _is_symbol_name(address) else _hexaddr(address)
    return _debug.cmd(f"runto {target}")


@mcp.tool()
def riscos_debug_symbols(action: str = "load", path: str = "", name: str = "",
                         address: str = "") -> dict:
    """Give guest addresses names, so backtraces and disassembly are readable.

    `action`:
      "load"    read a symbol file from the HOST path `path`, replacing any
                already loaded. Format is one symbol per line,
                "<hex address> <name>"; blank lines and #/; comments are
                skipped, and & or 0x address prefixes are accepted. A file that
                is not a symbol file is refused outright rather than partly
                loaded.
      "clear"   discard all symbols.
      "lookup"  name the symbol containing hex `address`.
      "find"    the address of symbol `name`.

    Once loaded, riscos_debug_disassemble labels addresses and annotates branch
    targets, riscos_debug_backtrace names frames, riscos_debug_status reports
    pc_symbol, and breakpoints can be set by name.

    A symbol reaches only as far as the next one starts, so an address outside
    the table is reported as unnamed rather than attributed to whatever symbol
    happens to sit below it. There is no way to read symbols out of the running
    guest: that would mean depending on RISC OS kernel internals that differ
    between versions, and a name that cannot be trusted is worse than a bare
    address.
    """
    action = action.strip().lower()
    if action == "clear":
        return _debug.cmd("sym clear")
    if action == "load":
        if not path:
            raise HostCmdError("sym load requires a path")
        return _debug.cmd(f"sym load {path}")
    if action == "lookup":
        if not address:
            raise HostCmdError("sym lookup requires an address")
        return _debug.cmd(f"sym lookup {_hexaddr(address)}")
    if action == "find":
        if not name:
            raise HostCmdError("sym find requires a name")
        return _debug.cmd(f"sym find {name}")
    raise HostCmdError('action must be "load", "clear", "lookup", or "find"')


@mcp.tool()
def riscos_debug_trace(max_events: int = 64) -> dict:
    """Drain the debug trace ring — exception, SWI, and log-only-watchpoint events
    captured during execution (up to 128). Returns {dropped, events:[{seq, type
    (0=exception 1=SWI 2=watchpoint), pc, opcode, arg0, arg1, arg2}]}. Tracing of
    exceptions/SWIs must be configured in the machine's debugger for events to
    appear; log-only watchpoints always feed it.
    """
    return _debug.cmd(f"trace {int(max_events)}")


# --------------------------------------------------------------------------
# NetCapCmd client - the network the machine is on
# --------------------------------------------------------------------------


class NetCapCmd(DebugCmd):
    """The capture socket.

    Speaks the same shape as the debugger's - a line in, a line of JSON out -
    so the connect, reconnect and read logic is inherited rather than written
    twice. Only the error message differs, because being told the wrong socket
    is unset is worse than being told nothing.
    """

    def _connect(self) -> None:
        if not self.spec:
            raise HostCmdError(
                "RPCEMU_NETCAP_SOCKET is not set - the network capture socket "
                "is unavailable (is netcap_enabled=1 for this machine?)."
            )
        super()._connect()

    def lines(self, line: str, terminator: str = "end", limit: int = 4096) -> list:
        """Send a command that answers with many lines, ending in `end`."""
        import json as _json

        if self.sock is None:
            self._connect()
        assert self.sock is not None
        self.sock.settimeout(5)
        self.sock.sendall((line + "\n").encode("latin-1", "replace"))
        out: list = []
        deadline = time.monotonic() + 10
        while len(out) < limit:
            while b"\n" not in self.buf:
                if time.monotonic() > deadline:
                    return out
                chunk = self.sock.recv(65536)
                if not chunk:
                    self._drop()
                    return out
                self.buf += chunk
            raw, _, self.buf = self.buf.partition(b"\n")
            text = raw.decode("latin-1").strip()
            if text == terminator:
                break
            if not text:
                continue
            try:
                out.append(_json.loads(text))
            except ValueError:
                pass
        return out


_netcap = NetCapCmd(NETCAP_SOCKET)


@mcp.tool()
def riscos_netcap_status() -> dict:
    """What the machine's network capture is doing: how many frames have been
    seen, how many sent and received, whether a pcap file is being written and
    how big it is, and whether frames are being kept in memory for reading back.
    """
    return _netcap.cmd("status")


@mcp.tool()
def riscos_netcap_start(path: str, max_bytes: int = 0) -> dict:
    """Have the emulator write a pcap file of every frame the machine sends or
    receives. The path is on the HOST, not in the guest. max_bytes stops the
    capture at that size (0 = no limit), which is worth setting: a busy machine
    left capturing will fill a disc. Open the result with Wireshark or tcpdump.
    """
    return _netcap.cmd(f"start {path} {int(max_bytes)}")


@mcp.tool()
def riscos_netcap_stop() -> dict:
    """Close the capture file. Frames carry on being counted."""
    return _netcap.cmd("stop")


@mcp.tool()
def riscos_netcap_clear() -> dict:
    """Forget the frames held in memory and zero the counters. Does not touch a
    capture file that is being written."""
    return _netcap.cmd("clear")


@mcp.tool()
def riscos_netcap_recent(count: int = 50) -> list:
    """The most recent network frames, decoded.

    Each is {serial, sec, usec, dir ("tx"/"rx"), len, cap, proto, src, dst,
    info, data (hex)}. The decode names the RISC OS protocols - Freeway and
    ShareFS are the ones a general-purpose tool shows as anonymous UDP ports.

    Frames are only held once something asks for them, so the first call turns
    that on and may come back empty; call again after the machine has done
    something. `data` is the frame itself as hex, if you want to decode further.
    """
    _netcap.cmd("ring on")
    return _netcap.lines(f"tail {int(count)}")


if __name__ == "__main__":
    mcp.run()
