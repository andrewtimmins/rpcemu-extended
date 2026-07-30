@ RPCEmuUSBSupport - report the emulated USB host controller to the machine
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ RPCEmu's USB card carries an OHCI host controller in its EASI space (see
@ docs/usb.md). What drives it is RISC OS's own USB stack, carried in that card's
@ own ROM - USBDriver and a patched OHCIDriver, in usbroms/.
@
@ This module drives nothing. It is a diagnostic, and it earns its place by
@ answering the one question the USB stack cannot: is the card there at all, and
@ is the controller alive? ROOL's *USBDevices needs USBDriver loaded and working;
@ ours needs only the card. When the two disagree, that is the whole of the
@ fault localised in one command.
@
@   *RPCEmuUSBInfo     the card, the controller, and how its interrupt is wired
@   *RPCEmuUSBDevices  what the root hub itself reports on each of its ports
@
@ Both are named RPCEmuUSB... rather than USB... so that nothing here can be
@ taken for part of RISC OS's USB stack, and so that ROOL adding a command of
@ their own can never collide with one of ours. Two modules offering the same
@ command name is resolved by module order, and module order is not a thing to
@ leave to chance - loading OHCIDriver before USBDriver is what registered the
@ bus twice and cost a day.
@
@ It also prints how the card's interrupt is wired - the card's own base and the
@ byte at it, against IOMD's shared registers and the device vector. Nothing else
@ shows that outside a debug build of OHCIDriver, and getting it wrong is what
@ wedges a machine: OHCIDriver was handed the ROM address rather than the card's,
@ and this is the command that found it.
@
@ The card is located by what is in it rather than where it is: every expansion
@ card is asked for its EASI space and the one answering HcRevision with an OHCI
@ revision is the one, so it survives the card being moved.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_WriteC			= 0x20000
	XOS_Write0			= 0x20002
	XOS_NewLine			= 0x20003
	XOS_ConvertHex8			= 0x200d4
	XOS_ReadMonotonicTime		= 0x20042
	XOS_Module			= 0x2001e
	XPodule_ReadInfo		= 0x6028d
	XPodule_SetSpeed		= 0x6028e

	Module_Claim			= 6
	Module_Free			= 7

	@ Podule_ReadInfo reason bits
	ReadInfo_CombinedAddress	= 1 << 7
	ReadInfo_EASILogical		= 1 << 9
	ReadInfo_IntRequest		= 1 << 14
	ReadInfo_IntMask		= 1 << 15
	ReadInfo_IntValue		= 1 << 16
	ReadInfo_IntDeviceVector	= 1 << 17

	@ Everything *RPCEmuUSBInfo shows about the card's interrupt, in one call.
	INTINFO_ITEMS = ReadInfo_CombinedAddress | ReadInfo_IntRequest | ReadInfo_IntMask | ReadInfo_IntValue | ReadInfo_IntDeviceVector
	INTINFO_SIZE			= 20

	@ ...and where they land in the block, in that order
	INFO_EASI			= 0
	INFO_SIZE			= 16

	@ Podule_SetSpeed access type. Type C is the quick one.
	Podule_Speed_TypeC		= 3

	@ How many expansion cards there are to look through
	PODULE_COUNT			= 8

	@ The controller's registers, as offsets in the card's EASI space
	OHCI_OFFSET			= 0x800000

	HcRevision			= 0x00
	HcControl			= 0x04
	HcFmNumber			= 0x3c
	HcRhDescriptorA			= 0x48
	HcRhPortStatus1			= 0x54

	@ What HcRevision must say. Only the bottom byte is a promise: it is the
	@ BCD revision of the specification the controller implements.
	OHCI_REVISION_MASK		= 0xff
	OHCI_REVISION_10		= 0x10

	@ HcControl, functional state
	HCFS_MASK			= 0xc0
	HCFS_OPERATIONAL		= 0x80

	@ Root hub port status
	RH_CONNECTED			= 1 << 0
	RH_LOW_SPEED			= 1 << 9

	@ Workspace: the slot the card was found in, and its register base.
	WS_SLOT				= 0
	WS_BASE				= 4
	WORKSPACE_SIZE			= 8


	.global	_start
_start:

module_start:
	.int	0		@ Start (not runnable)
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	0		@ Service Call
	.int	title		@ Title String
	.int	help		@ Help String
	.int	table		@ Help and Command keyword table
	.int	0		@ SWI Chunk base
	.int	0		@ SWI handler code
	.int	0		@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32-bit compatible

