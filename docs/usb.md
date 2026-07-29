# USB

RPCEmu can present a USB host controller to the guest, so that a machine which
never had USB can be given it. This describes the card, because the emulation has
to match what a real driver expects rather than whatever is convenient.

The card is deliberately **compatible with the driver RISC OS Open publish**,
`PHCIDriver` in their `HWSupport/USB/Controllers` group.

## How this was worked out

The same way the [Kinetic support](kinetic.md) was. RISC OS Open's driver source
was read as a **reference for how the hardware behaves**, and the emulation was
then written from scratch. None of their code is here, and none of it can be: it
is Apache 2.0 licensed and RPCEmu is GPL v2, so copying it in would not be
permissible even if it were desirable. What was taken is knowledge of what the
chip does, which is the same thing anyone writing a driver from a datasheet takes.

The publicly documented behaviour of the Simtec podule and the STD Unipod, which
use the same chip, filled in the rest.

## What is being emulated

A Philips (now NXP) **ISP1161** USB host and device controller. It matters that
it is this chip and not a PCI OHCI controller: OHCI expects to bus-master its
descriptor lists straight out of main memory, and **the Risc PC podule bus has no
bus mastering**. The ISP1161 was made for exactly that situation. It keeps its
transfer descriptors and data in its own on-chip buffer RAM, and the host
processor moves them in and out through a data port. So there is no DMA engine to
emulate, which is the single thing that makes this tractable.

The chip is presented as part of the RPCEmu support card in **expansion slot 0**,
the same card that carries the module ROM. That is not arbitrary: `PHCIDriver`
addresses slot 0 and nothing else, so anything in another slot would need the
driver changed. The module ROM occupies the bottom of the card's EASI space and
the controller sits well above it.

## The register window

Four ports, in the card's EASI space at offset **&800000**, one word apart:

| Offset | Port |
| --- | --- |
| &800000 | Host controller data |
| &800004 | Host controller command |
| &800008 | Device controller data |
| &80000c | Device controller command |

Only the bottom 16 bits of each word carry data. Everything works on a
command-then-data basis:

1. Write the register number to the command port. **Bit 7 set means a write**,
   clear means a read. So `HcChipID` (&27) is read by writing &27, and
   `HcScratch` (&28) is written by writing &a8.
2. Move the data through the data port, 16 bits at a time.

A 32-bit register is therefore **two consecutive accesses to the data port**,
low half first. A driver reading `HcInterruptStatus` writes &03 to the command
port and then reads the data port twice. This is why the emulation carries a
half-phase alongside the latched command, rather than treating each data port
access independently.

The two buffer ports work the same way but stream: `HcTransferCounter` is set to
the byte count first, then a command of `HcATLBufferPort` (&41, or &c1 to write)
opens the buffer and each data port access moves the next halfword.

## Registers

The OHCI-derived registers keep their OHCI meanings and are 32-bit:

| Number | Register |
| --- | --- |
| &00 | `HcRevision` |
| &01 | `HcControl` |
| &02 | `HcCommandStatus` |
| &03 | `HcInterruptStatus` |
| &04 | `HcInterruptEnable` |
| &05 | `HcInterruptDisable` |
| &0d - &0f | `HcFmInterval`, `HcFmRemaining`, `HcFmNumber` |
| &11 | `HcLSThreshold` |
| &12 - &13 | `HcRhDescriptorA`, `HcRhDescriptorB` |
| &14 | `HcRhStatus` |
| &15 - &16 | `HcRhPortStatus1`, `HcRhPortStatus2` |

The chip's own registers are 16-bit:

| Number | Register |
| --- | --- |
| &20 | `HcHardwareConfiguration` |
| &21 | `HcDMAConfiguration` |
| &22 | `HcTransferCounter` |
| &24 - &25 | `HcuPInterrupt`, `HcuPInterruptEnable` |
| &27 | `HcChipID` |
| &28 | `HcScratch` |
| &29 | `HcSoftwareReset` |
| &2a - &2b | `HcITLBufferLength`, `HcATLBufferLength` |
| &2c | `HcBufferStatus` |
| &2d - &2e | `HcReadBackITL0Length`, `HcReadBackITL1Length` |
| &40 - &41 | `HcITLBufferPort`, `HcATLBufferPort` |

`HcChipID` reads **&6110**. A driver is entitled to check it, and `PHCIDriver`
does: it takes the top byte and insists on &61, refusing to start otherwise. It
is the first thing worth testing, because it proves the command and data ports
are wired up correctly and nothing else has to work for it to answer.

`HcuPInterrupt` is where the chip reports what happened, and it is
write-to-clear: a driver acknowledges by writing back the bits it read. The card's
interrupt line follows whatever remains set and enabled, so acknowledging in the
chip is what lowers the expansion card interrupt.

## Buffer memory

