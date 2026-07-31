# USB

RPCEmu can present a USB host controller to the guest, so that a machine which
never had USB can be given it, and can hand it a real USB device plugged into
the computer it is running on.

## What is being emulated

An **OHCI** host controller, on an expansion card of its own, with a root hub of
**four ports**.

That choice needs explaining, because it is not what a Risc PC could have had.

The card that existed in reality was the Simtec podule and the STD Unipod, both
built around a **Philips ISP1161**, and RPCEmu emulated one for a while. The
ISP1161 is what you fit when the bus cannot bus-master: it keeps its transfer
descriptors and data in its own on-chip buffer RAM and the processor moves them
through a data port, so nothing ever reaches into main memory. A real Risc PC
expansion bus cannot bus-master, which is exactly why those cards used it.

OHCI is the opposite. It keeps a communication area and linked lists of endpoint
and transfer descriptors in main memory, and expects the controller to walk them
itself. No real Risc PC card could do that. **We are not a real card**, and
reaching into guest memory costs us nothing, so the guest cannot tell.

What that buys is the driver stack. RISC OS 5's USB support is the Castle stack,
adopted and maintained since by RISC OS Open: `USBDriver` with `OHCIDriver`
underneath it, and HID keyboard and mouse support built into `USBDriver` itself.
The only RISC OS driver for the ISP1161 is `PHCIDriver`, a Pace driver from 2001
that nobody maintains, and which assumes it is the only expansion card in the
machine - it claims the shared podule interrupt with a mask that matches *every*
card's interrupt, so with a second card fitted it starves the other's driver and
wedges the machine. That is a fault it would show on real hardware too.

Historical accuracy would have bought a card nothing can drive. This buys a card
the maintained stack can.

## The card

The controller lives in the USB card's EASI space at offset **&800000**, as a
normal OHCI register file: 32-bit registers at the offsets the specification
gives, `HcRevision` through `HcRhPortStatus`.

The card is its own expansion card rather than part of the RPCEmu support card,
for two reasons. It carries a ROM of its own (`usbroms/`) for the module that
tells RISC OS about it. And an expansion card with one interrupt and one thing
raising it is far easier to reason about - when the controller shared the support
card with the scroll wheel, two drivers ended up claiming the same podule
interrupt and clearing each other's, which wedged machines outright.

Which slot it lands in does not matter, and nothing should assume one. The
support card is in slot 0 and the USB card in slot 1, but the guest module finds
the controller by reading `HcRevision` through each card's EASI space until one
answers with an OHCI revision.

## Descriptors

OHCI's lists live in guest memory and the controller walks them once a frame.

An **endpoint descriptor** is four words: flags (function address, endpoint
number, direction, speed, maximum packet size), the tail and head pointers of
its queue, and a link to the next endpoint. The bottom bits of the head pointer
are not part of the address - bit 0 says the endpoint is halted and bit 1 carries
the data toggle between transfers.

A **transfer descriptor** is four words: flags (direction, rounding, delay
interrupt, toggle, and the condition code the controller writes back), the
current buffer pointer, a link to the next descriptor, and the last byte of the
buffer.

A buffer is described by its first and last byte, and is allowed to cross one
page boundary into a page that need not be next to it in physical memory. So a
length is a subtraction only when both ends are in the same page, and moving the
data is up to two runs rather than one. Getting that wrong would work for every
small transfer and fail on exactly the large ones.

Each frame the controller walks the periodic list (whose head for that frame is
one of the thirty-two entries in the communication area, chosen by the frame
number), then the control list, then the bulk list. Descriptors it has finished
with go on a done queue, newest first, which is handed back by writing its head
into the communication area and raising `WritebackDoneHead`. The queue is only
published once the driver has acknowledged the previous one.

A transfer whose device has nothing to say yet is left exactly where it is, and
the controller comes back to it on a later frame. That is what a real controller
does with an idle interrupt endpoint, and it is what makes a passed-through
device work without the guest knowing anything unusual is going on.

## Real devices

