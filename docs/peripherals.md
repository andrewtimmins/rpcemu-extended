# Serial and parallel ports

RPCEmu (Spork Edition) emulates the Risc PC's serial and parallel hardware and lets
you redirect each port to the host: a log file, a virtual printer, a real serial port
the host has, a printer the host can already print to, or a TCP "modem" that dials real
telnet BBSes.

Configure both from the menus:

- **Settings → Serial…**
- **Settings → Parallel…**

Settings are applied immediately and saved into the machine's `.cfg` file.

---

## Serial port

The Risc PC has a **single** hardware serial port: a 16550A UART in the SMC Super I/O
chip, memory-mapped at `0x3F8` (register spacing of 4 bytes). Its interrupt is wired
to **IOMD IRQ register B, bit 2** and gated by the UART's MCR **OUT2** line — RISC OS's
`Serial` module (`Serial710` driver) claims that IRQ and drives transmission from the
THRE (transmit-holding-register-empty) interrupt. The emulator models all of this, so
the standard RISC OS serial stack and ordinary terminal software work unchanged.


### Modes

| Mode | What it does |
| --- | --- |
| **Disabled** | No device attached. Guest sees a UART with nothing connected. |
| **Log to file** | Every byte the guest transmits is appended to a file on the host. |
| **TCP modem** | A Hayes-AT modem front end over a real TCP socket, with a telnet client layer. Dial telnet BBSes and other TCP services. |
| **Physical device** | A real serial port on the host: a USB adapter, a built-in port, or a pseudo terminal. |

### Log to file

Pick **Log to file** and a path (e.g. `~/Documents/serial.log`). The raw byte stream
the guest sends to the UART is appended to that file (binary, unbuffered-flush). If you
leave the path empty, a default is used under the machine's data directory
(`machines/<name>/serial_serial.log`).

This is the simplest way to capture whatever a program sends to the serial port.

**Sending serial output from RISC OS**

- Most serial-aware applications (terminals, comms software) open the port directly.
- From BASIC you can transmit bytes with `OS_SerialOp`:
  ```basic
  SYS "OS_SerialOp", 3, ASC"H"   : REM send one byte ('H')
  ```
- The legacy serial *output stream* still works too:
  ```basic
  *FX 3,1        : REM route OS_WriteC output to the serial port
  PRINT "Hello"
  *FX 3,0        : REM restore normal output
  ```

### TCP modem (telnet)

Choose **TCP modem** and the port behaves like a Hayes-compatible modem. Point your
RISC OS terminal software (ANSITerm, !Connector/Hearsay, etc.) at the serial port and
"dial" with an AT command — the emulator turns the dial string into a real TCP
connection.

**Dialling**

```
ATDT hostname:port
ATDT bbs.example.com:23
ATDT 192.168.0.50:6400
ATD  hostname            (defaults to port 23, telnet)
```

- The text after `ATD`/`ATDT`/`ATDP` is parsed as `host[:port]` (a space before the
  port also works). The default port is **23** (telnet).
- On success the modem replies `CONNECT` and enters data mode; on DNS or connection
  failure it replies `NO CARRIER`.

**Telnet and binary-clean transfers**

The TCP modem is a telnet client: it answers telnet option negotiation, escapes/
unescapes `IAC` (`0xFF`), and negotiates **binary** + **suppress-go-ahead** so the
link is fully 8-bit transparent. It performs no CR/LF translation. This means
**X/Y/ZMODEM** (and other binary) file transfers run cleanly over a connected BBS.

**Returning to command mode / hanging up**

- `+++` — the Hayes escape sequence. It is **guard-timed** (about one second of
  silence before and after), so a literal `+++` occurring inside a file transfer is
  treated as data, not an escape. After a successful escape the modem replies `OK`
  and is in command mode while still connected.
- `ATO` — return to data (online) mode.
- `ATH` — hang up (close the connection).
- Dropping **DTR** also hangs up (Hayes `&D2` behaviour), so a terminal that lowers
  DTR on exit disconnects cleanly. `DCD` reflects the live connection.

**Supported AT commands**

| Command | Effect |
| --- | --- |
| `AT` | Returns `OK` (used to probe the modem). |
| `ATDT`/`ATD`/`ATDP <host[:port]>` | Dial a TCP/telnet connection. |
| `ATH` / `ATH0` | Hang up. |
| `ATO` | Return online after `+++`. |
| `ATE0` / `ATE1` | Command-mode echo off / on (default on). |
| `ATV0` / `ATV1` | Numeric / verbose result codes (default verbose). |
| `ATQ0` / `ATQ1` | Result codes on / suppressed. |
| `ATI` | Identify (`RPCEmu TCP modem`). |
| `ATZ`, `AT&F`, other init strings | Accepted and answered with `OK`. |

