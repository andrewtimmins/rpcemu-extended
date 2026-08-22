@ CoProTest - exercise the RPCEmuCoPro module's emulator-facing SWIs
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ ★ WHY THIS EXISTS. The eight SWIs the module provides are ARM assembler, and no
@ host-side test can execute ARM assembler: tests/test_openbus_coproc.c drives the
@ same card through its registers, which proves the card and proves nothing about
@ the module. A clobbered register, a loop that runs one short, an aperture left
@ pointing somewhere - all of it is invisible until something calls the SWIs from
@ RISC OS. This is that something.
@
@ It maps a machine with a screen region, a latch and a stalling register, runs a
@ 6502 program against it, drains the log and checks what it got. Output goes
@ through OS_Write0 so a host driving this over HostCmd can read the verdict.
@
@ ★ IT IS A MODULE, NOT AN APPLICATION, and that is not a style choice.
@
@ It was written as an absolute file first and *Run over HostCmd never returned:
@ HostCmd drives the command line, and an application that takes over application
@ space and leaves through OS_Exit does not hand control back to whatever issued
@ the command, so the host waits for ever. A * command in a module runs in
@ supervisor mode, returns normally, and its output goes back down the channel.
@ Proved by a hello-world absolute file hanging the same way, which is what said
@ the mechanism was at fault rather than this program.
@
@ So: load it with *RMLoad and run *CoProSelfTest. It is deliberately NOT in
@ poduleroms - it is a test, and it has no business loading on every machine.

	.arch	armv3

	XOS_Write0		= 0x20002
	XOS_NewLine		= 0x20003
	V_bit			= 1 << 28

	@ The module's chunk. ★ PROVISIONAL - if RISC OS Open allocates a
	@ different one this constant and the module's must change together.
	CoPro			= 0x58d00
	XCoPro			= CoPro + 0x20000

	CoPro_Info		= XCoPro + 0
	CoPro_Reset		= XCoPro + 1
	CoPro_Write		= XCoPro + 2
	CoPro_Status		= XCoPro + 6
	CoPro_MapRegions	= XCoPro + 9
	CoPro_Latch		= XCoPro + 10
	CoPro_DrainLog		= XCoPro + 11
	CoPro_Interrupt		= XCoPro + 12
	CoPro_RunFor		= XCoPro + 13
	CoPro_Resume		= XCoPro + 14
	CoPro_Registers		= XCoPro + 15
	CoPro_Transfer		= XCoPro + 16

	@ Region kinds, from src/copro/copro_bus.h.
	KIND_RAM		= 0
	KIND_LOG		= 2
	KIND_LATCH		= 3
	KIND_STALL		= 4

	.global	_start
_start:

module_start:
	.int	0		@ Start (not runnable)
	.int	0		@ Initialisation
	.int	0		@ Finalisation
	.int	0		@ Service call
	.int	mod_title	@ Title
	.int	mod_help	@ Help
	.int	mod_table	@ Help and command keyword table
	.int	0		@ SWI chunk (none: this offers a command only)
	.int	0		@ SWI handler
	.int	0		@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Messages
	.int	mod_flags	@ Module flags

mod_flags:
	.int	1		@ 32-bit compatible

mod_title:
	.string	"CoProSelfTest"
	.align
mod_help:
	.string	"CoProSelfTest\t0.01 (20 Aug 2026) exercises RPCEmuCoPro's SWIs"
	.align

mod_table:
	.string	"CoProSelfTest"
	.align
	.int	cmd_selftest
	.int	0x00000000	@ no parameters: minimum 0, maximum 0
	.int	mod_syntax
	.int	mod_cmdhelp
	.byte	0		@ end of table
	.align

mod_syntax:
	.string	"Syntax: *CoProSelfTest"
	.align
mod_cmdhelp:
	.string	"*CoProSelfTest exercises the RPCEmuCoPro module's SWIs against\n\rwhatever co-processor card is fitted, and prints a line per check.\n\r"
	.align

@ The command itself. Runs in supervisor mode on the supervisor stack, so it
@ keeps its nesting shallow and its own buffers static rather than on the stack.
cmd_selftest:
	stmfd	sp!, {r0-r11, lr}

	@ Start from no failures, so running it twice means what it says.
	adrl	r0, failures
	mov	r1, #0
	str	r1, [r0]

	adrl	r0, msg_title
	bl	print_line

	bl	test_info
	bl	test_map_and_log
	bl	test_latch
	bl	test_registers
	bl	test_transfer
	bl	test_stall

	@ The verdict, in a form a host can grep for.
	@ ADRL has no unambiguous conditional form - "adrle" reads as ADR with the
	@ LE condition - so the choice is a branch rather than two conditional
	@ loads. The messages are at the end of the file, too far for plain ADR.
	adrl	r0, failures
	ldr	r4, [r0]
	cmp	r4, #0
	beq	verdict_passed
	adrl	r0, msg_failed
	b	verdict_say
