@ SharedClipboard - Shared Clipboard Helper
@
@ Copyright (C) 2026 Andy Timmins
@
@ The interface this speaks, and the design of both halves, is RiscOS
@ Cloverleaf's, from the RpcemuHelper module in their RPCEmu fork at
@ https://github.com/riscoscloverleaf/rpcemu (module.c and task.c,
@ Copyright (c) 2021 RiscOS Cloverleaf, 2-clause BSD). This is a fresh
@ implementation in assembler rather than a copy of their C, because there is no
@ RISC OS C toolchain in this build environment, but the protocol is theirs
@ verbatim so their module and this one are interchangeable: the same SWI, the
@ same reason codes, the same pollword, and the same idea of handing the host
@ RISC OS's own UCS conversion table so text is converted through the alphabet
@ the machine is actually configured for.
@
@ Their module also carried mouse wheel scrolling. That is not here: this
@ emulator does wheel scrolling itself. What is kept from their event handler is
@ the trick underneath it: RISC OS has no "the clipboard changed" event, so key
@ and mouse activity is used as the cue to go and look.
@
@ The Latin-1 UCS table below is NetSurf's, Copyright 2005 John M Bell, GPLv2,
@ by way of Cloverleaf's ucstables.c.
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ Text only. The filetype travels with the data, so images can be added without
@ the interface changing.

	.arch	armv3

	@ RISC OS SWIs
	OS_WriteC		= 0x00000
	OS_Write0		= 0x00002
	OS_NewLine		= 0x00003
	OS_Exit			= 0x00011
	XOS_CLI			= 0x20005
	XOS_Byte		= 0x20006
	XOS_ReadMonotonicTime	= 0x20042
	XOS_File		= 0x20008
	XOS_Module		= 0x2001e
	XOS_Claim		= 0x2001f
	XOS_Release		= 0x20020
	XOS_ConvertCardinal4	= 0x200d8
	XWimp_Initialise	= 0x600c0
	XWimp_Poll		= 0x600c7
	XWimp_PollIdle		= 0x600e1
	XWimp_CloseDown		= 0x600dd
	XWimp_StartTask		= 0x600de
	XWimp_SendMessage	= 0x600e7
	XWimp_SlotSize		= 0x600ec
	XWimp_TransferBlock	= 0x600f1

	@ The emulator's private SWI chunk, and the clipboard call within it. Both
	@ are Cloverleaf's numbers, kept so their module and this one can be
	@ swapped for one another.
	ARCEM_SWI_CHUNK		= 0x56ac0
	XARCEM_SWI_CLIPBOARD	= 0x76ad0		@ chunk + 0x10, X form

	CLIP_SETUP		= 1
	CLIP_HOST_SET		= 2
	CLIP_HOST_GET		= 3
	CLIP_HOST_CHECK		= 4

	@ Bits in our pollword. The host sets HOST_CHANGED; the event handler sets
	@ TICK to have the task go and look at the guest's clipboard.
	POLL_HOST_CHANGED	= 1
	POLL_TICK		= 2

	@ OS_Claim / events
	EventV			= 0x10
	Event_Mouse		= 10
	Event_Key		= 11
	OSByte_EnableEvent	= 14
	OSByte_DisableEvent	= 13
	OSByte_Alphabet		= 71

	@ Service calls
	Service_StartWimp	= 0x49

	@ Wimp messages
	Message_Quit		= 0x0
	Message_DataSave	= 0x1
	Message_DataSaveAck	= 0x2
	Message_DataLoad	= 0x3
	Message_DataLoadAck	= 0x4
	Message_RAMTransmit	= 0x6
	Message_RAMFetch	= 0x7
	Message_ClaimEntity	= 0x8
	Message_DataRequest	= 0x9

	@ "the clipboard", in ClaimEntity and DataRequest flags
	CLAIM_CLIPBOARD		= 1 << 2

	@ Wimp_Poll events
	EVENT_USERMSG		= 17
	EVENT_USERMSG_R		= 18
	EVENT_USERMSG_ACK	= 19
	EVENT_POLLWORD		= 13

	@ Wimp_Poll mask: no nulls, no redraws (we have no windows), and R3 is a
	@ pollword. The idle form wants nulls, since that is how the wait ends.
	POLL_MASK		= 0x00400001
	POLL_MASK_IDLE		= 0x00400000

	@ How long to leave an application to finish claiming the clipboard before
	@ asking it what it has, in centiseconds.
	CHECK_DELAY		= 25

	@ Wimp_SendMessage event codes
	SEND_USER		= 17
	SEND_RECORDED		= 18

	TASK_MAGIC		= 0x4b534154		@ "TASK"
	WIMP_VERSION		= 310

	FILETYPE_TEXT		= 0xfff

	@ Message block offsets
	MSG_SIZE		= 0
	MSG_SENDER		= 4
	MSG_MYREF		= 8
	MSG_YOURREF		= 12
	MSG_ACTION		= 16
	MSG_DATA		= 20
	@ ...for a data transfer
	XFER_ESTSIZE		= 36
	XFER_FILETYPE		= 40
	XFER_NAME		= 44
	@ ...for a RAM transfer
	RAM_ADDR		= 20
	RAM_SIZE		= 24

	@ How much of the recent past *SharedClipLog remembers.
	TRACE_MAX		= 48

	@ Our clipboard buffer, claimed from the RMA when the task starts.
	CLIP_MAX		= 64 * 1024

	NBIT			= 1 << 31


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Module header
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	.int	task_start	@ Start (entered as an application)
	.int	mod_init	@ Initialisation
	.int	mod_final	@ Finalisation
	.int	mod_service	@ Service Call
	.int	title		@ Title String
	.int	help		@ Help String
	.int	commands	@ Help and Command keyword table
	.int	0		@ SWI Chunk base
	.int	0		@ SWI handler code
	.int	0		@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	flags		@ Module Flags

