@ RPCEmuCoPro - drive the OPEN Bus co-processor card from RISC OS
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ The Risc PC's OPEN Bus is its second processor interface, and RPCEmu Extended
@ emulates a co-processor card for it with a choice of processor: RV32IM, 6502 or
@ Z80. This module is how a program on the RISC OS side loads code into that card,
@ starts it and collects the answer. See docs/openbus.md.
@
@ HOW THE CARD IS REACHED. Its registers live at physical &03600000, the window
@ the Risc PC Technical Reference Manual reserves for second bus master
@ registers. That is not a podule, so Podule_ReadInfo will never find it and
@ there is no logical address to inherit: the module asks the kernel for one with
@ OS_Memory 13, which maps I/O space permanently and hands back a logical address
@ in R3. With no flags in R0 the mapping is non-bufferable and non-cacheable,
@ which is what device registers need.
@
@ WHY IT PROBES BEFORE IT MAPS. This module ships in the emulator's podule ROM, so
@ it loads on every machine whether a card is fitted or not, and permanently
@ mapping two megabytes of I/O space on a machine with an empty slot would be rude.
@ So init makes a TEMPORARY mapping with OS_Memory 14, reads the ID register,
@ releases it with OS_Memory 15, and only maps permanently when the ID says a card
@ is really there. On a machine with no card the module initialises anyway and
@ every entry point reports "no card" - refusing to initialise would put an error
@ on the screen at every boot of every machine, which is worse than useless.
@
@ NO PHYSICAL ADDRESSES ARE NEEDED for anything else, and that is by design on the
@ card's side: the core has its own RAM, reached through an auto-incrementing
@ aperture in the register window, so loading a program is a loop of ordinary word
@ writes. No dynamic area, no page translation, no contiguity to arrange. The card
@ can also bus-master directly to and from host memory, which is faster for bulk
@ data, and this module deliberately does not use that path - see docs/openbus.md.
@
@ ★★ THE SWI CHUNK BELOW IS PROVISIONAL AND MUST BE ALLOCATED BEFORE RELEASE.
@ &58cc0 is allocated to RPCEmu and is in use by EtherRPCEm; nothing has been
@ allocated for this. &58d00, the chunk above it, is used here because asking for
@ the next one along is the natural request to make, but until RISC OS Open
@ actually allocates it another module could legitimately be using it. Said here,
@ in docs/openbus.md and in the module's help string rather than left to be
@ discovered.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_Write0			= 0x20002
	XOS_NewLine			= 0x20003
	XOS_Find			= 0x2000d
	XOS_GBPB			= 0x2000c
	XOS_Module			= 0x2001e
	XOS_Memory			= 0x20068
	XOS_ConvertHex8			= 0x200d4

	Module_Claim			= 6
	Module_Free			= 7

	@ OS_Memory reason codes, from the kernel's own definitions. The
	@ conventions are worth writing down because the returned register is not
	@ the one a reader expects:
	@
	@   13 MapIOPermanent   R1 = physical, R2 = size    -> R3 = logical
	@   14 AccessPhysAddr   R1 = physical               -> R2 = logical,
	@                                                      R3 = old state
	@   15 ReleasePhysAddr  R1 = old state
	@
	@ R0 bit 8 would ask for a bufferable mapping and bit 9 a cacheable one;
	@ both are left clear, which is what registers want.
	Memory_MapIOPermanent		= 13
	Memory_AccessPhysAddr		= 14
	Memory_ReleasePhysAddr		= 15

	@ The second bus master register window, from the Risc PC TRM. Megabyte
	@ aligned already, which OS_Memory 13 requires.
	OPENBUS_PHYS			= 0x03600000
	OPENBUS_SIZE			= 0x00200000

	@ The card's registers, as offsets in that window. Mirrors
	@ src/copro/openbus_coproc.h; keep the two in step.
	REG_ID				= 0x00
	REG_CORE			= 0x04
	REG_RAMSIZE			= 0x08
	REG_CTRL			= 0x0c
	REG_STATUS			= 0x10
	REG_ENTRY			= 0x14
	REG_ADDR			= 0x18
	REG_DATA			= 0x1c
	REG_MBOX_TX			= 0x20
	REG_MBOX_RX			= 0x24
	REG_CYCLES			= 0x28
	REG_PC				= 0x2c
	REG_FAULT			= 0x30
	REG_FAULTADDR			= 0x34
	REG_IRQCLEAR			= 0x38

	CARD_ID				= 0x4f424350	@ 'OBCP'

	CTRL_RUN			= 1 << 0
	CTRL_STEP			= 1 << 1
	CTRL_RESET			= 1 << 2
	CTRL_IRQ_ON_HALT		= 1 << 3

	STATUS_RUNNING			= 1 << 0
	STATUS_HALTED			= 1 << 1
	STATUS_FAULT			= 1 << 2
	STATUS_IRQ			= 1 << 3
	STATUS_DMA			= 1 << 4

	@ ★ PROVISIONAL - see the note at the top of this file.
	SWI_CHUNK			= 0x58d00

	CoPro_Info			= 0
	CoPro_Reset			= 1
	CoPro_Write			= 2
	CoPro_Read			= 3
	CoPro_Run			= 4
	CoPro_Stop			= 5
	CoPro_Status			= 6
	CoPro_Step			= 7
	CoPro_Mailbox			= 8
	SWI_COUNT			= 9

	@ Errors. The base follows RPCEmuPCIEmulator's, which is the precedent
	@ this fork already set for its own error numbers.
	Error_NoSuchSWI			= 0x1e6
	Error_Base			= 0x820b40
	Error_NoCard			= Error_Base + 0
	Error_Range			= Error_Base + 1
	Error_Busy			= Error_Base + 2
	Error_Syntax			= Error_Base + 3

	V_bit				= 1 << 28
	C_bit				= 1 << 29

	@ How much of a file to move at a time in *CoProLoad. Any size works; a
	@ page keeps the workspace small and the loop short.
	LOAD_CHUNK			= 4096

	@ Workspace
	WS_BASE				= 0	@ logical base of the window, 0 = no card
	WS_SCRATCH			= 4	@ one word, for number conversion
	WS_NUMBUF			= 8	@ 12 bytes, OS_ConvertHex8 output
	WS_BUFFER			= 20	@ LOAD_CHUNK bytes
	WORKSPACE_SIZE			= WS_BUFFER + LOAD_CHUNK


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
	.int	SWI_CHUNK	@ SWI Chunk base
	.int	swi_handler	@ SWI handler code
	.int	swi_names	@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32-bit compatible

