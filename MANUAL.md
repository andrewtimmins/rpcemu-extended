# RPCEmu Extended Edition — Manual

This is the manual: what everything does, in the order you are likely to need
it. It assumes nothing about RISC OS, and explains the operating system where
that is what you actually need to know.

If you only want to see RISC OS running, [QUICKSTART.md](QUICKSTART.md) does
that in about five minutes.

`README.md` is the project's own description: architecture, how to build it,
what differs from upstream RPCEmu. This is the one for using it.

## Contents

**Getting going**
[What this is](#what-this-is) ·
[Installing](#installing) ·
[Your first machine](#your-first-machine) ·
[Choosing a machine to emulate](#choosing-a-machine-to-emulate)

**Using RISC OS**
[The desktop](#the-desktop) ·
[The filer](#the-filer-and-how-files-work) ·
[What is on the disc](#what-is-on-the-hard-disc) ·
[The command line](#the-command-line) ·
[Shutting down](#shutting-down-properly)

**Getting things in and out**
[HostFS and Shared](#hostfs-and-shared-two-discs-that-are-really-folders) ·
[The clipboard](#the-clipboard) ·
[Installing software](#installing-software)

**The emulator**
[The menus](#the-menus-in-full) ·
[The display](#the-display) ·
[Keyboard and mouse](#keyboard-and-mouse) ·
[Discs](#floppy-and-cd-rom) ·
[Networking](#networking) ·
[Printing](#printing) ·
[Serial](#the-serial-port) ·
[USB](#usb-devices) ·
[Snapshots](#snapshots-save-suspend-and-resume) ·
[Several machines](#several-machines-at-once) ·
[Speed](#speed-and-cpu-usage)

**Reference**
[Machine settings](#machine-settings-in-full) ·
[Where your files live](#where-your-files-live) ·
[The command line options](#running-it-from-a-command-line) ·
[For developers](#for-developers) ·
[Troubleshooting](#troubleshooting) ·
[Credits](#credits-and-licence)

---

## What this is

RISC OS is the operating system Acorn wrote for its ARM computers, first
released in 1987 and still developed today. It is not Unix and not Windows. It
boots in a couple of seconds, the whole system is a few tens of megabytes, and
the desktop works in ways that were unusual then and still are: menus appear
where you click rather than at the top of a window, applications live on a bar
along the bottom of the screen, and you save a file by dragging it where you
want it.

RPCEmu emulates the hardware it ran on — the Acorn Risc PC and the A7000 — so
that operating system runs on your computer at a speed no real Risc PC ever
managed. RPCEmu is Sarah Walker's work, maintained since by Peter Howkins and
Matthew Howkins.

**Extended Edition** is a fork that adds a good deal: an emulated graphics card
with modern resolutions, USB passthrough from your computer, a package manager,
a proper debugger, remote access over VNC, shared networking between machines,
and a long list of fixes. Nothing from the original has been taken away.

You need no Acorn hardware, no original discs and nothing you have to buy.
RISC OS is fetched for you from RISC OS Open, who publish it free of charge.

## Installing

Take the build for your computer from the
[releases page](https://github.com/andrewtimmins/rpcemu-extended/releases/latest).
Each release lists SHA256 checksums if you want to verify the download.

**Windows.** Unzip anywhere and run `rpcemu-recompiler.exe`. Nothing is
installed and nothing is written outside the folder you pick on first run.

**macOS.** Open the `.dmg` and drag the application over. The build is not
notarised by Apple, so the **first** launch needs a right-click and **Open**
rather than a double-click — after that it opens normally. One download covers
both Intel and Apple Silicon.

**Linux.** Install the `.deb` with your package manager, or unpack the
`.tar.gz` and run `./rpcemu-recompiler` from inside it. Builds are published for
Intel/AMD (`amd64`) and ARM (`arm64`, which covers the Raspberry Pi).

Two binaries are in each build. `rpcemu-recompiler` translates ARM code to your
processor's own and is the one to use. `rpcemu-interpreter` executes instructions
one at a time: far slower, occasionally useful when something behaves oddly and
you want to rule the recompiler out.

## Your first machine

On first run it asks where to keep its data. Accept the suggestion unless you
have a reason not to — [Where your files live](#where-your-files-live) covers
what goes there and how to move it later.

You then get the machine list, empty to begin with. Nothing is shipped: no ROM,
no machine. A machine that cannot start would only teach you that starting
machines does not work.

### Making one

Press **New...** and you are asked two things.

**Which RISC OS.**

| Choice | When |
| --- | --- |
| **RISC OS 5.30, the stable release** | Start here. It is the released version and it stays put. |
| **RISC OS 5.31, the nightly development build** | What the developers are working on now. Moves often, occasionally breaks. |
| **Do not download; I will choose a ROM myself** | You already have a ROM image, including older ones like RISC OS 3.71. |

**Whether to include a hard disc.** Leave this ticked. It fetches HardDisc4 from
RISC OS Open — applications, fonts, `!System` and a `!Boot` configured for the
ROM you chose. Without it the machine starts to a command prompt with nothing on
it.

**Whether to set up networking.** Leave this ticked too. It adds the `!Boot`
files that bring the network up when RISC OS starts: an address from DHCP, name
resolution, and Access sharing so the machine can see others on your network.
Without it networking has to be configured by hand inside RISC OS, which is
fiddly. It needs the hard disc, since the files go into that disc's `!Boot`.

You will be asked to accept RISC OS Open's licence, which is theirs.

Select the machine and press **Start**.

### The other buttons

| Button | What it does |
| --- | --- |
| **Edit...** | The machine's settings, before it starts |
| **Clone...** | A complete copy, including its hard disc — the safe way to experiment |
| **Delete** | Removes the machine and its files |

The rest is on the menus:

| Menu item | What it does |
| --- | --- |
| **Machine ▸ Resume** | Picks up a machine you suspended, exactly where it was |
| **Machine ▸ Load State...** | Opens a snapshot you saved |
| **Machine ▸ Create Shortcut...** | A shortcut that opens this machine directly, skipping the manager |
| **Settings ▸ Default Machine** | That machine opens when you start RPCEmu |
| **File ▸ Settings...** | Where everything is kept, how a machine is drawn, and what the window asks before it does something |

Everything that belongs to a machine is on the **Machine** menu, including
**Edit...**, **Clone...** and **Delete**, which are greyed until one is selected.
**File** keeps **New Machine...**, **Settings...** and **Exit**.

### Opening one machine every time

If you always use the same machine, you do not have to pick it from the list.
Tick **Open this machine automatically at startup** on the machine's **System**
settings — or **Settings ▸ Default Machine** while it is running — and RPCEmu
opens straight into it.

**Hold Shift while starting** to get the list back. You will want that if the
machine you chose stops working, and it is the only way back other than editing
the setting from another machine.

`--machine <name>` does the same thing for one run without changing the setting;
see [Running it from a command line](#running-it-from-a-command-line).

### The manager's own settings

**File ▸ Settings...** is about RPCEmu rather than any one machine. These are
kept per computer, not in the data folder, so a set of machines carried to
another computer does not bring them along.

| Setting | What it does |
| --- | --- |
| **Data folder** | Where machines, ROMs, discs and settings are kept. **Change...** moves them, and offers to bring your files along |
| **Enable hardware acceleration** | Draws a machine's screen on the graphics card. On by default, and falls back on its own where it cannot start. Applies only to a machine shown in the manager |
| **Start with a minimal interface** | Open with the toolbar and machine list hidden |
| **Ask before stopping a machine** | Not asked for a machine set to suspend on exit, which loses nothing by being stopped |
| **Ask when closing with machines running** | The warning when the window closes with something still going |

### A less cluttered window

**View ▸ Minimal Interface** hides the toolbar and the machine list, leaving the
window to the machine it is showing. It is a tick rather than a preference: it
lasts for the session and does not change what the next launch does. The
**Settings** entry above is the one that persists.

**View ▸ Machine List** shows and hides the list on its own — the same thing as
the arrow at the left of the status bar, which is the quicker way to reach it.

## Choosing a machine to emulate

The default is fine and you can change it later, but the choice does matter.

| Model | Notes |
| --- | --- |
| **Risc PC - ARM610** | The original 1994 machine. Slowest. |
| **Risc PC - ARM710** | A little faster. |
| **Risc PC - StrongARM** | The one most people had by the end, and a sensible default. |
| **Risc PC - Kinetic** | A StrongARM card with its own memory. Supports up to 512MB, more than any other model here. |
| **A7000 / A7000+** | Cheaper machines without the Risc PC's expansion. |
| **Risc PC - ARM810** | Experimental. Rare hardware that barely shipped. |

**Memory.** 256MB is comfortable. Below 256MB some things behave differently
because RISC OS lays memory out differently, and only the Kinetic goes above it.

**Video memory (VRAM).** 2MB is the maximum a real Risc PC took and is what you
want. If you need higher resolutions, that is what the graphics card is for
rather than more VRAM.

## The desktop

Four things make the first hour much easier.

**Three mouse buttons, and RISC OS uses all three.**

| Button | RISC OS calls it | What it does |
| --- | --- | --- |
| Left | Select | Chooses, opens, drags |
| **Middle** | **Menu** | **Opens the menu for whatever you are pointing at** |
| Right | Adjust | A modifier: opens without closing what is behind, keeps menus open, extends selections |

The middle button is the one everyone misses. RISC OS has no menu bar inside its
windows — you click Menu wherever you are and get the menu for that thing.

Two buttons or a trackpad? Turn on **Settings ▸ Two-button Mouse Mode** and the
right button becomes Menu. You lose Adjust, which you can live without at first.

**The icon bar.** The strip along the bottom. Discs and devices on the left,
running applications on the right. Running an application does not open a window;
it puts an icon on the bar. Click that icon to get a window.

**Windows.** The close, resize and scroll furniture is in different places from
what you are used to. The button at the top left closes; the one at the top right
toggles full size; the bottom right resizes. Dragging a window's title bar with
Adjust moves it without bringing it to the front.

**Menus stay put.** Clicking a menu entry with Adjust keeps the menu open, which
is useful when setting several options at once.

## The filer, and how files work

The filer is RISC OS's file manager. Double-click a disc on the icon bar to open
it.

**Applications are folders.** Anything beginning with `!` is an application: a
directory containing everything it needs. `!Draw` is a folder. Double-clicking
runs it; **shift**-double-click opens it to look inside. For most software there
is no installation — put the folder somewhere and it works, and delete the folder
to uninstall.

**Filetypes are not extensions.** RISC OS stores a file's type separately from
its name, so a text file is just `ReadMe`, with a type of Text. The filer shows
the type as an icon and in the menu. This matters when files cross to your own
computer, where the type is stored as a suffix: `ReadMe,fff`. Removing that
suffix is what makes RISC OS stop recognising the file.

**Saving is dragging.** A save box shows a file icon and a name. Drag the icon
into a filer window to save it there. You can type a full path instead, but
dragging is how it is done.

**Selecting.** Click to select, Adjust-click to add to the selection, and drag a
box round several. The filer's Menu button gives you copy, rename, delete, set
type, and more.

## What is on the hard disc

HardDisc4, which the new-machine step fetches, comes with more than it first
appears.

- **`!Boot`** — the machine's configuration. Everything that runs at startup is
  in here. `!Boot ▸ Choices` holds settings; leave the rest alone until you know
  why you are in there.
- **Apps** — `!Draw` (vector drawing, and better than it sounds), `!Paint`
  (bitmaps), `!Edit` (text), `!Alarm`, `!Chars`, `!Help`.
- **`!System`** — shared modules that applications need. The package manager
  keeps this in order; adding things by hand is a common way to break a machine.
- **Utilities** — disc tools, configuration, fonts.

`!Help` is worth opening early: point at something and it tells you what it is.

## The command line

**F12** drops to the RISC OS command line from the desktop. Press Return on an
empty line to go back.

Useful ones:

| Command | Does |
| --- | --- |
| `*Help <thing>` | Explains a command or module |
| `*Cat` | Lists the current directory |
| `*Modules` | Lists loaded modules |
| `*Configure` | Machine settings held in CMOS |
| `*ShowFree` | Free space |
| `*Shutdown` | Shuts RISC OS down tidily |

Note that RPCEmu binds no host keyboard shortcuts at all, so F12, the other
function keys and every Ctrl combination go straight to RISC OS. Everything the
emulator itself does is on a menu or the toolbar. The single exception is
**Alt+Enter**.

## Shutting down properly

RISC OS caches disc writes, so closing the window on a machine that has not been
shut down can lose the last few seconds of work.

The tidy way: **Shutdown** from the Task Manager (the Acorn icon at the right of
the icon bar), or `*Shutdown` at the command line. Then close the window.

If you would rather not think about it, turn on **Suspend on Exit** in the
machine's settings: closing the window then saves the whole machine and resumes
it exactly where it was next time.

## HostFS and Shared: two discs that are really folders

Two of the discs on the icon bar are folders on your own computer. Put a file in
one and it appears on the other side immediately.

| Disc | Where it is | Shared between machines |
| --- | --- | --- |
| **HostFS** | that machine's own `hostfs` folder | No, unless you point several machines at the same folder |
| **Shared** | the `shared` folder in your data directory | Yes, always |

Use **Shared** for one folder every machine can see. Use **HostFS** when a
machine should have its own.

You can point HostFS anywhere: *Settings ▸ Machine ▸ System ▸ HostFS folder*.
Point it at a folder that already has files and they appear in RISC OS as they
are. Point it at an empty folder and you get an empty disc — which, if it is the
disc the machine boots from, means a machine that starts to a bare screen.

Remember the `,xxx` filetype suffix described above. Files created on your
computer without one arrive as plain data, and RISC OS will not know what they
are until you set the type from the filer menu.

More in [docs/hostfs.md](docs/hostfs.md).

## The clipboard

**Settings ▸ Share Clipboard with RISC OS** links the two clipboards, so text
copied in one can be pasted in the other. It is off by default because it means
your host clipboard is visible to the guest.

## Installing software

**Tools ▸ Package Manager**, with a machine open and started at least once.

Around 200 packages — applications, games, fonts, libraries, system components —
from the same repositories a real RISC OS machine uses. Pick one and it is
installed onto that machine's disc along with anything it depends on, and
`!System` is kept in order for you.

It is per machine: installing on one does not touch another.

Full details in [docs/packages.md](docs/packages.md).

## The menus in full

### File

| Item | What it does |
| --- | --- |
| **Screenshot...** | Saves what the guest is displaying as a PNG |
| **Save State...** | Writes the whole machine to a file |
| **Load State...** | Puts one back, exactly as it was |
| **Recent machines** | Switch to another machine you have used |
| **Reset** | Reboots the guest, as the reset button would |
| **Suspend** | Saves the machine and closes it |
| **Suspend on Exit** | Do that automatically when the window closes |

### Disc

**Floppy** loads a disc image into drive 0 or 1, ejects one, or creates a blank
formatted image. ADFS `.adf` and `.adl`, DOS `.img` and HFE are understood.

**CD-ROM** attaches an ISO image, empties the drive, or on Linux uses a real
drive in your computer.

Both keep a list of what you have used recently.

### Settings

| Item | What it does |
| --- | --- |
| **Machine...** | The full settings for this machine |
| **NAT Port Forwarding Rules...** | Let your network reach a server inside the guest |
| **VNC Server...** | Serve this machine's screen over the network |
| **Serial... / Parallel... / USB...** | The ports, described below |
| **Mute Sound** | Silence without changing the machine |
| **Full-screen Mode** | Also **Alt+Enter**, which is how you get back |
| **Pixel Perfect** | Scale only in whole multiples, so pixels stay square |
| **Fit to Window** | Scale the picture to whatever size you drag the window |
| **Mouse Follows Host Pointer** | On by default. Turn it off for games (see below) |
| **Two-button Mouse Mode** | Right button becomes Menu |
| **Share Clipboard with RISC OS** | Link the two clipboards |
| **Reduce CPU Usage** | Idle when RISC OS is idle |

### Tools

**Package Manager**, as above.

### Debug

Run, Pause, Step and Step ×5 control the emulated processor, and **Machine
Inspector** opens a window with registers, disassembly and a memory browser.
Useful if you are writing ARM code; ignorable otherwise.

### Help

**Online Manual** (this document), **Visit Website**, **Report an Issue**,
**Create Support Bundle...**, **Check for Updates...**, **About RISC OS** and
**About**.

## The display

RISC OS picks its own screen mode and the window follows, so the window changes
size when the guest changes mode. That is normal and surprising the first time.

**Fit to Window** scales the picture to the window at whatever size you drag it,
keeping the proportions right and putting black bars where they do not match.

**Pixel Perfect** scales only by whole numbers — twice, three times — so pixels
stay sharp squares. Better for games and anything pixel-art; costs you black
borders when the window is not an exact multiple.

**Full-screen Mode**, or **Alt+Enter**. The same key leaves.

**Follow the host display size** (in the machine's Options) tells the guest what
your monitor can do, so RISC OS offers sensible modes rather than 1980s ones.

**The graphics card** is an emulated card with its own memory, allowing
resolutions far beyond a real Risc PC and without reconfiguring the guest for
each. Turn it on in the machine's System settings, which also sets up its driver
inside the machine. One caution: it cannot be used with ADFFS, which writes to
screen memory at its physical address. See [docs/gfxcard.md](docs/gfxcard.md).

## Keyboard and mouse

**Every key goes to RISC OS.** No host shortcuts are bound, so F12, the function
keys and Ctrl combinations all reach the guest. The one exception is
**Alt+Enter**, which leaves full-screen, or releases a captured mouse when there
is no full-screen to leave. In full-screen with the mouse captured that is two
presses: one to leave, one to be given the mouse back.

**Keys are identified by position, not by character.** Whatever layout you use —
German, French, Dvorak — a key produces what is printed on it. AltGr works.

**The mouse** has two modes, and the default suits everything except games.

**Mouse Follows Host Pointer** (Settings, on by default) puts the RISC OS pointer
wherever your own pointer is. Nothing is grabbed, you can leave the window
whenever you like, and it works windowed, scaled and full-screen.

Turn it off and the machine is in **captured** mode instead. Click once in the
window and the mouse is captured: your pointer is pinned out of sight and RISC OS
is sent how far the mouse moved rather than where it ended up. **Alt+Enter** gives
it back. This is what games need — a game that draws and moves its own pointer,
or reads movement directly, cannot work with a pointer being put where the host's
is. Games are the reason the mode exists, so if one ignores the mouse or the
pointer fights you, this is the first thing to change.

The setting belongs to the machine and is remembered, so a machine left in
captured mode is still in it next time. Both modes work the same way in the
Manager as in a machine's own window; the status bar there says which mode you
are in and what to press.

## Floppy and CD-ROM

Disc images behave as discs. Load one and RISC OS sees a disc in the drive;
eject and it is gone. Creating a blank image lets you format it as ADFS D, E, F,
L or DOS, and RISC OS will then use it as a real floppy.

## Networking

A machine has no networking until you give it some, in *Settings ▸ Machine ▸
Network*.

**NAT** is the one to choose. Nothing on your computer needs configuring. The
guest gets a private address, can reach the internet, and can be reached from
your network through port forwarding. Sharing files with other RISC OS machines
over Access and ShareFS works through it too.

**Ethernet Bridging** and **IP Tunnelling** put the guest directly onto your real
network. They need the host set up to match — a TAP interface on Linux — and are
worth it only when you specifically need the guest to be a first-class machine on
your LAN.

**NAT Port Forwarding Rules** lets something outside reach a server inside the
guest: give it a host port and the guest port to send it to. The dialogue names
the machine and the address it will forward to.

Machines running on one computer see each other automatically, each with its own
address. See [docs/multi-machine.md](docs/multi-machine.md).

## Printing

*Settings ▸ Parallel* decides what happens to what RISC OS prints.

| Mode | What it does |
| --- | --- |
| **Disabled** | Nothing |
| **Log to file** | The raw byte stream, for debugging |
| **Virtual printer** | Writes each job as a `.prn` file, with optional automatic PDF conversion |
| **Print on this computer** | Sends the finished job to a real printer your computer knows about, by queue name or device path |

Worth knowing: this carries the print stream, not the pins. Devices needing real
bidirectional IEEE-1284 signalling — dongles, Zip drives, scanners — are not
supported and are not planned.

## The serial port

*Settings ▸ Serial*. The Risc PC has one serial port, so there is one here.

| Mode | What it does |
| --- | --- |
| **Disabled** | Nothing |
| **Log to file** | Captures what the guest sends |
| **TCP modem** | Answers Hayes AT commands; `ATDT host:port` opens a TCP connection, speaking telnet and negotiating binary mode, so BBSes and X/Y/ZMODEM transfers work |
| **A real port on the host** | Hands the guest an actual port: USB adapter, built-in, or a pseudo terminal |

Details, including the AT commands, in [docs/peripherals.md](docs/peripherals.md).

## USB devices

*Settings ▸ USB* passes a device from your computer through to RISC OS, which
sees it as though plugged into the machine. Mass storage works, and so do
webcams.

The machine needs the USB card fitted, which the settings do for you. Each
platform needs something different of you — permissions on Linux, driver
installation on Windows — and [docs/usb.md](docs/usb.md) covers each. This has
been tested most thoroughly on Linux.

## Snapshots: save, suspend and resume

**File ▸ Save State** writes everything — memory, registers, discs, what is on
screen — to a file, and **Load State** restores it exactly.

**File ▸ Suspend** does the same and closes the machine; **Resume** in the
machine list brings it back. **Suspend on Exit** does it whenever you close the
window, so a machine is always where you left it.

Snapshots belong to the machine that made them.

## Several machines at once

Run as many as your computer will take. Each is its own process with its own
network address and its own ports, so they do not tread on one another. A
machine refuses to start twice, because two copies writing to one disc image
will corrupt it — and you would not find out until much later.

Each machine costs a core when busy, plus its own RAM.

### From the manager window

The window RPCEmu opens on lists your machines down the left and shows one of
them on the right. Select a machine and press **Start**; select another and
start that one too. The list says which are running, and clicking a running
machine switches the display to it. The one being shown is named in the title
bar and in the status bar.

**Closing the window does not stop the machines.** They keep running, and
opening RPCEmu again reconnects to them — you are asked about this the first
time, with the option to stop them instead. **Machine ▸ Stop** shuts one down.

Drag the divider to make the list wider or narrower, or use the arrow at the
bottom left to hide it and give the whole window to the machine.

### One window per machine

`--machine` starts a machine in a window of its own, with no manager:

```sh
./rpcemu-recompiler --machine os530 &
./rpcemu-recompiler --machine os371 &
```

That is the older arrangement, and it still works. It is also what a machine
started from the manager runs as underneath — the manager draws that machine's
screen in its own window rather than letting it open one.

## Speed and CPU usage

The recompiler translates ARM code to your processor's, so a machine runs many
times faster than the hardware ever did. The MIPS figure in the status bar is
what the guest is achieving.

**Reduce CPU Usage** lets the emulator sleep when RISC OS is idle rather than
spinning. It costs a little speed and saves a great deal of battery and fan
noise. Turn it off if you are chasing maximum performance or if audio stutters.

A low MIPS figure is usually the host being busy rather than the emulator being
slow.

## Machine settings in full

*Settings ▸ Machine* while running, or **Edit...** in the list. Five pages.

**System** — the model, memory, VRAM, which ROM, the hard disc, the HostFS
folder, refresh rate, the high-resolution graphics card, and whether the machine
starts full-screen or opens automatically.

**Options** — pixel perfect, fit to window, follow the host display size, the
CD-ROM drive, two-button mouse, reducing CPU usage when idle, the VNC server, and
whether to explain full-screen when entering it.

**Network** — as above.

**IDE Drives** — the two hard disc images.

**Podules** — the expansion cards fitted: network, graphics, USB.

Most changes take effect at the machine's next reset; the dialogue says which.

## Where your files live

Everything is under the data directory you chose on first run:

```
<data directory>/
    configs/            one .cfg per machine
    machines/
        <name>/
            hostfs/     that machine's HostFS disc
            hd4.hdf     its hard disc image
            cmos.ram    its CMOS settings
            *.state     snapshots
    roms/               ROM images
    shared/             the Shared disc, visible to every machine
    rpclog.txt          the log
```

Moving it: **File ▸ Settings...**, then **Change...** under Data folder, which
offers to bring your files along. `--datadir <path>` points one run elsewhere
without changing anything permanently. See [docs/paths.md](docs/paths.md).

## Running it from a command line

Useful even if you normally use the window.

| Option | Does |
| --- | --- |
| `--machine <name>` | Start straight into that machine |
| `--list-machines` | List them and exit |
| `--datadir <dir>` | Use a different data directory for this run |
| `--fetch-riscos` | Download RISC OS and make a machine ready to run, then exit |
| `--headless` | Run with no window, reachable over VNC |
| `--vnc-port <n>` | Which port the VNC server uses |
| `--resume` | Resume that machine's saved state |
| `--pkg-list`, `--pkg-install=<name>` | Use the package manager without the GUI |

`--help` lists them all.

On Linux and macOS, sending `SIGUSR1` resets a running machine, the same as
**Reset** on the File menu:

```sh
kill -USR1 $(pgrep rpcemu-recompiler)
```

## For developers

Extended Edition carries a good deal aimed at people writing software for
RISC OS.

**HostCmd** runs commands inside the guest from your own shell and streams the
output back, so you can edit on your computer and build on RISC OS:
`rpcemu-run -- cc -c hello`, or `rpcemu-shell` for an interactive session.
[docs/hostcmd.md](docs/hostcmd.md)

**The debugger** stops the emulated ARM and inspects it: conditional
breakpoints, step over and out, backtraces, symbol names and disassembly — from
the Debug menu or from `rpcemu-debug` at a terminal.
[docs/debugcmd.md](docs/debugcmd.md)

**VNC and headless** expose a machine over the network or run one with no window
at all, for a build server or a machine on another computer.
[docs/vnc.md](docs/vnc.md)

**The MCP server** lets an AI assistant drive a machine: run commands, build,
screenshot, inspect the CPU. `tools/mcp/`

## Troubleshooting

**The machine starts to a grey screen or a `*` prompt.** Its disc has no `!Boot`
to run — usually an empty HostFS folder, or a machine made without the hard
disc. Make a new machine and let it fetch HardDisc4, or point HostFS at a folder
that has one.

**"The machine is already running."** Another copy has it open. If nothing is
running, nothing needs cleaning up: the lock is held by the operating system and
released when that process ends.

**No sound.** Check *Settings ▸ Mute Sound* is not ticked, and that sound is
enabled in the machine's own settings.

**Sound stutters or the machine feels uneven.** Turn off *Reduce CPU Usage*.

**The window is a strange size.** RISC OS chose that mode. *Fit to Window* lets
you drag it to whatever you want.

**Files from my computer are not recognised.** They need a RISC OS filetype:
either the `,xxx` suffix on your side, or set the type from the filer menu.

**macOS says it cannot open the application.** Right-click and choose Open the
first time; the build is not notarised.

**Something else.** *Help ▸ Create Support Bundle* collects the log, the
machine's settings and optionally a screenshot into one zip — that is the thing
to attach to a report. *Help ▸ Report an Issue* opens the tracker; the template
asks for your operating system, which build and which RISC OS version, because
those three answer most of what we would otherwise have to ask.

## Credits and licence

RPCEmu was written by Sarah Walker and is maintained by Peter Howkins and
Matthew Howkins. Extended Edition is a fork by Andy Timmins, with contributions
from David Ramsden, Nick Brown and others; the About box lists them, and each
release credits that release's contributors and the people whose reports became
fixes.

RISC OS is developed and published by
[RISC OS Open](https://www.riscosopen.org/), who are not connected with this
project. Their documentation covers the operating system itself far better than
this can.

The emulator is GPL v2. See `COPYING`.