verdict_passed:
	adrl	r0, msg_passed
verdict_say:
	bl	print_line

	@ A * command returns with V clear, through CPSR: putting a bit in LR and
	@ branching to it is the 26-bit idiom that aborts in 32-bit code.
	ldmfd	sp!, {r0-r11, lr}
	stmfd	sp!, {r0}
	mrs	r0, cpsr
	bic	r0, r0, #V_bit
	msr	cpsr_f, r0
	ldmfd	sp!, {r0}
	mov	pc, lr


@ ---------------------------------------------------------------- reporting

failures:
	.int	0

@ Print the string at r0, then a newline.
print_line:
	stmfd	sp!, {r0-r3, lr}
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, lr}
	mov	pc, lr

@ Print r0 as eight hex digits. Its own converter rather than OS_ConvertHex8,
@ because that returns the string in R0 and the pointer to its terminator in R1,
@ and getting those the wrong way round prints nothing at all - which is a
@ mistake this project has already made once.
print_hex:
	stmfd	sp!, {r0-r8, lr}
	adrl	r2, hexbuf
	mov	r3, #8
print_hex_digit:
	mov	r1, r0, lsr #28
	cmp	r1, #9
	addls	r1, r1, #'0'
	addhi	r1, r1, #'a' - 10
	strb	r1, [r2], #1
	mov	r0, r0, lsl #4
	subs	r3, r3, #1
	bne	print_hex_digit
	mov	r1, #0
	strb	r1, [r2]
	adrl	r0, hexbuf
	swi	XOS_Write0
	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ Report a check. In: r0 -> its name, r1 = non-zero if it passed.
check:
	stmfd	sp!, {r0-r8, lr}
	mov	r4, r0
	cmp	r1, #0
	beq	check_failed

	adrl	r0, msg_ok
	swi	XOS_Write0
	mov	r0, r4
	bl	print_line
	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

check_failed:
	adrl	r0, msg_fail
	swi	XOS_Write0
	mov	r0, r4
	swi	XOS_Write0
	@ Whatever the caller left in r2, in hex: a failure that only says it
	@ failed sends you back to the guest for another five minutes.
	adrl	r0, msg_got
	swi	XOS_Write0
	mov	r0, r2
	bl	print_hex
	swi	XOS_NewLine
	adrl	r0, failures
	ldr	r1, [r0]
	add	r1, r1, #1
	str	r1, [r0]
	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr


@ ------------------------------------------------------------------- tests

@ The module answers at all, and says which core is fitted.
test_info:
	stmfd	sp!, {r0-r8, lr}
	swi	CoPro_Info
	movvs	r1, #0
	movvc	r1, #1
	mov	r2, r0			@ the core id, for check to print
	adrl	r0, msg_info
	bl	check

	@ ★ Check the VALUE, not just that it returned. A SWI that answers without
	@ error but with rubbish is what a test that only looks at the V flag calls
	@ a pass - and if the handler had the wrong window, writing and reading one
	@ register would still agree with itself.
	swi	CoPro_Info
	mov	r2, r1			@ the card RAM size
	mov	r3, #0x10000		@ 64K, which is what a 6502 card carries
	cmp	r1, r3
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_ramsize
	bl	check
	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ ★ The one that matters most: a machine described in one call, a program run
@ against it, and its writes read back with the cycle each happened on.
@
@ The 6502 program, at 0: LDA #&aa : STA &4000 : LDA #&bb : STA &4001 : BRK
test_map_and_log:
	stmfd	sp!, {r0-r8, lr}

	@ Describe the machine: a screen at &4000 whose writes are recorded.
	adrl	r0, region_table
	mov	r1, #1
	swi	CoPro_MapRegions
	movvs	r1, #0
	movvc	r1, #1
	adrl	r0, msg_map
	bl	check

	@ Load the program through the module rather than by hand.
	adrl	r0, prog_6502
	mov	r1, #0			@ card address 0
	mov	r2, #prog_6502_end - prog_6502
	mov	r3, #0			@ to the card, card RAM
	swi	CoPro_Transfer
	movvs	r1, #0
	movvc	r1, #1
	adrl	r0, msg_load
	bl	check

	mov	r0, #0
	swi	CoPro_Reset
	mov	r0, #0			@ no budget: run until it stops
	swi	CoPro_RunFor

	@ Give it time to reach the BRK. The card runs on its share of the
	@ machine's cycles, so this returns immediately and the program is still
	@ going; a spin here is what a caller would really do.
	mov	r8, #0x800		@ NOT r0-r5: CoPro_Status returns in those
