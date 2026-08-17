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
	XWimp_CreateWindow	= 0x600c1
	XWimp_CreateIcon	= 0x600c2
	XWimp_DeleteIcon	= 0x600c4
	XWimp_CreateMenu	= 0x600d4
	XWimp_OpenTemplate	= 0x600d9
	XWimp_CloseTemplate	= 0x600da
	XWimp_LoadTemplate	= 0x600db
	XWimp_CloseDown		= 0x600dd
	XWimp_PollIdle		= 0x600e1
	XWimp_CreateSubMenu	= 0x600e8
	XWimp_SpriteOp		= 0x600e9

	XOS_ReadVarVal		= 0x20023

	XResourceFS_RegisterFiles	= 0x61b40
	XResourceFS_DeregisterFiles	= 0x61b41

	Module_Enter	= 2
	Module_Claim	= 6
	Module_Free	= 7

	Message_Quit		= 0

	Service_Reset		= 0x27
	Service_StartFiler	= 0x4b
	Service_StartedFiler	= 0x4c
	Service_FilerDying	= 0x4f
	Service_ResourceFSStarting = 0x60

	SpriteOp_ReadSpriteInfo	= 40
	ModeVariable_YEig	= 5

	FSControl_CanonicalisePath = 37

	WIMP_VERSION	= 300

	@ Null events are wanted: they are what drives the check for a stick
	@ arriving or going. Pointer entering and leaving are not.
	WIMP_POLL_MASK	= 0x00000030

	WS_MY_TASK_HANDLE	= 0
	WS_FILER_TASK_HANDLE	= 4
	@ -1 when there is no icon up, and it has to be -1 rather than 0: the Wimp
	@ handed this task 0 as the handle of its first icon bar icon, the code read
	@ that back as "nothing showing" and put up a second drive beside the first.
	WS_ICON_HANDLE		= 8
	NO_ICON			= -1
	WS_ICON_BAR_BLOCK	= 12	@ 9 words, 12-47
	WS_DELETE_BLOCK		= 48	@ 2 words, 48-55
	WS_NAME			= 56	@ the icon's text, 24 bytes
	WS_CANON		= 80	@ a canonicalised path, 128 bytes
	WS_WIMP_BLOCK		= 208	@ 256 bytes for Wimp_Poll, 208-463

	@ The task's own stack, and it has to be its own: see the note above the
	@ Start entry point.
	WS_STACK	= 512
	STACK_SIZE	= 1024

	@ A *command built at run time. The disc's name is not known until one is
	@ plugged in, so the command that opens it cannot be a constant.
	WS_CMD		= WS_STACK + STACK_SIZE
	CMD_SIZE	= 64

	@ The Disc info window.
	@
	@ Both the window definition and the menu are BUILT IN WORKSPACE rather
	@ than used where they sit in the module. The window has to be, because an
	@ indirected icon holds a pointer to its text and those pointers are only
	@ known once the module is running. The menu has to be, because the Disc
	@ info item carries the window's handle as its submenu, and that is not
	@ known until Wimp_CreateWindow has been called.
	INFO_ROWS	= 7
	INFO_ICONS	= INFO_ROWS * 2
	INFO_VALUE_LEN	= 32

	WS_WINDOW	= WS_CMD + CMD_SIZE	@ the window's handle
	WS_WINDEF	= WS_WINDOW + 4		@ 88 header + 32 per icon
	WINDEF_SIZE	= 88 + (INFO_ICONS * 32)
	WS_VALUES	= WS_WINDEF + WINDEF_SIZE
	VALUES_SIZE	= INFO_ROWS * INFO_VALUE_LEN
	WS_MENU		= WS_VALUES + VALUES_SIZE
	MENU_SIZE	= 28 + (3 * 24)

	@ The icon bar sprite, chosen by desktop theme.
	@
	@ One sprite file carries three: os3, os4 and os5. Which one belongs on
	@ the bar depends on what the desktop looks like, and the desktop says so
	@ in Wimp$IconTheme - "Morris4" on RISC OS 3's look, "Ursula" on 4's,
	@ "Sovereign" or "Iyonix" on 5's. The variable is unset on a machine old
	@ enough not to have themes at all, which is itself the answer: os3.
	@ Where the template's indirected strings are put when it is loaded. The
	@ icons point into this, and so does the Wimp once the window is created,
	@ so writing a value here is what changes what the window shows.
	WS_IND		= WS_MENU + MENU_SIZE
	IND_SIZE	= 512

	WS_THEME	= WS_IND + IND_SIZE	@ the theme name as read, 32 bytes
	THEME_LEN	= 32
	WS_VALID	= WS_THEME + THEME_LEN	@ "S" and the sprite name, 16 bytes
	VALID_LEN	= 16

	WORKSPACE_SIZE	= WS_VALID + VALID_LEN

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

	@ The sprites and the window template go into Resources:$ out of the
	@ card's own ROM, so there is nothing to install and they are there only
	@ while the card is.
	adrl	r0, resfs_files
	swi	XResourceFS_RegisterFiles

