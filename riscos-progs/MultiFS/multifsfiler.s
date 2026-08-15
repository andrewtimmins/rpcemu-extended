@ MultiFSFiler - a drive on the icon bar for USB media
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ MultiFS can read a USB stick; this is what makes it usable without knowing that.
@ A drive icon appears when one is plugged in, opens a Filer window when clicked,
@ and goes away when the stick does.
@
@ It is a Wimp task inside a module, which is how every filer on RISC OS is built
@ and how our own HostFSFiler is built: the Filer asks, through Service_StartFiler,
@ for the filers to start themselves, and each answers by entering its own module.
@
@ Whether a disc is there is decided by ASKING FILESWITCH rather than by talking to
@ MultiFS or to the USB stack. OS_File 5 on "MultiFS::0.$" answers 2 when there is a
@ mounted volume and 0 when there is not, which is exactly the question being asked
@ and needs no interface of our own between the two modules.
@
@ That check runs on the poll loop's one second idle rather than from a USB service
@ call. It is worth saying why, because a service call sounds like the tidier
@ answer: the stack below is several modules deep - USBDriver enumerates, SCSISoftUSB
@ claims the device, SCSIFS decides it has a disc - and a device is not readable at
@ the moment any of them first announces it. A filer that believed the announcement
@ would put up an icon for a drive that is not ready yet. Asking once a second costs
@ one OS_File on an idle machine and is right whenever it is asked.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	wp	.req	r12

	NBIT = 1 << 31

	XOS_CLI			= 0x20005
	XOS_File		= 0x20008
	XOS_Exit		= 0x20011
	XOS_Module		= 0x2001e
	XOS_FSControl		= 0x20029
	XOS_ReadModeVariable	= 0x20035
	XOS_ReadMonotonicTime	= 0x20042

	XWimp_Initialise	= 0x600c0
	XWimp_CreateIcon	= 0x600c2
	XWimp_DeleteIcon	= 0x600c4
	XWimp_CreateMenu	= 0x600d4
	XWimp_CloseDown		= 0x600dd
	XWimp_PollIdle		= 0x600e1
	XWimp_SpriteOp		= 0x600e9

	Module_Enter	= 2
	Module_Claim	= 6
	Module_Free	= 7

	Message_Quit		= 0

	Service_Reset		= 0x27
	Service_StartFiler	= 0x4b
	Service_StartedFiler	= 0x4c
	Service_FilerDying	= 0x4f

	SpriteOp_ReadSpriteInfo	= 40
	ModeVariable_YEig	= 5

	FSControl_CanonicalisePath = 37

	WIMP_VERSION	= 300

	@ Null events are wanted: they are what drives the check for a stick
	@ arriving or going. Pointer entering and leaving are not.
	WIMP_POLL_MASK	= 0x00000030

	WORKSPACE_SIZE	= 640

	WS_MY_TASK_HANDLE	= 0
	WS_FILER_TASK_HANDLE	= 4
	WS_ICON_HANDLE		= 8	@ 0 when there is no icon up
	WS_ICON_BAR_BLOCK	= 12	@ 9 words, 12-47
	WS_DELETE_BLOCK		= 48	@ 2 words, 48-55
	WS_NAME			= 56	@ the icon's text, 24 bytes
	WS_CANON		= 80	@ a canonicalised path, 128 bytes
	WS_WIMP_BLOCK		= 208	@ must be last

	.global	_start

_start:

module_start:

	.int	start		@ Start
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	service_pre	@ Service Call
	.int	modtitle	@ Title String
	.int	help		@ Help String
	.int	table		@ Help and Command keyword table
	.int	0		@ SWI Chunk base
	.int	0		@ SWI handler code
	.int	0		@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32 bit compatible

modtitle:
	.string	"MultiFSFiler"

	.include "multifsfiler-version.inc"


table:
desktop_multifsfiler:
	.string	"Desktop_MultiFSFiler"
	.align
	.int	command_desktop_multifsfiler
	.int	0x00070000
	.int	0
	.int	command_desktop_multifsfiler_help

	.byte	0	@ Table terminator

command_desktop_multifsfiler_help:
	.string	"The MultiFSFiler puts USB discs on the icon bar and opens them with the Filer.\rDo not use *Desktop_MultiFSFiler, use *Desktop instead."
	.align


init:
	stmfd	sp!, {lr}

	ldr	r0, [r12]
	teq	r0, #0
	bne	init_have_workspace

	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {pc}		@ no workspace, so decline to start

	str	r2, [r12]

