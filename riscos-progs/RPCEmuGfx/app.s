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
@ A Wimp task: an icon on the icon bar showing the mode the card is displaying,
@ and a menu carrying the card's figures and the two commands worth having to
@ hand. It is carried in the card's own ROM and registered with ResourceFS by the
@ driver, so it lives in Resources:$.Apps and nothing is installed on a disc.
@
@ The figures come from the driver, not from the card: expansion card space is
@ mapped for privileged access only, so an application cannot read those registers
@ however much it knows about them. *GfxCardVars has the driver format what there
@ is to say into GfxCard$Mode, GfxCard$Store and GfxCard$Frames, and this reads
@ those - which also means the same figures are available to an Obey file or a
@ BASIC program.
@
@ An absolute application (filetype &FF8): loaded at &8000 and entered there.

	.arch	armv3

	@ RISC OS SWIs (X-form where a failure is survivable)
	OS_WriteS		= 0x00001
	OS_Exit			= 0x00011
	XOS_CLI			= 0x20005
	XOS_ReadVarVal		= 0x20023
	XOS_ConvertCardinal4	= 0x200d8
	XWimp_Initialise	= 0x600c0
	XWimp_CreateIcon	= 0x600c2
	XWimp_Poll		= 0x600c7
	XWimp_CreateMenu	= 0x600d4
	XWimp_SetIconState	= 0x600cd
	XWimp_CloseDown		= 0x600dd
	XWimp_ReportError	= 0x600df
	XWimp_GetPointerInfo	= 0x600cf

	WIMP_VERSION	= 310
	TASK_MAGIC	= 0x4b534154		@ "TASK"

	@ Wimp_Poll reason codes we act on
	EVENT_NULL	= 0
	EVENT_CLICK	= 6
	EVENT_MENU	= 9
	EVENT_USERMSG	= 17
	EVENT_USERMSG_R	= 18

	@ Poll mask: nulls are not wanted. The live figures are in the menu, which is
	@ refreshed as it opens, so there is nothing to do between events.
	POLL_MASK	= 1

	@ Mouse buttons, as Wimp_Poll reports them for an icon-bar icon
	BUTTON_MENU	= 2

	@ The icon bar, as a window handle
	ICONBAR_RIGHT	= -1

	@ Icon flags: indirected text, centred, black on grey, click-reporting
	ICON_FLAGS	= 1 + 8 + 16 + 0x100 + (3 << 12) + (7 << 24)

	@ Menu item flags
	MENU_DOTTED	= 1 << 1
	MENU_LAST	= 1 << 7
	MENU_ITEM_FLAGS	= 1 + 16 + 0x100 + (7 << 24)

	@ Menu geometry, in OS units
	MENU_WIDTH	= 320
	MENU_HEIGHT	= 44

	@ Menu items, in order
	ITEM_MODE	= 0
	ITEM_STORE	= 1
	ITEM_FRAMES	= 2
	ITEM_ON		= 3
	ITEM_OFF	= 4
	ITEM_QUIT	= 5
	ITEM_COUNT	= 6

	@ Card registers, as offsets from RPCEmuGfx$Base. These must match the
	@ register map in src/gfxcard.h.
	REG_ID		= 0x00
	REG_FB_SIZE	= 0x10
	REG_CTRL	= 0x14
	REG_WIDTH	= 0x1c
	REG_HEIGHT	= 0x20
	REG_BPP		= 0x24
	REG_STRIDE	= 0x28
	REG_MAX_WIDTH	= 0x38
	REG_MAX_HEIGHT	= 0x3c
	REG_FRAMES	= 0x40

	GFXCARD_ID		= 0x52504778	@ "RPGx"
	GFXCARD_CTRL_ENABLE	= 0x0001

	@ Sizes
	TEXT_SIZE	= 48		@ per indirected text buffer
	STACK_SIZE	= 1024


	.global	_start
_start:
	@ Entered with nothing useful in the registers, so the stack comes first.
	adrl	sp, stack_top

	bl	find_card
	bl	wimp_start
	bl	make_icon

poll_loop:
	mov	r0, #POLL_MASK
	adrl	r1, poll_block
	swi	XWimp_Poll
	bvs	poll_loop		@ a failed poll is not worth dying for

	teq	r0, #EVENT_CLICK
	bleq	mouse_click
	teq	r0, #EVENT_MENU
	bleq	menu_selection
	teq	r0, #EVENT_USERMSG
	teqne	r0, #EVENT_USERMSG_R
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
	mov	r1, #1			@ an OK button
	adrl	r2, task_name
	swi	XWimp_ReportError
	swi	OS_Exit

	.ltorg

