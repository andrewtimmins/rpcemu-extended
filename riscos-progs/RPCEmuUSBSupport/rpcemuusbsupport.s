@ RPCEmuUSBSupport - talk to the emulated USB host controller and report what it finds
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ RPCEmu presents a Philips ISP1161 USB host controller in the support card's
@ EASI space (see docs/usb.md). There is no USB stack in the RISC OS 5
@ IOMD ROM and none on HardDisc4, so nothing in a stock guest will touch it. This
@ module is the harness that makes the emulation testable in the meantime: it
@ talks to the hardware directly, exercises the registers a real driver relies on,
@ and says what happened.
@
@ *USBTest deliberately proves the things that are easy to get wrong and
@ invisible from a single access:
@
@   - the command/data protocol, by reading HcChipID
@   - that a register really remembers, by writing HcScratch and reading it back
@   - that a 32-bit register is two halfwords low-half-first, by reading
@     HcRevision and HcFmInterval whose values are known
@   - that the frame counter is running, by reading HcFmNumber twice
@   - that buffer memory streams and holds its contents, by writing a pattern
@     through the ATL buffer port and reading it back
@
@ *USBDevices then does the thing all of that is for: it holds a conversation
@ with whatever is plugged in, asking each device what it is and what it calls
@ itself, and prints a table of the answers.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_WriteC			= 0x20000
	XOS_Write0			= 0x20002
	XOS_NewLine			= 0x20003
	XOS_ConvertHex2			= 0x200d1
	XOS_ConvertHex4			= 0x200d2
	XOS_ConvertHex8			= 0x200d4
	XOS_ConvertCardinal4		= 0x200d8
	XOS_ReadMonotonicTime		= 0x20042
	XPodule_ReadInfo		= 0x6028d
	XPodule_SetSpeed		= 0x6028e

	@ Podule_ReadInfo reason bits, and the podule the card is in. PHCIDriver
	@ addresses slot 0 and nothing else, so that is where the card has to be.
	ReadInfo_EASILogical		= 1 << 9
	ReadInfo_IntRequest		= 1 << 14
	ReadInfo_IntMask		= 1 << 15
	PODULE_NUMBER			= 0

	@ Podule_SetSpeed access type. Type C is what a driver asks for to get at
	@ the chip quickly, so ask for the same thing here.
	Podule_Speed_TypeC		= 3

	@ The controller's ports, as offsets in the card's EASI space
	ISP_OFFSET			= 0x800000
	HC_DATA				= 0x00
	HC_COMMAND			= 0x04

	@ Bit 7 of a command means "a write follows"
	COMMAND_WRITE			= 0x80

	@ Registers used here
	HcRevision			= 0x00
	HcControl			= 0x01
	HcFmInterval			= 0x0d
	HcFmNumber			= 0x0f
	HcRhDescriptorA			= 0x12
	HcRhPortStatus1			= 0x15
	HcRhPortStatus2			= 0x16
	HcTransferCounter		= 0x22
	HcuPInterrupt			= 0x24
	HcChipID			= 0x27
	HcScratch			= 0x28
	HcITLBufferLength		= 0x2a
	HcATLBufferLength		= 0x2b
	HcATLBufferPort			= 0x41

	@ What the chip should say it is. Only the top byte is a promise: a later
	@ revision may change the bottom, and a driver that insisted on the whole
	@ word would refuse to work with hardware that was fine.
	CHIP_ID_TOP			= 0x6100

	@ The pattern pushed through buffer memory, and how much of it
	TEST_PATTERN			= 0xa5
	TEST_BYTES			= 64

	@ Workspace. The read-back needs writable memory, and this module lives in a
	@ podule ROM, so there is nowhere else to put it.
	@
	@ WS_LIST is where a control transfer is built before it is streamed into
	@ the chip and read back afterwards: the request, the descriptors that carry
	@ it, and room for the answer.
	WS_BASE				= 0
	WS_BUFFER			= 8
	WS_LIST				= 72
	WORKSPACE_SIZE			= WS_LIST + LIST_SIZE

	@ Root hub port status bits, and the requests a write makes
	RH_CONNECTED			= 1 << 0
	RH_ENABLED			= 1 << 1
	RH_CLEAR_ENABLE			= 1 << 0
	RH_SET_RESET			= 1 << 4
	RH_SET_POWER			= 1 << 8
	RH_LOW_SPEED			= 1 << 9

	@ The change bits, which a driver acknowledges by writing them back
	RH_CHANGES			= 0x00130000

	@ How many downstream ports the chip has
	ROOT_HUB_PORTS			= 2

	@ HcuPInterrupt sources
	uPInt_ATL			= 1 << 1

	@ Completion codes a descriptor comes back with. NotAccessed is not a
	@ failure: it is the device saying "not yet", and the answer is to ask
	@ again rather than to give up.
	CC_NO_ERROR			= 0x0
	CC_NOT_ACCESSED			= 0xf

	@ How long a device is given to answer, in centiseconds. Generous, because
	@ a passed-through device is real hardware on the other side of the host's
	@ own USB stack and a control transfer there takes milliseconds.
	CONTROL_TIMEOUT			= 50
	ATL_TIMEOUT			= 10

	@ A control read, as three descriptors in the list control_in builds: the
	@ request, the data coming back, and a zero length status stage. The most
	@ any request here asks for is 255 bytes, which is as much as the length
	@ byte of a string descriptor can describe.
	CTRL_MAX			= 255
	LIST_SETUP_PTD			= 0
	LIST_SETUP			= 8
	LIST_IN_PTD			= 16
	LIST_DATA			= 24
	LIST_SIZE			= LIST_DATA + 256 + 8

	@ Standard requests, as the two bytes that start a request
	REQ_GET_DESCRIPTOR		= 0x0680	@ device to host, GET_DESCRIPTOR

	@ Descriptor types, in the top half of wValue
	DESC_DEVICE			= 0x0100
	DESC_CONFIG			= 0x0200
	DESC_STRING			= 0x0300

	@ Where things are in a device descriptor
	DD_DEVICE_CLASS			= 4
	DD_VENDOR			= 8
	DD_PRODUCT			= 10
	DD_IPRODUCT			= 15

	@ Every descriptor starts with its length and its type, which is how a
	@ configuration is walked looking for the interface inside it
	DESC_LENGTH_AT			= 0
	DESC_TYPE_AT			= 1
	DESC_TYPE_INTERFACE		= 4
	ID_CLASS			= 5	@ bInterfaceClass

	@ The one language these devices speak
	LANGUAGE_ENGLISH		= 0x0409

	@ Column widths of the table *USBDevices prints
	COL_SPEED			= 7
	COL_CLASS			= 16

	@ Podule_ReadInfo fills these in, in this order, for the bits asked for
	INFO_EASI			= 0
	INFO_INTREQUEST			= 4
	INFO_INTMASK			= 8
	INFO_SIZE			= 12


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

	@ Defines "help", the *Help and *Modules string, generated from VersionNum
	@ by the Makefile so the version and date live in one place.
	.include "version.inc"

	@ Help and Command keyword table
