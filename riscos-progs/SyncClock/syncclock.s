@ SyncClock - keep the RISC OS soft clock in step with the emulated RTC
@
@ Copyright (C) 2002 DEEJ Technology PLC
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ A translation of DEEJ Technology's SyncClock 0.11, which was written as a BBC
@ BASIC assembler program. Theirs is the design and the code; this is the same
@ instructions in GNU as syntax so it builds with the rest of the guest modules
@ and can be carried in the expansion ROM instead of having to be assembled
@ inside the guest.
@
@ Behaviour is deliberately unchanged, and it was checked rather than assumed:
@ assembled from this file, all 420 bytes of code and the module header are
@ byte-identical to the module built from their BASIC source. The only
@ differences in the whole 748-byte module are the date in the help string,
@ their uninitialised ALIGN padding, and one message string that they left
@ unaligned. Their originals are in git history from the initial commit,
@ 2032532.
@
@ Wanted for saving and restoring machine state: a state resumed later comes back
@ with the clock it was saved with. Nick Brown raised it as an outstanding item on
@ that work, which is where the idea of bringing this module in came from.
@
@ Why it is wanted: RISC OS reads the real-time clock once, at boot, and then
@ keeps its own soft copy ticking. The emulator's RTC (cmos.c, a PCF8583) answers
@ from the host clock every time it is read, so the two diverge whenever the host
@ clock moves without the guest running - across a suspend, a snapshot resumed
@ later, or a host that simply drifts. A ticker re-reads the chip every ten
@ seconds and sets the soft clock from it.
@
@ 32-bit compatible, and assembled to the ARMv3 floor like the other modules so
@ nothing appears that an ARM610/710 cannot run.

	.arch	armv3

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_Word			= 0x20007
	XOS_Module			= 0x2001e
	XOS_ReadUnsigned		= 0x20021
	XOS_CallEvery			= 0x2003c
	XOS_RemoveTickerEvent		= 0x2003d
	XIIC_Control			= 0x20240
	XTerritory_SetTime		= 0x63047
	XTerritory_ConvertTimeToUTCOrdinals = 0x63049
	XTerritory_ConvertOrdinalsToTime = 0x63051

	@ OS_Module reason codes
	Module_Claim	= 6
	Module_Free	= 7

	@ OS_Word 14, sub-reason 3: read the soft copy of the clock as UTC.
	OSWord_ReadClock	= 14
	ReadClock_UTC		= 3

	@ The PCF8583 sits at IIC address 0xA0. Bit 0 selects the direction, so
	@ 0xA0 addresses it for writing (to set the register pointer) and 0xA1 for
	@ reading. Registers 1 to 6 are centiseconds, seconds, minutes, hours, day
	@ and month, all BCD, with the year's low two bits in the top of the day
	@ byte and the weekday in the top of the month byte.
	CMOS_CLOCK	= 0xa0

	@ Ten seconds, in centiseconds, as OS_CallEvery counts them.
	INTERVAL	= 10 * 100

	@ Module workspace: just the current interval, so *SyncClock can be told a
	@ new one and the ticker restarted with it.
	WS_INTERVAL	= 0
	WORKSPACE_SIZE	= 4

	@ Blocks built on the stack by the ticker. Offsets and total match the
	@ original exactly: a 6-byte IIC buffer, the 5-byte UTC time, and the 9-word
	@ territory ordinals block, each word aligned.
	IBLOCK		= 0
	OBLOCK		= ((IBLOCK + 6) + 3) & ~3
	TBLOCK		= ((OBLOCK + 5) + 3) & ~3
	BLOCK_SIZE	= TBLOCK + 0x24

	@ Ordinals block, as Territory_ConvertTimeToUTCOrdinals fills it in
	ORD_CS		= 0
	ORD_SECOND	= 4
	ORD_MINUTE	= 8
	ORD_HOUR	= 12
	ORD_DAY		= 16
	ORD_MONTH	= 20
	ORD_YEAR	= 24


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
	.string	"SyncClock"
	.align

	@ Defines "help", the *Help and *Modules string, generated from VersionNum
	@ by the Makefile so the version and date live in one place.
	.include "version.inc"

	@ Help and Command keyword table
table:
	.string	"SyncClock"
	.align
	.int	command_syncclock
	.int	0x00010001		@ one parameter, minimum one
	.int	command_syncclock_syntax
	.int	command_syncclock_help

	.byte	0	@ Table terminator
	.align

	@ The trailing blank line is DEEJ's: their source ends this string with a
	@ line break and then adds another. Kept so the assembled module matches
	@ theirs byte for byte apart from the date in the help string above.
command_syncclock_help:
	.string	"*SyncClock regularly synchronises the soft copy of the clock with the CMOS real time clock. The parameter controls the interval in seconds, zero to disable.\n\r\n\r"
	.align