The chip has 4 KB of buffer RAM, divided between the isochronous list (ITL, two
alternating halves) and the acknowledged transfer list (ATL) by writing
`HcITLBufferLength` and `HcATLBufferLength`. A driver that only wants ATL gives
the whole 4 KB to it.

## Transfer descriptors

A transfer is described by an 8-byte **PTD** followed by its payload, padded to a
whole number of words, and the ATL holds as many of those as fit. As four
little-endian halfwords:

| Word | Fields |
| --- | --- |
| 0 | `ActualBytes` 0-9, `Toggle` 10, `Active` 11, `CompletionCode` 12-15 |
| 1 | `MaxPktSize` 0-9, `Speed` 10, `Last` 11, `EndpointNumber` 12-15 |
| 2 | `TotalBytes` 0-9, `DirToken` 10-11 (0 SETUP, 1 OUT, 2 IN) |
| 3 | `FunctionAddress` 0-6 |

That is from the data sheet, and it is worth saying why: the one hand-built
descriptor in ROOL's driver has **two comments that disagree with its own bytes**,
describing an 8-byte IN transfer as "max packet 16" and "16 bytes, out". The bytes
match the data sheet exactly. Anyone tempted to learn the layout from that example
should read the numbers, not the comments.

The driver fills the buffer, the controller performs each transfer and writes back
`ActualBytes` and a `CompletionCode` with `Active` cleared, and `ATLInt` in
`HcuPInterrupt` says the list has been dealt with. A descriptor that a device has
nothing to say to, which is the normal state of an idle input device, is left
active with a `NotAccessed` code so the driver leaves it in the list and asks
again.

The list ends at whichever comes first: a descriptor marked `Last`, the end of what
the driver handed over, or the end of buffer memory. Trusting `Last` alone would
leave a runaway one bad byte away.

## What can be plugged in

*Settings → USB...* lists the two ports and what is in each, the way
VirtualBox and VMware do it: a device is bound to the emulated bus deliberately.
Attaching or detaching drives the root hub's connect status change, so the guest
sees a plug going in at the moment you say so, and enumerates it in its own time.

The choice is per machine and lives in its configuration as `usb_port1` and
`usb_port2`, so a machine started again tomorrow has the same devices and the
dialogue is only a way of editing it. Applying goes through the emulator's command
queue rather than being poked straight into the controller, because the controller
belongs to the emulator thread.

What can be plugged in is the real devices plugged into this computer. There was
a synthesised gamepad here for a while, which is what proved the transfer engine
before there was any way to attach something real; it has been removed, because a
device whose answers the emulator already knows cannot tell you whether the
emulation is right.

## Real devices