init_have_workspace:
	ldr	wp, [r12]
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]
	mvn	r0, #0			@ NO_ICON
	str	r0, [wp, #WS_ICON_HANDLE]

	cmp	pc, #0
	ldmfd	sp!, {pc}


final:
	stmfd	sp!, {lr}

	adrl	r0, resfs_files
	swi	XResourceFS_DeregisterFiles

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
	.int	Service_ResourceFSStarting
	.int	0
	.int	service_codetable
service_pre:
	mov	r0, r0		@ magic: table pointer sits at service_pre-4
	teq	r1, #Service_Reset
	teqne	r1, #Service_StartFiler
	teqne	r1, #Service_StartedFiler
	teqne	r1, #Service_FilerDying
	teqne	r1, #Service_ResourceFSStarting
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
	ldr	r14, =Service_ResourceFSStarting
	teq	r1, r14
	beq	service_resourcefs

	ldmfd	sp!, {pc}

	/* ResourceFS has restarted and wants its files back. It is not on the
	 * module chain at that point, so the SWI cannot be used - the service
	 * hands over the address of its own registration routine instead.
	 */
service_resourcefs:
	stmfd	sp!, {r0}
	adrl	r0, resfs_files
	mov	lr, pc
	mov	pc, r2
	ldmfd	sp!, {r0}
	ldmfd	sp!, {pc}

service_reset:
	mov	r14, #0
	str	r14, [wp, #WS_MY_TASK_HANDLE]
	mvn	r14, #0			@ NO_ICON
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

	@ The validation string is built at run time at WS_VALID, because which
	@ sprite it names depends on the desktop's theme. What is left here is
	@ only the fallback the Wimp already has, for a machine where the
	@ resources did not register.
icon_bar_validation:
	.string	"Sharddisc"
	.align

	@ A Wimp menu block's title is a fixed twelve bytes and so is each item's
	@ text, so both are padded by measurement rather than by .align: .align
	@ lands on twelve only when the name happens to be the right length, and
	@ "MultiFS" is not. Overrunning the title by four bytes pushes the whole
	@ block along, and the Wimp then reads the item flags from the wrong word
	@ and walks off the end of the menu looking for the last item. Because the
	@ padding is worked out from the label, the name can be changed here
	@ without touching anything else.
menu_disc:
	.string	"MultiFS"	@ menu title
	.space	menu_disc + 12 - .

	.byte	7, 2, 7, 0	@ title colours
	.int	16 * 8		@ width
	.int	44		@ height
	.int	0		@ vertical gap

	@ Disc info. The submenu field is filled in with the window's handle at
	@ run time, and the Wimp opens it itself: no Message_MenuWarning is asked
	@ for, because the numbers are gathered when the MENU goes up instead. It
	@ is a fraction of a second earlier and one moving part fewer.
	.int	0
	.int	-1
	.int	0x07000001
menu_disc_item0:
	.string	"Disc info"
	.space	menu_disc_item0 + 12 - .

	@ Free
	.int	0
	.int	-1
	.int	0x07000001
menu_disc_item1:
	.string	"Free"
	.space	menu_disc_item1 + 12 - .

	@ Dismount, and it is the last
	.int	(1 << 7)
	.int	-1
	.int	0x07000001
menu_disc_item2:
	.string	"Dismount"
	.space	menu_disc_item2 + 12 - .
menu_disc_end:

	/* The Disc info window.
	 *
	 * Layout, from Acorn's own wimp.h and OSLib's Hdr.Wimp rather than from
	 * memory - the block is 88 bytes and then 32 per icon:
	 *
	 *    0 visible area (4 words)      40 work area extent (4 words)
	 *   16 scroll x, y                 56 title icon flags
	 *   24 window to open behind       60 work area icon flags
	 *   28 window flags                64 sprite area
	 *   32 seven colour BYTES and      68 minimum width, height (2 shorts)
	 *      one of extra flags          72 title icon data (3 words)
	 *                                  84 number of icons
	 *
	 * Every icon here is indirected text, so each carries a pointer to its
	 * own characters, which is why the whole thing is assembled in workspace.
	 * The labels point straight at the strings below - nothing writes to
	 * them - and the values point into WS_VALUES.
	 */
	INFO_ROW_H	= 44
	INFO_TOP	= -12
	@ The label column has to hold "Cluster (bytes)" - fifteen characters of
	@ the system font at sixteen OS units each - or it is silently clipped.
	INFO_LABEL_X0	= 12
	INFO_LABEL_X1	= 272
	INFO_VALUE_X0	= 280
	INFO_VALUE_X1	= 628

	@ Moveable, redrawn entirely by the Wimp, with a title bar, in the format
	@ where the colour bytes above mean what they say.
	INFO_WIN_FLAGS	= 0x2 + 0x10 + 0x4000000 + 0x80000000

	@ Text, indirected, vertically centred, black on grey.
	INFO_ICON_FLAGS	= 0x1 + 0x10 + 0x100 + (7 << 24) + (1 << 28)

window_template:
	.int	300, 300, 940, 632	@ visible area, moved when it is shown
	.int	0, 0			@ scroll offsets
	.int	-1			@ open in front
	.int	INFO_WIN_FLAGS
	.byte	7, 2, 7, 1, 3, 1, 12	@ title fg/bg, work fg/bg, scroll, highlight
	.byte	0			@ extra flags
	.int	0, -(12 + (INFO_ROWS * INFO_ROW_H)), 640, 0	@ work area extent
	.int	INFO_ICON_FLAGS		@ title icon flags
	.int	0			@ work area icon flags
	.int	1			@ the Wimp's own sprite area
	.int	0			@ no minimum size
	.int	0, 0, 0			@ title icon data, filled in at run time
	.int	INFO_ICONS
window_template_end:

	@ One entry per row: the label, and which value buffer it takes.
info_labels:
	.string	"Name"
	.align
info_label_format:
	.string	"Format"
	.align
info_label_drive:
	.string	"Drive"
	.align
info_label_size:
	.string	"Size (MB)"
	.align
info_label_free:
	.string	"Free (MB)"
	.align
info_label_used:
	.string	"Used (MB)"
	.align
info_label_cluster:
	.string	"Cluster (bytes)"
	.align

info_label_table:
	.int	info_labels
	.int	info_label_format
	.int	info_label_drive
	.int	info_label_size
	.int	info_label_free
	.int	info_label_used
	.int	info_label_cluster

	@ The system variables *MultiFSInfo sets, in the same order.
info_var_table:
	.int	var_label
	.int	var_format
	.int	var_drive
	.int	var_size
	.int	var_free
	.int	var_used
	.int	var_cluster

var_label:
	.string	"MultiFS$Label"
	.align
var_format:
	.string	"MultiFS$Format"
	.align
var_drive:
	.string	"MultiFS$Drive"
	.align
var_size:
	.string	"MultiFS$Size"
	.align
var_free:
	.string	"MultiFS$Free"
	.align
var_used:
	.string	"MultiFS$Used"
	.align
var_cluster:
	.string	"MultiFS$Cluster"
	.align

	@ Theme to sprite. Iyonix and Sovereign are both the RISC OS 5 look.
theme_table:
	.int	theme_morris4,	sprite_os3
	.int	theme_ursula,	sprite_os4
	.int	theme_sovereign, sprite_os5
	.int	theme_iyonix,	sprite_os5
	.int	0, 0

theme_morris4:
	.string	"Morris4"
	.align
theme_ursula:
	.string	"Ursula"
	.align
theme_sovereign:
	.string	"Sovereign"
	.align
theme_iyonix:
	.string	"Iyonix"
	.align

sprite_os3:
	.string	"os3"
	.align
sprite_os4:
	.string	"os4"
	.align
sprite_os5:
	.string	"os5"
	.align

var_icontheme:
	.string	"Wimp$IconTheme"
	.align

template_file:
	.string	"Resources:$.Resources.MultiFS.Templates"
	.align
	@ Wimp_LoadTemplate matches on a twelve-byte name, so it is padded here
	@ by measurement for the same reason the menu's is.
template_name:
	.string	"DriveInfo"
	.space	template_name + 12 - .

cli_iconsprites:
	.string	"IconSprites Resources:$.Resources.MultiFS.Sprites"
	.align

cli_info:
	.string	"MultiFSInfo -q"
	.align

empty_string:
	.byte	0
	.align

window_title:
	.string	"Disc info"
	.align

path_root:
	.string	"MultiFS::0.$"
	.align

	@ The device is not optional, whatever the shape of the command suggests:
	@ without it *ShowFree prints its syntax and stops. The name to give is the
	@ one MultiFS answers the Free module's "what is this device called" with,
	@ which is the filing system's own name.
cli_free:
	.string	"ShowFree -fs MultiFS MultiFS"
	.align

cli_dismount:
	.string	"MultiFSDismount"
	.align

	@ The two halves of the open command, with the disc's own name between
	@ them. RISC OS shows a disc by name rather than by drive number wherever
	@ it can, and a Filer window titled MultiFS::USB_STICK.$ says what is in
	@ the drive where MultiFS::0.$ only says which drive it is.
cli_open_head:
	.string	"Filer_OpenDir MultiFS::"
	.align
cli_open_tail:
	.string	".$"
	.align

default_name:
	.string	"USB"
	.align


	@ "Start" entry point, entered in user mode.
	@
	@ Set up a stack before doing anything else. OS_Module Enter does not give
	@ the module one, and when the Filer starts this task during the boot
	@ sequence it arrives with R13 = 0, so the first STMFD in the first
	@ subroutine call writes to &FFFFFFE4 and aborts. That was the whole of the
	@ "MultiFSFiler takes the desktop down at boot" bug, and it is invisible in
	@ HostFSFiler, the module this one was modelled on, only because its task
	@ never makes a subroutine call and so never touches a stack at all.
	@
	@ The stack lives in the module's own workspace rather than in application
	@ space, so that it does not depend on the size of the slot the task
	@ happens to be given - which at boot is none.
start:
	ldr	wp, [r12]
	add	sp, wp, #(WS_STACK + STACK_SIZE)

	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	cmp	r0, #0
	ble	start_go
	ldr	r1, TASK
	swi	XWimp_CloseDown
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]

start_go:
	@ Whether an older task was just closed down or there never was one, this
	@ task owns no icon yet, and the Wimp took any the old task had with it
	@ when it went. So forget the remembered handle rather than deleting it:
	@ by now that number may belong to somebody else's icon.
	mvn	r0, #0			@ NO_ICON
	str	r0, [wp, #WS_ICON_HANDLE]

	@ R3 = 0 asks for every message. It was left unset before, which happened
	@ to work for Message_Quit but is not something to rely on.
	ldr	r0, =WIMP_VERSION
	ldr	r1, TASK
	adrl	r2, task_name
	mov	r3, #0
	swi	XWimp_Initialise
	swivs	XOS_Exit

	str	r1, [wp, #WS_MY_TASK_HANDLE]

	@ The sprites go into the Wimp's own pool, where an icon's validation
	@ string can name one. *IconSprites is the ordinary way a filer does
	@ this; ours come out of the card's ROM through ResourceFS rather than
	@ off a disc.
	adrl	r0, cli_iconsprites
	swi	XOS_CLI

	bl	choose_sprite
	bl	build_info_window

	@ The icon is not created here: it goes up when a disc is found, which
	@ may be now or may be in an hour's time.
	@
	@ This branch is not decoration. Before the routines below were put here
	@ the code fell straight through into the poll loop, and inserting them
	@ meant it fell into build_info_window's BODY instead - a second time,
	@ with nothing on the stack to match its exit, so it created another
	@ window on every entry and then returned to whatever the pop happened to
	@ find. The visible symptom was "The area of memory reserved for
	@ relocatable modules is full" and a desktop with no icon bar.
	b	re_poll


	/* Work out which of the three icon bar sprites this desktop wants.
	 *
	 * Builds "S<name>" at WS_VALID, which is what an icon's validation
	 * string needs to name a sprite in the Wimp pool.
	 */
choose_sprite:
	stmfd	sp!, {r0-r8, lr}

	@ Read the theme. An unset variable is not an error here - it means a
	@ desktop from before themes existed, which is the RISC OS 3 look.
	adrl	r0, var_icontheme
	add	r1, wp, #WS_THEME
	mov	r2, #(THEME_LEN - 1)
	mov	r3, #0
	mov	r4, #3
	swi	XOS_ReadVarVal
	movvs	r2, #0
	add	r0, wp, #WS_THEME
	mov	r1, #0
	strb	r1, [r0, r2]

	@ The table holds ".int label", and the module is linked at zero, so
	@ those words are OFFSETS from the module's base rather than addresses -
	@ the same correction the icon table in build_info_window needs.
	adrl	r8, module_start
	adrl	r5, theme_table

choose_sprite_try:
	ldr	r6, [r5]
	cmp	r6, #0
	beq	choose_sprite_default
	add	r6, r6, r8		@ the theme name
	ldr	r7, [r5, #4]
	add	r7, r7, r8		@ and the sprite that goes with it

	add	r0, wp, #WS_THEME
	mov	r1, r6
	bl	theme_matches
	beq	choose_sprite_got

	add	r5, r5, #8
	b	choose_sprite_try

choose_sprite_default:
	adrl	r7, sprite_os3		@ no theme at all: the oldest look

choose_sprite_got:
	@ copy_string is (R0 = from, R1 = to), which is the opposite way round
	@ from the obvious guess and was got wrong here first time.
	add	r1, wp, #WS_VALID
	mov	r0, #'S'
	strb	r0, [r1], #1
	mov	r0, r7
	bl	copy_string

	cmp	pc, #0
	ldmfd	sp!, {r0-r8, pc}


	/* Does the theme at R0 begin with the name at R1?
	 *
	 * Exit: Z set if it does. Case is ignored, and the comparison stops at
	 * the end of the name - Wimp$IconTheme carries a trailing dot because it
	 * is used as part of a path, so "Sovereign" has to match "Sovereign.".
	 */
theme_matches:
	stmfd	sp!, {r0-r4, lr}
	mov	r4, #0
theme_matches_loop:
	ldrb	r2, [r1, r4]
	cmp	r2, #0
	beq	theme_matches_yes
	ldrb	r3, [r0, r4]
	cmp	r3, #0
	bne	theme_matches_fold
	cmp	r0, r1			@ ran out: no match
	b	theme_matches_out
theme_matches_fold:
	cmp	r3, #'A'
	blo	theme_matches_cmp
	cmp	r3, #'Z'
	addls	r3, r3, #32
theme_matches_cmp:
	cmp	r2, #'A'
	blo	theme_matches_test
	cmp	r2, #'Z'
	addls	r2, r2, #32
theme_matches_test:
	cmp	r2, r3
	bne	theme_matches_out
	add	r4, r4, #1
	b	theme_matches_loop
theme_matches_yes:
	cmp	r0, r0			@ Z
theme_matches_out:
	ldmfd	sp!, {r0-r4, pc}


	/* Copy the menu into workspace and build the Disc info window.
	 *
	 * Called once, from the task's start. Neither block can be used where it
	 * is assembled: the menu needs the window's handle written into it, and
	 * the window's icons need pointers to their text.
	 */
build_info_window:
	stmfd	sp!, {r0-r8, lr}

	@ The menu, copied byte for byte.
	adrl	r1, menu_disc
	ldr	r2, =WS_MENU
	add	r2, wp, r2
	mov	r3, #0
build_menu_copy:
	ldrb	r0, [r1, r3]
	strb	r0, [r2, r3]
	add	r3, r3, #1
	cmp	r3, #MENU_SIZE
	blo	build_menu_copy

	@ The window comes out of the template file rather than being assembled
	@ here, so its layout is a thing that can be edited with a template
	@ editor. The icons alternate label, value, label, value... so the value
	@ for row N is icon 2N+1.
	adrl	r1, template_file
	swi	XWimp_OpenTemplate
	bvs	build_info_out

	ldr	r1, =WS_WINDEF
	add	r1, wp, r1
	ldr	r2, =WS_IND
	add	r2, wp, r2
	mov	r3, r2
	add	r3, r3, #IND_SIZE
	mvn	r4, #0			@ no fonts
	adrl	r5, template_name
	mov	r6, #0
	swi	XWimp_LoadTemplate

	@ Close it whatever happened, but keep the LOAD's answer: the close sets
	@ the flags itself, so testing V after it tests the wrong call.
	movvs	r7, #1
	movvc	r7, #0
	mov	r8, r0
	swi	XWimp_CloseTemplate
	cmp	r7, #0
	bne	build_info_out
	mov	r0, r8

	ldr	r1, =WS_WINDEF
	add	r1, wp, r1
	swi	XWimp_CreateWindow
	bvs	build_info_out
	str	r0, [wp, #WS_WINDOW]

	@ Now the menu can be told which window its first item leads to.
	ldr	r1, =WS_MENU
	add	r1, wp, r1
	str	r0, [r1, #(28 + 4)]	@ the submenu field of item 0

build_info_out:
	ldmfd	sp!, {r0-r8, pc}


	/* Where icon N's indirected text buffer is, and how long it is.
	 *
	 * Entry: R0 = the icon number.
	 * Exit:  R0 = the buffer, R1 = its length.
	 *
	 * Read out of the window definition the template was loaded into. The
	 * Wimp kept those same pointers when the window was created, so writing
	 * there is what changes what the icon shows.
	 */
info_icon_buffer:
	stmfd	sp!, {r2-r4, lr}
	ldr	r2, =WS_WINDEF
	add	r2, wp, r2
	add	r2, r2, #88		@ past the window header
	add	r2, r2, r0, lsl #5	@ thirty-two bytes an icon
	ldr	r0, [r2, #20]		@ the buffer
	ldr	r1, [r2, #28]		@ and its length
	ldmfd	sp!, {r2-r4, pc}


	/* Fill the value icons in from MultiFS's own numbers.
	 *
	 * *MultiFSInfo -q sets a system variable for each of them and prints
	 * nothing, which is what the -q is for: this runs inside a Wimp task and
	 * anything written to the screen here lands on top of the desktop.
	 */
refresh_info:
	stmfd	sp!, {r0-r8, lr}

	adrl	r0, cli_info
	swi	XOS_CLI

	mov	r5, #0
refresh_info_row:
	@ Row N's value is icon 2N+1: the template alternates label, value.
	mov	r0, r5, lsl #1
	add	r0, r0, #1
	bl	info_icon_buffer
	mov	r6, r0			@ where the text goes
	mov	r7, r1			@ and how much room there is
	cmp	r6, #0
	beq	refresh_info_next

	adrl	r0, info_var_table
	ldr	r0, [r0, r5, lsl #2]
	adrl	r1, module_start
	add	r0, r0, r1		@ a table holds link-time offsets
	mov	r1, r6
	sub	r2, r7, #1
	mov	r3, #0
	mov	r4, #3
	swi	XOS_ReadVarVal
	movvs	r2, #0			@ not set: leave it blank

	mov	r0, #0
	strb	r0, [r6, r2]		@ OS_ReadVarVal does not terminate it

refresh_info_next:
	add	r5, r5, #1
	cmp	r5, #INFO_ROWS
	blo	refresh_info_row

	ldmfd	sp!, {r0-r8, pc}


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

	cmn	r1, #1			@ NO_ICON?
	bne	re_poll			@ already showing
	bl	icon_create
	b	re_poll

poll_gone:
	cmn	r1, #1			@ NO_ICON?
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
	add	r9, wp, #WS_VALID		@ "S" and the sprite the theme wants
	stmia	r1, {r2-r10}

	@ Make room for the sprite's height. The name is the one just chosen,
	@ which is the byte after the "S" in the validation string.
	mov	r0, #SpriteOp_ReadSpriteInfo
	add	r2, wp, #WS_VALID
	add	r2, r2, #1
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
	mvn	r0, #0			@ NO_ICON
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


	/* Build "Filer_OpenDir MultiFS::<disc>.$" in the workspace.
	 *
	 * Exit: R0 = the command.
	 *
	 * The disc's name is read fresh rather than remembered, so that a stick
	 * swapped for a different one opens the one that is actually in the
	 * drive. If the name cannot be had, the drive number still works and is
	 * used instead - an ugly window title beats a window that will not open.
	 */
build_open_command:
	stmfd	sp!, {r1-r5, lr}

	bl	read_disc_name		@ leaves the name at WS_NAME

	ldr	r0, =WS_CMD
	add	r4, wp, r0
	mov	r5, #0

	adrl	r1, cli_open_head
	bl	append_cmd
	add	r1, wp, #WS_NAME
	bl	append_cmd
	adrl	r1, cli_open_tail
	bl	append_cmd

	mov	r0, #0
	strb	r0, [r4, r5]
	mov	r0, r4
	ldmfd	sp!, {r1-r5, pc}

	/* Append the string at R1 to the command at R4, R5 bytes in so far. */
append_cmd:
	stmfd	sp!, {r0-r2, lr}
	mov	r2, #0
append_cmd_loop:
	ldrb	r0, [r1, r2]
	cmp	r0, #0
	beq	append_cmd_out
	add	r3, r5, r2
	cmp	r3, #(CMD_SIZE - 2)
	bhs	append_cmd_out		@ never run off the end of the buffer
	strb	r0, [r4, r3]
	add	r2, r2, #1
	b	append_cmd_loop
append_cmd_out:
	add	r5, r5, r2
	ldmfd	sp!, {r0-r2, pc}


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

	bl	build_open_command
	swi	XOS_CLI
	b	re_poll

mouse_click_menu:
	cmp	r5, #2			@ Menu
	bne	re_poll

	@ Gather the numbers before the menu appears, so whatever the Disc info
	@ window shows is current.
	stmfd	sp!, {r1}
	bl	refresh_info
	ldmfd	sp!, {r1}

	ldr	r2, [r1, #0]		@ X of the click
	sub	r2, r2, #64

	@ Where the TOP of the menu goes, in OS units up from the bottom of the
	@ screen: the icon bar's height and then one row per item. The TITLE is
	@ not counted, which is the whole trick - the Wimp puts the title above
	@ the position given, so counting it lifts the menu a row clear of where
	@ every other icon bar menu sits.
	@
	@ Measured rather than assumed: CDFS's menu on the same bar ends at the
	@ same pixel row this one does with the title left out, and one row higher
	@ with it in. Work it out rather than writing a number, because the number
	@ is wrong again the moment an item is added - which is what happened when
	@ Disc info was put in and the menu came up over the bar.
	MENU_ITEMS	= 3
	ICON_BAR_H	= 96
	mov	r3, #(ICON_BAR_H + (44 * MENU_ITEMS))
	@ The workspace copy, not the one in the module: that is the one carrying
	@ the Disc info window's handle.
	ldr	r1, =WS_MENU
	add	r1, wp, r1
	swi	XWimp_CreateMenu
	b	re_poll


menu_selection:
	@ The block holds one index per level of menu, so the first word says
	@ which item of ours was chosen. Without looking, every selection ran
	@ ShowFree - which was right while there was only one item.
	ldr	r0, [r1]
	cmp	r0, #2
	beq	menu_selection_dismount
	cmp	r0, #0
	beq	re_poll			@ Disc info opens on the warning, not here

	adrl	r0, cli_free
	swi	XOS_CLI
	b	re_poll

menu_selection_dismount:
	adrl	r0, cli_dismount
	swi	XOS_CLI

	@ The disc has gone as far as anything here is concerned, so the icon
	@ goes now rather than at the next poll: a Dismount that leaves the drive
	@ sitting on the bar looks as though it did nothing.
	ldr	r0, [wp, #WS_ICON_HANDLE]
	cmn	r0, #1
	blne	icon_delete
	b	re_poll

user_message:
	ldr	r0, [r1, #16]

	teq	r0, #Message_Quit
	bne	re_poll
	b	close_down

close_down:
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	ldr	r1, TASK
	swi	XWimp_CloseDown

	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]
	mvn	r0, #0			@ NO_ICON
	str	r0, [wp, #WS_ICON_HANDLE]

	swi	XOS_Exit


	@ The literal pool has to be emitted HERE, with the code. Left to the
	@ end of the file it lands beyond four kilobytes of sprite and template,
	@ and every "ldr rX, =value" above complains that the pool is too far.
	.ltorg

@ ---------------------------------------------------------------------------
@ The files MultiFSFiler puts into Resources:$
@ ---------------------------------------------------------------------------
@
@ At the END of the module on purpose. These are several kilobytes of sprite
@ and template, and putting them among the code pushed the routines apart far
@ enough that adrl could no longer reach its own data.

	.include "resfs.inc"

	.end