title:
	.string	"SharedClipboard"
	.align

	.include "version.inc"

flags:
	.int	1			@ 32-bit compatible


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Module initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Claim the event vector and ask for key and mouse events. Those are what tell us
@ to go and look at the guest's clipboard: RISC OS has no notification of its own,
@ and this is Cloverleaf's answer to that.
mod_init:
	stmfd	sp!, {r0, r1, r2, r3, lr}

	adrl	r0, task_handle
	mov	r1, #0
	str	r1, [r0]
	adrl	r0, task_wanted
	str	r1, [r0]
	adrl	r0, pollword
	str	r1, [r0]
	adrl	r0, own_clipboard
	str	r1, [r0]
	adrl	r0, clip_buffer
	str	r1, [r0]
	adrl	r0, clip_len
	str	r1, [r0]

	mov	r0, #EventV
	adrl	r1, event_handler
	mov	r2, #0
	swi	XOS_Claim

	mov	r0, #OSByte_EnableEvent
	mov	r1, #Event_Key
	swi	XOS_Byte
	mov	r0, #OSByte_EnableEvent
	mov	r1, #Event_Mouse
	swi	XOS_Byte

	cmp	pc, #0			@ clear V: initialised
	ldmfd	sp!, {r0, r1, r2, r3, pc}

	.ltorg

mod_final:
	stmfd	sp!, {r0, r1, r2, r3, lr}

	mov	r0, #OSByte_DisableEvent
	mov	r1, #Event_Mouse
	swi	XOS_Byte
	mov	r0, #OSByte_DisableEvent
	mov	r1, #Event_Key
	swi	XOS_Byte

	mov	r0, #EventV
	adrl	r1, event_handler
	mov	r2, #0
	swi	XOS_Release

	@ Tell the host we are gone, so it stops writing to a pollword that is
	@ about to be someone else's memory.
	mov	r0, #CLIP_SETUP
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	swi	XARCEM_SWI_CLIPBOARD

	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, r2, r3, pc}

	.ltorg

@ The desktop is starting: start our task. This is how a module gets to be a Wimp
@ task without anything being run from a disc.
mod_service:
	teq	r1, #Service_StartWimp
	movne	pc, lr

	@ Claim it, and hand back the command the Wimp should run as a task.
	@
	@ Two things to know here. Calling Wimp_StartTask from inside the service
	@ call instead re-enters the Wimp while it is still starting, and the desktop
	@ never comes up. And the Wimp keeps issuing this until nobody claims it, so
	@ that every module wanting a task gets one: claim it more than once and it
	@ starts another copy each time round, until the Wimp gives up with "Too many
	@ tasks". So claim exactly once.
	stmfd	sp!, {r2, lr}
	adrl	r2, task_wanted
	ldr	lr, [r2]
	teq	lr, #0
	bne	1f
	mov	lr, #1
	str	lr, [r2]
	adrl	r0, task_cmd
	mov	r1, #0			@ claim
1:
	ldmfd	sp!, {r2, lr}
	mov	pc, lr

	.ltorg

@ Key and mouse events. All we do is note that something happened, so the task
@ wakes up and asks whoever owns the clipboard whether its contents have changed.
@ This runs in interrupt context, so it does as little as possible.
event_handler:
	stmfd	sp!, {r0, r1, lr}

	adrl	r0, task_handle
	ldr	r0, [r0]
	teq	r0, #0
	beq	1f			@ no task, nothing to wake

	adrl	r0, own_clipboard
	ldr	r0, [r0]
	teq	r0, #0
	bne	1f			@ we own it: no need to ask anyone

	adrl	r0, pollword
	ldr	r1, [r0]
	orr	r1, r1, #POLL_TICK
	str	r1, [r0]
1:
	ldmfd	sp!, {r0, r1, lr}
	mov	pc, lr			@ pass the event on

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Commands
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

commands:
	.string	"Desktop_SharedClip"
	.align
	.int	command_desktop
	.int	0x00000000
	.int	0
	.int	help_desktop

	.string	"SharedClipStatus"
	.align
	.int	command_status
	.int	0x00000000
	.int	0
	.int	help_status

	.string	"SharedClipPut"
	.align
	.int	command_put
	.int	0x00ff0000		@ up to a line of text
	.int	0
	.int	help_put

	.string	"SharedClipGet"
	.align
	.int	command_get
	.int	0x00000000
	.int	0
	.int	help_get

	.string	"SharedClipLog"
	.align
	.int	command_log
	.int	0x00000000
	.int	0
	.int	help_log

	.int	0			@ end of the table

help_desktop:
	.string	"*Desktop_SharedClip starts the clipboard task. The desktop does this itself; there is no need to run it.\rSyntax: *Desktop_SharedClip"
	.align
