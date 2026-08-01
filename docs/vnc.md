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

*Settings → VNC Server* while a machine is running, or the **VNC server** tick in
the machine editor. Both write to the emulator's own settings file rather than to
a machine's, so it is on or off for RPCEmu as a whole.

That file is `rpcemu.cfg` in the data directory, alongside `configs/` and
`machines/`. It is plain `key=value` text, editable by hand, which matters when the
only way in is ssh:

```
vnc_enabled=1
vnc_port=5900
vnc_password=
hostcmd_enabled=1
hostcmd_socket=
```

An empty password means no authentication. Anyone who can reach the port can use
the machine and, with the control menu, reset it or shut the emulator down, so set
one on anything reachable from a network you do not control.

### Why these are not per-machine

They used to be. The VNC server and the HostCmd socket are how a person or a script
reaches the running process, no more a property of an emulated Risc PC than the
window is, and keeping them per-machine had two consequences. "Which machine did I
turn VNC on for?" became a real question. And nothing could listen before a machine
was chosen, which makes a remote machine selector impossible.

Settings from before this change are carried forward: the first time a machine with
its own `vnc_port` or `hostcmd_socket` is loaded, the value is moved into
`rpcemu.cfg` and a line is written to the log saying so. Where two machines
disagree, the first one loaded wins.

## Choosing a machine over VNC

Headless mode used to require `--machine`. Started without one, it now serves the
machine list to any client that connects:

```bash
./rpcemu-recompiler --headless
```

Arrow keys to move, or type the number beside an entry, then Enter to start it.
Escape gives up and exits. The port and password come from `rpcemu.cfg`, since no
machine has been chosen to ask.

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