table:
	.string	"USBDevices"
	.align
	.int	command_usbdevices
	.int	0x00000000		@ no parameters
	.int	command_usbdevices_syntax
	.int	command_usbdevices_help

	.string	"USBTest"
	.align
	.int	command_usbtest
	.int	0x00000000		@ no parameters
	.int	command_usbtest_syntax
	.int	command_usbtest_help

	.byte	0	@ Table terminator
	.align

command_usbdevices_help:
	.string	"*USBDevices lists what is plugged into the emulated USB ports, asking each\n\rdevice to describe itself. It resets the ports to do so, so do not run it\n\runderneath a driver that is using them.\n\r"
	.align

command_usbdevices_syntax:
	.string	"Syntax: *USBDevices\n\r"
	.align

command_usbtest_help:
	.string	"*USBTest checks the emulated USB host controller and reports what it finds.\n\r"
	.align

command_usbtest_syntax:
	.string	"Syntax: *USBTest\n\r"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	Module_Claim	= 6
	Module_Free	= 7
	XOS_Module	= 0x2001e

init:
	stmfd	sp!, {lr}
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {pc}		@ no workspace: refuse to initialise
	str	r2, [r12]		@ private word points at the workspace
	mov	r0, #0
	str	r0, [r2, #WS_BASE]	@ base is looked up when first needed
	ldmfd	sp!, {pc}

final:
	stmfd	sp!, {lr}
	ldr	r12, [r12]
	mov	r0, #Module_Free
	mov	r2, r12
	swi	XOS_Module
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Reaching the chip
@
@ Every access is a command then data. Bit 7 of the command says a write
@ follows, and a 32-bit register is two data accesses, low half first. Get the
@ halves the wrong way round and the value still looks plausible, which is why
@ the tests below use registers whose contents are known in advance.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	@ r11 = port base throughout, as the driver does it

@ Read a 16-bit register. r0 = register number, returns r0 = value.
read16:
	bic	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	ldr	r0, [r11, #HC_DATA]
	mov	r0, r0, lsl #16
	mov	r0, r0, lsr #16
	mov	pc, lr

@ Write a 16-bit register. r0 = register number, r1 = value.
write16:
	orr	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	str	r1, [r11, #HC_DATA]
	mov	pc, lr

@ Read a 32-bit register. r0 = register number, returns r0 = value.
read32:
	stmfd	sp!, {r1, lr}
	bic	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	ldr	r0, [r11, #HC_DATA]
	mov	r0, r0, lsl #16
	mov	r0, r0, lsr #16		@ low half
	ldr	r1, [r11, #HC_DATA]	@ high half
	orr	r0, r0, r1, lsl #16
	ldmfd	sp!, {r1, pc}

@ Write a 32-bit register. r0 = register number, r1 = value.
write32:
	stmfd	sp!, {r1, lr}
	orr	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	str	r1, [r11, #HC_DATA]	@ low half
	mov	r1, r1, lsr #16
	str	r1, [r11, #HC_DATA]	@ high half
	ldmfd	sp!, {r1, pc}


@ Find the card and leave r11 pointing at the controller's ports.
@
@ Returns r0 = the card's EASI base, or zero with r1 pointing at a string saying
@ why not. Corrupts r0-r3.
find_card:
	stmfd	sp!, {lr}
	sub	sp, sp, #INFO_SIZE

	@ Ask the podule manager rather than assuming: the logical address of EASI
	@ space is not something to hard-code.
	ldr	r0, =(ReadInfo_EASILogical | ReadInfo_IntRequest | ReadInfo_IntMask)
	mov	r1, sp
	mov	r2, #INFO_SIZE
	mov	r3, #PODULE_NUMBER
	swi	XPodule_ReadInfo
	bvs	find_card_none

	ldr	r0, [sp, #INFO_EASI]
	teq	r0, #0
	beq	find_card_no_easi

	add	r11, r0, #ISP_OFFSET

	@ Ask for fast access, as a driver would, and carry on regardless if the
	@ podule manager will not give it: it changes speed, not correctness.
	mov	r0, #Podule_Speed_TypeC
	mov	r3, #PODULE_NUMBER
	swi	XPodule_SetSpeed
	sub	r0, r11, #ISP_OFFSET
	b	find_card_done

find_card_none:
	adrl	r1, str_no_podule
	mov	r0, #0
	b	find_card_done

find_card_no_easi:
	adrl	r1, str_no_easi
	mov	r0, #0

find_card_done:
	add	sp, sp, #INFO_SIZE
	ldmfd	sp!, {pc}

	@ The constants each routine loads live just past it. This module is now
	@ long enough that one pool at the end would be out of reach of the code
	@ that needs it.
	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ One control transfer
@
@ A control read is three descriptors, not one: the eight byte request, the data
@ coming back, and a zero length status stage that says the host has it. They go
@ into the list together, because the controller runs the whole list the moment
@ the last halfword of it arrives, and the answer is read back out of the same
@ place.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Ask the device on address 0 to hand something over.
@
@ Entry: r0 = bmRequestType and bRequest, as the two bytes begin the request
@        r1 = wValue, r2 = wIndex, r3 = wLength (at most CTRL_MAX)
@ Exit:  r0 = bytes transferred and r1 -> them, or r0 = -1 if it did not answer.
@
@ What r1 points at stays valid only until the next transfer, which builds its
@ list over the top.
control_in:
	stmfd	sp!, {r4-r9, lr}
	mov	r4, r3			@ how much was asked for
	add	r5, r10, #WS_LIST

	@ The request itself, which is the same for every attempt below. Reading
	@ the list back does not disturb it: the controller returns the eight bytes
	@ of it unaltered, and only the descriptors come back changed.
	orr	r6, r0, r1, lsl #16	@ bmRequestType, bRequest, wValue
	str	r6, [r5, #LIST_SETUP]
	orr	r6, r2, r4, lsl #16	@ wIndex, wLength
	str	r6, [r5, #LIST_SETUP + 4]

	@ All of buffer memory to the ATL, since nothing here is isochronous
	mov	r0, #HcITLBufferLength
	mov	r1, #0
	bl	write16
	mov	r0, #HcATLBufferLength
	ldr	r1, =0x1000
	bl	write16

	@ How long the whole thing is allowed to take, retries included
	swi	XOS_ReadMonotonicTime
	add	r9, r0, #CONTROL_TIMEOUT

	@ Each attempt builds the descriptors again from nothing. It has to: the
	@ controller writes its answers into them and clears the active bit on the
	@ ones it dealt with, so handing back what came out would re-run only the
	@ part that had not finished, without the request that gives it meaning.
control_in_attempt:
	@ The request goes out as a SETUP to endpoint 0 of address 0, eight bytes
	@ with a maximum packet size of eight. Descriptor words are packed as the
	@ chip wants them: two little-endian halfwords to a word.
	ldr	r6, =0x00080800		@ active, max packet 8, endpoint 0
	str	r6, [r5, #LIST_SETUP_PTD]
	mov	r6, #8			@ eight bytes, SETUP, address 0
	str	r6, [r5, #LIST_SETUP_PTD + 4]

	@ Then the data stage, which is where the answer lands
	ldr	r6, =0x00080800
	str	r6, [r5, #LIST_IN_PTD]
	orr	r6, r4, #0x800		@ wLength bytes, IN
	str	r6, [r5, #LIST_IN_PTD + 4]

	@ Clear the answer first, so a device that says less than it was asked for
	@ cannot leave the tail of the last one behind looking like data.
	add	r7, r4, #3
	bic	r7, r7, #3		@ the payload is padded to a word
	add	r8, r5, #LIST_DATA
	add	r6, r8, r7
control_in_clear:
	cmp	r8, r6
	movlo	r2, #0
	strlo	r2, [r8], #4
	blo	control_in_clear

	@ And the status stage, which ends the list
	add	r8, r7, #LIST_DATA
	ldr	r6, =0x08080800		@ active, max packet 8, endpoint 0, last
	str	r6, [r5, r8]
	mov	r6, #0x400		@ no bytes, OUT
	add	r2, r8, #4
	str	r6, [r5, r2]
	add	r7, r8, #8		@ the list, in bytes

	@ A stale acknowledgement from the transfer before would be taken for this
	@ one finishing, so put it down before starting.
	mov	r0, #HcuPInterrupt
	mov	r1, #uPInt_ATL
	bl	write16

	mov	r0, #HcTransferCounter
	mov	r1, r7
	bl	write16

	mov	r0, #HcATLBufferPort
	orr	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	mov	r2, r5
	mov	r6, r7
control_in_fill:
	ldr	r1, [r2], #4
	str	r1, [r11, #HC_DATA]	@ low half
	mov	r1, r1, lsr #16
	str	r1, [r11, #HC_DATA]	@ high half
	subs	r6, r6, #4
	bhi	control_in_fill

	@ Wait for the controller to say it has dealt with the list
	swi	XOS_ReadMonotonicTime
	add	r8, r0, #ATL_TIMEOUT
control_in_wait:
	mov	r0, #HcuPInterrupt
	bl	read16
	tst	r0, #uPInt_ATL
	bne	control_in_ready
	swi	XOS_ReadMonotonicTime
	cmp	r0, r8
	blt	control_in_wait
	b	control_in_failed

control_in_ready:
	mov	r0, #HcTransferCounter
	mov	r1, r7
	bl	write16

	mov	r0, #HcATLBufferPort
	bic	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	mov	r2, r5
	mov	r6, r7
control_in_read:
	ldr	r1, [r11, #HC_DATA]
	strb	r1, [r2], #1
	mov	r1, r1, lsr #8
	strb	r1, [r2], #1
	subs	r6, r6, #2
	bhi	control_in_read

	@ How the data stage ended: completion code in the top four bits of its
	@ first word, bytes actually transferred in the bottom ten.
	ldrb	r0, [r5, #LIST_IN_PTD]
	ldrb	r1, [r5, #LIST_IN_PTD + 1]
	orr	r0, r0, r1, lsl #8
	movs	r1, r0, lsr #12
	beq	control_in_answered

	@ NotAccessed means the device had nothing ready and the descriptor was
	@ left for another go. That is not a failure and must not be treated as
	@ one: a device passed through from the host answers this way every time,
	@ because a real transfer takes longer than the list took to run. Hand the
	@ same list over again until it has been dealt with or time runs out.
	teq	r1, #CC_NOT_ACCESSED
	bne	control_in_failed
	swi	XOS_ReadMonotonicTime
	cmp	r0, r9
	blt	control_in_attempt
	b	control_in_failed

control_in_answered:
	mov	r0, r0, lsl #22
	mov	r0, r0, lsr #22
	add	r1, r5, #LIST_DATA
	ldmfd	sp!, {r4-r9, pc}

control_in_failed:
	mvn	r0, #0
	ldmfd	sp!, {r4-r9, pc}

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

@ Print "<label at r4> 0x<r5 as four hex digits>" and a newline.
print_hex4:
	stmfd	sp!, {r0-r3, lr}
	mov	r0, r4
	swi	XOS_Write0
	adrl	r0, str_hex_prefix
	swi	XOS_Write0
	mov	r0, r5
	bl	print_hex4_raw
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, pc}

@ Print "<label at r4> 0x<r5 as eight hex digits>" and a newline.
print_hex8:
	stmfd	sp!, {r0-r3, lr}
	mov	r0, r4
	swi	XOS_Write0
	adrl	r0, str_hex_prefix
	swi	XOS_Write0
	mov	r0, r5
	adrl	r1, hexbuf
	mov	r2, #16
	swi	XOS_ConvertHex8
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, pc}

@ Print the string at r0, then spaces until r1 characters have gone by.
@
@ A name longer than its column still gets printed in full and a single space
@ after it: a table that runs its columns together is easier to read than one
@ that silently drops the end of a name.
print_padded:
	stmfd	sp!, {r0-r4, lr}
	mov	r4, r1
	mov	r3, r0
print_padded_char:
	ldrb	r0, [r3], #1
	teq	r0, #0
	beq	print_padded_fill
	swi	XOS_WriteC
	subs	r4, r4, #1
	movmi	r4, #0
	b	print_padded_char
print_padded_fill:
	teq	r4, #0
	moveq	r4, #1
print_padded_space:
	mov	r0, #' '
	swi	XOS_WriteC
	subs	r4, r4, #1
	bhi	print_padded_space
	ldmfd	sp!, {r0-r4, pc}

@ Print r0 as two hex digits, with nothing around them.
print_hex2_raw:
	stmfd	sp!, {r0-r3, lr}
	adrl	r1, hexbuf
	mov	r2, #16
	swi	XOS_ConvertHex2
	swi	XOS_Write0
	ldmfd	sp!, {r0-r3, pc}

@ Print r0 as four hex digits, with nothing around them.
print_hex4_raw:
	stmfd	sp!, {r0-r3, lr}
	adrl	r1, hexbuf
	mov	r2, #16
	swi	XOS_ConvertHex4
	swi	XOS_Write0
	ldmfd	sp!, {r0-r3, pc}

@ Name a USB class code: r0 = the code, returns r0 -> what it is called.
@
@ The table holds distances from itself rather than addresses. A module is
@ relocated to wherever it is loaded, so an address assembled in would be an
@ offset from zero and point at whatever happens to be there.
class_name:
	stmfd	sp!, {r1-r4, lr}
	adrl	r4, class_table
	mov	r1, r4
class_name_next:
	ldr	r2, [r1], #4
	ldr	r3, [r1], #4
	cmn	r2, #1			@ the last entry names everything else
	beq	class_name_found
	teq	r2, r0
	bne	class_name_next
class_name_found:
	add	r0, r4, r3
	ldmfd	sp!, {r1-r4, pc}

@ Print a pass or fail line: r4 -> what was tested, r5 = 0 for pass.
print_result:
	stmfd	sp!, {r0-r3, lr}
	mov	r0, r4
	swi	XOS_Write0
	teq	r5, #0
	bne	print_result_fail
	adrl	r0, str_pass
	b	print_result_say
print_result_fail:
	adrl	r0, str_fail
print_result_say:
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *USBDevices
@
@ What is plugged in, asked of the devices themselves rather than of the
@ emulator: each port is reset and the device on it is made to describe itself,
@ which is the same conversation a real host has and so exercises the same code.
@
@ Resetting is not politeness, it is the only way in: a device only answers on
@ an enabled port, and it comes back from a reset on address 0, which is the
@ address this can ask about without having enumerated anything. It does mean
@ this is not a command to run underneath a driver that is using the ports.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_usbdevices:
	stmfd	sp!, {r4-r11, lr}
	ldr	r10, [r12]		@ our workspace, the only writable memory we have

	bl	find_card
	teq	r0, #0
	beq	devices_no_card

	@ Say so rather than printing an empty table if this is not the card we
	@ think it is: every reading below would otherwise be meaningless.
	mov	r0, #HcChipID
	bl	read16
	mov	r0, r0, lsr #8
	teq	r0, #(CHIP_ID_TOP >> 8)
	bne	devices_wrong_chip

	adrl	r0, str_devices_header
	bl	print_line

	mov	r9, #0			@ the port being looked at, counting from 0

port_loop:
	add	r0, r9, #1
	add	r0, r0, #'0'
	swi	XOS_WriteC
	adrl	r0, str_pad5
	swi	XOS_Write0

	add	r0, r9, #HcRhPortStatus1
	bl	read32
	mov	r8, r0
	tst	r8, #RH_CONNECTED
	beq	port_empty

	@ One port at a time. A device that has not been given an address answers
	@ on address 0, so two enabled ports with unaddressed devices in them both
	@ answer the same question and the wrong one can be the one heard. A host
	@ that enumerated properly would hand out addresses; this only looks, so it
	@ turns the other port off instead.
	mov	r0, #HcRhPortStatus1
	mov	r1, #RH_CLEAR_ENABLE
	bl	write32
	mov	r0, #HcRhPortStatus2
	mov	r1, #RH_CLEAR_ENABLE
	bl	write32

	add	r0, r9, #HcRhPortStatus1
	mov	r1, #RH_SET_POWER
	orr	r1, r1, #RH_SET_RESET
	bl	write32

	add	r0, r9, #HcRhPortStatus1
	bl	read32
	mov	r8, r0
	tst	r8, #RH_ENABLED
	beq	port_stuck

	@ Speed, which the root hub reports from the connection itself and not
	@ from anything the device has been asked
	adrl	r0, str_speed_full
	tst	r8, #RH_LOW_SPEED
	beq	port_speed_known
	adrl	r0, str_speed_low
port_speed_known:
	mov	r1, #COL_SPEED
	bl	print_padded

	@ Now make it describe itself
	ldr	r0, =REQ_GET_DESCRIPTOR
	ldr	r1, =DESC_DEVICE
	mov	r2, #0
	mov	r3, #18			@ the length of a device descriptor
	bl	control_in
	cmp	r0, #18
	blt	port_silent

	mov	r2, r1
	ldrb	r6, [r2, #DD_VENDOR]
	ldrb	r0, [r2, #DD_VENDOR + 1]
	orr	r6, r6, r0, lsl #8
	ldrb	r0, [r2, #DD_PRODUCT]
	ldrb	r1, [r2, #DD_PRODUCT + 1]
	orr	r0, r0, r1, lsl #8
	orr	r6, r0, r6, lsl #16	@ vendor above, product below
	ldrb	r7, [r2, #DD_DEVICE_CLASS]
	ldrb	r0, [r2, #DD_IPRODUCT]
	orr	r7, r7, r0, lsl #8	@ class, and which string names it

	mov	r0, r6, lsr #16
	bl	print_hex4_raw
	mov	r0, #':'
	swi	XOS_WriteC
	mov	r0, r6
	bl	print_hex4_raw
	adrl	r0, str_pad2
	swi	XOS_Write0

	@ A device class of zero means the class is a property of each interface
	@ rather than of the device, so the answer is in the configuration.
	ands	r4, r7, #0xff
	bne	port_class_known

	ldr	r0, =REQ_GET_DESCRIPTOR
	ldr	r1, =DESC_CONFIG
	mov	r2, #0
	mov	r3, #64			@ enough for a configuration worth reading
	bl	control_in
	cmp	r0, #0
	blt	port_class_known

	mov	r5, #0
port_class_walk:
	add	r2, r5, #ID_CLASS + 1
	cmp	r2, r0
	bhi	port_class_known	@ too little left to be an interface
	add	r2, r1, r5
	ldrb	r3, [r2, #DESC_LENGTH_AT]
	teq	r3, #0
	beq	port_class_known	@ a zero length would never end
	ldrb	r2, [r2, #DESC_TYPE_AT]
	teq	r2, #DESC_TYPE_INTERFACE
	beq	port_class_interface
	add	r5, r5, r3
	b	port_class_walk
port_class_interface:
	add	r2, r1, r5
	ldrb	r4, [r2, #ID_CLASS]

port_class_known:
	mov	r0, r4
	bl	print_hex2_raw
	mov	r0, #' '
	swi	XOS_WriteC
	mov	r0, r4
	bl	class_name
	mov	r1, #COL_CLASS
	bl	print_padded

	@ And what it calls itself, if it calls itself anything
	movs	r1, r7, lsr #8
	beq	port_unnamed

	orr	r1, r1, #DESC_STRING
	ldr	r0, =REQ_GET_DESCRIPTOR
	ldr	r2, =LANGUAGE_ENGLISH
	mov	r3, #CTRL_MAX
	bl	control_in
	cmp	r0, #4			@ two bytes of header and one character
	blt	port_unnamed

	@ USB strings are UTF-16LE. These are all ASCII, so every second byte is
	@ the character and the one after it is a zero.
	mov	r4, r0
	mov	r5, r1
	mov	r2, #2
port_name_char:
	cmp	r2, r4
	bhs	port_name_done
	ldrb	r0, [r5, r2]
	teq	r0, #0
	beq	port_name_done
	swi	XOS_WriteC
	add	r2, r2, #2
	b	port_name_char
port_name_done:
	swi	XOS_NewLine
	b	port_next

port_unnamed:
	adrl	r0, str_unnamed
	bl	print_line
	b	port_next

port_empty:
	adrl	r0, str_nothing
	bl	print_line
	add	r9, r9, #1
	cmp	r9, #ROOT_HUB_PORTS
	blt	port_loop
	b	devices_done

port_stuck:
	adrl	r0, str_port_stuck
	bl	print_line
	b	port_next

port_silent:
	adrl	r0, str_port_silent
	bl	print_line

port_next:
	@ Acknowledge the changes the reset made, so a driver loaded afterwards is
	@ not told about a plugging in that happened before it existed.
	add	r0, r9, #HcRhPortStatus1
	ldr	r1, =RH_CHANGES
	bl	write32

	add	r9, r9, #1
	cmp	r9, #ROOT_HUB_PORTS
	blt	port_loop

devices_done:
	@ And leave the chip's interrupt state as we found it
	mov	r0, #HcuPInterrupt
	ldr	r1, =0x77
	bl	write16

	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0			@ V clear: no error
	mov	pc, lr

devices_no_card:
	mov	r0, r1
	bl	print_line
	b	devices_exit

devices_wrong_chip:
	adrl	r0, str_wrong_chip
	bl	print_line

devices_exit:
	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *USBTest
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_usbtest:
	stmfd	sp!, {r4-r11, lr}
	ldr	r10, [r12]		@ our workspace, the only writable memory we have

	adrl	r0, str_banner
	bl	print_line

	bl	find_card
	teq	r0, #0
	beq	no_card

	adrl	r4, str_easi_base
	mov	r5, r0
	bl	print_hex8

	@@@ 1. The chip identifies itself. This proves the command and data
	@@@ ports are wired up and nothing else has to work for it to answer.
	mov	r0, #HcChipID
	bl	read16
	mov	r5, r0
	adrl	r4, str_chip_id
	bl	print_hex4

	mov	r0, r5
	mov	r0, r0, lsr #8
	mov	r0, r0, lsl #8		@ top byte only
	ldr	r1, =CHIP_ID_TOP
	subs	r5, r0, r1		@ zero if it is an ISP1161
	adrl	r4, str_test_chip
	bl	print_result
	bne	wrong_chip

	@@@ 2. A register that remembers. HcScratch exists for exactly this, so
	@@@ write something no reset value would be and read it back.
	mov	r0, #HcScratch
	ldr	r1, =0xbeef
	bl	write16
	mov	r0, #HcScratch
	bl	read16
	ldr	r1, =0xbeef
	subs	r5, r0, r1
	adrl	r4, str_test_scratch
	bl	print_result

	@@@ 3. A 32-bit register really is two halves, low first. HcRevision
	@@@ reads 0x00000110: if the halves were swapped it would read
	@@@ 0x01100000, and if the phase never advanced, 0x01100110.
	mov	r0, #HcRevision
	bl	read32
	mov	r5, r0
	adrl	r4, str_revision
	bl	print_hex8
	ldr	r1, =0x110
	subs	r5, r0, r1
	adrl	r4, str_test_halves
	bl	print_result

	@@@ 4. And the same for a value that has both halves populated.
	@@@ HcFmInterval starts at 11999, which is 0x2edf, so a swap shows up.
	mov	r0, #HcFmInterval
	bl	read32
	mov	r5, r0
	adrl	r4, str_fminterval
	bl	print_hex8

	@@@ 5. The root hub. Two ports, nothing plugged into either yet.
	mov	r0, #HcRhDescriptorA
	bl	read32
	mov	r5, r0
	adrl	r4, str_rhdesca
	bl	print_hex8
	and	r5, r0, #0xff
	adrl	r4, str_ports
	bl	print_hex4

	mov	r0, #HcRhPortStatus1
	bl	read32
	mov	r5, r0
	adrl	r4, str_port1
	bl	print_hex8

	@@@ 6. The controller is alive: the frame counter moves on its own.
	mov	r0, #HcFmNumber
	bl	read32
	mov	r6, r0			@ first reading

	@ Wait on the clock, not on a cycle count: a frame is a millisecond of
	@ real time, and how many instructions fit in one depends on the host.
	@ Three centiseconds is thirty frames, so this cannot miss by being
	@ marginally too quick.
	swi	XOS_ReadMonotonicTime
	add	r8, r0, #3
frame_wait:
	swi	XOS_ReadMonotonicTime
	cmp	r0, r8
	blt	frame_wait

	mov	r0, #HcFmNumber
	bl	read32
	mov	r5, r0
	adrl	r4, str_frame
	bl	print_hex8
	subs	r5, r0, r6
	moveq	r5, #1			@ unchanged is a failure
	movne	r5, #0
	adrl	r4, str_test_frame
	bl	print_result

	@@@ 7. Buffer memory. Give the whole 4K to the ATL, stream a pattern in
	@@@ through the buffer port, then read it back the same way. This is the
	@@@ path a real driver hands transfer descriptors over on, so it is the
	@@@ part most worth proving before there is a driver to prove it with.
	mov	r0, #HcITLBufferLength
	mov	r1, #0
	bl	write16
	mov	r0, #HcATLBufferLength
	ldr	r1, =0x1000
	bl	write16

	mov	r0, #HcTransferCounter
	mov	r1, #TEST_BYTES
	bl	write16

	mov	r0, #HcATLBufferPort
	orr	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	mov	r6, #TEST_BYTES
	mov	r7, #TEST_PATTERN
buffer_fill:
	orr	r1, r7, r7, lsl #8	@ the same byte in both halves
	str	r1, [r11, #HC_DATA]
	add	r7, r7, #1
	and	r7, r7, #0xff
	subs	r6, r6, #2
	bhi	buffer_fill

	mov	r0, #HcTransferCounter
	mov	r1, #TEST_BYTES
	bl	write16

	mov	r0, #HcATLBufferPort
	bic	r0, r0, #COMMAND_WRITE
	str	r0, [r11, #HC_COMMAND]
	mov	r6, #TEST_BYTES
	mov	r7, #TEST_PATTERN
	mov	r8, #0			@ mismatch count
buffer_check:
	ldr	r1, [r11, #HC_DATA]
	mov	r1, r1, lsl #16
	mov	r1, r1, lsr #16
	orr	r2, r7, r7, lsl #8
	teq	r1, r2
	addne	r8, r8, #1
	add	r7, r7, #1
	and	r7, r7, #0xff
	subs	r6, r6, #2
	bhi	buffer_check

	mov	r5, r8
	adrl	r4, str_test_buffer
	bl	print_result

	@@@ 8. A real transfer, if there is something to talk to. This is the whole
	@@@ point of the exercise: hand the controller a pair of transfer
	@@@ descriptors asking a device to describe itself, and see whether the
	@@@ answer comes back.
	mov	r0, #HcRhPortStatus1
	bl	read32
	tst	r0, #RH_CONNECTED
	beq	no_device

	adrl	r0, str_device_found
	bl	print_line

	@ Reset the port, which is what enables it and puts the device back on
	@ address 0. A driver does this before it will talk to anything.
	mov	r0, #HcRhPortStatus1
	mov	r1, #RH_SET_POWER
	orr	r1, r1, #RH_SET_RESET
	bl	write32

	mov	r0, #HcRhPortStatus1
	bl	read32
	mov	r5, r0
	adrl	r4, str_port1_after
	bl	print_hex8
	tst	r5, #RH_ENABLED
	moveq	r5, #1
	movne	r5, #0
	adrl	r4, str_test_enable
	bl	print_result
	bne	enum_done

	@ Ask it to describe itself, through the same routine *USBDevices uses.
	@ That matters for more than tidiness: a device passed through from the
	@ host answers "not yet" the first time every time, and a one-shot transfer
	@ would report a working device as a failure.
	ldr	r0, =REQ_GET_DESCRIPTOR
	ldr	r1, =DESC_DEVICE
	mov	r2, #0
	mov	r3, #18
	bl	control_in
	cmp	r0, #0
	blt	enum_no_answer

	mov	r3, r1			@ what came back
	mov	r5, r0
	adrl	r4, str_actual
	bl	print_hex4

	@ A device descriptor is 18 bytes and says so in its first byte, with its
	@ type second.
	ldrb	r5, [r3, #0]
	adrl	r4, str_dd_length
	bl	print_hex4
	ldrb	r5, [r3, #1]
	adrl	r4, str_dd_type
	bl	print_hex4
	ldrb	r5, [r3, #DD_VENDOR]
	ldrb	r0, [r3, #DD_VENDOR + 1]
	orr	r5, r5, r0, lsl #8
	adrl	r4, str_dd_vendor
	bl	print_hex4

	ldrb	r0, [r3, #0]
	ldrb	r1, [r3, #1]
	teq	r0, #18
	teqeq	r1, #1
	moveq	r5, #0
	movne	r5, #1
	adrl	r4, str_test_enum
	bl	print_result
	b	enum_done

enum_no_answer:
	adrl	r0, str_no_answer
	bl	print_line
	b	enum_done

no_device:
	adrl	r0, str_no_device
	bl	print_line

enum_done:
	@@@ Done. Leave the chip's interrupt state as we found it, so a driver
	@@@ loaded afterwards is not surprised by a stale interrupt.
	mov	r0, #HcuPInterrupt
	ldr	r1, =0x77
	bl	write16

	adrl	r0, str_done
	bl	print_line

	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0			@ V clear: no error
	mov	pc, lr

no_card:
	mov	r0, r1			@ find_card said why
	bl	print_line
	b	command_exit

wrong_chip:
	adrl	r0, str_wrong_chip
	bl	print_line

command_exit:
	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0
	mov	pc, lr


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Strings
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

str_banner:
	.string	"USBTest: checking the emulated USB host controller"
	.align
str_easi_base:
	.string	"Card EASI space at "
	.align
str_chip_id:
	.string	"HcChipID reads "
	.align
str_revision:
	.string	"HcRevision reads "
	.align
str_fminterval:
	.string	"HcFmInterval reads "
	.align
str_rhdesca:
	.string	"HcRhDescriptorA reads "
	.align
str_ports:
	.string	"Root hub downstream ports: "
	.align
str_port1:
	.string	"HcRhPortStatus1 reads "
	.align
str_frame:
	.string	"HcFmNumber reads "
	.align
str_hex_prefix:
	.string	"0x"
	.align

str_test_chip:
	.string	"Chip identifies as an ISP1161: "
	.align
str_test_scratch:
	.string	"HcScratch holds what was written: "
	.align
str_test_halves:
	.string	"32-bit register reads as two halves, low first: "
	.align
str_test_frame:
	.string	"Frame counter is running: "
	.align
str_test_buffer:
	.string	"Buffer memory streams and holds its contents: "
	.align

str_pass:
	.string	"pass"
	.align
str_fail:
	.string	"FAIL"
	.align

str_device_found:
	.string	"A device is connected to port 1; asking it to describe itself"
	.align
str_port1_after:
	.string	"HcRhPortStatus1 after reset reads "
	.align
str_actual:
	.string	"Bytes transferred: "
	.align
str_dd_length:
	.string	"Device descriptor bLength: "
	.align
str_dd_type:
	.string	"Device descriptor bDescriptorType: "
	.align
str_dd_vendor:
	.string	"Device descriptor idVendor: "
	.align
str_test_enable:
	.string	"Port enables after a reset: "
	.align
str_test_enum:
	.string	"Device answered with a device descriptor: "
	.align
str_no_answer:
	.string	"The device never answered, so there is nothing to report about it"
	.align
str_no_device:
	.string	"Nothing is connected to port 1, so there is no transfer to try"
	.align

str_done:
	.string	"USBTest: finished"
	.align

str_no_podule:
	.string	"Podule 0 could not be read, so there is no card to talk to"
	.align
str_no_easi:
	.string	"Podule 0 has no EASI space, so the controller cannot be reached"
	.align
str_wrong_chip:
	.string	"That is not an ISP1161, so there is nothing here worth asking"
	.align


@ --- *USBDevices ---------------------------------------------------------- @

	@ The columns are Port, Speed, ID, Class and Name, and the widths here
	@ have to agree with what the rows below print.
str_devices_header:
	.string	"Port  Speed  ID         Class              Name"
	.align
str_pad5:
	.string	"     "
	.align
str_pad2:
	.string	"  "
	.align
str_speed_full:
	.string	"full"
	.align
str_speed_low:
	.string	"low"
	.align
str_nothing:
	.string	"nothing connected"
	.align
str_unnamed:
	.string	"unnamed"
	.align
str_port_stuck:
	.string	"something is connected, but the port will not enable"
	.align
str_port_silent:
	.string	"connected, but it did not describe itself when asked"
	.align

	@ What the class codes mean, in the words the USB device class list uses,
	@ shortened where the full name would not earn its width. The last entry
	@ has no code and names everything the rest did not.
	.align
class_table:
	.int	0x00, str_class_interface - class_table
	.int	0x01, str_class_audio - class_table
	.int	0x02, str_class_comms - class_table
	.int	0x03, str_class_hid - class_table
	.int	0x05, str_class_physical - class_table
	.int	0x06, str_class_image - class_table
	.int	0x07, str_class_printer - class_table
	.int	0x08, str_class_storage - class_table
	.int	0x09, str_class_hub - class_table
	.int	0x0a, str_class_cdcdata - class_table
	.int	0x0b, str_class_smartcard - class_table
	.int	0x0d, str_class_security - class_table
	.int	0x0e, str_class_video - class_table
	.int	0x0f, str_class_health - class_table
	.int	0xdc, str_class_diagnostic - class_table
	.int	0xe0, str_class_wireless - class_table
	.int	0xef, str_class_misc - class_table
	.int	0xfe, str_class_application - class_table
	.int	0xff, str_class_vendor - class_table
	.int	-1,   str_class_unknown - class_table

str_class_interface:
	.string	"per interface"
	.align
str_class_audio:
	.string	"audio"
	.align
str_class_comms:
	.string	"communications"
	.align
str_class_hid:
	.string	"human interface"
	.align
str_class_physical:
	.string	"physical"
	.align
str_class_image:
	.string	"image"
	.align
str_class_printer:
	.string	"printer"
	.align
str_class_storage:
	.string	"mass storage"
	.align
str_class_hub:
	.string	"hub"
	.align
str_class_cdcdata:
	.string	"CDC data"
	.align
str_class_smartcard:
	.string	"smart card"
	.align
str_class_security:
	.string	"content security"
	.align
str_class_video:
	.string	"video"
	.align
str_class_health:
	.string	"healthcare"
	.align
str_class_diagnostic:
	.string	"diagnostic"
	.align
str_class_wireless:
	.string	"wireless"
	.align
str_class_misc:
	.string	"miscellaneous"
	.align
str_class_application:
	.string	"application specific"
	.align
str_class_vendor:
	.string	"vendor specific"
	.align
str_class_unknown:
	.string	"unknown"
	.align

hexbuf:
	.space	16
	.align

	.end
