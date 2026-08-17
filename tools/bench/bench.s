@ bench.s - a small RISC OS benchmark for comparing two emulators
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version.
@
@ Built as a RISC OS absolute file (&FF8) so it runs from the supervisor prompt
@ with one short command and prints its results, which is what makes it usable
@ in an emulator that has no HostCmd to drive it - the numbers can be read off
@ the screen, or captured directly where HostCmd does exist.
@
@ Four timings, chosen to separate the things a slow emulator can be slow at:
@
@   alu      a register-only loop. No memory traffic at all, so this is the
@            recompiler's raw instruction throughput and nothing else.
@   copy64k  LDM/STM through a 64KB working set: 16 pages, which any sensible
@            host-pointer cache holds.
@   copy2m   the same number of bytes moved through a 2MB working set: 512
@            pages. Against copy64k this says whether the cost is the memory
@            access itself or the page lookup behind it.
@   draw     Draw_Stroke on a real path - the mixed case, with SWIs (so
@            privilege changes) and screen writes in it.
@
@ Times are centiseconds from OS_ReadMonotonicTime.

	.arch	armv3
	.text
	.global	_start

	OS_WriteC		= 0x00
	OS_Write0		= 0x02
	OS_NewLine		= 0x03
	OS_ReadMonotonicTime	= 0x42
	OS_ConvertInteger4	= 0xdc
	OS_Exit			= 0x11
	OS_Module		= 0x1e
	Draw_Stroke		= 0x40702

	ALU_COUNT	= 2000000	@ iterations per alu pass
	ALU_PASSES	= 100
	COPY_SMALL	= 65520		@ bytes, a multiple of 24
	COPY_LARGE	= 2097144	@ 2MB less a bit, also a multiple of 24
	SMALL_OUTER	= 25600		@ chosen so both copies move the same bytes
	LARGE_OUTER	= 800
	DRAW_COUNT	= 3000

_start:
	@ Our own stack and our own buffers, rather than whatever the OS handed
	@ us: run from the supervisor prompt with no desktop, the application
	@ slot is small enough that both the stack push in report() and a two
	@ megabyte buffer abort on it.
	ldr	sp, =stack_top

	@ Two 2MB buffers out of the RMA, which has the whole machine behind it.
	mov	r0, #6			@ OS_Module 6 = claim
	ldr	r3, =0x400040
	swi	OS_Module
	ldr	r0, =bufptr
	str	r2, [r0]

	@ No mode change here on purpose: both machines boot the same ROM with
	@ the same CMOS and so are already in the same mode, and switching it
	@ redirects the output away from anything capturing it.

	@ --- alu: no memory traffic
	bl	time_now
	mov	r11, r0
	mov	r10, #ALU_PASSES
1:	ldr	r0, =ALU_COUNT
	bl	alu_loop
	subs	r10, r10, #1
	bne	1b
	adr	r4, str_alu
	bl	report

	@ --- copy through 64KB
	bl	time_now
	mov	r11, r0
	ldr	r0, =COPY_SMALL
	bl	buffers
	ldr	r3, =SMALL_OUTER
	bl	copy_loop
	adr	r4, str_copy64k
	bl	report

	@ --- the same bytes through 2MB
	bl	time_now
	mov	r11, r0
	ldr	r0, =COPY_LARGE
	bl	buffers
	mov	r3, #LARGE_OUTER
	bl	copy_loop
	adr	r4, str_copy2m
	bl	report

	@ --- Draw_Stroke
	bl	time_now
	mov	r11, r0
	ldr	r9, =DRAW_COUNT
1:	ldr	r0, =path
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	mov	r4, #200
	mov	r5, #0
	mov	r6, #0
	swi	Draw_Stroke
	subs	r9, r9, #1
	bne	1b
	adr	r4, str_stroke
	bl	report

	swi	OS_Exit

@ R0 = iterations. Registers only - the point is that nothing here touches
@ memory, so what it measures is instruction dispatch.
alu_loop:
	mov	r1, #0
1:	add	r1, r1, #1
	sub	r2, r1, #1
	eor	r3, r2, r1
	add	r12, r3, r2
	cmp	r1, r0
	blt	1b
	mov	pc, lr

@ R0 = bytes per pass, R1 = source, R2 = destination, R3 = passes.
copy_loop:
	stmfd	sp!, {r4-r11, lr}
1:	mov	r12, r0
	mov	r4, r1
	mov	r5, r2
2:	ldmia	r4!, {r6-r11}
	stmia	r5!, {r6-r11}
	subs	r12, r12, #24
	bgt	2b
	subs	r3, r3, #1
	bgt	1b
	ldmfd	sp!, {r4-r11, pc}

@ R1 = source buffer, R2 = destination, from the claimed block.
buffers:
	ldr	r1, =bufptr
	ldr	r1, [r1]
	add	r1, r1, #64
	add	r2, r1, #0x200000
	mov	pc, lr

time_now:
	swi	OS_ReadMonotonicTime
	mov	pc, lr

@ R4 = label, R11 = start time. Prints "label <centiseconds>".
report:
	stmfd	sp!, {r4-r11, lr}
	bl	time_now
	sub	r5, r0, r11
	mov	r0, r4
	swi	OS_Write0
	mov	r0, #' '
	swi	OS_WriteC
	mov	r0, r5
	ldr	r1, =numbuf
	mov	r2, #16
	swi	OS_ConvertInteger4
	swi	OS_Write0
	swi	OS_NewLine
	ldmfd	sp!, {r4-r11, pc}

str_alu:	.asciz	"alu"
str_copy64k:	.asciz	"copy64k"
str_copy2m:	.asciz	"copy2m"
str_stroke:	.asciz	"drawstroke"
	.balign	4

@ A closed triangle, in Draw units (256 to the OS unit), big enough that
@ filling and stroking it is real work rather than rounding.
path:
	.int	2, 20*256, 20*256		@ move to
	.int	8, 1200*256, 900*256		@ line to
	.int	8, 600*256, 100*256		@ line to
	.int	8, 20*256, 20*256		@ close the shape
	.int	0				@ end of path

	.ltorg

numbuf:
	.space	32
	.balign	4

bufptr:
	.int	0

	.space	1024			@ our own stack, small but ours
stack_top:
