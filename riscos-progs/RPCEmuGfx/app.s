@ !GfxCard - the graphics card's desktop utility
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ A Wimp task: the card's icon on the icon bar, with a menu carrying the two
@ commands worth having to hand and two windows - what the card is displaying,
@ and what this is. It is carried in the card's own ROM and registered with
@ ResourceFS by the driver, so it lives in Resources:$.Apps and nothing is
@ installed on a disc.
@
@ The two windows come out of the Templates file and the words out of the
@ Messages file, both carried in the same ROM and reached through GfxCard: - the
@ path !Run sets to the application directory.
@
@ The figures come from the driver, not from the card: expansion card space is
@ mapped for privileged access only, so an application cannot read those registers
@ however much it knows about them. *GfxCardVars has the driver publish what there
@ is to say as GfxCard$Mode, GfxCard$Store and GfxCard$Frames, and this reads
@ those - which also means the same figures are available to an Obey file or a
@ BASIC program.
@
@ An absolute application (filetype &FF8): loaded at &8000 and entered there.

	.arch	armv3

	@ RISC OS SWIs (X-form where a failure is survivable)
	OS_Exit			= 0x00011
	XOS_CLI			= 0x20005
	XOS_File		= 0x20008
	XOS_ReadVarVal		= 0x20023
	XWimp_Initialise	= 0x600c0
	XWimp_CreateWindow	= 0x600c1
	XWimp_CreateIcon	= 0x600c2
	XWimp_OpenWindow	= 0x600c5
	XWimp_CloseWindow	= 0x600c6
	XWimp_Poll		= 0x600c7
	XWimp_RedrawWindow	= 0x600c8
	XWimp_GetRectangle	= 0x600ca
	XWimp_SetIconState	= 0x600cd
	XWimp_GetIconState	= 0x600ce
	XWimp_GetPointerInfo	= 0x600cf
	XWimp_CreateMenu	= 0x600d4
	XWimp_CreateSubMenu	= 0x600e8
	XWimp_OpenTemplate	= 0x600d9
	XWimp_CloseTemplate	= 0x600da
	XWimp_LoadTemplate	= 0x600db
	XWimp_CloseDown		= 0x600dd
	XWimp_ReportError	= 0x600df

	OSFILE_LOAD	= 16			@ load a named file

	WIMP_VERSION	= 310
	TASK_MAGIC	= 0x4b534154		@ "TASK"

	@ Wimp_Poll reason codes we act on
	EVENT_REDRAW	= 1
	EVENT_OPEN	= 2
	EVENT_CLOSE	= 3
	EVENT_CLICK	= 6
	EVENT_MENU	= 9
	EVENT_USERMSG	= 17
	EVENT_USERMSG_R	= 18

	@ Poll mask: nulls are not wanted. The figures are rewritten as the menu
	@ opens, so there is nothing to do between events.
	POLL_MASK	= 1

	@ The icon bar, as a window handle
	ICONBAR_RIGHT	= -1

	@ Mouse buttons, as Wimp_Poll reports a click on a click-type icon
	BUTTON_ADJUST	= 1 << 0
	BUTTON_MENU	= 1 << 1
	BUTTON_SELECT	= 1 << 2

	@ Icon flags: text and sprite, centred, indirected, reporting clicks. The
	@ sprite is named in the validation string ("S<name>") rather than through
	@ the sprite-only form of the icon data: that is how the Wimp finds a sprite
	@ in its own pool for an icon bar icon, and the sprite-only form draws
	@ nothing here. The text is empty, so only the sprite shows.
	ICONBAR_FLAGS	= 1 + 2 + 8 + 16 + 0x100 + (3 << 12) + (7 << 24)

	@ Icon numbers in the two templates
	ICON_MODE	= 1			@ displayinfo: resolution
	ICON_STORE	= 2			@ displayinfo: framestore
	ICON_FRAMES	= 3			@ displayinfo: frames
	ICON_NAME	= 1			@ proginfo: name
	ICON_PURPOSE	= 2			@ proginfo: purpose
	ICON_AUTHOR	= 3			@ proginfo: author
	ICON_VERSION	= 4			@ proginfo: version

	@ Icon flags, as Wimp_GetIconState reports them
	ICON_INDIRECTED	= 0x100

	@ Wimp messages we act on
	MSG_MENUWARNING	= 0x400c0

	@ Menu item flags
	MENU_DOTTED	= 1 << 1
	MENU_WARN	= 1 << 3	@ tell us before showing the submenu
	MENU_LAST	= 1 << 7
	MENU_ITEM_FLAGS	= 1 + 16 + 0x100 + (7 << 24)

	@ Menu layout, in bytes: a header then one entry per item
	MENU_HEADER	= 28
	MENU_ITEM_SIZE	= 24

	@ Menu geometry, in OS units
	MENU_WIDTH	= 320
	MENU_HEIGHT	= 44

	@ Menu items, in order
	ITEM_INFO	= 0
	ITEM_DISPLAY	= 1
	ITEM_ON		= 2
	ITEM_OFF	= 3
	ITEM_QUIT	= 4
	ITEM_COUNT	= 5

	@ Sizes
	TEXT_SIZE	= 24		@ per menu item text buffer
	VAR_SIZE	= 48		@ longest thing the driver publishes
	MSG_SIZE	= 512		@ the Messages file, as loaded
	WINDOW_SIZE	= 512		@ one window definition from the template
	INDIRECT_SIZE	= 1024		@ both windows' indirected data
	STACK_SIZE	= 1024


	.global	_start
_start:
	@ Entered with nothing useful in the registers, so the stack comes first.
	adrl	sp, stack_top

	bl	find_card
	bl	wimp_start
	bl	load_windows
	bl	fill_proginfo
	bl	make_icon

poll_loop:
	mov	r0, #POLL_MASK
	adrl	r1, poll_block
	swi	XWimp_Poll
	bvs	poll_loop		@ a failed poll is not worth dying for

	@ The event code is kept in r4, not r0: a handler is free to leave anything
	@ in r0, and testing that against the remaining event codes would dispatch
	@ the same event twice. Every handler preserves r4.
	mov	r4, r0
	teq	r4, #EVENT_REDRAW
	bleq	redraw_window
	teq	r4, #EVENT_OPEN
	bleq	open_window
	teq	r4, #EVENT_CLOSE
	bleq	close_window
	teq	r4, #EVENT_CLICK
	bleq	mouse_click
	teq	r4, #EVENT_MENU
	bleq	menu_selection
	teq	r4, #EVENT_USERMSG
	teqne	r4, #EVENT_USERMSG_R
	bleq	user_message
	b	poll_loop

quit:
	swi	XWimp_CloseDown
	swi	OS_Exit


@ Check the driver is here, by asking it to publish its figures. If the command
@ is not there, neither is the card.
find_card:
	stmfd	sp!, {lr}
	adrl	r0, cmd_vars
	swi	XOS_CLI
	bvs	no_card
	ldmfd	sp!, {pc}

no_card:
	adrl	r0, err_no_card
	b	fatal

	.ltorg

@ Become a Wimp task.
wimp_start:
	stmfd	sp!, {lr}
	ldr	r0, =WIMP_VERSION
	ldr	r1, =TASK_MAGIC
	adrl	r2, task_name
	adrl	r3, msg_list
	swi	XWimp_Initialise
	bvs	no_wimp
	adrl	r0, task_handle
	str	r1, [r0]
	ldmfd	sp!, {pc}

no_wimp:
	adrl	r0, err_no_wimp
	b	fatal

@ Report the error at r0 and go. Nothing here is worth struggling on without.
fatal:
	mov	r1, #1			@ an OK button
	adrl	r2, task_name
	swi	XWimp_ReportError
	swi	OS_Exit

	.ltorg

@ Put the icon on the icon bar: the card's own sprite, out of the ROM and into
@ the Wimp's pool by !Run.
make_icon:
	stmfd	sp!, {lr}
	mov	r0, #0			@ the Wimp reads r0 as flags
	adrl	r1, icon_def
	swi	XWimp_CreateIcon
	bvs	fatal			@ an application with no icon is no use
	adrl	r1, icon_handle
	str	r0, [r1]
	ldmfd	sp!, {pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The windows
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Load both windows out of the Templates file and hang them off the menu. A menu
@ item whose submenu field is a window handle opens that window, which is how
@ RISC OS has shown an information window from a menu since the Wimp had menus.
load_windows:
	stmfd	sp!, {r4, lr}
	mov	r0, #0
	adrl	r1, path_templates
	swi	XWimp_OpenTemplate
	bvs	no_templates

	adrl	r0, name_displayinfo
	bl	load_one
	adrl	r1, display_window
	str	r0, [r1]

	adrl	r0, name_proginfo
	bl	load_one
	adrl	r1, proginfo_window
	str	r0, [r1]

	mov	r0, #0
	swi	XWimp_CloseTemplate

	adrl	r4, menu_block
	adrl	r0, proginfo_window
	ldr	r0, [r0]
	str	r0, [r4, #MENU_HEADER + ITEM_INFO * MENU_ITEM_SIZE + 4]
	adrl	r0, display_window
	ldr	r0, [r0]
	str	r0, [r4, #MENU_HEADER + ITEM_DISPLAY * MENU_ITEM_SIZE + 4]

	ldmfd	sp!, {r4, pc}

no_templates:
	adrl	r0, err_no_templates
	b	fatal

@ Load the template named at r0 and create the window. Out: r0 = its handle.
load_one:
	stmfd	sp!, {r4, r5, r6, lr}
	mov	r5, r0			@ the name to look for
	adrl	r0, indirect_free
	ldr	r2, [r0]		@ what is left of the indirected workspace
	adrl	r1, win_buf
	adrl	r3, indirect_end
	mvn	r4, #0			@ no fonts
	mov	r6, #0			@ from the start of the file
	mov	r0, #0
	swi	XWimp_LoadTemplate
	bvs	no_templates
	teq	r6, #0
	beq	no_templates		@ not in the file
	adrl	r0, indirect_free
	str	r2, [r0]

	mov	r0, #0
	adrl	r1, win_buf
	swi	XWimp_CreateWindow
	bvs	no_templates
	ldmfd	sp!, {r4, r5, r6, pc}

	.ltorg

@ The Wimp draws the icons; there is nothing of our own in either window, so a
@ redraw is the rectangle loop and no more.
redraw_window:
	stmfd	sp!, {lr}
	adrl	r1, poll_block
	swi	XWimp_RedrawWindow
	bvs	2f
1:
	teq	r0, #0
	beq	2f
	adrl	r1, poll_block
	swi	XWimp_GetRectangle
	bvc	1b
2:
	ldmfd	sp!, {pc}

open_window:
	stmfd	sp!, {lr}
	adrl	r1, poll_block
	swi	XWimp_OpenWindow
	ldmfd	sp!, {pc}

close_window:
	stmfd	sp!, {lr}
	adrl	r1, poll_block
	swi	XWimp_CloseWindow
	ldmfd	sp!, {pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The menu
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Menu on our icon opens the menu. Select and Adjust are left alone, as they are
@ on any icon bar icon that has nothing to open.
mouse_click:
	stmfd	sp!, {r4, lr}
	adrl	r4, poll_block
	ldr	r0, [r4, #12]		@ window handle
	cmn	r0, #2			@ the icon bar is window handle -2
	bne	1f
	ldr	r0, [r4, #16]
	adrl	r1, icon_handle
	ldr	r1, [r1]
	teq	r0, r1
	bne	1f
	ldr	r0, [r4, #8]		@ which button
	tst	r0, #BUTTON_MENU
	beq	1f			@ Select and Adjust are not ours

	adrl	r1, menu_block
	ldr	r0, [r4, #0]		@ pointer x
	sub	r2, r0, #64
	mov	r3, #96 + ITEM_COUNT * MENU_HEIGHT
	swi	XWimp_CreateMenu
1:
	ldmfd	sp!, {r4, pc}

	.ltorg

@ Menu selections: the two commands, and Quit. The first two items open windows,
@ which the Wimp does for us.
menu_selection:
	stmfd	sp!, {lr}
	adrl	r0, poll_block
	ldr	r0, [r0, #0]		@ first (and only) menu level

	teq	r0, #ITEM_ON
	beq	sel_on
	teq	r0, #ITEM_OFF
	beq	sel_off
	teq	r0, #ITEM_QUIT
	beq	quit
	ldmfd	sp!, {pc}

sel_on:
	adrl	r0, cmd_on
	b	sel_run
sel_off:
	adrl	r0, cmd_off
sel_run:
	swi	XOS_CLI
	bvc	1f
	@ A refusal is the interesting case - the monitor definition not offering the
	@ mode, most often - so pass on what RISC OS said rather than appearing to do
	@ nothing. *GfxCardStatus is the place to look when it is terse.
	mov	r1, #1			@ an OK button
	adrl	r2, task_name
	swi	XWimp_ReportError
1:
	bl	update_display
	ldmfd	sp!, {pc}

	.ltorg

@ The desktop asking us to go, or telling us a submenu is about to be shown.
user_message:
	stmfd	sp!, {lr}
	adrl	r0, poll_block
	ldr	r0, [r0, #16]		@ message action
	teq	r0, #0			@ Message_Quit
	beq	quit
	ldr	r1, =MSG_MENUWARNING
	teq	r0, r1
	bleq	menu_warning
	ldmfd	sp!, {pc}

@ A menu item whose submenu is a window: the Wimp tells us just before it would
@ show it, and we open the window ourselves. That is the handshake for a window
@ on a menu - the handle in the item is only what puts the arrow there.
menu_warning:
	stmfd	sp!, {r4, lr}
	adrl	r4, poll_block
	ldr	r1, [r4, #20]		@ what the item pointed at

	@ The figures are worth having fresh, and this is the moment they are about
	@ to be looked at.
	adrl	r0, display_window
	ldr	r0, [r0]
	teq	r0, r1
	bleq	update_display

	ldr	r1, [r4, #20]
	ldr	r2, [r4, #24]		@ where the Wimp wants it
	ldr	r3, [r4, #28]
	swi	XWimp_CreateSubMenu
	bvs	fatal
	ldmfd	sp!, {r4, pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ What the card is doing
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Rewrite the three figures: ask the driver to publish them, then read them back.
@ Done as the menu opens, so the window is right whenever it can be seen.
update_display:
	stmfd	sp!, {lr}

	adrl	r0, cmd_vars
	swi	XOS_CLI			@ if this fails the old figures simply stay

	adrl	r0, var_mode
	mov	r1, #ICON_MODE
	bl	var_icon
	adrl	r0, var_store
	mov	r1, #ICON_STORE
	bl	var_icon
	adrl	r0, var_frames
	mov	r1, #ICON_FRAMES
	bl	var_icon

	ldmfd	sp!, {pc}

@ Put the value of variable r0 into icon r1 of the display window.
var_icon:
	stmfd	sp!, {r0, r1, r2, r3, lr}
	mov	r3, r1
	adrl	r1, var_buf
	mov	r2, #0
	strb	r2, [r1]		@ so a failed read shows as nothing to say
	bl	read_var

	adrl	r2, var_buf
	ldrb	r0, [r2]
	teq	r0, #0
	beq	1f
	mov	r1, r3
	adrl	r0, display_window
	ldr	r0, [r0]
	bl	set_icon_text
1:
	ldmfd	sp!, {r0, r1, r2, r3, pc}

@ Read the variable named at r0 into the buffer at r1, NUL terminated. A variable
@ that cannot be read leaves the buffer as it was.
read_var:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	mov	r2, #VAR_SIZE - 1
	mov	r3, #0
	mov	r4, #0			@ as a string, unexpanded
	swi	XOS_ReadVarVal
	bvs	1f
	cmp	r2, #0
	blt	1f
	cmp	r2, #VAR_SIZE - 1
	movgt	r2, #VAR_SIZE - 1
	mov	r0, #0
	strb	r0, [r1, r2]		@ OS_ReadVarVal does not terminate it
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ What this is
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Fill the information window from the Messages file. Its four lines are the only
@ place the name, purpose, author and version are written down, so the window and
@ the message file cannot disagree.
fill_proginfo:
	stmfd	sp!, {lr}
	bl	load_messages

	adrl	r0, tok_name
	mov	r1, #ICON_NAME
	bl	msg_icon
	adrl	r0, tok_purpose
	mov	r1, #ICON_PURPOSE
	bl	msg_icon
	adrl	r0, tok_author
	mov	r1, #ICON_AUTHOR
	bl	msg_icon
	adrl	r0, tok_version
	mov	r1, #ICON_VERSION
	bl	msg_icon

	ldmfd	sp!, {pc}

@ Load the Messages file, whole, and terminate it. If it is not there the
@ templates' own text stays as it is.
load_messages:
	stmfd	sp!, {r4, r5, lr}
	adrl	r5, msg_buf
	mov	r0, #0
	strb	r0, [r5]

	mov	r0, #OSFILE_LOAD
	adrl	r1, path_messages
	mov	r2, r5
	mov	r3, #0			@ here, not at the file's own load address
	swi	XOS_File
	bvs	1f
	cmp	r4, #MSG_SIZE		@ what came back is the length loaded
	movhi	r4, #MSG_SIZE
	mov	r0, #0
	strb	r0, [r5, r4]		@ the spare word after it holds this
1:
	ldmfd	sp!, {r4, r5, pc}

@ Put the value of message token r0 into icon r1 of the information window.
msg_icon:
	stmfd	sp!, {r0, r1, r2, r3, lr}
	mov	r3, r1
	bl	msg_find
	teq	r2, #0
	beq	1f
	mov	r1, r3
	adrl	r0, proginfo_window
	ldr	r0, [r0]
	bl	set_icon_text
1:
	ldmfd	sp!, {r0, r1, r2, r3, pc}

@ Find the token at r0 in the loaded messages. Out: r2 = its value, or 0. The
@ token carries its own colon, so "Version:" cannot match "Versions:".
msg_find:
	stmfd	sp!, {r0, r1, r4, r5, r6, r7, lr}
	adrl	r2, msg_buf
1:					@ r2 is at the start of a line
	ldrb	r1, [r2]
	teq	r1, #0
	beq	5f			@ the end: it is not there
	mov	r4, r0
	mov	r5, r2
2:
	ldrb	r6, [r4], #1
	teq	r6, #0
	beq	4f			@ the whole token matched
	ldrb	r7, [r5], #1
	teq	r6, r7
	beq	2b
3:					@ on to the next line
	ldrb	r1, [r2], #1
	teq	r1, #0
	subeq	r2, r2, #1		@ leave it on the terminator
	beq	1b
	teq	r1, #10
	teqne	r1, #13
	bne	3b
	b	1b
4:
	mov	r2, r5
	b	6f
5:
	mov	r2, #0
6:
	ldmfd	sp!, {r0, r1, r4, r5, r6, r7, pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Small change
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Put the string at r2 into icon r1 of window r0, and have it redrawn. The buffer
@ and its length are the icon's own, so a template with more room needs no change
@ here and one with less cannot be overrun.
set_icon_text:
	stmfd	sp!, {r0, r1, r2, r3, r4, r5, lr}
	adrl	r5, icon_block
	str	r0, [r5, #0]
	str	r1, [r5, #4]
	mov	r0, #0
	mov	r1, r5
	swi	XWimp_GetIconState
	bvs	1f

	ldr	r0, [r5, #24]		@ icon flags
	tst	r0, #ICON_INDIRECTED
	beq	1f			@ nowhere to put it
	ldr	r3, [r5, #28]		@ the text buffer
	ldr	r4, [r5, #36]		@ and its length
	bl	copy_line

	mov	r0, #0
	str	r0, [r5, #8]		@ EOR nothing
	str	r0, [r5, #12]		@ and clear nothing: just redraw it
	mov	r1, r5
	swi	XWimp_SetIconState
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, r5, pc}

@ Copy the string at r2 to r3 in at most r4 bytes, terminator included. Stops at
@ NUL, CR or LF, so a line straight out of a message file needs no preparation.
copy_line:
	stmfd	sp!, {r0, r2, r3, r4, lr}
	cmp	r4, #0
	ble	3f
	sub	r4, r4, #1		@ room for the terminator
1:
	cmp	r4, #0
	ble	2f
	ldrb	r0, [r2], #1
	teq	r0, #0
	teqne	r0, #10
	teqne	r0, #13
	beq	2f
	strb	r0, [r3], #1
	sub	r4, r4, #1
	b	1b
2:
	mov	r0, #0
	strb	r0, [r3]
3:
	ldmfd	sp!, {r0, r2, r3, r4, pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Text and tables
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	@ The messages we want. Message_Quit is always delivered, and is the only
	@ other one we act on.
msg_list:
	.int	MSG_MENUWARNING
	.int	0			@ end of the list

	@ An empty validation string, for indirected menu items that need none: the
	@ Wimp walks this looking for a terminator, so -1 will not do.
empty_string:
	.byte	13, 0, 0, 0

	@ The card's sprite, as *IconSprites in !Run puts it into the Wimp's pool.
sprite_valid:
	.byte	'S'
	.string	"!gfxcard"
	.balign	4, 0
task_name:
	.string	"GfxCard"
	.balign	4, 0

	@ The application directory, as !Run leaves it: GfxCard$Path.
path_templates:
	.string	"GfxCard:Templates"
	.balign	4, 0
path_messages:
	.string	"GfxCard:Messages"
	.balign	4, 0

	@ Template names. Room for the twelve characters a template name can have,
	@ because the Wimp writes back the name it matched.
name_displayinfo:
	.string	"displayinfo"
	.space	16 - 12, 0
name_proginfo:
	.string	"proginfo"
	.space	16 - 9, 0

	@ Message tokens, colon included
tok_name:
	.string	"Name:"
	.balign	4, 0
tok_purpose:
	.string	"Purpose:"
	.balign	4, 0
tok_author:
	.string	"Author:"
	.balign	4, 0
tok_version:
	.string	"Version:"
	.balign	4, 0

var_mode:
	.string	"GfxCard$Mode"
	.balign	4, 0
var_store:
	.string	"GfxCard$Store"
	.balign	4, 0
var_frames:
	.string	"GfxCard$Frames"
	.balign	4, 0

cmd_vars:
	.string	"GfxCardVars"
	.balign	4, 0
cmd_on:
	.string	"GfxCardOn"
	.balign	4, 0
cmd_off:
	.string	"GfxCardOff"
	.balign	4, 0

err_no_card:
	.int	0
	.string	"No graphics card here: this needs the RPCEmu graphics card and its driver."
	.balign	4, 0
err_no_wimp:
	.int	0
	.string	"GfxCard could not start as a desktop task."
	.balign	4, 0
err_no_templates:
	.int	0
	.string	"GfxCard could not load its windows: run it from its own directory."
	.balign	4, 0

	@ The icon bar icon: the card's sprite, centred in the usual 68 by 68.
icon_def:
	.int	ICONBAR_RIGHT
	.int	0			@ x0
	.int	0			@ y0
	.int	68			@ x1
	.int	68			@ y1
	.int	ICONBAR_FLAGS
	.int	empty_string		@ no text: the sprite says it
	.int	sprite_valid
	.int	1

	@ The menu: a header, then one entry per item. The first two items open the
	@ windows; their submenu fields are filled in once the windows exist.
menu_block:
	.ascii	"GfxCard"		@ title, 12 bytes
	.byte	0, 0, 0, 0, 0
	.byte	7			@ title foreground
	.byte	2			@ title background
	.byte	7			@ work area foreground
	.byte	0			@ work area background
	.int	MENU_WIDTH
	.int	MENU_HEIGHT
	.int	0			@ gap between items

	.macro	menu_item index, flags
	.int	\flags
	.int	-1			@ no submenu, unless one is filled in
	.int	MENU_ITEM_FLAGS
	.int	item_text + \index * TEXT_SIZE
	.int	empty_string		@ never -1: the Wimp walks this
	.int	TEXT_SIZE
	.endm

	menu_item ITEM_INFO, MENU_WARN
	menu_item ITEM_DISPLAY, MENU_WARN + MENU_DOTTED
	menu_item ITEM_ON, 0
	menu_item ITEM_OFF, MENU_DOTTED
	menu_item ITEM_QUIT, MENU_LAST


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Workspace
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

task_handle:
	.int	0
icon_handle:
	.int	0
display_window:
	.int	0
proginfo_window:
	.int	0

	@ The indirected data of both windows, as the Wimp copies it out of the
	@ template file, and how much of it is left.
indirect_free:
	.int	indirect_ws

	@ The menu's item text. It never changes: the figures are in the windows.
item_text:
	.asciz	"Info"
	.space	TEXT_SIZE - 5
	.asciz	"Display"
	.space	TEXT_SIZE - 8
	.asciz	"Display on the card"
	.space	TEXT_SIZE - 20
	.asciz	"Display on VIDC20"
	.space	TEXT_SIZE - 18
	.asciz	"Quit"
	.space	TEXT_SIZE - 5

var_buf:
	.space	VAR_SIZE, 0
msg_buf:
	.space	MSG_SIZE + 4, 0		@ a spare word for the terminator
icon_block:
	.space	40, 0
win_buf:
	.space	WINDOW_SIZE, 0
indirect_ws:
	.space	INDIRECT_SIZE, 0
indirect_end:

poll_block:
	.space	256, 0

	.space	STACK_SIZE, 0
stack_top:

	.end
