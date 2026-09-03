# DebugCmd — control the emulated CPU from the host

DebugCmd exposes RPCEmu's host-side debugger/inspector over a local socket, so
an external tool (the [MCP server](../tools/mcp/README.md), an IDE, a script)
can inspect and control the **emulated ARM CPU** — read registers and memory,
disassemble, set breakpoints and watchpoints, and single-step. It is the
programmatic counterpart to the GUI Machine Inspector.

```
host tool (rpcemu-debug / the MCP server / your script)
        │   local socket (AF_UNIX or TCP 127.0.0.1)
        ▼
   emulator (debugcmd.c)  ──▶  debugger core + arm register file + memory
                               + disassembler + symbols
```

`rpcemu-debug` is the command-line client, built alongside the emulator. It is
the counterpart of `rpcemu-run`: that one drives RISC OS's command line, this
one drives the processor underneath it.

```sh
rpcemu-debug pause
rpcemu-debug bt
rpcemu-debug 'bp add 8000 if r0 == 0'
rpcemu-debug            # no command: an interactive prompt
```

It takes `--socket PATH` or `--tcp host:port`, defaulting to
`$RPCEMU_DEBUG_SOCKET` and then to `$RPCEMU_DATADIR/rpcemu-debug.sock`. It
prints the emulator's JSON unchanged, and exits 0 when the emulator reported
success, 1 when it reported an error, and 2 when it could not be reached.

Both the socket service and the debugger core run on the **emulator thread**, so
DebugCmd calls the `debugger_*` API, `mem_phys_read8_debug()`, `translateaddress2()`
and `arm_disasm()` directly with no locking. The socket is serviced while the CPU
is running *and* while it is paused, so a paused CPU can always be resumed.

## Configuration

Per-machine `.cfg` keys (under `[General]`):

| Key | Default | Meaning |
| --- | --- | --- |
| `debug_enabled` | `1` | Enable the DebugCmd socket. |
| `debug_socket` | *(empty)* | Empty ⇒ `<data-dir>/rpcemu-debug.sock` (AF_UNIX). A path ⇒ that AF_UNIX path. A bare port number ⇒ TCP on `127.0.0.1:<port>`. |

**On Windows** there is no useful AF_UNIX, so the transport is always TCP on the
loopback. An empty `debug_socket` (or a path, which cannot be honoured) means
`127.0.0.1:15591`; a bare port number selects that port instead. `rpcemu-debug.exe`
defaults to the same address, so it needs no arguments. Everything else — the
commands, the responses, the debugger itself — is identical on all platforms.

> **Security.** Whoever can open this socket can halt the emulated CPU and read
> its memory. The default transport is an AF_UNIX socket under the machine's data
> directory (filesystem-permission limited, never on the network); the optional
> TCP mode binds `127.0.0.1` only. A paused CPU freezes the whole machine until
> resumed. Set `debug_enabled=0` to disable.

## Wire protocol

Newline-delimited, request/response. The client sends one request line
`<verb> [args]\n`; the server replies with exactly one JSON object line. All
addresses are hex; numeric args accept decimal or `0x`-prefixed hex. Every
response has an `"ok"` boolean; failures carry `"error"`.

| Request | Response (JSON) |
| --- | --- |
| `ping` | `{ok, paused, model, dynarec}` |
| `regs` | `{ok, paused, pc, cpsr, mode, flags, regs:[16 hex]}` |
| `reg set <0-15\|pc\|cpsr> <hex>` | `{ok, reg, value}` — refused unless paused |
`regs` also returns `spsr`: the saved PSR of the current mode as a hex string,
or `null` in User and System mode, which bank none. Reporting the array's
contents there would be whatever the last exception left behind. Knowing the
mode an exception return is about to restore is not answerable from the CPSR.