A device on the host's own USB bus can be handed to the guest, through
[libusb](https://libusb.info/). The controller does not know the difference: a
device is three questions - describe yourself, do this control request, have you
anything on this endpoint - and passthrough answers them by putting the same
question to real hardware.

This is the one part of USB that is a build-time option. The controller and the
card are always built; passthrough needs **libusb-1.0** present when the emulator
is compiled, and `src/usb_host.c` has a stub half that answers politely when it
was not. Install it with `./setup-build-env.sh` on Linux,
`pacman -S mingw-w64-x86_64-libusb` in MSYS2, `brew install libusb` on macOS, or
`./setup-cross-build-env.sh` for a MinGW cross build.

Release builds pass `-DRPCEMU_REQUIRE_LIBUSB=ON`, which turns a missing libusb
from a `STATUS` line into a configure failure. That is not pedantry: the Windows
and macOS releases shipped without passthrough for exactly this reason, because
neither CI job installed libusb and nothing in the build complained. A Linux
release records the answer in `BUILDINFO.txt`.

*Settings → USB...* lists the four ports and what is in each. The choice is per
machine and is remembered as the device's **identifiers rather than its
position** - `usb_port1=host:046d:c077` - so unplugging it and putting it in a
different socket does not lose it. If two identical devices are plugged in, the
first found is the one taken.

Handing a device over is not a neutral act. The host's own driver is detached
from it for as long as the guest has it, and it is given back when the device is
detached or RPCEmu closes. A device the host is using is marked as such in the
dialogue and takes a confirmation, because doing this to the mouse in your hand
would leave you without a pointer to undo it with.

### Getting permission

Every platform has some claim of its own on a USB device, and passthrough means
getting past it. What that takes differs enough to be worth setting out
separately. In all three cases the emulated card itself needs nothing.

#### Linux

The device nodes under `/dev/bus/usb` belong to root, so RPCEmu cannot open one
until the system is told to allow it. A rule naming the one device you want is
the least you can get away with:

```
# /etc/udev/rules.d/70-rpcemu-usb.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="046d", ATTRS{idProduct}=="c077", TAG+="uaccess"
```

then `sudo udevadm control --reload-rules && sudo udevadm trigger`, and unplug
and replug the device. `TAG+="uaccess"` gives access to whoever is logged in at
the machine rather than to everyone with an account on it. Dropping the two
`ATTRS` clauses grants the same for every USB device, which is convenient and
worth thinking about before doing.

Without the rule the dialogue still lists the device, marked "no permission",
and says what to do rather than failing when you try to use it.

The host's own driver is detached automatically for as long as the guest holds
the device (`libusb_set_auto_detach_kernel_driver`), and given back afterwards,
so there is nothing to unbind by hand.

#### Windows

Permission is not the obstacle here; the driver binding is. Windows attaches one
of its own class drivers to a device as soon as it appears - `usbstor` for a mass
storage device, HID for input, `usbccgp` for a composite one - and libusb cannot
open a device through any of them. It needs **WinUSB** bound to the device
instead.

[Zadig](https://zadig.akeo.ie/) is the usual way to do that: *Options → List All
Devices*, select the device, choose **WinUSB** as the target driver, and replace.
Two consequences to be clear about before doing it:

- **Windows stops using the device** while WinUSB is bound to it. That is the
  same trade the Linux driver detach makes, but it persists until reverted rather
  than lasting only while the guest holds it.
- **Reverting is manual**: *Device Manager*, find the device, *Uninstall device*
  with "delete the driver software for this device" ticked, then unplug and
  replug so Windows re-detects it and reinstates its own driver.

Keyboards, mice and hubs are held by Windows and are not realistic candidates.

#### macOS

Usually nothing to do. libusb reaches devices through IOKit and needs no
third-party driver and no equivalent of a udev rule, so a device macOS is not
already using can simply be selected.

Where it does not work, the reason is that one of Apple's own class drivers has
already claimed the interface - HID, mass storage, audio and USB serial are the
common ones - and the claim fails. There is no supported way to detach an Apple
driver the way `libusb_detach_kernel_driver` does on Linux, so those devices are
not available. RPCEmu is not sandboxed, so no entitlement is involved.

### Timing, and why it works at all

A real transfer takes milliseconds and the emulator thread cannot wait that long.
Nothing in the passthrough ever waits: a transfer is submitted, the descriptor is
left for a later frame, and the answer is collected whenever it arrives.

That is not a workaround. It is precisely what a device does when it has nothing
ready, so a driver written for real hardware already copes, and no allowance for
the emulator is needed anywhere in the guest.

Completions are noticed once per USB frame, on the emulator thread, so there is
no locking anywhere in the passthrough and no second thread to go wrong.

### Pulling it out

A device unplugged from the computer is unplugged from the guest on the next
frame: libusb says which device has left, and the port drops its connection and
raises the change the driver watches for. What the guest is told is that somebody
pulled a lead out, which every USB driver understands. Where libusb cannot report
hotplug, the same conclusion is reached from a transfer coming back to say nobody
answered.

It is **not** taken up again by itself when the device comes back. The
configuration still names it, so resetting the machine or pressing Apply will
take it up; but quietly seizing a device at the moment somebody plugs it into
their own computer is not a thing to do unasked.

## Seeing what is there

`RPCEmuUSBSupport`, in the support card's ROM, provides two commands.
`*RPCEmuUSBInfo` reports the card and the controller:

```
RPCEmuUSBInfo: the emulated USB host controller
Expansion card carrying it: 0x00000001
Controller registers at: 0xF2E00000
HcRevision reads: 0x00000010
HcRhDescriptorA reads: 0x01000004
Root hub downstream ports: 0x00000004
HcControl reads: 0x000000BF
The controller is running, so something is driving it.
HcFmNumber reads: 0x000023D6
The frame counter is running.
```

`HcRevision` reading &10 is OHCI 1.0. `HcRhDescriptorA` says four downstream
ports with per-port power switching. The frame counter moving is the cheapest
proof the controller is alive: it advances whether or not anything is driving it.
It goes on to print how the card's interrupt is wired, which nothing else shows
outside a debug build of `OHCIDriver`.

`*RPCEmuUSBDevices` reports what the root hub itself has on each port. That
sounds like ROOL's `*USBDevices` and deliberately is not: `*USBDevices` describes
devices properly, with class and manufacturer, but only once `USBDriver` has
enumerated them, whereas this reads the port registers directly and needs no USB
stack at all. A device present in one and missing from the other says which side
the fault is on.

Both are named `RPCEmuUSB...` rather than `USB...` so that nothing here can be
taken for part of RISC OS's USB stack, and so that a future ROOL command cannot
collide with one of ours. Where two modules offer the same command name, which
one answers depends on module order, and that is not a thing to leave to chance.

## The guest side

The card carries RISC OS's own USB stack in its ROM - `usbroms/10-usbdriver,ffa`
and `usbroms/20-ohcidriver,ffa`, both from RISC OS Open. A real device passed
through from the host enumerates:

```
*USBDevices
No. Bus Dev Class Description
  1   1   1  9/ 0 Built-in OHCI root hub
  2   1   2 EF/ 2 Azurewave USB2.0 HD IR UVC WebCam
```

Four things had to be true for that, and each was wrong to begin with.

**`OHCIDriver` had to be patched to find the card.** It normally asks the
machine's HAL what USB hardware exists - `HAL_USBControllerInfo`, HAL entry 108 -
and the IOMD HAL has no USB support at all; Castle never needed it on a Risc PC.
That entry point lives in the ROM, which RPCEmu does not build, and a module
cannot supply one: the kernel dispatches `OS_Hardware` to the HAL's own entry
table, not to anything registered later. The patch adds a third search after PCI
and the HAL - walk the expansion cards and read the controller's revision
register through each card's EASI space, so the card is found by what is in it
rather than by a slot number somebody wrote down.

The patch is kept in `riscos-patches/ohcidriver/`, against ROOL's OHCIDriver 0.56,
so it can be applied to a clean checkout of theirs and rebuilt rather than only
read about here. It touches one file, `c/ohcimodule`. The modification notice
Apache 2.0 asks for is in `usbroms/LICENCES.txt`.

**The interrupt had to be claimed with the card's own status byte.**
`OS_ClaimDeviceVector` wants the address of a byte the kernel can test and a mask
for the bit in it, and the kernel calls only the first claimant that matches - it
jumps rather than returns. The address is the podule manager's combined address
**rounded down to the 16K expansion card boundary**: the combined address is
where the card's ROM starts, not where the card starts. Claiming with the podule
manager's `IntRequest`/`IntValue` instead gets IOMD's shared registers, which
match every podule interrupt in the machine - that is what made `PHCIDriver`
starve every other card.

**`PCI_RAMAlloc` had to exist.** `OHCIDriver` gets bus-masterable memory through
PCI, for no reason to do with PCI, and there is no PCI on IOMD. `RPCEmuPCIEmulator`
supplies it - see `riscos-progs/RPCEmuPCIEmulator/`.

**USBDriver has to load before OHCIDriver.** See the note in `usbroms/README.txt`;
get it the wrong way round and the bus is registered twice and no port is ever
examined.

## Streaming: isochronous transfers

Audio and video devices do not move their data the way everything else does.
A keyboard is asked for its state and answers; a camera produces a packet every
millisecond whether anyone asked or not, and a packet that misses its millisecond
is of no use to anybody. USB gives that its own kind of transfer, isochronous,
with no handshake and no retry, and OHCI gives it its own kind of descriptor.

An isochronous descriptor is eight words rather than four, and it describes up to
eight consecutive frames rather than one transfer: a starting frame, a buffer
given as two pages, and a halfword per packet which is an offset going in and a
length and status coming back. The controller visits it once per frame, fills
that frame's packet, and only when its last frame has gone does the descriptor
join the done queue. A device that sent nothing in a frame produces a packet with
no bytes in it, which is a truthful answer and not a reason to wait: a camera
between frames does exactly that, every frame.

Two consequences are worth knowing, because both differ from every other transfer
here. An error does **not** halt the endpoint, unlike a general descriptor, since
a stream that stops at its first lost packet loses everything after it as well and
the driver is told which packet failed regardless. And a descriptor whose frames
have already gone by is retired immediately rather than run late, because those
frames cannot be replayed and a descriptor that is never returned is a driver
waiting for ever.

On the host side a stream cannot be run as a question and an answer either. Four
transfers of eight packets each are kept in flight and resubmitted the moment they
come back, and what they collect waits in a short queue for the guest to take a
packet at a time. When the queue fills, the oldest packets go: in a stream the
recent ones are the useful ones. That is logged, once.

One thing this needed elsewhere: an endpoint's maximum packet size is a property
of the **alternate setting** in force, not of the interface. A streaming device
typically asks for no bandwidth at all in setting 0 and offers a range above it,
so the endpoint map is now read from the setting the guest has selected. Reading
any other one gets every packet's size wrong.

### Streaming from a device

Nothing in RISC OS will open a webcam for you, but `USBDriver` exposes every
endpoint through DeviceFS, so a few lines of BASIC will read one. Note the
parameters are separated by **semicolons**: a comma is not a separator and gives a
bad-name error.

```basic
DIM b% 4096
d$="devices#endpoint1;interface1;alternate1:$.USB2"
SYS "XOS_Find",&4F,d$ TO f%;e%
IF (e% AND 1)=1 THEN PRINT "OPEN ERR &";~!f%:END
SYS "XOS_GBPB",4,f%,b%,1024 TO ,,,notdone%
PRINT 1024-notdone%;" bytes"
CLOSE#f%
```

`alternate1` matters. Without it the endpoint is matched in setting 0, where a
streaming device reserves no bandwidth and there is nothing to read. Which
settings a device offers, and what they cost, is in its own descriptors.

Reading a laptop's built-in camera this way returns its video payload, headers and
all - the first two bytes of each are the UVC header's length and flags. Around
90 KB/s of a 128-byte-per-frame setting reaches a BASIC program, which is most of
what that setting can carry.

## Where it is not finished

Not implemented:

- **Only one transfer in flight per endpoint and direction** for bulk and
  interrupt in the passthrough. Fine for control traffic; it would cap throughput
  on bulk, where a real driver double-buffers. Streams are not affected: those
  keep four transfers in flight, as described above.
- **Hubs cannot be passed through.** Taking one would take everything below it,
  and the root hub is the only hub in the emulated bus.
- **High speed devices.** The controller is full speed, so a high speed device is
  flagged in the dialogue; its maximum packet sizes will not be what a full speed
  driver expects.