test_map_wait:
	swi	CoPro_Status
	tst	r0, #2			@ STATUS_HALTED; the status is in r0
	bne	test_map_halted
	subs	r8, r8, #1
	bne	test_map_wait
test_map_halted:
	mov	r2, r0			@ the status, for check to print
	tst	r0, #2
	movne	r1, #1
	moveq	r1, #0
	adrl	r0, msg_halted
	bl	check

	@ Drain the log and check both entries, including their addresses.
	adrl	r0, log_buffer
	mov	r1, #8
	swi	CoPro_DrainLog
	cmp	r2, #2
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_drained
	bl	check		@ r2 is the count, which check prints on failure

	adrl	r5, log_buffer
	ldr	r1, [r5, #4]		@ first entry's address
	ldr	r2, [r5, #8]		@ and value
	mov	r3, #0x4000
	cmp	r1, r3
	cmpeq	r2, #0xaa
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_entry1
	bl	check

	ldr	r1, [r5, #20]		@ second entry, sixteen bytes on
	ldr	r2, [r5, #24]
	add	r3, r3, #1
	cmp	r1, r3
	cmpeq	r2, #0xbb
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_entry2
	bl	check

	@ A second drain must find nothing: the first advanced the tail.
	adrl	r0, log_buffer
	mov	r1, #8
	swi	CoPro_DrainLog
	cmp	r2, #0
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_drained_twice
	bl	check

	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ A latch: the program reads what this program put there.
@ The 6502 program: LDA &d000 : STA &10 : BRK
test_latch:
	stmfd	sp!, {r0-r8, lr}

	adrl	r0, region_latch
	mov	r1, #1
	swi	CoPro_MapRegions

	mov	r0, #0			@ the latch offset in its region entry
	mov	r1, #0x5e
	swi	CoPro_Latch
	movvs	r1, #0
	movvc	r1, #1
	adrl	r0, msg_latch_set
	bl	check

	adrl	r0, prog_latch
	mov	r1, #0
	mov	r2, #prog_latch_end - prog_latch
	mov	r3, #0
	swi	CoPro_Transfer

	mov	r0, #0
	swi	CoPro_Reset
	mov	r0, #0
	swi	CoPro_RunFor

	mov	r8, #0x800		@ NOT r0-r5: CoPro_Status returns in those
test_latch_wait:
	swi	CoPro_Status
	tst	r0, #2
	bne	test_latch_done
	subs	r8, r8, #1
	bne	test_latch_wait
test_latch_done:

	@ The program stored what it read at &0010; read it back off the card.
	adrl	r0, byte_buffer
	mov	r1, #0x10
	mov	r2, #1
	mov	r3, #1			@ from the card
	swi	CoPro_Transfer
	adrl	r0, byte_buffer
	ldrb	r1, [r0]
	cmp	r1, #0x5e
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_latch_read
	bl	check

	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ The core's own registers, by its own numbering: for a 6502, 0 is A.
test_registers:
	stmfd	sp!, {r0-r8, lr}

	mov	r0, #1			@ write
	mov	r1, #0			@ register 0, the accumulator
	mov	r2, #0x3c
	swi	CoPro_Registers

	mov	r0, #0			@ read it back
	mov	r1, #0
	swi	CoPro_Registers
	cmp	r2, #0x3c
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_registers
	bl	check

	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ Transfer both ways, which is also what every test above relies on.
test_transfer:
	stmfd	sp!, {r0-r8, lr}

	adrl	r0, pattern
	mov	r1, #0x200
	mov	r2, #4
	mov	r3, #0
	swi	CoPro_Transfer

	adrl	r0, byte_buffer
	mov	r1, #0x200
	mov	r2, #4
	mov	r3, #1
	swi	CoPro_Transfer

	adrl	r0, byte_buffer
	ldr	r2, [r0]		@ what came back, for check to print
	adrl	r0, pattern
	ldr	r3, [r0]
	cmp	r2, r3
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_transfer
	bl	check

	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr

@ A stalling register: the core stops, this program answers, the core carries on.
@ The 6502 program: LDA &dc00 : STA &11 : BRK
test_stall:
	stmfd	sp!, {r0-r8, lr}

	adrl	r0, region_stall
	mov	r1, #1
	swi	CoPro_MapRegions

	adrl	r0, prog_stall
	mov	r1, #0
	mov	r2, #prog_stall_end - prog_stall
	mov	r3, #0
	swi	CoPro_Transfer

	mov	r0, #0
	swi	CoPro_Reset
	mov	r0, #0
	swi	CoPro_RunFor

	@ Wait for it to stop on the stalling read.
	mov	r8, #0x800		@ NOT r0-r5: CoPro_Status returns in those
test_stall_wait:
	swi	CoPro_Status
	tst	r0, #0x20		@ STATUS_WAITING; the status is in r0
	bne	test_stall_waiting
	subs	r8, r8, #1
	bne	test_stall_wait
test_stall_waiting:
	mov	r2, r0			@ the status, for check to print
	tst	r0, #0x20
	movne	r1, #1
	moveq	r1, #0
	adrl	r0, msg_waiting
	bl	check

	@ Answer it, and let it finish.
	mov	r0, #0x7d
	swi	CoPro_Resume
	movvs	r1, #0
	movvc	r1, #1
	adrl	r0, msg_resumed
	bl	check

	mov	r0, #0
	swi	CoPro_RunFor
	mov	r8, #0x800		@ NOT r0-r5: CoPro_Status returns in those
test_stall_finish:
	swi	CoPro_Status
	tst	r0, #2
	bne	test_stall_finished
	subs	r8, r8, #1
	bne	test_stall_finish
test_stall_finished:

	adrl	r0, byte_buffer
	mov	r1, #0x11
	mov	r2, #1
	mov	r3, #1
	swi	CoPro_Transfer
	adrl	r0, byte_buffer
	ldrb	r1, [r0]
	cmp	r1, #0x7d
	moveq	r1, #1
	movne	r1, #0
	adrl	r0, msg_stall_answer
	bl	check

	@ And answering when nothing is waiting must be refused, not ignored: a
	@ caller that thinks it answered something has lost track of the core.
	mov	r0, #0
	swi	CoPro_Resume
	movvs	r1, #1
	movvc	r1, #0
	adrl	r0, msg_resume_refused
	bl	check

	ldmfd	sp!, {r0-r8, lr}
	mov	pc, lr


@ ------------------------------------------------------------------- data

	.align
@ base, size, kind, latch/offset
region_table:
	.int	0x4000, 0x1b00, KIND_LOG, 0
region_latch:
	.int	0xd000, 0x0100, KIND_LATCH, 0
region_stall:
	.int	0xdc00, 0x0010, KIND_STALL, 0

prog_6502:
	.byte	0xa9, 0xaa		@ LDA #&aa
	.byte	0x8d, 0x00, 0x40	@ STA &4000
	.byte	0xa9, 0xbb		@ LDA #&bb
	.byte	0x8d, 0x01, 0x40	@ STA &4001
	.byte	0x00			@ BRK
prog_6502_end:

prog_latch:
	.byte	0xad, 0x00, 0xd0	@ LDA &d000
	.byte	0x85, 0x10		@ STA &10
	.byte	0x00			@ BRK
prog_latch_end:

prog_stall:
	.byte	0xad, 0x00, 0xdc	@ LDA &dc00
	.byte	0x85, 0x11		@ STA &11
	.byte	0x00			@ BRK
prog_stall_end:

	.align
pattern:
	.int	0x89abcdef

	.align
byte_buffer:
	.space	16
log_buffer:
	.space	8 * 16

msg_title:
	.string	"RPCEmuCoPro: the emulator-facing SWIs"
	.align
msg_ok:
	.string	"  ok   "
	.align
msg_got:
	.string	", got &"
	.align
hexbuf:
	.space	12
	.align
msg_fail:
	.string	"  FAIL "
	.align
msg_passed:
	.string	"COPROTEST-PASSED"
	.align
msg_failed:
	.string	"COPROTEST-FAILED"
	.align
msg_info:
	.string	"CoPro_Info answers"
	.align
msg_ramsize:
	.string	"and reports the card's real RAM size"
	.align
msg_map:
	.string	"CoPro_MapRegions describes a machine in one call"
	.align
msg_load:
	.string	"CoPro_Transfer put the program on the card"
	.align
msg_halted:
	.string	"the program ran to its BRK"
	.align
msg_drained:
	.string	"CoPro_DrainLog returned both writes"
	.align
msg_entry1:
	.string	"the first write was &aa to &4000"
	.align
msg_entry2:
	.string	"the second was &bb to &4001"
	.align
msg_drained_twice:
	.string	"a second drain finds nothing, so the tail advanced"
	.align
msg_latch_set:
	.string	"CoPro_Latch accepted a value"
	.align
msg_latch_read:
	.string	"and the program read it back"
	.align
msg_registers:
	.string	"CoPro_Registers wrote and read the accumulator"
	.align
msg_transfer:
	.string	"CoPro_Transfer round-tripped a word"
	.align
msg_waiting:
	.string	"a stalling read stopped the core"
	.align
msg_resumed:
	.string	"CoPro_Resume answered it"
	.align
msg_stall_answer:
	.string	"and the answer reached the program"
	.align
msg_resume_refused:
	.string	"resuming with nothing waiting is refused"
	.align

	.end