| `status` | `{ok, paused, pause_requested, reason, halt_pc, halt_opcode, last_pc, hit_address, hit_value, hit_size, hit_is_write, step_active, trace_pending, pc_symbol, pc_offset, symbols_loaded, breakpoints:[…], watchpoints:[…]}` — see below |
| `mem <addr> <len> [phys]` | `{ok, addr, physical, len, data:"<hex>"}` — `len` capped 4096; virtual unless `phys` |
| `mem write <addr> <hexbytes> [phys]` | `{ok, addr, written, requested}` — reports how many landed rather than failing the whole request |
| `dis <addr> [count] [phys]` | `{ok, lines:["<addr>: <opcode>  <mnemonic>"]}` — `count` capped 256 |
| `bp add <addr> [once] [count <n>] [if <expr>]` | `{ok, address}` — see *Breakpoints* below |
| `bp del\|enable\|disable <addr>` / `bp clear` | `{ok, address}` / `{ok}` |
| `wp add\|del <addr> <size> <r\|w\|rw> [log]` / `wp clear` | `{ok}` |
| `pause` | `{ok, paused}` — pause is **deferred** (takes effect at the next instruction); poll `status` |
| `resume` (alias `continue`) | `{ok, paused:false}` |
| `step [n]` / `step into [n]` | `{ok, stepped, mode}` — steps `n` instructions then re-pauses |
| `step over` | `{ok, mode}` — runs a call to completion; poll `status` |
| `step out` | `{ok, mode}` — runs until the current function returns; poll `status` |
| `runto <addr>` | `{ok}` — runs until the address is reached; poll `status` |
| `bt [depth]` (alias `backtrace`) | `{ok, truncated, frames:[{level,pc,lr,sp,fp,symbol,offset}]}` — see *Backtraces* below |
| `sym load <path>` / `sym clear` | `{ok, count}` — host path, not a guest one |
| `swi load <path>` / `swi clear` | `{ok, count}` — SWI names for the disassembler, host path. See [disassembly.md](disassembly.md) |
| `sym lookup <addr>` | `{ok, address, symbol, offset}` — `symbol` is `null` if nothing covers the address |
| `sym find <name>` | `{ok, symbol, address}` |
| `trace [max]` | `{ok, dropped, events:[{seq,type,pc,opcode,arg0,arg1,arg2}]}` — `max` capped 128 |
| `trace config [key=value …]` | `{ok, …}` — keys `data_abort`, `prefetch_abort`, `undefined`, `log_exceptions`, `swi_log`, `swi_halt`, `swi_min`, `swi_max`, `step_skip_irq`, `step_skip_os`. No arguments reports the current setting |
| `state save <path>` | `{ok, saved}` — snapshot the whole machine to a host file |
| `state load <path>` | `{ok, loaded}` — restore one; a snapshot that does not match the machine is refused with `{ok:false, error}` and the machine carries on untouched |
| `reset` | `{ok, reset}` — reset the machine, as the reset button would |
| `clipboard get` | `{ok, type, text}` — RISC OS filetype, and `text` is `null` for an image or an empty clipboard |
| `clipboard set <text>` | `{ok, set}` — put text on the shared clipboard for the guest to paste |

Wherever an address is taken, the name of a loaded symbol is accepted instead.
A bare hex number always wins, so loading a symbol table cannot change what an
existing script means.

`status.breakpoints` holds objects, not bare addresses:
`{address, enabled, one_shot, condition, ignore_count, hit_count, eval_errors}`.
`status.watchpoints` holds `{address, size, on_read, on_write, log_only}`.

## Breakpoints

`bp add` takes three optional extras, and `if` must come last because it
swallows the rest of the line — which is how a condition containing spaces gets
through a protocol with no quoting.

| | |
| --- | --- |
| `once` | remove the breakpoint once it halts |
| `count <n>` | skip `n` matches before halting |
| `if <expr>` | halt only when `<expr>` is true |

```
bp add 8000
bp add 8000 if r0 == 0
bp add main once count 10 if [sp + 4] != 0 && !z
```

Conditions are evaluated between instructions, reading memory by the same
side-effect-free path as `mem` — they cannot fault, touch I/O or trip a
watchpoint. Operands are registers (`r0`–`r15`, `pc`, `sp`, `lr`, `cpsr`), the
flags (`n z c v`), literals (`0x10`, `&10`, `16`), memory (`[addr]`, or
`[addr]:1`/`:2`/`:4` for a byte or halfword) and parentheses. Operators, lowest
precedence first:

```
||   &&   |   ^   &   == !=   < > <= >=   << >>   + -   * / %   unary ! ~ -
```

Comparisons are **unsigned**: the usual comparand is an address, and signed
address comparison is wrong more often than it is right.

A malformed condition is refused when the breakpoint is set, not when it is
reached. `bp add 8000 if r0 ==` is an error rather than a breakpoint that
silently never fires.

**When a breakpoint does not fire**, `status` distinguishes the two reasons that
look identical from outside: `hit_count` counts every time the address was
reached while armed, and `eval_errors` counts conditions that could not be
evaluated (in practice, a dereference of an address that was not mapped at the
time — those do not halt). A `hit_count` of zero means the code never ran; a
large one means the condition is never true.

## Backtraces

`bt` walks the APCS frame-pointer chain from R11. Frame 0 comes from the live
registers; the rest are read from the frame records in memory. Addresses are
masked so the 26-bit cores (ARM610/710/7500) work, where a saved link register
carries the processor mode and flags rather than being an address on its own.