command_syncclock_syntax:
	.string	"Syntax: *SyncClock <interval>\n\r"
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
	str	r2, [r12]		@ private word points at the workspace
	mov	r12, r2

	@ INTERVAL is 1000, which is not an encodable immediate, so it is built a
	@ byte at a time as the original did.
	mov	r0, #INTERVAL & 0x0000ff
	orr	r0, r0, #INTERVAL & 0x00ff00
	orr	r0, r0, #INTERVAL & 0xff0000
	str	r0, [r12, #WS_INTERVAL]

	@ R2 still holds the workspace address, which is what OS_CallEvery passes
	@ to the ticker in R12.
	adr	r1, sync
	swi	XOS_CallEvery
	ldmfd	sp!, {pc}

final:
	stmfd	sp!, {lr}
	ldr	r12, [r12]		@ workspace
	adr	r0, sync
	mov	r1, r12
	swi	XOS_RemoveTickerEvent
	mov	r0, #Module_Free
	mov	r2, r12
	swi	XOS_Module
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *SyncClock <interval in seconds>, zero to stop
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_syncclock:
	stmfd	sp!, {lr}
	ldr	r12, [r12]		@ workspace
	mov	r1, r0			@ command tail
	mov	r0, #10			@ base ten
	swi	XOS_ReadUnsigned
	mov	r1, #100
	mul	r2, r1, r2		@ seconds to centiseconds
	strvc	r2, [r12, #WS_INTERVAL]

	@ Remove the ticker whatever was asked for, then put it back unless the
	@ new interval is zero.
	adr	r0, sync
	mov	r1, r12
	swi	XOS_RemoveTickerEvent
	movs	r0, r2
	adrne	r1, sync
	movne	r2, r12
	swine	XOS_CallEvery
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The ticker: read the RTC, and set the soft clock from it
@
@ Entered at ticker time, so it must not call anything that is unsafe there. It
@ works in a block on its own stack and gives up quietly on any error, which is
@ the right answer for something that will be along again in ten seconds.
@
@ The chip only stores the year modulo four, so the year is taken from the soft
@ clock and only its low two bits are replaced from the chip. That is the
@ original's trick and it is why this cannot correct a clock that is years out.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

sync:
	stmfd	sp!, {r0-r2, lr}
	sub	sp, sp, #BLOCK_SIZE

	@ Read the soft copy of the clock, as UTC
	mov	r0, #OSWord_ReadClock
	add	r1, sp, #OBLOCK
	mov	r2, #ReadClock_UTC
	strb	r2, [r1]
	swi	XOS_Word
	addvs	sp, sp, #BLOCK_SIZE
	ldmvsfd	sp!, {r0-r2, pc}

	@ Convert it to ordinals, which is where the year comes from
	mvn	r0, #0			@ current territory
	add	r2, sp, #TBLOCK
	swi	XTerritory_ConvertTimeToUTCOrdinals
	addvs	sp, sp, #BLOCK_SIZE
	ldmvsfd	sp!, {r0-r2, pc}

	@ Point the chip's register pointer at register 1
	mov	r0, #CMOS_CLOCK + 0	@ write
	add	r1, sp, #IBLOCK
	mov	r2, #1
	strb	r2, [r1]
	swi	XIIC_Control
	addvs	sp, sp, #BLOCK_SIZE
	ldmvsfd	sp!, {r0-r2, pc}

	@ Read six bytes: cs, s, m, h, day, month
	mov	r0, #CMOS_CLOCK + 1	@ read
	add	r1, sp, #IBLOCK
	mov	r2, #6
	swi	XIIC_Control
	addvs	sp, sp, #BLOCK_SIZE
	ldmvsfd	sp!, {r0-r2, pc}

	@ Convert the BCD the chip gave us into the ordinals block
	ldrb	r0, [sp, #IBLOCK + 0]		@ centiseconds
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_CS]
	ldrb	r0, [sp, #IBLOCK + 1]		@ seconds
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_SECOND]
	ldrb	r0, [sp, #IBLOCK + 2]		@ minutes
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_MINUTE]
	ldrb	r0, [sp, #IBLOCK + 3]		@ hours
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_HOUR]

	@ Day is six bits; the other two are the year modulo four, kept in r2
	ldrb	r2, [sp, #IBLOCK + 4]
	and	r0, r2, #0x3f
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_DAY]

	@ Month is five bits; the top three are the weekday, which is not wanted
	ldrb	r0, [sp, #IBLOCK + 5]
	and	r0, r0, #0x1f
	bl	bcd
	str	r0, [sp, #TBLOCK + ORD_MONTH]

	@ Keep the century and decade from the soft clock, take the rest from the
	@ chip's two bits
	ldr	r0, [sp, #TBLOCK + ORD_YEAR]
	bic	r0, r0, #3
	add	r0, r0, r2, lsr #6
	str	r0, [sp, #TBLOCK + ORD_YEAR]

	@ Back to a UTC time, and set the clock from it
	mvn	r0, #0			@ current territory
	add	r1, sp, #OBLOCK
	add	r2, sp, #TBLOCK
	swi	XTerritory_ConvertOrdinalsToTime
	addvs	sp, sp, #BLOCK_SIZE
	ldmvsfd	sp!, {r0-r2, pc}

	add	r0, sp, #OBLOCK
	swi	XTerritory_SetTime
	add	sp, sp, #BLOCK_SIZE
	ldmfd	sp!, {r0-r2, pc}


@ BCD byte in r0 to binary in r0. Corrupts r1.
bcd:
	mov	r1, r0, lsr #4		@ high nibble
	add	r1, r1, r1, lsl #2	@ times five
	and	r0, r0, #0xf		@ low nibble
	add	r0, r0, r1, lsl #1	@ plus twice the above, so times ten
	mov	pc, lr

	.end