A device on the host's own USB bus is handed to the guest through
[libusb](https://libusb.info/). The emulated controller is not told that it is
real: a device is three questions - describe yourself, do this control request,
have you anything on this endpoint - and passthrough answers them by putting the
same question to real hardware.

Handing a device over is not a neutral act. The host's own driver is detached
from it for as long as the guest has it, which is what makes it answer the guest
instead of the host, and it is given back when the device is detached or RPCEmu
closes. A device the host is using is marked as such in the dialogue and takes a
confirmation, because doing this to the mouse in your hand would leave you
without a pointer to undo it with.

The choice is remembered as the device's **identifiers rather than its position**
- `usb_port1=host:046d:c077` - so unplugging it and putting it in a different
socket does not lose it. If two identical devices are plugged in, the first found
is the one taken.

### Getting permission

On Linux the device nodes under `/dev/bus/usb` belong to root, so RPCEmu cannot
open one until the system is told to allow it. That is a matter for the host and
not something the emulator can arrange for itself. A rule naming the one device
you want to pass through is the least you can get away with:

```
# /etc/udev/rules.d/70-rpcemu-usb.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="046d", ATTRS{idProduct}=="c077", TAG+="uaccess"
```

then `sudo udevadm control --reload-rules && sudo udevadm trigger`, and unplug
and replug the device. `TAG+="uaccess"` gives access to whoever is logged in at
the machine rather than to everyone with an account on it, which is why it is
preferred here to adding a group. Dropping the two `ATTRS` clauses would grant
the same for every USB device, which is convenient and worth thinking about
before doing.

Without the rule the dialogue still lists the device, marked "no permission",
and says what to do rather than failing when you try to use it.

### Timing, and why it works at all

A real control transfer takes milliseconds and the emulator thread cannot wait
that long. Nothing in the passthrough ever waits: a transfer is submitted, the
descriptor is left active with a `NotAccessed` code, and the answer is collected
whenever a later frame finds it has arrived.

That is not a workaround. It is precisely what a device does when it has nothing
ready - it NAKs, and the host asks again - so a driver written for real hardware
already copes with it, and no allowance for the emulator is needed anywhere in
the guest. It is also why the ordinary idle case costs nothing: an input device
with nothing to report and a transfer still in flight look identical.

Completions are noticed once per USB frame, on the emulator thread, so there is
no locking anywhere in the passthrough and no second thread to go wrong.

### What will not work

- **Isochronous transfers are not implemented at all**, so audio and video
  devices are out. A webcam will describe itself perfectly and then have nothing
  to say, which is the honest result rather than a bug.
- **The controller is full speed**, as the real ISP1161 is. A high speed device
  is marked in the dialogue: it may work, since almost everything falls back,
  but its maximum packet sizes will not be what a full speed driver expects.
- **A hub cannot be passed through.** Hubs are left out of the list: taking one
  would take everything below it, and the guest's own root hub is the only hub
  in the emulated bus.
- **There is still no USB driver on the guest** to make use of any of it. What
  passthrough gives today is a real device to develop that driver against, which
  is the whole reason for preferring it to something made up.

## Where it is not finished

- **Isochronous transfers are not implemented.** The ITL exists as memory and the
  registers behave, but no descriptors are executed from it, so no audio or video
  class device would work.
- **The device (peripheral) side of the chip is not implemented.** Its ports
  answer, and its chip ID reads as zero so a driver probing it stops rather than
  going further on a false positive.
- **There is still no USB stack on the guest.** Not in the RISC OS 5 IOMD ROM, not
  on HardDisc4, and ROOL do not build `PHCIDriver` for IOMD, so nothing in a stock
  guest will use any of this. The card is complete enough to develop a driver
  against, and `RPCEmuUSBSupport` is how that is checked in the meantime.

## Seeing what is plugged in

`*USBDevices` in the guest asks the devices themselves rather than asking the
emulator, and prints what they say:

```
Port  Speed  ID         Class              Name
1     full   045E:0810  EF miscellaneous   Microsoft® LifeCam HD-3000
2     nothing connected
```

Every column but the port number and the speed comes back over the bus. The
identifiers and the class come from the device descriptor, the name from a
string descriptor fetched separately, and a device that declares its class as
zero is asked for its configuration so that the first interface can supply one
instead. That is the same sequence a real host performs before it decides which
driver to load, which is the point of doing it this way: a listing built from
what the emulator already knows would tell you nothing about whether a driver
could have got the same answer.

Each port is reset first, and the other one is disabled while it is being asked.
Both are necessary rather than tidy. A device answers only on an enabled port and
comes back from a reset on address 0, which is the one address that can be asked
about without having enumerated anything - but two enabled ports holding
unaddressed devices would then both answer, and the wrong one could be the one
heard. A host that enumerated properly would hand out addresses instead; this
only looks, so it turns the other port off. It does mean this is not a command to
run underneath a driver that is using the ports, and the help text says so.

A descriptor that comes back marked `NotAccessed` is handed over again rather
than reported as a failure. That is what makes the same code work against a
passed-through device, which always answers "not yet" the first time.

## Testing it

`*USBTest` in the guest reports the card, the chip ID, and the result of writing
and reading back registers and buffer memory. That is the harness the emulation
was built against, and it is the quickest way to see whether a change broke
something.

A working card looks like this:

```
USBTest: checking the emulated USB host controller
Card EASI space at 0xF1600000
HcChipID reads 0x6110
Chip identifies as an ISP1161: pass
HcScratch holds what was written: pass
HcRevision reads 0x00000110
32-bit register reads as two halves, low first: pass
HcFmInterval reads 0x00002EDF
HcRhDescriptorA reads 0x00000902
Root hub downstream ports: 0x0002
HcRhPortStatus1 reads 0x00000000
HcFmNumber reads 0x00001148
Frame counter is running: pass
Buffer memory streams and holds its contents: pass
USBTest: finished
```

Three of those numbers are worth knowing by sight. `HcRevision` must read
`0x00000110` and not `0x01100000` or `0x01100110`: the first is the halves
swapped and the second is a half-phase that never advanced, and both are the sort
of wrong answer that looks plausible. `HcFmInterval` is 11999, the OHCI frame
interval. `HcRhDescriptorA` is two downstream ports with per-port power switching
and per-port over-current reporting.

The frame counter test waits on `OS_ReadMonotonicTime` rather than a cycle count,
because a frame is a millisecond of real time and how many instructions fit in one
depends entirely on the host.

With something attached, it goes on to do a real transfer: it resets the port,
asks the device to describe itself, and reports what came back.

```
A device is connected to port 1; asking it to describe itself
HcRhPortStatus1 after reset reads 0x00120103
Port enables after a reset: pass
Bytes transferred: 0x0012
Device descriptor bLength: 0x0012
Device descriptor bDescriptorType: 0x0001
Device descriptor idVendor: 0x045E
Device answered with a device descriptor: pass
```

18 bytes is the length of a device descriptor, and `0x00120103` is connected,
enabled and powered with the reset change bit set. That is a complete control
transfer - SETUP, then IN, then the status stage - executed from RISC OS against
a webcam plugged into the host, and it is the shortest way to see whether
passthrough is working.