help_status:
	.string	"*SharedClipStatus shows whether the clipboard is being shared with the host, and what is on it.\rSyntax: *SharedClipStatus"
	.align
help_put:
	.string	"*SharedClipPut puts text on the host's clipboard, which is a way of testing the connection without a desktop application.\rSyntax: *SharedClipPut <text>"
	.align
help_get:
	.string	"*SharedClipGet shows the text on the host's clipboard.\rSyntax: *SharedClipGet"
	.align
help_log:
	.string	"*SharedClipLog shows what the clipboard task has been doing, most recent last: 1xx a Wimp event, 2xx a message received, 3xxx something we did.\rSyntax: *SharedClipLog"
	.align

@ Enter ourselves as an application, which is what makes us a Wimp task.
command_desktop:
	stmfd	sp!, {r0, r1, lr}
	mov	r0, #2			@ OS_Module 2: enter
	adrl	r1, title
	swi	XOS_Module
	ldmfd	sp!, {r0, r1, pc}

	.ltorg

command_status:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}

	adrl	r0, text_status
	swi	OS_Write0

	adrl	r0, task_handle
	ldr	r0, [r0]
	teq	r0, #0
	beq	5f
	adrl	r0, text_task_yes
	b	6f
5:
	adrl	r0, text_task_no
6:
	swi	OS_Write0

	adrl	r0, registered
	ldr	r0, [r0]
	teq	r0, #0
	beq	7f
	adrl	r0, text_registered
	swi	OS_Write0
	b	8f
7:
	adrl	r0, text_unregistered
	swi	OS_Write0
8:
	adrl	r0, own_clipboard
	ldr	r0, [r0]
	teq	r0, #0
	beq	1f
	adrl	r0, text_own
	swi	OS_Write0
	adrl	r0, clip_len
	ldr	r0, [r0]
	bl	print_number
	adrl	r0, text_bytes
	swi	OS_Write0
1:
	@ What the host has, if anything.
	mov	r0, #CLIP_HOST_CHECK
	swi	XARCEM_SWI_CLIPBOARD
	bvs	2f
	teq	r0, #0
	beq	3f
	adrl	r1, text_host_has
	mov	r4, r0
	mov	r0, r1
	swi	OS_Write0
	mov	r0, r4
	bl	print_number
	adrl	r0, text_bytes
	swi	OS_Write0
	b	4f
3:
	adrl	r0, text_host_empty
	swi	OS_Write0
	b	4f
2:
	adrl	r0, text_off
	swi	OS_Write0
4:
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

	.ltorg

@ *SharedClipLog - what the task has seen and done, oldest first.
command_log:
	stmfd	sp!, {r0, r1, r2, r4, r5, lr}
	adrl	r0, text_log
	swi	OS_Write0
	adrl	r4, trace_pos
	ldr	r4, [r4]		@ oldest entry
	mov	r5, #0
1:
	adrl	r0, trace_ring
	ldr	r0, [r0, r4, lsl #2]
	teq	r0, #0
	beq	2f
	bl	print_hex
	mov	r0, #' '
	swi	OS_WriteC
2:
	add	r4, r4, #1
	cmp	r4, #TRACE_MAX
	movge	r4, #0
	add	r5, r5, #1
	cmp	r5, #TRACE_MAX
	blt	1b
	swi	OS_NewLine
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, r2, r4, r5, pc}

	.ltorg

@ *SharedClipPut <text> - hand text straight to the host. This is here to test
@ the connection from the command line, with no desktop and no application.
command_put:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	mov	r4, r0			@ the argument, terminated by CR

	@ Length up to the terminator.
	mov	r2, #0
1:
	ldrb	r3, [r4, r2]
	cmp	r3, #32
	blt	2f
	add	r2, r2, #1
	cmp	r2, #CLIP_MAX
	blt	1b
2:
	teq	r2, #0
	beq	3f

	mov	r0, #CLIP_HOST_SET
	mov	r1, r4
	ldr	r3, =FILETYPE_TEXT
	swi	XARCEM_SWI_CLIPBOARD
	bvs	4f
	adrl	r0, text_put_ok
	swi	OS_Write0
	b	3f
4:
	adrl	r0, text_off
	swi	OS_Write0
3:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

	.ltorg

@ *SharedClipGet - show what the host has, again for testing without a desktop.
command_get:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}

	mov	r0, #CLIP_HOST_CHECK
	swi	XARCEM_SWI_CLIPBOARD
	bvs	2f
	teq	r0, #0
	beq	3f

	@ Straight into the scratch buffer, which is big enough for anything the
	@ command line is going to be shown.
	mov	r4, r0
	cmp	r4, #1024
	movgt	r4, #1024
	mov	r0, #CLIP_HOST_GET
	adrl	r1, scratch
	mov	r2, r4
	swi	XARCEM_SWI_CLIPBOARD
	bvs	2f
	adrl	r0, scratch
	swi	OS_Write0
	swi	OS_NewLine
	b	4f
3:
	adrl	r0, text_host_empty
	swi	OS_Write0
	b	4f
2:
	adrl	r0, text_off
	swi	OS_Write0
4:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The task
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Entered as an application by *Desktop_SharedClip. From here on we are a Wimp
@ task, in user mode, with the module's own code and data still where they were.
task_start:
	adrl	sp, stack_top

	@ One is enough, whoever asked.
	adrl	r0, task_handle
	ldr	r0, [r0]
	teq	r0, #0
	bne	task_give_up

	@ A clipboard's worth of RMA, once.
	adrl	r0, clip_buffer
	ldr	r0, [r0]
	teq	r0, #0
	bne	1f
	mov	r0, #6			@ OS_Module 6: claim
	ldr	r3, =CLIP_MAX
	swi	XOS_Module
	bvs	task_give_up
	adrl	r0, clip_buffer
	str	r2, [r0]
1:
	ldr	r0, =WIMP_VERSION
	ldr	r1, =TASK_MAGIC
	adrl	r2, task_name
	adrl	r3, msg_list
	swi	XWimp_Initialise
	bvs	task_give_up
	adrl	r0, task_handle
	str	r1, [r0]

	@ We need almost nothing: hand the rest of the slot back.
	mov	r0, #1
	mvn	r1, #0
	swi	XWimp_SlotSize

	@ Tell the host where our pollword is, what alphabet we are using, and how
	@ that alphabet maps to UCS-4. Cloverleaf's idea: the host then converts
	@ text through the guest's own table rather than assuming one.
	mov	r0, #OSByte_Alphabet
	mov	r1, #127
	swi	XOS_Byte
	movvs	r2, #101		@ Latin-1 if it will not say
	teq	r2, #0
	moveq	r2, #101
	mov	r4, r2

	adrl	r0, alphabet
	str	r4, [r0]
	bl	tell_the_host

	@ Anything already on the host's clipboard is worth having now.
	adrl	r0, pollword
	mov	r1, #POLL_HOST_CHANGED
	str	r1, [r0]

poll_loop:
	@ With a check pending, wake when it is due; otherwise sleep until something
	@ happens.
	adrl	r0, check_at
	ldr	r2, [r0]
	teq	r2, #0
	beq	1f
	ldr	r0, =POLL_MASK_IDLE
	adrl	r1, poll_block
	adrl	r3, pollword
	swi	XWimp_PollIdle
	b	2f
1:
	ldr	r0, =POLL_MASK
	adrl	r1, poll_block
	adrl	r3, pollword
	swi	XWimp_Poll
2:
	bvs	poll_loop

	mov	r4, r0			@ the event, kept clear of the handlers

	orr	r0, r4, #0x1000		@ 1xx: an event from Wimp_Poll
	bl	trace

	teq	r4, #0			@ Null: the wait is over
	bleq	handle_null
	teq	r4, #EVENT_POLLWORD
	bleq	handle_pollword
	teq	r4, #EVENT_USERMSG
	teqne	r4, #EVENT_USERMSG_R
	bleq	handle_message
	b	poll_loop

task_quit:
	adrl	r0, task_wanted
	mov	r1, #0
	str	r1, [r0]
	@ Tell the host to stop knocking, then go.
	mov	r0, #CLIP_SETUP
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	swi	XARCEM_SWI_CLIPBOARD
	adrl	r0, task_handle
	mov	r1, #0
	str	r1, [r0]
	swi	XWimp_CloseDown
	swi	OS_Exit

task_give_up:
	adrl	r0, task_handle
	mov	r1, #0
	str	r1, [r0]
	swi	OS_Exit

	.ltorg

@ Tell the host where our pollword is, what alphabet we are using, and how that
@ alphabet maps to UCS-4. Cloverleaf's idea: the host then converts text through
@ the guest's own table rather than assuming one.
@
@ The emulator does not claim this SWI at all while sharing is switched off, so
@ this can fail, and the setting can be turned on long after we started. Note
@ whether it took, and the caller tries again later.
tell_the_host:
	stmfd	sp!, {r0, r1, r2, r3, lr}
	adrl	r0, alphabet
	ldr	r2, [r0]
	mov	r0, #CLIP_SETUP
	adrl	r1, pollword
	adrl	r3, ucs_table
	swi	XARCEM_SWI_CLIPBOARD
	adrl	r0, registered
	movvs	r1, #0
	movvc	r1, #1
	str	r1, [r0]
	ldmfd	sp!, {r0, r1, r2, r3, pc}

	.ltorg

@ Something set our pollword: either the host's clipboard changed, or there has
@ been enough activity that the guest's is worth a look.
handle_pollword:
	stmfd	sp!, {r4, lr}

	adrl	r0, pollword
	ldr	r4, [r0]
	mov	r1, #0
	str	r1, [r0]

	@ Still not registered? Sharing may have been switched on since we started.
	adrl	r0, registered
	ldr	r0, [r0]
	teq	r0, #0
	bleq	tell_the_host

	tst	r4, #POLL_HOST_CHANGED
	blne	take_host_clipboard

	tst	r4, #POLL_TICK
	blne	schedule_check

	ldmfd	sp!, {r4, pc}

@ Ask again shortly. Whoever copied needs a moment to claim the clipboard before
@ there is anything to ask them for.
schedule_check:
	stmfd	sp!, {r0, r1, lr}
	adrl	r1, check_at
	ldr	r0, [r1]
	teq	r0, #0
	bne	1f			@ already waiting
	swi	XOS_ReadMonotonicTime
	add	r0, r0, #CHECK_DELAY
	teq	r0, #0			@ never store the "nothing pending" value
	moveq	r0, #1
	str	r0, [r1]
