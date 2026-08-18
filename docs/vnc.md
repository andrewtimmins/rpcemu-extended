# VNC: the emulator over a network

RPCEmu has a VNC server built in, so a machine can be used from another computer
with any VNC client. It is not only for headless servers: a desktop session can be
picked up remotely, which is the point of most of what follows.

Three things are worth knowing beyond "it shows the screen":

- The server belongs to the **emulator**, not to a machine, so its settings live in
  one place and it stays up across machines starting and stopping.
- Started headless with no machine named, it offers a **machine list** to choose
  from.
- **`Ctrl`+`Alt`+`Shift`+`M`** brings up a control menu over a running machine, so
  a remote user is not limited to typing at the guest.

## Turning it on

*Settings → VNC Server* while a machine is running, or the **Host access** group on
the machine editor's Options page, which has the port, control password and
read-only password beside the tick. Both write to that machine's own configuration,
so each machine has its own server, port and passwords, and several can run at once
without being told on the command line every time.

There is a second file for what applies before any machine is chosen: `rpcemu.cfg`
in the data directory, alongside `configs/` and `machines/`. Headless with no
machine named has to be reachable before there is a machine to ask, so this is
where that server gets its port and passwords. It also supplies the default for a
machine whose own configuration does not mention these keys. Plain `key=value`
text, editable by hand, which matters when the only way in is ssh:

```
vnc_enabled=1
vnc_port=5900
vnc_password=
vnc_password_readonly=
hostcmd_enabled=1
hostcmd_socket=
```

`vnc_password` is the control password. A client authenticated with it can use the
keyboard and mouse and, through the control menu, reset the machine or shut the
emulator down. An empty control password means no authentication, so anyone who
can reach the port has that control; set one on anything reachable from a network
you do not control.

`vnc_password_readonly` is optional. A client authenticated with this second
password can watch the framebuffer but cannot send keyboard or mouse input, choose
a machine in the headless selector, or open and operate the VNC control menu. It
therefore cannot reset or shut down the emulator through VNC. Leaving it empty
disables the read-only tier and preserves the previous single-password behaviour.
A read-only password requires a non-empty control password; the server refuses to
start rather than silently expose passwordless control if only the read-only value
is set. Traditional VNC authentication uses only the first eight bytes of a
password, so the server also refuses values whose first eight bytes are identical.

For the bundled MCP server, set `RPCEMU_VNC_PASSWORD` to the control password. An
AI agent can then use the MCP screen and input tools while a human connects a VNC
viewer with the read-only password to watch the same live session without sending
input.

### Which file wins

Three layers, weakest first:

1. The built-in defaults: VNC off, port 5900, no control or read-only password,
   HostCmd on.
2. `rpcemu.cfg`, for anything it mentions. This is what the machine selector uses,
   since no machine has been chosen yet, and the default for a machine that says
   nothing about these keys.
3. The machine's own `.cfg`, which wins. This is where the machine editor and
   *Settings → VNC Server* write.

`--vnc-port` and `--hostcmd-socket` on the command line are applied after all
three, which is how several copies sharing one data directory each get their own.

A machine created while these were emulator-wide settings has none of these keys in
its configuration, so it keeps running with whatever `rpcemu.cfg` says until it is
saved again. Nothing is moved between the files.

## Choosing a machine over VNC

Headless mode used to require `--machine`. Started without one, it now serves the
machine list to any client that connects:

```bash
./rpcemu-recompiler --headless
```

Arrow keys to move, or type the number beside an entry, then Enter to start it.
Escape gives up and exits. The port and passwords come from `rpcemu.cfg`, since no
machine has been chosen to ask. A read-only client can see this list but cannot
choose from it.

The connection carries straight through into the machine you pick: it is the same
server, so there is no reconnecting.

This is deliberately headless-only. In a desktop session the machine has already
been chosen by the person sitting at it, and offering the list remotely as well
would let a remote client boot a machine on somebody's desktop unasked. What a
remote user of a desktop session wants is control of the machine that is running,
which is the next section.

## The control menu

**`Ctrl`+`Alt`+`Shift`+`M`** over a running machine, in any session, headless or
not. A desktop session you walked away from and reconnected to gets the same menu.
Only a client authenticated with the control password can open or use it.

- **Reset the machine** — as the Reset item on the File menu.
- **Return to the machine** — close the menu. Escape does the same.
- **Shut the emulator down** — saves CMOS, discs and settings on the way out, as
  quitting normally does.

Up and Down to move, Enter to choose. Each item says what it will do, so nothing
destructive is picked from a three-word label.

It is drawn into the VNC display only, so nobody at the local window sees it, and
the menu scales with the guest's resolution rather than being an 8x16 cell adrift
on a 1920x1080 screen.

Three modifiers deliberately: RISC OS makes heavy use of Alt, and a shorter
combination risks taking a key the guest wanted. While the menu is closed every key
goes straight to the guest and only that combination is watched for.

While the menu is up the machine keeps running and drawing; the client simply sees
the menu instead of the guest until it closes.

## Notes and limitations

- **Keyboard only** in both the selector and the menu. The mouse belongs to the
  guest.
- **One VNC server per process.** Two emulators on one host need different ports,
  set in each one's data directory.
- **What the menu cannot do yet**: snapshots, and going back to the machine
  selector without stopping the process.
- **`--headless` starts the server whatever the settings say**, since without a
  window there is no other way in. The setting itself is left alone.

## Running more than one emulator at once

Several machines can run side by side, each in its own process, which is how
VirtualBox and VMware do it too. The emulator core is one machine per process, so
this is the arrangement rather than a limitation to work around.

The catch is that the settings above are per *installation*, and three instances
sharing a data directory would all want port 5900 and the same control sockets. So
give each one its own on the command line:

```bash
./rpcemu-recompiler --machine os371 --vnc-port 5901 --hostcmd-socket /tmp/os371.sock &
./rpcemu-recompiler --machine os530 --vnc-port 5902 --hostcmd-socket /tmp/os530.sock &
./rpcemu-recompiler --machine os531 --vnc-port 5903 --hostcmd-socket /tmp/os531.sock &
```

The command line beats the settings file, which beats a machine's own legacy value,
which beats the built-in default.

### Access sharing, and why only one instance relays

The relay bridges Access broadcasts between the guests and your real network, and
Access uses fixed UDP ports, so it is a **host-wide** service rather than a
per-machine one. Only one emulator can sensibly do it: two would each relay every
packet, so the network would see everything twice and each instance would see the
other's copies. Nothing would fail, which is what makes it unpleasant to diagnose.

So the first emulator to start claims it and the rest decline, saying so in the
log. They still reach the network through NAT as usual; only Access sharing goes
through the instance that holds the relay. If the holder exits, the next one to
start takes it over. `--no-relay` declines deliberately.

Emulated machines cannot yet see *each other* over Access, since each has its own
NAT. That needs a virtual switch, which is a separate piece of work.

### What it costs

Each instance carries its own guest RAM, 32MB of address-translation tables and a
recompiler code cache, and its emulator thread will use a core. Three machines is
roughly a gigabyte and three busy cores, which is fine on a development machine and
worth knowing before starting six.