Result codes: `OK`, `CONNECT`, `NO CARRIER`, `ERROR` (verbose) or `0`,`1`,`3`,`4`
(numeric).

**Limitations**

- **DNS resolution is synchronous.** Dialling a host that is slow to resolve can
  briefly pause the emulator until the lookup completes; dialling a literal IP address
  avoids this.
- The modem operates in **telnet mode**. Pure raw-TCP services that send literal
  `0xFF` data bytes are not the target use case (telnet would double them); BBSes and
  telnet services are.
- Physical `/dev/tty*` passthrough is not implemented.

---

### Physical device (a real serial port)

Choose **Physical device** and pick a port. The dropdown lists the ports the machine
actually has, and stays editable so you can type anything else, a pseudo terminal
included.

| Host | Typical names |
| --- | --- |
| Linux | `/dev/ttyUSB0` (USB adapter), `/dev/ttyACM0`, `/dev/ttyS0` (built-in) |
| macOS | `/dev/cu.usbserial-…`, `/dev/cu.usbmodem…` |
| Windows | `COM1` to `COM8`, or `\\.\COM12` for higher numbers |

**What is and is not translated.** The host's own UART does the signalling, so nothing
here reproduces bit timing. What is passed on is the *configuration*: the divisor and
line control register the guest programmed become the host port's speed and framing, so
`*Configure Baud` and a program setting the port directly both behave as they would on
real hardware. DTR and RTS follow the guest's modem control register, and the host's
CTS, DSR, RI and DCD are reported back to it.

Speed comes out as `115200 / divisor`, the Risc PC's UART being clocked at 1.8432MHz and
dividing by 16. If the guest picks a rate the host cannot do, the nearest is used and
the substitution is logged rather than silently applied.

**Flow control.** The kernel is left to do RTS/CTS, and the backend reads no more from
the host than the guest's 16-byte receive FIFO can take, leaving the rest in the host's
buffer where flow control can act on it. That matters because the emulator's speed
depends on the host's load: a sender that ignores flow control can still overrun a guest
that has fallen behind, and bytes lost that way are counted and logged rather than
quietly dropped.

**Devices with no modem lines.** A pseudo terminal cannot report CTS, DSR or DCD; it
answers the enquiry with `ENOTTY`, and some USB adapters do the same for lines they do
not wire. Such a device is treated as asserting CTS, DSR and DCD. This is deliberate:
RISC OS waits for CTS before transmitting, so reporting the lines as low leaves the
guest waiting indefinitely, and no handshake at all is closer to the truth than a
permanently blocked one.

**Permissions.** On Linux a serial port usually requires membership of the `dialout`
group:

```bash
sudo usermod -aG dialout "$USER"    # then log out and back in
```

The dialogue says so rather than reporting a bare failure.

**Trying it without any hardware.** A pseudo terminal makes a serviceable null modem:

```bash
python3 -c "import pty,os,time;m,s=pty.openpty();print(os.ttyname(s));time.sleep(600)"
```

Give the `/dev/pts/N` it prints to the dialogue, then from RISC OS:

```
*Echo Hello|M { > Devices:$.Serial }
```

Anything the guest sends appears on the master end, and anything written there arrives
in the guest. The `serial_host:` lines in `rpclog.txt` report the speed and framing each
time RISC OS reconfigures the port.

**Unplugging.** If the device disappears, or the far end of a pseudo terminal closes, the
port is dropped and the guest sees its status lines go away, as it would if a real cable
were pulled out. It is not reopened automatically; choose the port again.

---

## Parallel port

The Risc PC's Centronics parallel port is emulated through the Super I/O chip. As with
serial, you can redirect it to the host.

### Modes

| Mode | What it does |
| --- | --- |
| **Disabled** | No device attached. |
| **Log to file** | Raw bytes written to the parallel port are appended to a file. |
| **Virtual printer** | Captures each print job to a `.prn` file, with optional PDF conversion. |
| **Print on this computer** | Sends each finished job to a printer the host already has, or to a printer device node. |

### Log to file

Captures the raw parallel byte stream to the chosen file. Useful for inspecting exactly
what a printer driver emits.

### Virtual printer

The virtual printer presents a ready, online Centronics device to RISC OS and collects
each print job into a separate `.prn` file in the chosen output folder (default:
`machines/<name>/printjobs/`).

To print from RISC OS:

1. In **!Printers**, choose a printer driver whose output goes to the **parallel**
   (Centronics) port — typically a PostScript driver.
2. Print as normal. Each job is written to a new `.prn` file.