**Check `truncated`.** The chain is a compiler convention and a great deal of
RISC OS is hand-written assembler that keeps no frame pointer. A two-frame
answer with `truncated:false` is the whole stack; the same answer with
`truncated:true` is where the walk gave up, and the real caller is not shown.
Every link is checked for alignment, readability and for moving up the stack
rather than down, so a corrupt chain stops rather than looping or inventing
frames.

## Symbols

`sym load` reads a text file from the **host**, one symbol per line:

```
00008000 main
&8100    parse_options
0x8200   emit          ; blank lines and #/; comments are skipped
```

The address is hex with an optional `0x` or `&` prefix; the rest of the line is
the name, trimmed, so names may contain spaces. A file that is not a symbol file
is refused outright rather than partly loaded — a table that is silently three
entries instead of three thousand mis-attributes everything.

A symbol reaches only as far as the next one begins (and the last one for 64K).
An address outside the table is reported as unnamed rather than attributed to
whichever symbol happens to sit below it, because `main+0x3f21c` for an address
in the kernel reads as an answer and is not one.

There is no way to read symbols out of the running guest. That would mean
depending on RISC OS kernel workspace layout, which differs between versions,
and a name that cannot be trusted is worth less than a bare address.

`swi load` is the same idea for SWI numbers, and is deliberately the same shape.
It differs in one respect: a line that is not a number-and-name pair is skipped
rather than failing the file, because a SWI list is usually a handful of lines
maintained by hand next to the module, not a generated table of thousands where
a silent partial load would mis-attribute everything. The format is in
[disassembly.md](disassembly.md).

`state` and `reset` are here rather than anywhere else because a snapshot has to
be taken between instructions on the emulator thread, which is the thread this
socket is serviced on. They exist so that something driving the machine from
outside can undo its own mistakes: take a snapshot, try something, and put the
machine back if it goes wrong, without a reboot and without a person. A failed
load costs nothing, since the header is checked before anything is restored.

The first command sent to the guest after a restore is often swallowed, the same
way the first one after a boot is; send a throwaway command before trusting an
empty result.

`status.reason` / pause reasons: `0`=none `1`=user `2`=breakpoint `3`=watchpoint
`4`=step `5`=exception `6`=SWI `7`=reserved CPU mode `8`=breakpoint SWI
(`SWI &FFFFFF` in the guest's own code; see
[debugger-tracing.md](debugger-tracing.md)). Trace `type`: `0`=exception
`1`=SWI `2`=watchpoint; for an exception, `arg0` is `0`=undefined instruction
`1`=prefetch abort `2`=data abort `3`=reserved CPU mode.

Reason `7` is not a trap that can be switched on: writing one of the nine
reserved mode values is always a fault, and the machine halts on it whatever the
trace configuration says. `arg1` of its trace event carries the mode that was
written. See *A reserved CPU mode* in
[debugger-tracing.md](debugger-tracing.md).

`step over`, `step out` and `runto` all report reason `4`, since all three are
"stopped where you asked", not "stopped at a breakpoint".

## In CI

`tests/boot_smoke.py` uses this socket to watch a real boot for data and
prefetch aborts, which no other check can see — RISC OS handles the abort and
the machine boots to a perfect desktop anyway. Aborts while RISC OS is probing
for hardware are expected and reported; aborts once the machine is up fail the
build. See *Aborts during the boot test* in [testing.md](testing.md).

## Safety notes

- **Memory reads and disassembly are side-effect-free.** Virtual addresses are
  translated with the CPU's data-abort event saved/restored (so a read of an
  unmapped page can't inject a spurious abort into execution), then read via the
  no-side-effect `mem_phys_read8_debug()` — they never fire watchpoints.
- **Breakpoints/watchpoints and pause/step change execution.** A breakpoint or
  non-log watchpoint hit pauses the CPU (freezing the machine) until `resume`.
- **`step over`, `step out` and `runto` run the machine.** They return at once,
  having started it; the machine is *not* stopped when they answer. If the
  target is never reached it simply keeps running, so poll `status`. Their
  target is separate from the breakpoint list, so it never disturbs a
  breakpoint you have set, and it is abandoned by any `resume` or `step` so it
  cannot fire later out of nowhere.
- **A conditional breakpoint is still side-effect free.** Conditions read
  memory by the same path as `mem`.
- **Symbols are host-side.** `sym load` reads a file on the host and changes
  nothing in the guest.
- Limits: 64 breakpoints, 32 watchpoints (mirrors the GUI inspector), 64
  backtrace frames, and 63 characters of breakpoint condition.