init_have_workspace:
	ldr	wp, [r12]
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]
	str	r0, [wp, #WS_ICON_HANDLE]

	cmp	pc, #0
	ldmfd	sp!, {pc}


final:
	stmfd	sp!, {lr}

	ldr	wp, [r12]

	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	cmp	r0, #0
	ldrgt	r1, TASK
	swigt	XWimp_CloseDown

	mov	r0, #Module_Free
	mov	r2, r12
	swi	XOS_Module

	cmp	pc, #0
	ldmfd	sp!, {pc}


	@ RISC OS 4 service call table
service_codetable:
	.int	0
	.int	service_main
	.int	Service_Reset
	.int	Service_StartFiler
	.int	Service_StartedFiler
	.int	Service_FilerDying
	.int	0
	.int	service_codetable
service_pre:
	mov	r0, r0		@ magic: table pointer sits at service_pre-4
	teq	r1, #Service_Reset
	teqne	r1, #Service_StartFiler
	teqne	r1, #Service_StartedFiler
	teqne	r1, #Service_FilerDying
	movne	pc, lr

service_main:
	stmfd	sp!, {lr}

	ldr	wp, [r12]

	teq	r1, #Service_Reset
	beq	service_reset
	teq	r1, #Service_StartFiler
	beq	service_startfiler
	teq	r1, #Service_StartedFiler
	beq	service_startedfiler
	teq	r1, #Service_FilerDying
	beq	service_filerdying

	ldmfd	sp!, {pc}

service_reset:
	mov	r14, #0
	str	r14, [wp, #WS_MY_TASK_HANDLE]
	str	r14, [wp, #WS_ICON_HANDLE]
	ldmfd	sp!, {pc}

service_startfiler:
	ldr	r14, [wp, #WS_MY_TASK_HANDLE]
	teq	r14, #0			@ already running?
	mvneq	r14, #0			@ no: mark as starting
	streq	r14, [wp, #WS_MY_TASK_HANDLE]
	streq	r1, [wp, #WS_FILER_TASK_HANDLE]
	adreq	r0, desktop_multifsfiler
	moveq	r1, #0			@ claim the service
	ldmfd	sp!, {pc}

service_startedfiler:
	ldr	r14, [wp, #WS_MY_TASK_HANDLE]
	cmn	r14, #1
	moveq	r14, #0
	streq	r14, [wp, #WS_MY_TASK_HANDLE]
	ldmfd	sp!, {pc}

service_filerdying:
	stmfd	sp!, {r0-r1}
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	cmp	r0, #0
	movne	r14, #0
	strne	r14, [wp, #WS_MY_TASK_HANDLE]
	ldrgt	r1, TASK
	swigt	XWimp_CloseDown
	ldmfd	sp!, {r0-r1}
	ldmfd	sp!, {pc}


command_desktop_multifsfiler:
	stmfd	sp!, {lr}
	mov	r2, r0
	adr	r1, modtitle
	mov	r0, #Module_Enter
	swi	XOS_Module
	ldmfd	sp!, {pc}


TASK:
	.ascii	"TASK"

task_name:
	.string	"USB Disc Filer"
	.align

	@ The icon's text is indirected into the workspace, because it is the
	@ disc's own name and is not known until one is plugged in.
icon_bar_block:
	.int	-5		@ left side of the icon bar, scanning from the left
	.int	0		@ minimum X
	.int	-16		@ minimum Y
	.int	96		@ maximum X
	.int	20		@ maximum Y, before the sprite's height is added
	.int	0x1700310b	@ indirected text and sprite
	.int	0		@ filled in: text
	.int	0		@ filled in: validation
	.int	20		@ length of the text buffer

icon_bar_validation:
	.ascii	"S"		@ deliberately unterminated: the name follows
icon_bar_sprite:
	.string	"harddisc"
	.align

menu_disc:
	.string	"USB disc"	@ menu title, padded to 12 bytes
	.align
	.int	0

	.byte	7, 2, 7, 0	@ title colours
	.int	16 * 6		@ width
	.int	44		@ height
	.int	0		@ vertical gap
	@ One item, and it is the last
	.int	(1 << 7)
	.int	-1
	.int	0x07000001
	.string	"Free"
	.align
	.int	0

path_root:
	.string	"MultiFS::0.$"
	.align

cli_open:
	.string	"Filer_OpenDir MultiFS::0.$"
	.align

cli_free:
	.string	"ShowFree -fs MultiFS"
	.align

default_name:
	.string	"USB"
	.align


	@ "Start" entry point, entered in user mode.
start:
	ldr	wp, [r12]
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	cmp	r0, #0
	ble	start_go
	ldr	r1, TASK
	swi	XWimp_CloseDown
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]

start_go:
	ldr	r0, =WIMP_VERSION
	ldr	r1, TASK
	adrl	r2, task_name
	swi	XWimp_Initialise
	swivs	XOS_Exit

	str	r1, [wp, #WS_MY_TASK_HANDLE]

	@ An icon left over from a previous run of this task has to go before the
	@ handle is forgotten, or it stays on the bar for ever with nothing owning
	@ it - start the task twice and you get two identical drives.
	ldr	r0, [wp, #WS_ICON_HANDLE]
	cmp	r0, #0
	blne	icon_delete

	mov	r0, #0
	str	r0, [wp, #WS_ICON_HANDLE]

	@ The icon is not created here: it goes up when a disc is found, which
	@ may be now or may be in an hour's time.

re_poll:
	swi	XOS_ReadMonotonicTime
	add	r2, r0, #100		@ a second, unless something happens first
	ldr	r0, =WIMP_POLL_MASK
	add	r1, wp, #WS_WIMP_BLOCK
	swi	XWimp_PollIdle
	bvs	close_down

	teq	r0, #0			@ null: time to look for a disc
	beq	poll_null
	teq	r0, #6			@ mouse click
	beq	mouse_click
	teq	r0, #9			@ menu selection
	beq	menu_selection
	teq	r0, #17			@ user message
	teqne	r0, #18
	beq	user_message
	b	re_poll


	@ Has a disc arrived, or gone?
poll_null:
	bl	disc_present
	ldr	r1, [wp, #WS_ICON_HANDLE]

	cmp	r0, #0
	beq	poll_gone

	cmp	r1, #0
	bne	re_poll			@ already showing
	bl	icon_create
	b	re_poll

poll_gone:
	cmp	r1, #0
	beq	re_poll			@ nothing showing
	bl	icon_delete
	b	re_poll


	/* Is there a MultiFS disc? Exit R0 = 1 if there is.
	 *
	 * OS_File 5 answers the question directly and needs nothing of MultiFS
	 * beyond what FileSwitch already asks it. An error means no.
	 */
disc_present:
	stmfd	sp!, {r1-r6, lr}
	mov	r0, #5
	adrl	r1, path_root
	swi	XOS_File
	movvs	r0, #0
	cmp	r0, #2			@ a directory: the root is there
	moveq	r0, #1
	movne	r0, #0
	ldmfd	sp!, {r1-r6, pc}


	/* Put the icon up, named after the disc. */
icon_create:
	stmfd	sp!, {r0-r10, lr}

	bl	read_disc_name

	adrl	r0, icon_bar_block
	add	r1, wp, #WS_ICON_BAR_BLOCK
	ldmia	r0, {r2-r10}
	add	r8, wp, #WS_NAME		@ indirected text: the disc's name
	adrl	r9, icon_bar_validation
	stmia	r1, {r2-r10}

	@ Make room for the sprite's height
	mov	r0, #SpriteOp_ReadSpriteInfo
	adrl	r2, icon_bar_sprite
	swi	XWimp_SpriteOp
	movvc	r0, r6
	movvc	r1, #ModeVariable_YEig
	swivc	XOS_ReadModeVariable
	bvs	icon_create_out

	ldr	r0, [wp, #WS_ICON_BAR_BLOCK + 16]
	add	r0, r0, r4, lsl r2
	str	r0, [wp, #WS_ICON_BAR_BLOCK + 16]

	@ Below the hard disc, above nothing much: a removable disc belongs to
	@ the right of the fixed ones.
	mov	r0, #0x71000000
	add	r0, r0, #2
	add	r1, wp, #WS_ICON_BAR_BLOCK
	swi	XWimp_CreateIcon
	bvs	icon_create_out
	str	r0, [wp, #WS_ICON_HANDLE]

icon_create_out:
	ldmfd	sp!, {r0-r10, pc}


	/* Take the icon down again. */
icon_delete:
	stmfd	sp!, {r0-r2, lr}
	mvn	r0, #1			@ -2: the icon bar
	str	r0, [wp, #WS_DELETE_BLOCK]
	ldr	r0, [wp, #WS_ICON_HANDLE]
	str	r0, [wp, #WS_DELETE_BLOCK + 4]
	add	r1, wp, #WS_DELETE_BLOCK
	swi	XWimp_DeleteIcon
	mov	r0, #0
	str	r0, [wp, #WS_ICON_HANDLE]
	ldmfd	sp!, {r0-r2, pc}


	/* The disc's name, for the icon.
	 *
	 * Canonicalising "MultiFS::0.$" turns the drive number into the disc's
	 * own name - MultiFS answers Func 11 with it - so the icon says "USB
	 * STICK" rather than "0". If anything goes wrong the icon still appears,
	 * named plainly, because an unnamed disc is far better than no disc.
	 */
read_disc_name:
	stmfd	sp!, {r0-r7, lr}

	add	r7, wp, #WS_NAME
	adrl	r0, default_name
	mov	r1, r7
	bl	copy_string

	mov	r0, #FSControl_CanonicalisePath
	adrl	r1, path_root
	add	r2, wp, #WS_CANON
	mov	r3, #0
	mov	r4, #0
	mov	r5, #128
	swi	XOS_FSControl
	bvs	read_disc_name_out

	@ Find the "::", then copy up to the "."
	add	r0, wp, #WS_CANON
	mov	r1, #0
read_disc_name_find:
	ldrb	r2, [r0, r1]
	cmp	r2, #0
	beq	read_disc_name_out
	cmp	r2, #':'
	beq	read_disc_name_found
	add	r1, r1, #1
	cmp	r1, #120
	blo	read_disc_name_find
	b	read_disc_name_out

read_disc_name_found:
	add	r1, r1, #1
	ldrb	r2, [r0, r1]
	cmp	r2, #':'
	bne	read_disc_name_out	@ a single colon: not a disc name
	add	r1, r1, #1

	mov	r3, #0
read_disc_name_copy:
	ldrb	r2, [r0, r1]
	cmp	r2, #0
	beq	read_disc_name_end
	cmp	r2, #'.'
	beq	read_disc_name_end
	strb	r2, [r7, r3]
	add	r1, r1, #1
	add	r3, r3, #1
	cmp	r3, #18
	blo	read_disc_name_copy

read_disc_name_end:
	cmp	r3, #0
	beq	read_disc_name_out	@ empty: keep the default
	mov	r2, #0
	strb	r2, [r7, r3]

read_disc_name_out:
	ldmfd	sp!, {r0-r7, pc}


	/* R0 = source, R1 = destination, both NUL terminated. */
copy_string:
	stmfd	sp!, {r0-r3, lr}
	mov	r3, #0
copy_string_loop:
	ldrb	r2, [r0, r3]
	strb	r2, [r1, r3]
	cmp	r2, #0
	beq	copy_string_out
	add	r3, r3, #1
	cmp	r3, #18
	blo	copy_string_loop
	mov	r2, #0
	strb	r2, [r1, r3]
copy_string_out:
	ldmfd	sp!, {r0-r3, pc}


mouse_click:
	ldr	r2, [r1, #12]		@ window handle
	cmn	r2, #2			@ -2 is the icon bar
	bne	re_poll

	ldr	r3, [r1, #16]		@ icon handle
	ldr	r4, [wp, #WS_ICON_HANDLE]
	cmp	r3, r4
	bne	re_poll

	ldr	r5, [r1, #8]		@ buttons, kept: loading an address below
					@ would otherwise overwrite them before the
					@ Menu test had a chance to look

	cmp	r5, #4			@ Select
	cmpne	r5, #1			@ Adjust
	bne	mouse_click_menu

	adrl	r0, cli_open
	swi	XOS_CLI
	b	re_poll

mouse_click_menu:
	cmp	r5, #2			@ Menu
	bne	re_poll

	ldr	r2, [r1, #0]		@ X of the click
	sub	r2, r2, #64
	mov	r3, #(96 + 44)
	adrl	r1, menu_disc
	swi	XWimp_CreateMenu
	b	re_poll


menu_selection:
	adrl	r0, cli_free
	swi	XOS_CLI
	b	re_poll


user_message:
	ldr	r0, [r1, #16]
	teq	r0, #Message_Quit
	bne	re_poll

close_down:
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	ldr	r1, TASK
	swi	XWimp_CloseDown

	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]
	str	r0, [wp, #WS_ICON_HANDLE]

	swi	XOS_Exit

	.end