title:
	.string	"RPCEmuUSBSupport"
	.align

	@ Defines "help", generated from VersionNum by the Makefile.
	.include "version.inc"

table:
	.string	"RPCEmuUSBInfo"
	.align
	.int	command_usbinfo
	.int	0x00000000		@ no parameters
	.int	command_usbinfo_syntax
	.int	command_usbinfo_help

	.string	"RPCEmuUSBDevices"
	.align
	.int	command_usbdevices
	.int	0x00000000		@ no parameters
	.int	command_usbdevices_syntax
	.int	command_usbdevices_help

	.byte	0	@ Table terminator
	.align

command_usbinfo_help:
	.string	"*RPCEmuUSBInfo reports RPCEmu's emulated USB host controller: which\n\rexpansion card carries it, where its registers are, whether anything has\n\rstarted it, and how its interrupt is wired.\n\r"
	.align

command_usbinfo_syntax:
	.string	"Syntax: *RPCEmuUSBInfo\n\r"
	.align

command_usbdevices_help:
	.string	"*RPCEmuUSBDevices lists what the emulated root hub reports on each of\n\rits ports, read straight from the controller. *USBDevices shows what\n\rRISC OS's USB stack has made of them, which is not the same question.\n\r"
	.align

command_usbdevices_syntax:
	.string	"Syntax: *RPCEmuUSBDevices\n\r"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