1:
	ldmfd	sp!, {r0, r1, pc}

	.ltorg

@ The wait is over: go and ask.
handle_null:
	stmfd	sp!, {r0, r1, r2, lr}
	adrl	r2, check_at
	ldr	r1, [r2]
	teq	r1, #0
	beq	1f
	swi	XOS_ReadMonotonicTime
	subs	r0, r0, r1
	bmi	1f			@ not yet
	mov	r0, #0
	str	r0, [r2]
	bl	ask_who_owns_it
1:
	ldmfd	sp!, {r0, r1, r2, pc}

	.ltorg

@ The host has new text. Fetch it and tell the desktop we are holding the
@ clipboard, so applications come to us when they paste.
take_host_clipboard:
	stmfd	sp!, {r4, r5, lr}

	mov	r0, #CLIP_HOST_CHECK
	swi	XARCEM_SWI_CLIPBOARD
	bvs	1f
	teq	r0, #0
	beq	1f
	mov	r4, r0			@ how much there is, before r0 is reused
	ldr	r0, =FILETYPE_TEXT
	teq	r1, r0
	bne	1f			@ text only, for now

	ldr	r0, =CLIP_MAX
	cmp	r4, r0
	movgt	r4, r0

	adrl	r0, clip_buffer
	ldr	r5, [r0]
	teq	r5, #0
	beq	1f

	mov	r0, #CLIP_HOST_GET
	mov	r1, r5
	mov	r2, r4
	swi	XARCEM_SWI_CLIPBOARD
	bvs	1f
	@ r0 is the length actually written, without the terminator.
	adrl	r1, clip_len
	str	r0, [r1]
	adrl	r1, own_clipboard
	mov	r0, #1
	str	r0, [r1]

	ldr	r0, =0x3001		@ took the host's text
	bl	trace
	bl	claim_clipboard
1:
	ldmfd	sp!, {r4, r5, pc}

	.ltorg

