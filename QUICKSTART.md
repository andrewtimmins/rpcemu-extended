# Quick start

RISC OS running on your computer, in about five minutes. No RISC OS knowledge
needed, and nothing to find or buy first.

If you want to know what any of it means afterwards, [MANUAL.md](MANUAL.md)
explains it properly.

## 1. Get RPCEmu Extended

Download the build for your computer from the
[releases page](https://github.com/andrewtimmins/rpcemu-extended/releases/latest).

| Your computer | Take |
| --- | --- |
| Windows | `rpcemu_<version>_windows_amd64.zip`, and unzip it |
| macOS | `rpcemu_<version>_macos_universal.dmg` |
| Linux (Intel/AMD) | `rpcemu_<version>_amd64.deb`, or the `linux_amd64.tar.gz` |
| Linux (Raspberry Pi, ARM) | `rpcemu_<version>_arm64.deb`, or the `linux_arm64.tar.gz` |

**macOS:** the build is not notarised by Apple, so the first launch needs
right-click then **Open**, rather than a double-click. You only do this once.

## 2. Start it

It asks where to keep its data the first time. The suggestion is fine; press
OK. That folder is where your machines, their discs and their settings will
live, and you can move it later.

You then get a list of machines, which is empty.

## 3. Make a machine

Press **New...**.

It offers to fetch RISC OS for you. Take the defaults:

- **RISC OS 5.30, the stable release** — the version to start with.
- **Include a ready-to-use hard disc** — leave this ticked. This is HardDisc4
  from RISC OS Open: the applications, fonts and a configured `!Boot` that make
  the machine usable. Without it you get a working computer with nothing on it.

You will be asked to accept RISC OS Open's licence, which is theirs and not
ours. Then it downloads, which takes a minute or two.

## 4. Start it

Select your machine and press **Start**.

RISC OS boots to its desktop. The bar along the bottom is the icon bar: discs
and devices on the left, running applications on the right.

That is it. You are running RISC OS.

## The five things worth knowing straight away

**The mouse has three buttons and RISC OS uses all of them.** Left selects,
middle opens a menu wherever you are pointing, right adjusts. The middle button
is the one people miss: in RISC OS the menu is not at the top of the window, you
click for it. If you have a two-button mouse or a trackpad, turn on
**Settings ▸ Two-button Mouse Mode** and the right button becomes the menu.

**Applications live on the icon bar, not in windows.** Double-click an
application and it puts an icon on the bar rather than opening a window. Click
that icon to get a window from it.

**To get files in and out**, use the **HostFS** disc on the icon bar. It is a
folder on your own computer, so anything you put there appears in RISC OS and
the other way round. There is also a **Shared** disc that every machine can see.

**To install software**, open **Tools ▸ Package Manager**. Around 200
applications, games, fonts and libraries from the same repositories a real
RISC OS machine uses.

**To leave full-screen**, press **Alt+Enter**. The same key gets you into it
from **Settings ▸ Full-screen Mode**.

## If something is not right

- **The window is too small or too large.** *Settings ▸ Fit to Window* scales
  the display to whatever size you drag the window to. *Pixel Perfect* keeps
  the pixels sharp instead.
- **No sound.** *Settings ▸ Mute Sound* may be ticked; the machine also needs
  sound enabled in its own settings.
- **The machine will not start.** *Help ▸ Create Support Bundle* collects the
  log and the machine's settings into one zip file, which is the thing to attach
  to a bug report.

## Where next

- [MANUAL.md](MANUAL.md) — what all of it does, written for someone new to
  RISC OS as well as to this emulator.
- [The RISC OS Open website](https://www.riscosopen.org/) — RISC OS itself, its
  documentation and its community.