init:
	stmfd	sp!, {lr}
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {pc}		@ no workspace: refuse to initialise
	str	r2, [r12]
	mvn	r0, #0
	str	r0, [r2, #WS_SLOT]	@ not looked for yet
	mov	r0, #0
	str	r0, [r2, #WS_BASE]
	ldmfd	sp!, {pc}

final:
	stmfd	sp!, {lr}
	ldr	r12, [r12]
	mov	r0, #Module_Free
	mov	r2, r12
	swi	XOS_Module
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Finding the card
@
@ By what is in it rather than by where it is. Every expansion card is asked for
@ its EASI space, and the controller's revision register is read through it; the
@ card that answers with an OHCI revision is the one. A slot number would have
@ been shorter and would have broken the moment the card moved - which is exactly
@ what happened to the driver this card used to carry.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Returns r0 = slot, or -1 if there is no such card, and r1 = register base.
find_controller:
	stmfd	sp!, {r5, r6, lr}
	sub	sp, sp, #INFO_SIZE
	mov	r5, #0			@ slot being tried

find_next:
	ldr	r0, =ReadInfo_EASILogical
	mov	r1, sp
	mov	r2, #4
	mov	r3, r5
	swi	XPodule_ReadInfo
	bvs	find_skip

	ldr	r6, [sp, #INFO_EASI]
	teq	r6, #0
	beq	find_skip		@ no EASI space, so not our card

	add	r6, r6, #OHCI_OFFSET
	ldr	r0, [r6, #HcRevision]
	and	r0, r0, #OHCI_REVISION_MASK
	teq	r0, #OHCI_REVISION_10
	beq	find_found

find_skip:
	add	r5, r5, #1
	cmp	r5, #PODULE_COUNT
	blt	find_next

	mvn	r0, #0
	mov	r1, #0
	b	find_done

find_found:
	@ Ask for fast access, as a driver would. It changes speed, not
	@ correctness, so carry on if the podule manager will not give it.
	mov	r0, #Podule_Speed_TypeC
	mov	r3, r5
	swi	XPodule_SetSpeed
	mov	r0, r5
	mov	r1, r6

find_done:
	add	sp, sp, #INFO_SIZE
	ldmfd	sp!, {r5, r6, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Reporting
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Print the string at r0, then a newline. Corrupts r0-r3.
print_line:
	stmfd	sp!, {lr}
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {pc}

@ Print r0 as eight hex digits, with nothing around them.
print_hex8_raw:
	stmfd	sp!, {r0-r3, lr}
	adrl	r1, hexbuf
	mov	r2, #16
	swi	XOS_ConvertHex8
	swi	XOS_Write0
	ldmfd	sp!, {r0-r3, pc}

@ Print "<label at r4> 0x<r5 as eight hex digits>" and a newline.
print_hex8:
	stmfd	sp!, {r0-r3, lr}
	mov	r0, r4
	swi	XOS_Write0
	adrl	r0, str_hex_prefix
	swi	XOS_Write0
	mov	r0, r5
	bl	print_hex8_raw
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *USBInfo
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_usbinfo:
	stmfd	sp!, {r4-r11, lr}
	ldr	r10, [r12]

	adrl	r0, str_banner
	bl	print_line

	bl	find_controller
	cmp	r0, #0
	blt	usbinfo_none

	str	r0, [r10, #WS_SLOT]
	str	r1, [r10, #WS_BASE]
	mov	r11, r1			@ register base throughout

	mov	r5, r0
	adrl	r4, str_slot
	bl	print_hex8
	mov	r5, r11
	adrl	r4, str_registers
	bl	print_hex8

	@@@ What the controller says it is, and how big its root hub is.
	ldr	r5, [r11, #HcRevision]
	adrl	r4, str_revision
	bl	print_hex8

	ldr	r5, [r11, #HcRhDescriptorA]
	adrl	r4, str_rhdesca
	bl	print_hex8
	and	r5, r5, #0xff
	adrl	r4, str_ports
	bl	print_hex8

	@@@ Whether it has been started. A driver puts it into the operational
	@@@ state; until then it reads as reset, which is not a fault.
	ldr	r5, [r11, #HcControl]
	adrl	r4, str_control
	bl	print_hex8
	and	r0, r5, #HCFS_MASK
	teq	r0, #HCFS_OPERATIONAL
	adrl	r0, str_state_operational
	beq	usbinfo_say_state
	adrl	r0, str_state_stopped
usbinfo_say_state:
	bl	print_line

	@@@ The frame counter, which is the cheapest proof the controller is
	@@@ alive: it moves on its own whether or not anything is driving it.
	ldr	r6, [r11, #HcFmNumber]
	swi	XOS_ReadMonotonicTime
	add	r8, r0, #3		@ three centiseconds is thirty frames
usbinfo_wait:
	swi	XOS_ReadMonotonicTime
	cmp	r0, r8
	blt	usbinfo_wait
	ldr	r5, [r11, #HcFmNumber]
	adrl	r4, str_frame
	bl	print_hex8
	teq	r5, r6
	adrl	r0, str_frame_running
	bne	usbinfo_say_frame
	adrl	r0, str_frame_stopped
usbinfo_say_frame:
	bl	print_line

	@@@ How the card's interrupt is wired. A driver claims the podule vector
	@@@ with these, and getting them wrong is what wedges a machine, so being
	@@@ able to see them is worth the few lines.
	@@@
	@@@ The words come back in ascending order of the bits asked for, so
	@@@ this block is: combined address, IntRequest, IntMask, IntValue,
	@@@ device vector.
	@@@
	@@@ The combined address is the card's own base, and the byte there is
	@@@ the card's own interrupt status - the thing a driver must hand
	@@@ OS_ClaimDeviceVector. IntRequest and IntValue are IOMD's shared
	@@@ registers, which is the trap PHCIDriver fell into.
	sub	sp, sp, #INTINFO_SIZE
	ldr	r0, =INTINFO_ITEMS
	mov	r1, sp
	mov	r2, #INTINFO_SIZE
	ldr	r3, [r10, #WS_SLOT]
	swi	XPodule_ReadInfo
	bvs	usbinfo_no_int

	ldr	r5, [sp, #0]
	adrl	r4, str_card_base
	bl	print_hex8
	ldr	r0, [sp, #0]
	teq	r0, #0
	beq	usbinfo_no_status
	ldrb	r5, [r0]
	adrl	r4, str_card_status
	bl	print_hex8

	@@@ The combined address is where the card's ROM starts, which is not
	@@@ the same as where the card starts: each expansion card gets 16K of
	@@@ I/O space and the interrupt status byte is at the bottom of it.
	ldr	r0, [sp, #0]
	bic	r0, r0, #0x3f00
	bic	r0, r0, #0x00ff
	mov	r5, r0
	adrl	r4, str_slot_base
	bl	print_hex8
	ldrb	r5, [r0]
	adrl	r4, str_slot_status
	bl	print_hex8
usbinfo_no_status:

	ldr	r5, [sp, #4]
	adrl	r4, str_int_request
	bl	print_hex8
	ldr	r5, [sp, #8]
	adrl	r4, str_int_mask
	bl	print_hex8
	ldr	r5, [sp, #12]
	adrl	r4, str_int_value
	bl	print_hex8
	ldr	r5, [sp, #16]
	adrl	r4, str_int_devno
	bl	print_hex8
usbinfo_no_int:
	add	sp, sp, #INTINFO_SIZE

	adrl	r0, str_note
	bl	print_line
	b	usbinfo_exit

usbinfo_none:
	adrl	r0, str_no_card
	bl	print_line

usbinfo_exit:
	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0			@ V clear: no error
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *RPCEmuUSBDevices
@
@ What the root hub says is on each port, read from the controller rather than
@ from the USB stack. That is the whole point of it being separate: *USBDevices
@ describes devices properly, with class and manufacturer, but only once
@ USBDriver has enumerated them. This says what the hardware presents, so a port
@ listed here and absent there puts the fault in the guest, and the other way
@ round puts it in the emulation.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_usbdevices:
	stmfd	sp!, {r4-r11, lr}
	ldr	r10, [r12]

	bl	find_controller
	cmp	r0, #0
	blt	usbdev_none

	str	r0, [r10, #WS_SLOT]
	str	r1, [r10, #WS_BASE]
	mov	r11, r1			@ register base throughout

	adrl	r0, str_ports_header
	bl	print_line

	ldr	r7, [r11, #HcRhDescriptorA]
	and	r7, r7, #0xff		@ how many ports there are
	mov	r9, #0

usbdev_port:
	add	r0, r9, #1
	add	r0, r0, #'0'
	swi	XOS_WriteC
	adrl	r0, str_pad4
	swi	XOS_Write0

	add	r0, r11, #HcRhPortStatus1
	ldr	r8, [r0, r9, lsl #2]

	mov	r0, r8
	bl	print_hex8_raw
	adrl	r0, str_pad2
	swi	XOS_Write0

	tst	r8, #RH_CONNECTED
	adrl	r0, str_nothing
	beq	usbdev_port_say
	tst	r8, #RH_LOW_SPEED
	adrl	r0, str_low
	bne	usbdev_port_say
	adrl	r0, str_full
usbdev_port_say:
	bl	print_line

	add	r9, r9, #1
	cmp	r9, r7
	blt	usbdev_port

	adrl	r0, str_dev_note
	bl	print_line
	b	usbdev_exit

usbdev_none:
	adrl	r0, str_no_card
	bl	print_line

usbdev_exit:
	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0			@ V clear: no error
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Strings
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

str_banner:
	.string	"RPCEmuUSBInfo: the emulated USB host controller"
	.align
str_hex_prefix:
	.string	"0x"
	.align
str_slot:
	.string	"Expansion card carrying it: "
	.align
str_registers:
	.string	"Controller registers at: "
	.align
str_revision:
	.string	"HcRevision reads: "
	.align
str_rhdesca:
	.string	"HcRhDescriptorA reads: "
	.align
str_ports:
	.string	"Root hub downstream ports: "
	.align
str_control:
	.string	"HcControl reads: "
	.align
str_frame:
	.string	"HcFmNumber reads: "
	.align
str_card_base:
	.string	"Card base (its own interrupt status): "
	.align
str_card_status:
	.string	"Byte there reads: "
	.align
str_slot_base:
	.string	"Slot base (16K boundary): "
	.align
str_slot_status:
	.string	"Byte there reads: "
	.align
str_int_request:
	.string	"IOMD shared IntRequest: "
	.align
str_int_mask:
	.string	"IOMD IntMask register: "
	.align
str_int_value:
	.string	"IOMD IntMask bit: "
	.align
str_int_devno:
	.string	"Interrupt device vector: "
	.align

str_state_operational:
	.string	"The controller is running, so something is driving it."
	.align
str_state_stopped:
	.string	"The controller is not started; no driver has taken it up yet."
	.align
str_frame_running:
	.string	"The frame counter is running."
	.align
str_frame_stopped:
	.string	"The frame counter is NOT running, which it should be."
	.align

str_ports_header:
	.string	"What the root hub itself reports:\n\r\n\rPort  Status      What is in it"
	.align
str_pad4:
	.string	"    "
	.align
str_pad2:
	.string	"  "
	.align
str_nothing:
	.string	"nothing connected"
	.align
str_full:
	.string	"a full speed device"
	.align
str_low:
	.string	"a low speed device"
	.align

str_note:
	.string	"The card's own ROM carries USBDriver and a patched OHCIDriver, which\n\ris what drives this. *RPCEmuUSBDevices shows the root hub's own view of\n\rits ports, and *USBDevices what the stack has made of them."
	.align

str_dev_note:
	.string	"This is the controller's own account of its ports. *USBDevices is what\n\rRISC OS's USB stack has made of them: a device listed here and missing\n\rthere puts the fault in the guest rather than in the emulation."
	.align

str_no_card:
	.string	"No expansion card has an OHCI controller in its EASI space."
	.align

hexbuf:
	.space	16
	.align

	.end