@ Become a Wimp task.
wimp_start:
	stmfd	sp!, {lr}
	ldr	r0, =WIMP_VERSION
	ldr	r1, =TASK_MAGIC
	adrl	r2, task_name
	mov	r3, #0			@ every message: we only look at Quit
	swi	XWimp_Initialise
	bvs	no_wimp
	adrl	r0, task_handle
	str	r1, [r0]
	ldmfd	sp!, {pc}

no_wimp:
	adrl	r0, err_no_wimp
	mov	r1, #1
	adrl	r2, task_name
	swi	XWimp_ReportError
	swi	OS_Exit

	.ltorg

@ Put the icon on the icon bar. Text only, deliberately: a sprite would have to
@ be carried in the ROM as well, and the text says something useful.
make_icon:
	stmfd	sp!, {r4, lr}
	adrl	r4, poll_block
	mov	r0, #ICONBAR_RIGHT
	str	r0, [r4, #0]
	mov	r0, #0
	str	r0, [r4, #4]		@ x0
	str	r0, [r4, #8]		@ y0
	mov	r0, #160
	str	r0, [r4, #12]		@ x1
	mov	r0, #68
	str	r0, [r4, #16]		@ y1
	ldr	r0, =ICON_FLAGS
	str	r0, [r4, #20]
	adrl	r0, icon_text
	str	r0, [r4, #24]
	@ The validation pointer must point at a string, even an empty one: this Wimp
	@ walks it looking for a terminator, so -1 ("none") aborts inside
	@ Wimp_CreateIcon rather than being understood.
	adrl	r0, empty_string
	str	r0, [r4, #28]
	mov	r0, #TEXT_SIZE
	str	r0, [r4, #32]

	@ R0 must be zero: the Wimp reads it as flags, and anything left in it from
	@ building the block above is taken as a request for a different block shape.
	mov	r0, #0
	mov	r1, r4
	swi	XWimp_CreateIcon
	adrl	r1, icon_handle
	strvc	r0, [r1]

	ldmfd	sp!, {r4, pc}

	.ltorg

@ A click on our icon opens the menu. Menu button or Select, either will do: the
@ menu is all there is.
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

	bl	update_menu
	adrl	r1, menu_block
	ldr	r0, [r4, #0]		@ pointer x
	sub	r2, r0, #64
	mov	r3, #96 + ITEM_COUNT * MENU_HEIGHT
	swi	XWimp_CreateMenu
1:
	ldmfd	sp!, {r4, pc}

	.ltorg

@ Menu selections: the two commands, and Quit. The figures above them are there
@ to be read, and do nothing when chosen.
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
	swi	XOS_CLI			@ its own errors are reported by the OS
	ldmfd	sp!, {pc}

	.ltorg

@ The desktop asking us to go.
user_message:
	stmfd	sp!, {lr}
	adrl	r0, poll_block
	ldr	r0, [r0, #16]		@ message action
	teq	r0, #0			@ Message_Quit
	beq	quit
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The menu
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Rewrite the three figures, just before the menu opens: ask the driver to
@ publish them, then read them back.
update_menu:
	stmfd	sp!, {lr}

	adrl	r0, cmd_vars
	swi	XOS_CLI			@ if this fails the old text simply stays

	adrl	r0, var_mode
	adrl	r1, item_text + ITEM_MODE * TEXT_SIZE
	bl	read_var
	adrl	r0, var_store
	adrl	r1, item_text + ITEM_STORE * TEXT_SIZE
	bl	read_var
	adrl	r0, var_frames
	adrl	r1, item_text + ITEM_FRAMES * TEXT_SIZE
	bl	read_var

	ldmfd	sp!, {pc}

@ Read the variable named at r0 into the buffer at r1, NUL terminated. A variable
@ that cannot be read leaves the buffer as it was.
read_var:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	mov	r2, #TEXT_SIZE - 1
	mov	r3, #0
	mov	r4, #0			@ as a string, unexpanded
	swi	XOS_ReadVarVal
	bvs	1f
	cmp	r2, #0
	blt	1f
	cmp	r2, #TEXT_SIZE - 1
	movgt	r2, #TEXT_SIZE - 1
	mov	r0, #0
	strb	r0, [r1, r2]		@ OS_ReadVarVal does not terminate it
1:
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

	.ltorg

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Small change
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Is the card scanning out? Z set if it is not.
card_displaying:
	stmfd	sp!, {r0, r1, lr}
	adrl	r0, card_base
	ldr	r0, [r0]
	ldr	r0, [r0, #REG_CTRL]
	tst	r0, #GFXCARD_CTRL_ENABLE
	ldmfd	sp!, {r0, r1, pc}

@ Append the NUL-terminated string at r6 to r5, leaving r5 after the last byte.
append_string:
	stmfd	sp!, {r0, lr}
1:
	ldrb	r0, [r6], #1
	teq	r0, #0
	strneb	r0, [r5], #1
	bne	1b
	ldmfd	sp!, {r0, pc}

@ Append the decimal value of card register r0 to r5.
append_reg:
	stmfd	sp!, {r0, r1, lr}
	adrl	r1, card_base
	ldr	r1, [r1]
	ldr	r0, [r1, r0]
	bl	append_number
	ldmfd	sp!, {r0, r1, pc}

@ Append r0 in decimal to r5.
append_number:
	stmfd	sp!, {r0, r1, r2, lr}
	mov	r1, r5
	mov	r2, #12
	swi	XOS_ConvertCardinal4	@ writes at r1; r1 -> the terminating NUL
	movvc	r5, r1
	ldmfd	sp!, {r0, r1, r2, pc}

@ Copy the NUL-terminated string at r1 to r0.
string_copy:
	stmfd	sp!, {r0, r1, r2, lr}
1:
	ldrb	r2, [r1], #1
	strb	r2, [r0], #1
	teq	r2, #0
	bne	1b
	ldmfd	sp!, {r0, r1, r2, pc}

@ Compare the NUL-terminated strings at r0 and r1. Z set if they match.
string_equal:
	stmfd	sp!, {r0, r1, r2, r3, lr}
1:
	ldrb	r2, [r0], #1
	ldrb	r3, [r1], #1
	teq	r2, r3
	bne	2f
	teq	r2, #0
	bne	1b
2:
	ldmfd	sp!, {r0, r1, r2, r3, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Text and tables
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	@ An empty validation string, for indirected icons that need none.
empty_string:
	.byte	13, 0, 0, 0

task_name:
	.string	"GfxCard"
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

text_idle:
	.string	"GfxCard"
	.balign	4, 0
text_by_x:
	.string	"x"
	.balign	4, 0
text_by:
	.string	" x "
	.balign	4, 0
text_comma:
	.string	", "
	.balign	4, 0
text_bpp:
	.string	"bpp"
	.balign	4, 0
text_not_displaying:
	.string	"Not displaying"
	.balign	4, 0
text_store:
	.string	"Framestore "
	.balign	4, 0
text_of:
	.string	"K of "
	.balign	4, 0
text_kb:
	.string	"K"
	.balign	4, 0
text_frames:
	.string	"Frames displayed "
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

	@ The menu: a header, then one entry per item. The three figures are
	@ rewritten before the menu opens; the rest never change.
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
	.int	-1			@ no submenu
	.int	MENU_ITEM_FLAGS
	.int	item_text + \index * TEXT_SIZE
	.int	empty_string		@ never -1: the Wimp walks this
	.int	TEXT_SIZE
	.endm

	menu_item ITEM_MODE, 0
	menu_item ITEM_STORE, 0
	menu_item ITEM_FRAMES, MENU_DOTTED
	menu_item ITEM_ON, 0
	menu_item ITEM_OFF, MENU_DOTTED
	menu_item ITEM_QUIT, MENU_LAST


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Workspace
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

card_base:
	.int	0
task_handle:
	.int	0
icon_handle:
	.int	0

var_buffer:
	.space	16, 0
icon_text:
	.asciz	"GfxCard"
	.space	TEXT_SIZE - 8, 0

	@ The menu's item text. The fixed entries are set up here; the figures are
	@ written before the menu opens.
item_text:
	.asciz	"-"
	.space	TEXT_SIZE - 2
	.asciz	"-"
	.space	TEXT_SIZE - 2
	.asciz	"-"
	.space	TEXT_SIZE - 2
	.asciz	"Display on the card"
	.space	TEXT_SIZE - 20
	.asciz	"Display on VIDC20"
	.space	TEXT_SIZE - 18
	.asciz	"Quit"
	.space	TEXT_SIZE - 5

poll_block:
	.space	256, 0

	.space	STACK_SIZE, 0
stack_top:

	.end