@ Broadcast Message_ClaimEntity for the clipboard, which is how a task says it is
@ holding it.
claim_clipboard:
	stmfd	sp!, {r0, r1, r2, r4, lr}
	adrl	r4, msg_block

	mov	r0, #24
	str	r0, [r4, #MSG_SIZE]
	mov	r0, #0
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_ClaimEntity
	str	r0, [r4, #MSG_ACTION]
	mov	r0, #CLAIM_CLIPBOARD
	str	r0, [r4, #MSG_DATA]

	mov	r0, #SEND_USER
	mov	r1, r4
	mov	r2, #0			@ everyone
	swi	XWimp_SendMessage
	ldr	r0, =0x3002		@ told everyone we hold it
	ldrvs	r0, =0x3003		@ ...or failed to
	bl	trace
	ldmfd	sp!, {r0, r1, r2, r4, pc}

	.ltorg

@ Ask whoever owns the clipboard for its contents, so a copy made in the guest
@ reaches the host.
ask_who_owns_it:
	stmfd	sp!, {r0, r1, r2, r4, lr}

	@ Nothing to ask if the clipboard is ours.
	adrl	r0, own_clipboard
	ldr	r0, [r0]
	teq	r0, #0
	bne	2f

	adrl	r4, msg_block

	mov	r0, #48
	str	r0, [r4, #MSG_SIZE]
	mov	r0, #0
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_DataRequest
	str	r0, [r4, #MSG_ACTION]
	mvn	r0, #0
	str	r0, [r4, #MSG_DATA]		@ window: none
	mov	r0, #0
	str	r0, [r4, #MSG_DATA + 4]		@ icon
	str	r0, [r4, #MSG_DATA + 8]		@ x
	str	r0, [r4, #MSG_DATA + 12]	@ y
	mov	r0, #CLAIM_CLIPBOARD
	str	r0, [r4, #MSG_DATA + 16]	@ flags: the clipboard
	ldr	r0, =FILETYPE_TEXT
	str	r0, [r4, #MSG_DATA + 20]
	mvn	r0, #0
	str	r0, [r4, #MSG_DATA + 24]	@ end of the list

	mov	r0, #SEND_RECORDED
	mov	r1, r4
	mov	r2, #0
	swi	XWimp_SendMessage
	bvs	1f
	ldr	r0, [r4, #MSG_MYREF]
	adrl	r1, check_ref
	str	r0, [r1]
	ldr	r0, =0x3004		@ asked who owns the clipboard
	bl	trace
1:
2:
	ldmfd	sp!, {r0, r1, r2, r4, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Messages
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

handle_message:
	stmfd	sp!, {r4, r5, lr}
	adrl	r4, poll_block
	ldr	r5, [r4, #MSG_ACTION]

	orr	r0, r5, #0x2000		@ 2xx: a message received
	bl	trace

	@ Our own? A broadcast is delivered to its sender as well, and answering our
	@ own request starts a conversation with ourselves that uses up the reference
	@ the real answer has to match - so the application's reply gets thrown away
	@ as stale. Nothing here ever needs to hear from itself.
	ldr	r0, [r4, #MSG_SENDER]
	adrl	r1, task_handle
	ldr	r1, [r1]
	teq	r0, r1
	ldreq	r0, =0x3020		@ ignored our own message
	bleq	trace
	beq	2f

	teq	r5, #Message_Quit
	beq	task_quit

	teq	r5, #Message_ClaimEntity
	bleq	msg_claim_entity
	teq	r5, #Message_DataRequest
	bleq	msg_data_request
	teq	r5, #Message_DataSaveAck
	bleq	msg_data_save_ack
	teq	r5, #Message_RAMFetch
	bleq	msg_ram_fetch
	teq	r5, #Message_DataSave
	bleq	msg_data_save
	teq	r5, #Message_DataLoad
	bleq	msg_data_load
2:
	ldmfd	sp!, {r4, r5, pc}

	.ltorg

@ Someone else is holding the clipboard now. Drop ours, and go and ask them what
@ is on it.
msg_claim_entity:
	stmfd	sp!, {r0, r1, lr}
	adrl	r0, poll_block
	ldr	r0, [r0, #MSG_DATA]
	and	r0, r0, #0xff
	orr	r0, r0, #0x4100			@ 41xx: a claim's flags
	bl	trace
	adrl	r0, poll_block
	ldr	r1, [r0, #MSG_SENDER]
	adrl	r0, task_handle
	ldr	r0, [r0]
	teq	r0, r1
	beq	1f			@ our own broadcast

	adrl	r0, poll_block
	ldr	r0, [r0, #MSG_DATA]
	tst	r0, #CLAIM_CLIPBOARD
	beq	1f

	adrl	r0, own_clipboard
	mov	r1, #0
	str	r1, [r0]
	bl	schedule_check
1:
	ldmfd	sp!, {r0, r1, pc}

	.ltorg

@ An application wants to paste, and we are holding the clipboard: offer it.
msg_data_request:
	stmfd	sp!, {r0, r1, r2, r4, r5, lr}
	adrl	r4, poll_block

	@ Note the flags we were sent, and why we turn a request down.
	ldr	r0, [r4, #MSG_DATA + 16]
	and	r0, r0, #0xff
	orr	r0, r0, #0x4000			@ 40xx: a request's flags
	bl	trace

	ldr	r0, [r4, #MSG_DATA + 16]	@ flags
	tst	r0, #CLAIM_CLIPBOARD
	ldreq	r0, =0x3010			@ not a clipboard request
	bleq	trace
	beq	1f
	adrl	r0, own_clipboard
	ldr	r0, [r0]
	teq	r0, #0
	ldreq	r0, =0x3011			@ we do not hold the clipboard
	bleq	trace
	beq	1f
	adrl	r0, clip_len
	ldr	r0, [r0]
	teq	r0, #0
	ldreq	r0, =0x3012			@ we hold it but have nothing
	bleq	trace
	beq	1f

	mov	r5, r0			@ how much we have
	ldr	r0, [r4, #MSG_SENDER]
	mov	r2, r0			@ who asked

	@ Turn their request into our offer, in place.
	ldr	r0, [r4, #MSG_MYREF]
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_DataSave
	str	r0, [r4, #MSG_ACTION]
	str	r5, [r4, #XFER_ESTSIZE]
	ldr	r0, =FILETYPE_TEXT
	str	r0, [r4, #XFER_FILETYPE]
	add	r0, r4, #XFER_NAME
	adrl	r1, name_clipboard
	bl	copy_string_cr
	mov	r0, #64
	str	r0, [r4, #MSG_SIZE]

	mov	r0, #SEND_USER
	mov	r1, r4
	swi	XWimp_SendMessage
	ldr	r0, =0x3005		@ offered our text to an application
	ldrvs	r0, =0x3006		@ ...or failed to
	bl	trace
	bvs	1f
	ldr	r0, [r4, #MSG_MYREF]
	adrl	r1, paste_ref
	str	r0, [r1]
	adrl	r1, paste_offset
	mov	r0, #0
	str	r0, [r1]
1:
	ldmfd	sp!, {r0, r1, r2, r4, r5, pc}

	.ltorg

@ They have given us a filename to save our text into.
msg_data_save_ack:
	stmfd	sp!, {r0, r1, r2, r3, r4, r5, lr}
	adrl	r4, poll_block

	ldr	r0, [r4, #MSG_YOURREF]
	adrl	r1, paste_ref
	ldr	r1, [r1]
	teq	r0, r1
	bne	1f
	adrl	r0, paste_ref
	mov	r1, #0
	str	r1, [r0]

	@ OS_File 10: save a block of memory as a typed file.
	adrl	r0, clip_buffer
	ldr	r4, [r0]		@ start of the data
	adrl	r0, clip_len
	ldr	r0, [r0]
	add	r5, r4, r0		@ end of the data
	adrl	r1, poll_block
	add	r1, r1, #XFER_NAME
	ldr	r2, =0xffffff00		@ load address: a text file
	mov	r3, #0
	mov	r0, #10
	swi	XOS_File
	bvs	1f

	@ Saved: tell them to load it.
	adrl	r4, poll_block
	ldr	r0, [r4, #MSG_MYREF]
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_DataLoad
	str	r0, [r4, #MSG_ACTION]
	ldr	r2, [r4, #MSG_SENDER]
	mov	r0, #SEND_RECORDED
	mov	r1, r4
	swi	XWimp_SendMessage
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, r5, pc}

	.ltorg

@ They would rather have it in memory. Copy it into their space and say so.
msg_ram_fetch:
	stmfd	sp!, {r0, r1, r2, r3, r4, r5, lr}
	adrl	r4, poll_block

	ldr	r0, [r4, #MSG_YOURREF]
	adrl	r1, paste_ref
	ldr	r1, [r1]
	teq	r0, r1
	bne	1f

	@ How much is left, and how much they will take this time.
	adrl	r0, paste_offset
	ldr	r1, [r0]
	adrl	r0, clip_len
	ldr	r5, [r0]
	subs	r5, r5, r1		@ what remains
	movmi	r5, #0
	ldr	r0, [r4, #RAM_SIZE]
	cmp	r5, r0
	movgt	r5, r0			@ no more than they asked for
	teq	r5, #0
	beq	1f

	ldr	r2, [r4, #MSG_SENDER]
	ldr	r3, [r4, #RAM_ADDR]
	adrl	r0, clip_buffer
	ldr	r1, [r0]
	adrl	r0, paste_offset
	ldr	r0, [r0]
	add	r1, r1, r0		@ carry on from where we left off
	adrl	r0, task_handle
	ldr	r0, [r0]		@ source task (us)
	mov	r4, r5			@ length
	swi	XWimp_TransferBlock
	adrl	r4, poll_block
	bvs	1f

	ldr	r0, [r4, #RAM_SIZE]
	adrl	r1, ram_size_req
	str	r0, [r1]
	ldr	r0, [r4, #MSG_MYREF]
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_RAMTransmit
	str	r0, [r4, #MSG_ACTION]
	str	r5, [r4, #RAM_SIZE]
	mov	r0, #28
	str	r0, [r4, #MSG_SIZE]
	ldr	r2, [r4, #MSG_SENDER]

	@ Filling their buffer means there may be more to come, so keep the
	@ conversation open (a recorded message, and remember the reference); giving
	@ them less than they asked for is how the protocol says "that is all".
	adrl	r0, paste_offset
	ldr	r1, [r0]
	add	r1, r1, r5
	str	r1, [r0]
	adrl	r0, ram_size_req
	ldr	r0, [r0]
	cmp	r5, r0
	movlt	r0, #SEND_USER
	movge	r0, #SEND_RECORDED
	mov	r1, r4
	stmfd	sp!, {r0}
	swi	XWimp_SendMessage
	ldmfd	sp!, {r0}
	adrl	r1, paste_ref
	teq	r0, #SEND_RECORDED
	ldreq	r0, [r4, #MSG_MYREF]
	movne	r0, #0
	str	r0, [r1]
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, r5, pc}

	.ltorg

@ A reply to our DataRequest: they own the clipboard and are offering us its
@ contents. Ask for it in a scrap file, which every application can do.
msg_data_save:
	stmfd	sp!, {r0, r1, r2, r4, lr}
	adrl	r4, poll_block

	ldr	r0, [r4, #MSG_YOURREF]
	adrl	r1, check_ref
	ldr	r1, [r1]
	teq	r0, r1
	bne	1f
	teq	r0, #0
	beq	1f

	ldr	r0, [r4, #XFER_FILETYPE]
	ldr	r1, =FILETYPE_TEXT
	teq	r0, r1
	bne	1f
	ldr	r0, [r4, #XFER_ESTSIZE]
	teq	r0, #0
	beq	1f

	ldr	r0, [r4, #MSG_MYREF]
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_DataSaveAck
	str	r0, [r4, #MSG_ACTION]
	mvn	r0, #0
	str	r0, [r4, #XFER_ESTSIZE]		@ -1: it is a scrap file
	add	r0, r4, #XFER_NAME
	adrl	r1, name_scrap
	bl	copy_string_cr
	mov	r0, #60
	str	r0, [r4, #MSG_SIZE]

	ldr	r2, [r4, #MSG_SENDER]
	mov	r0, #SEND_RECORDED
	mov	r1, r4
	swi	XWimp_SendMessage
	bvs	1f
	ldr	r0, [r4, #MSG_MYREF]
	adrl	r1, check_ref
	str	r0, [r1]
1:
	ldmfd	sp!, {r0, r1, r2, r4, pc}

	.ltorg

@ They have saved the clipboard into the scrap file: read it and hand it to the
@ host.
msg_data_load:
	stmfd	sp!, {r0, r1, r2, r3, r4, r5, lr}
	adrl	r4, poll_block

	ldr	r0, [r4, #MSG_YOURREF]
	adrl	r1, check_ref
	ldr	r1, [r1]
	teq	r0, r1
	bne	1f
	adrl	r0, check_ref
	mov	r1, #0
	str	r1, [r0]

	@ OS_File 16: load the named file at our buffer.
	adrl	r0, clip_buffer
	ldr	r2, [r0]
	teq	r2, #0
	beq	1f
	add	r1, r4, #XFER_NAME
	mov	r3, #0
	mov	r0, #16
	swi	XOS_File
	bvs	2f
	@ r4 is the length loaded.
	adrl	r0, clip_len
	str	r4, [r0]

	adrl	r0, clip_buffer
	ldr	r1, [r0]
	mov	r0, #CLIP_HOST_SET
	adrl	r2, clip_len
	ldr	r2, [r2]
	ldr	r3, =FILETYPE_TEXT
	swi	XARCEM_SWI_CLIPBOARD
2:
	@ Acknowledge either way, so the sender is not left waiting.
	adrl	r4, poll_block
	ldr	r0, [r4, #MSG_MYREF]
	str	r0, [r4, #MSG_YOURREF]
	mov	r0, #Message_DataLoadAck
	str	r0, [r4, #MSG_ACTION]
	ldr	r2, [r4, #MSG_SENDER]
	mov	r0, #SEND_USER
	mov	r1, r4
	swi	XWimp_SendMessage
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, r5, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Small change
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Note r0 in the ring, so *SharedClipLog can say what happened. Small enough to
@ call from anywhere, including where a message is being handled.
trace:
	stmfd	sp!, {r0, r1, r2, r3, lr}
	@ Keep the flags. Callers test a condition, note it with a call to here, and
	@ then branch on that condition: without this the branch tests the
	@ arithmetic below instead, which quietly inverts decisions. It did exactly
	@ that to the paste handshake.
	mrs	lr, cpsr
	stmfd	sp!, {lr}
	adrl	r1, trace_pos
	ldr	r2, [r1]
	adrl	r3, trace_ring
	str	r0, [r3, r2, lsl #2]
	add	r2, r2, #1
	cmp	r2, #TRACE_MAX
	movge	r2, #0
	str	r2, [r1]
	ldmfd	sp!, {lr}
	msr	cpsr_f, lr
	ldmfd	sp!, {r0, r1, r2, r3, pc}

	.ltorg

@ Copy the NUL-terminated string at r1 to r0, terminated with a CR as Wimp
@ messages carry their names.
copy_string_cr:
	stmfd	sp!, {r0, r1, r2, lr}
1:
	ldrb	r2, [r1], #1
	teq	r2, #0
	beq	2f
	strb	r2, [r0], #1
	b	1b
2:
	mov	r2, #13
	strb	r2, [r0]
	ldmfd	sp!, {r0, r1, r2, pc}

@ Print r0 as hex.
print_hex:
	stmfd	sp!, {r0, r1, r2, lr}
	adrl	r1, scratch
	mov	r2, #16
	swi	0x200d2		@ XOS_ConvertHex4
	swivc	OS_Write0
	ldmfd	sp!, {r0, r1, r2, pc}

@ Print r0 in decimal.
print_number:
	stmfd	sp!, {r0, r1, r2, lr}
	adrl	r1, scratch
	mov	r2, #16
	swi	XOS_ConvertCardinal4
	@ On exit r0 points at the string and r1 at its terminator, not the other
	@ way about.
	swivc	OS_Write0
	ldmfd	sp!, {r0, r1, r2, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Text and tables
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

task_name:
	.string	"Shared Clipboard"
	.align
task_cmd:
	.string	"Desktop_SharedClip"
	.align
name_clipboard:
	.string	"Clipboard"
	.align
name_scrap:
	.string	"<Wimp$Scrap>"
	.align

text_status:
	.string	"Shared clipboard: "
	.align
text_task_yes:
	.string	"task running\r\n"
	.align
text_task_no:
	.string	"no task (the desktop starts it)\r\n"
	.align
text_own:
	.string	"  Holding the clipboard for the host, "
	.align
text_bytes:
	.string	" bytes\r\n"
	.align
text_host_has:
	.string	"  The host has "
	.align
text_host_empty:
	.string	"  The host's clipboard is empty\r\n"
	.align
text_registered:
	.string	"  Connected to the host\r\n"
	.align
text_unregistered:
	.string	"  Not connected to the host yet\r\n"
	.align
text_log:
	.string	"Recent activity (1xx event, 2xx message in, 3xxx action):\r\n"
	.align
text_off:
	.string	"  Not enabled in the emulator (Settings, Share Clipboard with RISC OS)\r\n"
	.align
text_put_ok:
	.string	"Put on the host's clipboard\r\n"
	.align

	@ Messages we want to hear about.
	@ Message_Quit is not listed: its number is 0, which is what ends the list,
	@ and the Wimp delivers it whether or not it is asked for. Listing it first
	@ registers for nothing, which is a quiet way to receive no messages at all.
msg_list:
	.int	Message_DataSave
	.int	Message_DataSaveAck
	.int	Message_DataLoad
	.int	Message_DataLoadAck
	.int	Message_RAMTransmit
	.int	Message_RAMFetch
	.int	Message_ClaimEntity
	.int	Message_DataRequest
	.int	0

	@ The RISC OS Latin-1 alphabet as UCS-4. NetSurf's table (ucstables.c,
	@ Copyright 2005 John M Bell, GPLv2), by way of Cloverleaf's module. 0xffffffff
	@ marks a character with no equivalent.
ucs_table:
	.include "ucstable.inc"


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Workspace
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	@ The host writes to this, so it must not move once we have told it where
	@ it is. The module image stays put in the RMA, so it does not.
pollword:
	.int	0
task_handle:
	.int	0
task_wanted:
	.int	0
registered:
	.int	0
alphabet:
	.int	101
own_clipboard:
	.int	0
clip_buffer:
	.int	0
clip_len:
	.int	0
check_ref:
	.int	0
check_at:
	.int	0
paste_ref:
	.int	0
paste_offset:
	.int	0
ram_size_req:
	.int	0

trace_pos:
	.int	0
trace_ring:
	.space	TRACE_MAX * 4, 0

scratch:
	.space	1088, 0
poll_block:
	.space	256, 0
msg_block:
	.space	256, 0

	.space	1024, 0
stack_top:

	.end