title:
	.string	"RPCEmuCoPro"
	.align

	.include "version.inc"

swi_names:
	@ The SWI prefix is "CoPro" while the module is called RPCEmuCoPro: the
	@ module name follows this fork's convention for its own modules, and the
	@ SWI and * command names stay short because they are what a programmer
	@ types. RPCEmuPCIEmulator already has that shape, providing PCI_* SWIs.
	.string	"CoPro"		@ SWI prefix
	.string	"Info"
	.string	"Reset"
	.string	"Write"
	.string	"Read"
	.string	"Run"
	.string	"Stop"
	.string	"Status"
	.string	"Step"
	.string	"Mailbox"
	.byte	0		@ end of table
	.align


@ ---------------------------------------------------------------- init/final

@ Entry: r12 -> private word. Exit: r12 preserved, V clear, or R0 -> error.
init:
	stmfd	sp!, {r0-r5, lr}

	@ Workspace first, so that a failed probe still has somewhere to record
	@ that there is no card.
	mov	r0, #Module_Claim
	@ WORKSPACE_SIZE is not one of the constants an ARM instruction can carry
	@ as an immediate, so it is built from two that are. A literal pool would
	@ do as well for a plain number, but keeping the code free of pools means
	@ nothing has to be in reach of anything.
	mov	r3, #LOAD_CHUNK
	add	r3, r3, #WS_BUFFER
	swi	XOS_Module
	bvs	init_failed
	str	r2, [r12]
	mov	r4, r2			@ r4 -> workspace for the rest of init

	mov	r0, #0
	str	r0, [r4, #WS_BASE]

	@ Probe with a temporary mapping. This is the whole reason init does not
	@ simply map permanently: on a machine with an empty second slot, mapping
	@ two megabytes of I/O space would cost page tables for nothing.
	mov	r0, #Memory_AccessPhysAddr
	ldr	r1, =OPENBUS_PHYS
	swi	XOS_Memory
	bvs	init_done		@ old kernels: no reason 14, so no card

	ldr	r5, [r2, #REG_ID]	@ read the ID through the temporary mapping

	mov	r0, #Memory_ReleasePhysAddr
	mov	r1, r3			@ the old state reason 14 handed back
	swi	XOS_Memory

	ldr	r0, =CARD_ID
	cmp	r5, r0
	bne	init_done		@ an undriven bus, or somebody else's card

	@ There is a card, so map the window for good.
	mov	r0, #Memory_MapIOPermanent
	ldr	r1, =OPENBUS_PHYS
	ldr	r2, =OPENBUS_SIZE
	swi	XOS_Memory
	bvs	init_done		@ no room for it: stay inert rather than fail
	str	r3, [r4, #WS_BASE]	@ reason 13 returns the logical address in R3

init_done:
	ldmfd	sp!, {r0-r5, lr}
	bic	lr, lr, #V_bit		@ initialise even with no card; see the top
	mov	pc, lr

init_failed:
	add	sp, sp, #4		@ drop the saved r0, keeping the error
	ldmfd	sp!, {r1-r5, lr}
	orr	lr, lr, #V_bit
	mov	pc, lr


@ Entry: r12 -> private word.
final:
	stmfd	sp!, {r0-r3, lr}

	@ The permanent I/O mapping is not released: OS_Memory has no call to undo
	@ reason 13, which is what "permanent" means. Nothing leaks that a reload
	@ would double up, because the kernel returns the same logical address for
	@ the same physical area.
	ldr	r2, [r12]
	cmp	r2, #0
	beq	final_done
	mov	r0, #Module_Free
	swi	XOS_Module
	mov	r0, #0
	str	r0, [r12]

final_done:
	ldmfd	sp!, {r0-r3, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr


@ ------------------------------------------------------------------ helpers

@ Get the window's logical base.
@   In:  r12 -> private word
@   Out: r0 = base, and Z clear; or Z set and nothing valid in r0
@ Corrupts nothing else.
get_base:
	ldr	r0, [r12]
	cmp	r0, #0
	moveq	pc, lr
	ldr	r0, [r0, #WS_BASE]
	cmp	r0, #0
	mov	pc, lr

@ Get the workspace pointer, or zero.
get_ws:
	ldr	r0, [r12]
	cmp	r0, #0
	mov	pc, lr

@ Return "no co-processor card is fitted", V set.
error_no_card:
	adrl	r0, err_no_card
	orr	lr, lr, #V_bit
	mov	pc, lr


@ ------------------------------------------------------------- SWI handler

@ Entry: r11 = SWI number within the chunk, r12 -> private word.
@ May corrupt r10-r12. Everything else is the SWI's own business.
swi_handler:
	cmp	r11, #SWI_COUNT
	bhs	swi_unknown

	stmfd	sp!, {lr}

	@ Every entry point needs the window, and the same answer when there is
	@ none, so it is fetched once here.
	bl	get_base
	beq	swi_no_card
	mov	r10, r0			@ r10 = the window, for every handler below

	ldr	pc, [pc, r11, lsl #2]
	nop				@ the table is one word past the load
swi_table:
	.int	swi_info
	.int	swi_reset
	.int	swi_write
	.int	swi_read
	.int	swi_run
	.int	swi_stop
	.int	swi_status
	.int	swi_step
	.int	swi_mailbox

swi_no_card:
	adrl	r0, err_no_card
	ldmfd	sp!, {lr}
	orr	lr, lr, #V_bit
	mov	pc, lr

swi_unknown:
	@ The same error RISC OS gives for an unclaimed SWI in a claimed chunk, so
	@ a caller probing for this module cannot tell it from its absence.
	adrl	r0, err_bad_swi
	orr	lr, lr, #V_bit
	mov	pc, lr

@ Return successfully from a SWI entered through the table above.
swi_return:
	ldmfd	sp!, {lr}
	bic	lr, lr, #V_bit
	mov	pc, lr

@ Return an error from one. r0 -> error block.
swi_error:
	ldmfd	sp!, {lr}
	orr	lr, lr, #V_bit
	mov	pc, lr


@ CoPro_Info. Out: r0 = core id ('RV32', '6502' or 'Z80 '), r1 = card RAM size,
@ r2 = status.
swi_info:
	ldr	r0, [r10, #REG_CORE]
	ldr	r1, [r10, #REG_RAMSIZE]
	ldr	r2, [r10, #REG_STATUS]
	b	swi_return

@ CoPro_Reset. In: r0 = entry address for the core.
@ The entry is written before the reset, because the reset is what makes the core
@ take it.
swi_reset:
	ldr	r1, [r10, #REG_RAMSIZE]
	cmp	r0, r1
	bhs	swi_range
	str	r0, [r10, #REG_ENTRY]
	mov	r1, #CTRL_RESET
	str	r1, [r10, #REG_CTRL]
	b	swi_return

swi_range:
	adrl	r0, err_range
	b	swi_error

@ CoPro_Write. In: r0 = address in card RAM, r1 -> buffer, r2 = length in bytes.
@ Bytes rather than words, because a program image is a file and a file is bytes.
swi_write:
	stmfd	sp!, {r0-r4}
	bl	check_range
	bcs	swi_write_range
	str	r0, [r10, #REG_ADDR]
	mov	r3, r1
	mov	r4, r2
swi_write_loop:
	cmp	r4, #0
	beq	swi_write_done
	ldrb	r0, [r3], #1
	strb	r0, [r10, #REG_DATA]	@ a byte write moves one byte and steps one
	sub	r4, r4, #1
	b	swi_write_loop
swi_write_done:
	ldmfd	sp!, {r0-r4}
	b	swi_return

swi_write_range:
	ldmfd	sp!, {r0-r4}
	adrl	r0, err_range
	b	swi_error

@ CoPro_Read. In: r0 = address in card RAM, r1 -> buffer, r2 = length in bytes.
swi_read:
	stmfd	sp!, {r0-r4}
	bl	check_range
	bcs	swi_write_range
	str	r0, [r10, #REG_ADDR]
	mov	r3, r1
	mov	r4, r2
swi_read_loop:
	cmp	r4, #0
	beq	swi_read_done
	ldrb	r0, [r10, #REG_DATA]
	strb	r0, [r3], #1
	sub	r4, r4, #1
	b	swi_read_loop
swi_read_done:
	ldmfd	sp!, {r0-r4}
	b	swi_return

@ Is [r0, r0 + r2) inside card RAM? C set if not.
@ Written as a subtraction against the size so that a length near 2^32 cannot
@ wrap the sum and come out looking valid.
check_range:
	stmfd	sp!, {r1, r3, lr}
	ldr	r3, [r10, #REG_RAMSIZE]
	cmp	r0, r3
	bhi	check_range_bad
	sub	r1, r3, r0
	cmp	r2, r1
	bhi	check_range_bad
	ldmfd	sp!, {r1, r3, lr}
	bic	lr, lr, #C_bit
	movs	pc, lr
check_range_bad:
	ldmfd	sp!, {r1, r3, lr}
	orr	lr, lr, #C_bit
	movs	pc, lr

@ CoPro_Run. In: r0 = flags, bit 0 = raise the host's interrupt when the core
@ stops. The core runs whenever the emulator gives the card a share of the ARM's
@ time, so a caller that wants to wait simply polls CoPro_Status.
swi_run:
	tst	r0, #1
	moveq	r1, #CTRL_RUN
	movne	r1, #CTRL_RUN + CTRL_IRQ_ON_HALT
	str	r1, [r10, #REG_CTRL]
	b	swi_return

@ CoPro_Stop. Clears the run bit; the core keeps its state and can be resumed.
swi_stop:
	mov	r1, #0
	str	r1, [r10, #REG_CTRL]
	b	swi_return

@ CoPro_Status. Out: r0 = status, r1 = program counter, r2 = instructions
@ retired, r3 = mailbox from the core, r4 = fault cause, r5 = fault address.
swi_status:
	ldr	r0, [r10, #REG_STATUS]
	ldr	r1, [r10, #REG_PC]
	ldr	r2, [r10, #REG_CYCLES]
	ldr	r3, [r10, #REG_MBOX_RX]
	ldr	r4, [r10, #REG_FAULT]
	ldr	r5, [r10, #REG_FAULTADDR]
	b	swi_return

@ CoPro_Step. Executes one instruction whether or not the core is running.
swi_step:
	mov	r1, #CTRL_STEP
	str	r1, [r10, #REG_CTRL]
	b	swi_return

@ CoPro_Mailbox. In: r0 = word to hand the core, or -1 to leave it alone.
@ Out: r1 = the word the core has posted back.
@ Only a Z80 can READ the mailbox, through its port space; an RV32I or 6502
@ program takes its parameters in card RAM. See src/copro/openbus_coproc.h.
swi_mailbox:
	cmn	r0, #1
	strne	r0, [r10, #REG_MBOX_TX]
	ldr	r1, [r10, #REG_MBOX_RX]
	b	swi_return


@ -------------------------------------------------------------- * commands

table:
	.string	"CoProInfo"
	.align
	.int	cmd_info
	.int	0x00000000		@ no parameters
	.int	0
	.int	syntax_info

	.string	"CoProStatus"
	.align
	.int	cmd_status
	.int	0x00000000
	.int	0
	.int	syntax_status

	.string	"CoProLoad"
	.align
	.int	cmd_load
	.int	0x00010002		@ 1 to 2 parameters
	.int	0
	.int	syntax_load

	.string	"CoProRun"
	.align
	.int	cmd_run
	.int	0x00000001		@ 0 or 1 parameters
	.int	0
	.int	syntax_run

	.byte	0			@ end of table
	.align


@ Print the zero-terminated string at r0, ignoring errors: a * command that
@ cannot write to the screen has nothing useful to say about it.
print:
	stmfd	sp!, {r0-r3, lr}
	swi	XOS_Write0
	ldmfd	sp!, {r0-r3, lr}
	mov	pc, lr

newline:
	stmfd	sp!, {r0-r3, lr}
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, lr}
	mov	pc, lr

@ Print r0 as eight hex digits. r10 must hold the window and r9 the workspace.
print_hex:
	stmfd	sp!, {r0-r3, lr}
	add	r1, r9, #WS_NUMBUF
	mov	r2, #12
	swi	XOS_ConvertHex8
	movvc	r0, r1
	blvc	print
	ldmfd	sp!, {r0-r3, lr}
	mov	pc, lr

@ Print the four characters of a core identifier held in r0, most significant
@ byte first - the register holds 'RV32' as 0x52563332, so the letters come out
@ of the top.
print_core_id:
	stmfd	sp!, {r0-r4, lr}
	mov	r4, r0
	mov	r3, #4
print_core_loop:
	mov	r0, r4, lsr #24
	and	r0, r0, #0xff
	strb	r0, [r9, #WS_NUMBUF]
	mov	r0, #0
	strb	r0, [r9, #WS_NUMBUF + 1]
	add	r0, r9, #WS_NUMBUF
	bl	print
	mov	r4, r4, lsl #8
	subs	r3, r3, #1
	bne	print_core_loop
	ldmfd	sp!, {r0-r4, lr}
	mov	pc, lr

@ Common prologue for a * command: r9 -> workspace, r10 -> window, or report
@ that there is no card and return.
@ Uses the caller's stack frame, so it is entered with bl and the caller must
@ have saved lr already.
cmd_setup:
	stmfd	sp!, {r0-r2, lr}
	bl	get_ws
	beq	cmd_setup_none
	mov	r9, r0
	bl	get_base
	beq	cmd_setup_none
	mov	r10, r0
	ldmfd	sp!, {r0-r2, lr}
	bic	lr, lr, #C_bit
	movs	pc, lr
cmd_setup_none:
	ldmfd	sp!, {r0-r2, lr}
	orr	lr, lr, #C_bit
	movs	pc, lr


@ *CoProInfo
cmd_info:
	stmfd	sp!, {r0-r10, lr}
	bl	cmd_setup
	bcs	cmd_no_card

	adrl	r0, msg_fitted
	bl	print
	ldr	r0, [r10, #REG_CORE]
	bl	print_core_id
	adrl	r0, msg_with
	bl	print
	ldr	r0, [r10, #REG_RAMSIZE]
	mov	r0, r0, lsr #10
	bl	print_decimal
	adrl	r0, msg_kbytes
	bl	print
	bl	newline

	adrl	r0, msg_swi_note
	bl	print
	bl	newline

	ldmfd	sp!, {r0-r10, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr

cmd_no_card:
	ldmfd	sp!, {r0-r10, lr}
	adrl	r0, err_no_card
	orr	lr, lr, #V_bit
	mov	pc, lr


@ *CoProStatus
cmd_status:
	stmfd	sp!, {r0-r10, lr}
	bl	cmd_setup
	bcs	cmd_no_card

	adrl	r0, msg_state
	bl	print
	@ Branched rather than done with conditional ADRs: the messages are at the
	@ end of the module, too far for ADR to reach, and ADRL has no
	@ unambiguous conditional form - "adrle" would read as ADR with the LE
	@ condition.
	ldr	r1, [r10, #REG_STATUS]
	tst	r1, #STATUS_FAULT
	bne	cmd_status_faulted
	tst	r1, #STATUS_HALTED
	bne	cmd_status_halted
	tst	r1, #STATUS_RUNNING
	bne	cmd_status_running
	adrl	r0, msg_stopped
	b	cmd_status_said
cmd_status_faulted:
	adrl	r0, msg_faulted
	b	cmd_status_said
cmd_status_halted:
	adrl	r0, msg_halted
	b	cmd_status_said
cmd_status_running:
	adrl	r0, msg_running
cmd_status_said:
	bl	print
	bl	newline

	adrl	r0, msg_pc
	bl	print
	ldr	r0, [r10, #REG_PC]
	bl	print_hex
	bl	newline

	adrl	r0, msg_cycles
	bl	print
	ldr	r0, [r10, #REG_CYCLES]
	bl	print_decimal
	bl	newline

	adrl	r0, msg_mailbox
	bl	print
	ldr	r0, [r10, #REG_MBOX_RX]
	bl	print_hex
	bl	newline

	ldr	r1, [r10, #REG_STATUS]
	tst	r1, #STATUS_FAULT
	beq	cmd_status_done
	adrl	r0, msg_fault
	bl	print
	ldr	r0, [r10, #REG_FAULT]
	bl	print_decimal
	adrl	r0, msg_at
	bl	print
	ldr	r0, [r10, #REG_FAULTADDR]
	bl	print_hex
	bl	newline

cmd_status_done:
	ldmfd	sp!, {r0-r10, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr


@ *CoProLoad <file> [<hex address>]
@
@ Streamed through the module's own buffer rather than loaded whole: a file of any
@ size then works without claiming memory for it, and the aperture wants the bytes
@ in order anyway.
cmd_load:
	stmfd	sp!, {r0-r11, lr}
	mov	r11, r0			@ the command tail
	bl	cmd_setup
	bcs	cmd_no_card

	@ The filename runs to the first space or terminator. OS_Find needs it
	@ terminated, and the tail is the OS's copy, so the terminator is written
	@ in place after the name is measured.
	mov	r1, r11
cmd_load_skip:
	ldrb	r0, [r1]
	cmp	r0, #' '
	bne	cmd_load_name
	add	r1, r1, #1
	b	cmd_load_skip
cmd_load_name:
	mov	r11, r1			@ r11 -> the name itself
cmd_load_findend:
	ldrb	r0, [r1]
	cmp	r0, #' '
	beq	cmd_load_gotend
	cmp	r0, #13
	bls	cmd_load_gotend		@ 0 or CR: the name ends the line
	add	r1, r1, #1
	b	cmd_load_findend
cmd_load_gotend:
	@ An address may follow. Parse it before the name is terminated, since
	@ terminating overwrites the separator.
	mov	r8, #0			@ the load address, zero unless given
	cmp	r0, #' '
	bne	cmd_load_terminate
	mov	r2, r1
cmd_load_addrskip:
	add	r2, r2, #1
	ldrb	r0, [r2]
	cmp	r0, #' '
	beq	cmd_load_addrskip
	cmp	r0, #13
	bls	cmd_load_terminate
	bl	parse_hex		@ r2 -> digits; r0 = value, C set if bad
	bcs	cmd_load_syntax
	mov	r8, r0

cmd_load_terminate:
	mov	r0, #0
	strb	r0, [r1]		@ terminate the filename in place

	@ Open for input.
	mov	r0, #0x4f
	mov	r1, r11
	swi	XOS_Find
	bvs	cmd_load_failed
	cmp	r1, #0
	beq	cmd_load_notfound
	mov	r11, r1			@ r11 = the file handle

	@ Where in card RAM to start. Checked against the card's own size rather
	@ than assumed, because a Z80 image conventionally loads at &100 and an
	@ RV32I one at 0, and neither should be able to run off the end.
	ldr	r0, [r10, #REG_RAMSIZE]
	cmp	r8, r0
	bhs	cmd_load_closerange
	str	r8, [r10, #REG_ADDR]

	mov	r7, #0			@ bytes transferred
cmd_load_chunk:
	mov	r0, #4			@ OS_GBPB 4: read from the current pointer
	mov	r1, r11
	add	r2, r9, #WS_BUFFER
	mov	r3, #LOAD_CHUNK
	swi	XOS_GBPB
	bvs	cmd_load_closefail
	@ r3 comes back as the number NOT transferred, so a short read is the end
	@ of the file and a full one means there may be more.
	rsb	r4, r3, #LOAD_CHUNK	@ r4 = bytes actually read
	cmp	r4, #0
	beq	cmd_load_close

	@ Would this chunk run past the end of card RAM?
	ldr	r0, [r10, #REG_RAMSIZE]
	sub	r0, r0, r8
	cmp	r4, r0
	bhi	cmd_load_closerange

	add	r5, r9, #WS_BUFFER
cmd_load_push:
	ldrb	r0, [r5], #1
	strb	r0, [r10, #REG_DATA]
	subs	r4, r4, #1
	bne	cmd_load_push

	rsb	r4, r3, #LOAD_CHUNK
	add	r7, r7, r4
	add	r8, r8, r4
	cmp	r3, #0			@ nothing left over: the buffer filled
	beq	cmd_load_chunk

cmd_load_close:
	mov	r0, #0
	mov	r1, r11
	swi	XOS_Find

	adrl	r0, msg_loaded
	bl	print
	mov	r0, r7
	bl	print_decimal
	adrl	r0, msg_bytes
	bl	print
	bl	newline

	ldmfd	sp!, {r0-r11, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr

cmd_load_closerange:
	mov	r0, #0
	mov	r1, r11
	swi	XOS_Find
	ldmfd	sp!, {r0-r11, lr}
	adrl	r0, err_range
	orr	lr, lr, #V_bit
	mov	pc, lr

cmd_load_closefail:
	stmfd	sp!, {r0}
	mov	r0, #0
	mov	r1, r11
	swi	XOS_Find
	ldmfd	sp!, {r0}
	add	sp, sp, #4		@ drop the stacked r0, keeping the error
	ldmfd	sp!, {r1-r11, lr}
	orr	lr, lr, #V_bit
	mov	pc, lr

cmd_load_failed:
	add	sp, sp, #4
	ldmfd	sp!, {r1-r11, lr}
	orr	lr, lr, #V_bit
	mov	pc, lr

cmd_load_notfound:
	ldmfd	sp!, {r0-r11, lr}
	adrl	r0, err_syntax
	orr	lr, lr, #V_bit
	mov	pc, lr

cmd_load_syntax:
	ldmfd	sp!, {r0-r11, lr}
	adrl	r0, err_syntax
	orr	lr, lr, #V_bit
	mov	pc, lr


@ *CoProRun [<hex entry>]
@
@ Resets to the entry point, starts the core and waits. Waiting is a poll rather
@ than a sleep on purpose: the card only advances when the emulator gives it a
@ share of the ARM's time, so a caller that stopped executing would stop the
@ co-processor with it.
cmd_run:
	stmfd	sp!, {r0-r11, lr}
	mov	r11, r0
	bl	cmd_setup
	bcs	cmd_no_card

	mov	r8, #0			@ the entry point
	mov	r2, r11
cmd_run_skip:
	ldrb	r0, [r2]
	cmp	r0, #' '
	addeq	r2, r2, #1
	beq	cmd_run_skip
	cmp	r0, #13
	bls	cmd_run_go		@ nothing given, so start at zero
	bl	parse_hex
	bcs	cmd_run_syntax
	mov	r8, r0

cmd_run_go:
	ldr	r0, [r10, #REG_RAMSIZE]
	cmp	r8, r0
	bhs	cmd_run_range

	str	r8, [r10, #REG_ENTRY]
	mov	r0, #CTRL_RESET
	str	r0, [r10, #REG_CTRL]
	mov	r0, #CTRL_RUN
	str	r0, [r10, #REG_CTRL]

	@ Poll until it stops. There is no timeout: a co-processor program that
	@ never stops is the caller's own doing, and Escape is how RISC OS says
	@ so - which is why the loop reads the status rather than spinning on a
	@ register the OS cannot interrupt.
cmd_run_wait:
	ldr	r0, [r10, #REG_STATUS]
	tst	r0, #STATUS_HALTED + STATUS_FAULT
	beq	cmd_run_wait

	tst	r0, #STATUS_FAULT
	bne	cmd_run_faulted

	adrl	r0, msg_stopped_with
	bl	print
	ldr	r0, [r10, #REG_MBOX_RX]
	bl	print_hex
	adrl	r0, msg_after
	bl	print
	ldr	r0, [r10, #REG_CYCLES]
	bl	print_decimal
	adrl	r0, msg_instructions
	bl	print
	bl	newline

	ldmfd	sp!, {r0-r11, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr

cmd_run_faulted:
	adrl	r0, msg_fault
	bl	print
	ldr	r0, [r10, #REG_FAULT]
	bl	print_decimal
	adrl	r0, msg_at
	bl	print
	ldr	r0, [r10, #REG_FAULTADDR]
	bl	print_hex
	bl	newline
	ldmfd	sp!, {r0-r11, lr}
	bic	lr, lr, #V_bit
	mov	pc, lr

cmd_run_range:
	ldmfd	sp!, {r0-r11, lr}
	adrl	r0, err_range
	orr	lr, lr, #V_bit
	mov	pc, lr

cmd_run_syntax:
	ldmfd	sp!, {r0-r11, lr}
	adrl	r0, err_syntax
	orr	lr, lr, #V_bit
	mov	pc, lr


@ Parse a hexadecimal number.
@   In:  r2 -> the first character; an "&" or "0x" prefix is accepted
@   Out: r0 = the value, r2 -> past the digits, C clear
@        C set and r0 undefined if there were no digits at all
@ Written here rather than through OS_ReadUnsigned because it is fifteen
@ instructions and needs no assumptions about that call's flag bits.
parse_hex:
	stmfd	sp!, {r1, r3, r4, lr}
	mov	r0, #0
	mov	r4, #0			@ how many digits were seen

	ldrb	r1, [r2]
	cmp	r1, #'&'
	addeq	r2, r2, #1
	beq	parse_hex_loop
	cmp	r1, #'0'
	bne	parse_hex_loop
	ldrb	r1, [r2, #1]
	orr	r1, r1, #0x20		@ fold case for the x
	cmp	r1, #'x'
	addeq	r2, r2, #2

parse_hex_loop:
	ldrb	r1, [r2]
	cmp	r1, #'0'
	blo	parse_hex_end
	cmp	r1, #'9'
	subls	r3, r1, #'0'
	bls	parse_hex_digit
	orr	r1, r1, #0x20		@ fold case for the digits
	cmp	r1, #'a'
	blo	parse_hex_end
	cmp	r1, #'f'
	bhi	parse_hex_end
	sub	r3, r1, #'a'
	add	r3, r3, #10

parse_hex_digit:
	mov	r0, r0, lsl #4
	orr	r0, r0, r3
	add	r2, r2, #1
	add	r4, r4, #1
	b	parse_hex_loop

parse_hex_end:
	cmp	r4, #0
	ldmfd	sp!, {r1, r3, r4, lr}
	orreq	lr, lr, #C_bit		@ no digits at all
	bicne	lr, lr, #C_bit
	movs	pc, lr


@ Print r0 as an unsigned decimal number.
@ r9 must hold the workspace. Digits are generated backwards into the number
@ buffer, which is what makes this shorter than any division-first arrangement.
print_decimal:
	stmfd	sp!, {r0-r5, lr}
	add	r4, r9, #WS_NUMBUF
	add	r4, r4, #11
	mov	r5, #0
	strb	r5, [r4]		@ terminator

	cmp	r0, #0
	bne	print_decimal_loop
	mov	r5, #'0'
	sub	r4, r4, #1
	strb	r5, [r4]
	b	print_decimal_done

print_decimal_loop:
	cmp	r0, #0
	beq	print_decimal_done
	bl	divide_by_ten		@ r0 = quotient, r1 = remainder
	add	r1, r1, #'0'
	sub	r4, r4, #1
	strb	r1, [r4]
	b	print_decimal_loop

print_decimal_done:
	mov	r0, r4
	bl	print
	ldmfd	sp!, {r0-r5, lr}
	mov	pc, lr

@ r0 / 10 -> r0, remainder in r1. Long division a bit at a time: ARMv3 has no
@ divide instruction, and the reciprocal-multiply trick would need a 64-bit
@ multiply this architecture also lacks.
divide_by_ten:
	stmfd	sp!, {r2, r3, lr}
	mov	r1, #0			@ running remainder
	mov	r2, #32			@ bits to go
	mov	r3, #0			@ quotient
divide_loop:
	mov	r1, r1, lsl #1
	tst	r0, #0x80000000
	orrne	r1, r1, #1
	mov	r0, r0, lsl #1
	mov	r3, r3, lsl #1
	cmp	r1, #10
	subhs	r1, r1, #10
	orrhs	r3, r3, #1
	subs	r2, r2, #1
	bne	divide_loop
	mov	r0, r3
	ldmfd	sp!, {r2, r3, lr}
	mov	pc, lr


@ -------------------------------------------------------------- text and errors

msg_fitted:
	.string	"OPEN Bus co-processor card fitted, core "
	.align
msg_with:
	.string	", with "
	.align
msg_kbytes:
	.string	"K of card RAM."
	.align
msg_swi_note:
	.string	"SWI chunk &58D00 is provisional and awaits an allocation."
	.align
msg_state:
	.string	"Core: "
	.align
msg_running:
	.string	"running"
	.align
msg_halted:
	.string	"halted"
	.align
msg_faulted:
	.string	"faulted"
	.align
msg_stopped:
	.string	"stopped"
	.align
msg_pc:
	.string	"Program counter: &"
	.align
msg_cycles:
	.string	"Instructions retired: "
	.align
msg_mailbox:
	.string	"Mailbox from the core: &"
	.align
msg_fault:
	.string	"Fault "
	.align
msg_at:
	.string	" at &"
	.align
msg_loaded:
	.string	"Loaded "
	.align
msg_bytes:
	.string	" bytes into card RAM."
	.align
msg_stopped_with:
	.string	"Stopped with &"
	.align
msg_after:
	.string	" after "
	.align
msg_instructions:
	.string	" instructions."
	.align

err_no_card:
	.int	Error_NoCard
	.string	"No OPEN Bus co-processor card is fitted. Start the emulator with --openbus-card=rv32i, 6502 or z80."
	.align

err_range:
	.int	Error_Range
	.string	"Address or length outside the co-processor card's RAM"
	.align

err_busy:
	.int	Error_Busy
	.string	"The co-processor is running"
	.align

err_syntax:
	.int	Error_Syntax
	.string	"Bad parameters"
	.align

err_bad_swi:
	.int	Error_NoSuchSWI
	.string	"SWI value not known"
	.align

syntax_info:
	.string	"Syntax: *CoProInfo"
	.align
syntax_status:
	.string	"Syntax: *CoProStatus"
	.align
syntax_load:
	.string	"Syntax: *CoProLoad <filename> [<hex address>]"
	.align
syntax_run:
	.string	"Syntax: *CoProRun [<hex entry>]"
	.align

	.end
