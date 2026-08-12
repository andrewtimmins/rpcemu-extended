# RPCEmu MCP server

An [MCP](https://modelcontextprotocol.io) server that lets an MCP client
(Claude Code, Claude Desktop, the API's MCP connector, …) drive a RISC OS
machine running under RPCEmu — run commands, edit/build/read files, and see and
click the screen. It turns the emulator into an agent-drivable RISC OS
development target ("edit on the host, build on the guest, read the result").

It is a thin adapter over interfaces RPCEmu exposes:

| Interface | Used for |
| --- | --- |
| **HostCmd** socket (see [../../docs/hostcmd.md](../../docs/hostcmd.md)) | running guest CLI commands, capturing output + return code |
| **HostFS** directory (host filesystem) | reading/writing/listing files on the machine's HostFS drive |
| **VNC** server | screen capture and keyboard/mouse input |
| **DebugCmd** socket (see [../../docs/debugcmd.md](../../docs/debugcmd.md)) | inspecting and controlling the emulated ARM CPU (registers, memory, disassembly, breakpoints and conditions, watchpoints, stepping, backtraces, symbols) |

## Tools

| Tool | What it does |
| --- | --- |
| `riscos_run(command, timeout_s=30)` | Run a RISC OS CLI command; returns `{return_code, output, notices}`. The RISC OS session (current dir, system variables) persists across calls. |
| `riscos_write_file(path, content, filetype="fff")` | Write a text file onto the HostFS drive (`,xxx` filetype suffix). |
| `riscos_read_file(path)` | Read a file from the HostFS drive. |
| `riscos_list(path=".")` | List a HostFS directory. |
| `riscos_screenshot()` | Capture the screen as a PNG (so the model can *see* the display). |
| `riscos_send_text(text, press_return=True)` | Type a string at the keyboard, character by character. `press_return=False` fills a writable field without committing it. |
| `riscos_send_key(keysym)` | Press one key by X11 keysym (Return=0xFF0D, Escape=0xFF1B, …). |
| `riscos_click(x, y, button="select", count=1)` | Click at a screen pixel. `button` is `select`, `menu` or `adjust`; `count=2` double-clicks. |
| `riscos_mouse_move(x, y)` | Move the pointer without pressing anything. |
| `riscos_drag(x1, y1, x2, y2, button="select")` | Drag between two points, stepped so the Wimp follows it. |

RISC OS has three mouse buttons and they are not interchangeable: **Menu (the
middle button) is how nearly all of the desktop is reached**, and there is no
keyboard route to it. Dragging is how files are moved and windows are resized.

### Machine control

| Tool | What it does |
| --- | --- |
| `riscos_save_state(path)` | Snapshot the whole machine (CPU, RAM, VRAM, devices, graphics card, networking) to a host file. |
| `riscos_load_state(path)` | Restore one. A snapshot that does not match the machine is refused and the machine carries on untouched. |
| `riscos_reset()` | Reset the machine, for when the guest is wedged and no longer answering. |
| `riscos_clipboard_set(text)` | Put text on the shared clipboard for the guest to paste. |
| `riscos_clipboard_get()` | Read what is on it; returns the RISC OS filetype, and `text` is null for an image. |

**Snapshots are an undo.** Take one before anything that might wedge the guest,
and restore it instead of rebooting: it is far quicker, and it brings back open
applications and unsaved work that a reboot would lose. Give the guest a moment
afterwards - the first `riscos_run` after a restore is often swallowed, as the
first one after a boot is.

The clipboard is a *data channel*, and a different thing from typing: it moves a
whole string in one go and needs no host clipboard, so it works headless. Typing
remains the way to drive anything that is watching for keystrokes. It needs the
shared clipboard enabled for the machine and the guest module running.

### Debugger tools (control the *emulated CPU*, not a RISC OS debugger)

| Tool | What it does |
| --- | --- |
| `riscos_debug_registers()` | Read the ARM registers (r0–r15, pc, cpsr, mode, decoded flags). |
| `riscos_debug_status()` | Paused state + reason, halt/last PC, `pc_symbol`, watchpoint hit, breakpoint & watchpoint lists. |
| `riscos_debug_read_memory(address, length=64, physical=False)` | Side-effect-free memory read (virtual by default; hex `data`). |
| `riscos_debug_disassemble(address, count=16, physical=False)` | Disassemble ARM instructions (side-effect-free), FPA included, symbolised where symbols are loaded. |
| `riscos_debug_pause()` / `riscos_debug_resume()` / `riscos_debug_step(count=1)` | Halt / resume / single-step the CPU. |
| `riscos_debug_step_over()` / `riscos_debug_step_out()` | Step a call to completion / run until the current function returns. |
| `riscos_debug_run_to(address)` | Run until an address (or symbol) is reached, then pause. |
| `riscos_debug_backtrace(depth=32)` | Walk the call stack. **Check `truncated`** — see below. |
| `riscos_debug_breakpoint(action, address, condition, ignore_count, once)` | `add`/`del`/`enable`/`disable`/`clear` PC breakpoints (max 64), with conditions. |
| `riscos_debug_watchpoint(action, address, size, access, log_only)` | `add`/`del`/`clear` data watchpoints (max 32). |
| `riscos_debug_symbols(action, path, name, address)` | `load`/`clear`/`lookup`/`find` guest symbols, so addresses get names. |
| `riscos_debug_trace(max_events=64)` | Drain the exception/SWI/log-watchpoint trace ring. |

Three things about these are worth knowing before relying on them.

**Conditional breakpoints.** `riscos_debug_breakpoint("add", "8000",
condition="r0 == 0")` halts only when the expression is true, which is usually
the difference between reaching an interesting state and stepping towards it
for an hour. Expressions cover registers, flags, literals and memory
(`[sp + 4] != 0 && !z`); comparisons are unsigned. A malformed condition is
refused when the breakpoint is set rather than silently never firing. If a
breakpoint is not firing, `riscos_debug_status` reports its `hit_count` and
`eval_errors`, which separates "the condition is never true" from "this code
never runs".

**Backtraces can be incomplete, and say so.** The frame chain is a compiler
convention, and much of RISC OS is hand-written assembler that keeps no frame
pointer. `truncated:true` means the walk gave up and the real caller is *not*
in the list. Reading that as a complete stack will send you to the wrong code.

**Symbols come from a host file, not from the guest.** `riscos_debug_symbols`
loads `<hex address> <name>` lines from a path on the host. An address the
table does not cover is reported as unnamed rather than attributed to the
nearest symbol below it, so a name in a backtrace can be trusted.

Memory reads and disassembly are **side-effect-free** (they never trigger
watchpoints or inject aborts) and take **virtual** addresses by default, matching
the CPU registers — pass `physical=True` for raw physical addresses. Pausing the
CPU (directly, or via a breakpoint/watchpoint hit) freezes the whole machine
(guest OS, HostCmd, screen) until you resume.

## Requirements

- The MCP Python SDK: `pip install -r requirements.txt` (installs `mcp[cli]`).
- A running RPCEmu machine with **HostCmd enabled** (on by default) and, for the
  screen/input tools, the **VNC server enabled** (`vnc_enabled=1` in the
  machine's `.cfg`). A `--headless` machine works out of the box: headless
  implies VNC and starts the server whatever that setting says.
- Set `RPCEMU_VNC_PASSWORD` to the machine's VNC control password when it has
  one. Set `RPCEMU_VNC_PORT` to match the machine's `vnc_port` when it is not the
  5900 default — each machine needs its own port, so this matters as soon as you
  run more than one.

## Configuration (environment variables)

| Variable | Meaning |
| --- | --- |
| `RPCEMU_HOSTCMD_SOCKET` | The machine's HostCmd socket: an AF_UNIX path (e.g. `<data-dir>/hostcmd.sock`) or `host:port` for TCP. |
| `RPCEMU_HOSTFS_DIR` | Host directory backing the machine's HostFS drive (the guest sees it as `HostFS::HostFS.$`). Usually `<data-dir>/machines/<name>/hostfs`. |
| `RPCEMU_VNC_HOST` | VNC host (default `127.0.0.1`). |
| `RPCEMU_VNC_PORT` | VNC port (default `5900`; matches the machine's `vnc_port`). |
| `RPCEMU_VNC_PASSWORD` | VNC control password (empty by default). Use the control password, not the read-only one, because the MCP tools send keyboard and mouse input. |
| `RPCEMU_DEBUG_SOCKET` | The machine's DebugCmd socket (AF_UNIX path e.g. `<data-dir>/rpcemu-debug.sock`, or `host:port` for TCP). Needed only for the `riscos_debug_*` tools; on by default (`debug_enabled=1`). |

## Wiring into Claude Code

Copy `mcp.json.example` to `.mcp.json` in your project (or merge it into an
existing one) and fill in the paths for your machine. Claude Code discovers
project-scoped servers from `.mcp.json`. Alternatively:

```bash
claude mcp add rpcemu -- python3 /path/to/tools/mcp/rpcemu_mcp.py
# then set the RPCEMU_* env vars in your shell or the .mcp.json "env" block
```

The script path depends on how RPCEmu was installed: `tools/mcp/rpcemu_mcp.py` in
a source or `.tar.gz` release tree, or `/usr/share/rpcemu/mcp/rpcemu_mcp.py` from
the `.deb`.

Run standalone (stdio transport) for testing:

```bash
RPCEMU_HOSTCMD_SOCKET=/path/hostcmd.sock \
RPCEMU_HOSTFS_DIR=/path/machines/Default/hostfs \
RPCEMU_VNC_PORT=5900 \
RPCEMU_VNC_PASSWORD=control-password \
python3 rpcemu_mcp.py
```

## Limitations & tips

- **No stdin.** `riscos_run` forwards whole command lines and captures output;
  it has no keyboard input. A command that prompts (bare `BASIC`, editors, Y/N
  confirmations) will hang until `timeout_s`. Use non-interactive forms
  (`BASIC -quit <file>`, batch flags).
- **Memory-hungry commands need the desktop.** `cc`, `BASIC`, and other programs
  that need an application slot only get one once the **desktop is running**
  (HostCmd then runs them in a TaskWindow child). Before the desktop is up they
  run in a restricted context and can hang — which will wedge the HostCmd
  channel until the machine is reset. Start the desktop first (e.g.
  `riscos_send_text("Desktop")`), then use `riscos_run` for builds. Return-code
  capture on the desktop path relies on `Wimp$ScrapDir` being set (it is under a
  normal `!Boot`).
- **VNC input is paced.** Keystrokes are sent slowly on purpose; the emulator
  drops keys sent faster than ~10/s.
