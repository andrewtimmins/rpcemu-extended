Module ROM images for the USB expansion card.

Everything in here is built into that card's ROM and started by RISC OS at boot,
the same way poduleroms/ works for the support card.

  10-usbdriver,ffa   RISC OS's USB stack, from RISC OS Open. HID keyboard and
                     mouse are compiled into it, so they come for free.
  20-ohcidriver,ffa  The host controller driver, from RISC OS Open, patched to
                     drive a controller on an expansion card.

Neither module is ours and neither is GPL, so RPCEmu's COPYING does not cover
them. Both are a mixture of Apache 2.0 and BSD, some of it the original four
clause BSD with its advertising acknowledgement. See LICENCES.txt here for the
notices these binaries oblige us to carry, and for the record of what we changed
in OHCIDriver.

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

The OHCIDriver patch is needed because the stock one finds controllers by asking
the machine's HAL what USB hardware exists, and HAL_IOMD has no USB support at
all - Castle never needed it on a Risc PC. A module cannot make up the
difference either: OS_Hardware can add a device, but HAL entries live in the
ROM. The patched driver searches the expansion cards instead, identifying the
right one by reading the controller's revision register through its EASI space.

The patch is kept in riscos-patches/ohcidriver/, against ROOL's OHCIDriver 0.56,
so it can be applied to a fresh checkout and rebuilt. See docs/usb.md for how
these are built.