If the emulator was built with Ghostscript support (`libgs-dev`), tick **Also create
PDF files** to convert PostScript `.prn` jobs to PDF in-process — no external tools
needed. See [COMPILE.md](../COMPILE.md) for the GhostPDL build options.

### Print on this computer

Sends each finished job to a printer the host already knows about, which is usually what
is wanted: a modern printer has no parallel port, but the host can already talk to it.

Give the dialogue either

- **the name of a print queue**, in which case the job is spooled to it as raw data
  through the host print system (`lp` on Linux and macOS), or
- **a device path** such as `/dev/usb/lp0` or `/dev/lp0`, which is written to directly.

The dropdown lists the queues the host has and any printer device node that is really
there, and says **No printers found** rather than offering names that do not exist. The
field is editable either way.

The bytes are passed through untouched. What the guest produces is whatever its RISC OS
printer driver emits, so choose a driver in **!Printers** that matches the printer at
the other end: a PCL driver for a PCL printer, PostScript for PostScript. Nothing here
interprets or converts the stream, which is what the virtual printer's PDF option is
for.

On Windows, a queue name is refused with a message pointing at a device path or the
virtual printer instead; spooling by queue name there needs the Windows print API, which
this does not use.

**What this is not.** It carries the print stream, not the parallel port's pins. Devices
needing real bidirectional IEEE-1284 signalling — dongles, Zip drives, scanners — are
not supported and are not planned. Raw pin access needs hardware almost no machine has
any more, there is no portable way to reach it (`ppdev` needs a real LPT port, Windows
needs a kernel driver, macOS has nothing), and the handshake turnarounds are shorter
than an emulator that is not locked to the wall clock can meet. Printing is different
only because it needs no timing fidelity: the guest writes a byte, strobes, and waits
for acknowledgement, which can be given at once.

---

## Configuration keys

These live under the `[General]` group of each machine's `.cfg` file and are written
by the Serial/Parallel dialogs; you don't normally edit them by hand.

| Key | Meaning |
| --- | --- |
| `serial_com1_mode` | `0` disabled, `1` log to file, `2` TCP modem, `3` a real host port |
| `serial_com1_log` | Log file path (log mode) |
| `serial_com1_device` | Host serial device: a path such as `/dev/ttyUSB0` or `/dev/pts/3`, or a name such as `COM3` |
| `parallel_mode` | `0` disabled, `1` log to file, `2` virtual printer, `3` print on this computer |
| `parallel_log` | Log file path (log mode) |
| `parallel_device` | Print queue name, or a device path such as `/dev/usb/lp0` |
| `printer_output_path` | Folder for captured `.prn` jobs (virtual printer) |
| `printer_auto_pdf` | `1` to also write a PDF per job (needs Ghostscript build) |

> `serial_com2_*` keys may exist in older configuration files. They are ignored — the
> Risc PC has only one serial port.

---

## Implementation notes

| File | Role |
| --- | --- |
| `src/superio.c` | 16550 UART(s) and Centronics registers; serial IRQ on IOMD IRQB bit 2, gated by MCR OUT2 |
| `src/serial.c` | Virtual serial "bus" connecting the UART to a backend device |
| `src/parallel.c` | Virtual parallel bus |
| `src/printer.c` | Virtual printer (job capture, optional PDF via `print_convert.c`) |
| `src/peripheral_config.c` | Backends: file logging, the telnet TCP modem, and printer wiring |
| `src/serial_host.c` | A real host serial port as a device on that bus (termios, or Win32 `SetCommState`) |
| `src/gui/serial_dialog.cpp`, `src/gui/parallel_dialog.cpp` | Configuration dialogs |

The TCP modem's socket I/O is non-blocking and serviced from `serial_modem_poll()` on
the emulation thread, so it never stalls emulation (aside from the synchronous DNS
lookup noted above). Incoming data is paced into the UART's 16-byte receive FIFO via
`superio_serial_rx_space()` so fast downloads don't overrun it.

A physical port works the same way, from `serial_host_poll()` alongside it, and paces
itself against the same FIFO. Two details there are easy to get wrong if this is ever
rewritten. The receive descriptor is opened `O_NONBLOCK` with `VMIN` set to 1, not 0:
with `VMIN` at 0 a tty returns zero bytes both when it is idle and when the other end
has gone, so an idle port would look like an unplugged one. And DLAB is masked out of
the line control register before it is compared with the last value applied, because it
selects whether the divisor latches are visible rather than being a line setting, and
the guest toggles it every time it sets the speed.

The emulated UART stores the divisor but does not pace bytes by it, which is why the
guest's baud rate is passed to the host port rather than simulated.
