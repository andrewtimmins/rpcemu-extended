Module ROM images for the USB expansion card.

Everything in here is built into that card's ROM and started by RISC OS at boot,
the same way poduleroms/ works for the support card.

  10-usbdriver,ffa    RISC OS's USB stack, from RISC OS Open. HID keyboard and
                      mouse are compiled into it, so they come for free.
  20-ohcidriver,ffa   The host controller driver, from RISC OS Open, patched to
                      drive a controller on an expansion card.
  30-rtsupport,ffa    RISC OS's thread scheduler. Not in the Risc PC ROM and
                      only on the hard disc image as a System module, so it has
                      to be here: SCSISoftUSB runs its transfers on it.
  40-scsidriver,ffa   SCSISwitch, which provides the SCSI_* SWIs and hands them
                      to whichever back end claims a device. It registers itself
                      as "SCSIDriver", which is what it replaces.
  50-scsisoftusb,ffa  SCSI over USB: the mass storage transport, so a USB drive
                      answers SCSI commands.
  60-scsifs,ffa       The filing system that presents a SCSI disc, as SCSI::0.

None of these is ours and none is GPL, so RPCEmu's COPYING does not cover them.
They are a mixture of Apache 2.0 and BSD, some of it the original four clause
BSD with its advertising acknowledgement. See LICENCES.txt here for the notices
these binaries oblige us to carry, and for the record of what we changed in
OHCIDriver - which is the only one of the six we changed at all.

★ The numbers matter. Files load in name order, and USBDriver has to be first.

OHCIDriver registers its bus from two places - a callback it queues at the end
of its own initialisation, and its Service_USB_USBDriverStarting handler - and
only the second checks whether that has already happened. On the machines it was
written for, OHCIDriver starts after USBDriver is already resident, so the
service call never fires and only one registration happens. Start it first and
both fire: the bus is registered twice, and the explore callback can no longer
match the bus it is handed against the list of live ones. It gives up with "bus
has been removed", so no port is ever examined and nothing is ever found.

Named plainly, "ohcidriver" sorts before "usbdriver" and that is exactly what
happened.

The mass storage four have an order of their own, for a duller reason: each needs
the one before it to exist when it initialises. RTSupport before SCSISoftUSB,
which runs its transfers on it; SCSISwitch before SCSISoftUSB, which registers
with it; and SCSIFS last, since it is looking for a SCSI driver to sit on.

★ They must be in this ROM, not loaded afterwards. Loading them by hand once the
machine is up leaves *SCSIDevices empty even with a drive plugged in, because
USBDriver announces a device as it enumerates it and there is nobody listening
yet - nothing goes looking for devices that arrived before it did. In the card's
ROM they are resident before enumeration, which is the same order a real machine
with these modules on its podule would boot in.

★ SCSISwitch, not SCSIDriver. There is a component in ROOL's tree literally
called SCSIDriver, and it is the wrong one: its default build drives a WD33C93
chip directly, so on a machine that has not got one it sits in a loop poking
absent hardware and the machine never finishes the *RMLoad. The "soft" flag in
its makefile means "be a back end for SCSISwitch" rather than "be a software
implementation", which is easy to read the wrong way round.

The OHCIDriver patch is needed because the stock one finds controllers by asking
the machine's HAL what USB hardware exists, and HAL_IOMD has no USB support at
all - Castle never needed it on a Risc PC. A module cannot make up the
difference either: OS_Hardware can add a device, but HAL entries live in the
ROM. The patched driver searches the expansion cards instead, identifying the
right one by reading the controller's revision register through its EASI space.

The patch is kept in riscos-patches/ohcidriver/, against ROOL's OHCIDriver 0.56,
so it can be applied to a fresh checkout and rebuilt. See docs/usb.md for how
these are built.
