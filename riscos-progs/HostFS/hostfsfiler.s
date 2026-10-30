        @
        @ $Id: hostfsfiler.s,v 1.3 2008/12/08 20:30:30 mhowkins Exp $
        @
        @ HostFS Filer
        @

        @ Register naming
        wp .req r12

        @ ARM constants
        VBIT = 1 << 28
        CBIT = 1 << 29
        ZBIT = 1 << 30
        NBIT = 1 << 31

        @ RISC OS constants
        XOS_CLI               = 0x20005
        XOS_Exit              = 0x20011
        XOS_Module            = 0x2001e
        XOS_ReadModeVariable  = 0x20035
        XOS_ReadMonotonicTime = 0x20042
        XWimp_Initialise = 0x600c0
        XWimp_CreateIcon    = 0x600c2
	XWimp_DeleteIcon    = 0x600c4
	XWimp_SetIconState  = 0x600c6
	XWimp_CreateMenu    = 0x600d4
        XWimp_CloseDown     = 0x600dd
        XWimp_PollIdle      = 0x600e1
        XWimp_SpriteOp      = 0x600e9

        Module_Enter = 2
        Module_Claim = 6
        Module_Free  = 7

        Message_Quit = 0

        Service_Reset             = 0x27
        Service_StartFiler        = 0x4b
        Service_StartedFiler      = 0x4c
        Service_FilerDying        = 0x4f

        SpriteOp_ReadSpriteInfo = 40

        ModeVariable_YEig = 5

        WIMP_VERSION = 300

        WIMP_POLL_MASK = 0x00000031     @ no Null, Pointer Entering or Pointer Leaving events

        @ ArcEm SWI chunk
        ARCEM_SWI_CHUNK  = 0x56ac0
        ARCEM_SWI_CHUNKX = ARCEM_SWI_CHUNK | 0x20000
        ArcEm_HostFS    = ARCEM_SWI_CHUNKX + 1

        HOSTFS_OP_QUERY_DISC_NAME = 8

        WORKSPACE_SIZE = 832	@ includes 64-byte stack at end

        WS_MY_TASK_HANDLE      = 0
        WS_FILER_TASK_HANDLE   = 4
        WS_WIMP_VERSION        = 8
        WS_ICON_BAR_BLOCK      = 12	@ 9 words = 36 bytes (12-47)
        WS_ICON_BAR_BLOCK2     = 48	@ 9 words = 36 bytes (48-83)
        WS_HOSTFS_ICON_HANDLE  = 84
        WS_SHARED_ICON_HANDLE  = 88
        WS_CURRENT_MENU        = 92	@ 0 = HostFS, 1 = Shared
        @ Dynamic strings built from the configured disc name
        WS_DISC_NAME           = 96	@ 12 bytes: disc name (up to 10 + NUL)
        WS_ICON_TEXT           = 108	@ 12 bytes: icon bar text (same as disc name)
        WS_FILER_CMD           = 120	@ 48 bytes: "Filer_OpenDir HostFS::<name>.$"
        WS_FREE_CMD            = 168	@ 48 bytes: "ShowFree -fs HostFS <name>"
        WS_MENU                = 216	@ 76 bytes: 2-item main menu (Name disc + Free)
        WS_MENU_NAME           = 292	@ 52 bytes: 1-item submenu (writable name field)
        WS_MENU_NAME_BUF       = 344	@ 12 bytes: writable item buffer
        WS_NAMEDISC_CMD        = 356	@ 32 bytes: "HostFS_NameDisc <name>"
        WS_WIMP_BLOCK          = 388	@ must be last



        .global _start

_start:

module_start:

        .int	start           @ Start
        .int	init            @ Initialisation
        .int	final           @ Finalisation
        .int    service_pre     @ Service Call
        .int	modtitle        @ Title String
        .int	help            @ Help String
        .int	table           @ Help and Command keyword table
        .int	0               @ SWI Chunk base
        .int	0               @ SWI handler code
        .int	0               @ SWI decoding table
        .int	0               @ SWI decoding code
        .int    0               @ Message File
        .int    modflags        @ Module Flags

modflags:
        .int    1               @ 32 bit compatible

modtitle:
        .string	"RPCEmuHostFSFiler"

help:
        .string	"HostFSFiler\t0.08 (30 Oct 2026)"
        .align


        @ Help and Command keyword table
table:
desktop_hostfsfiler:
        .string "Desktop_HostFSFiler"
        .align
        .int    command_desktop_hostfsfiler
        .int    0x00070000
        .int    0
        .int    command_desktop_hostfsfiler_help

        .byte   0       @ Table terminator

command_desktop_hostfsfiler_help:
        .string	"The HostFSFiler provides the HostFS icons on the icon bar, and uses the Filer to display HostFS directories.\rDo not use *Desktop_HostFSFiler, use *Desktop instead."

        .align



init:
	stmfd	sp!, {lr}

	@ See if we need to claim some workspace
	ldr	r0, [r12]
	teq	r0, #0
	bne	1f

	@ Claim some workspace
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {pc}       @ no memory claimed then refuse to initialise

	str	r2, [r12]
1:
	ldr	wp, [r12]

	@ Initialise the workspace
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]

	ldmfd	sp!, {pc}



final:
	stmfd	sp!, {lr}

	ldr	wp, [r12]

	@ Close Wimp task if active
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	cmp	r0, #0
	ldrgt	r1, TASK
	swigt	XWimp_CloseDown

	@ Free workspace
	mov	r0, #Module_Free
	mov	r2, r12
	swi	XOS_Module

        @ Clear V flag (26/32 bit safe) so our module will die
        cmp     pc, #0          @ Clears V (also clears N, Z, and sets C)
	ldmfd	sp!, {pc}



         @ RISC OS 4 Service codetable
service_codetable:
        .int    0               @ no special flags enabled
        .int    service_main
        .int    Service_Reset
        .int    Service_StartFiler
        .int    Service_StartedFiler
        .int    Service_FilerDying
        .int    0               @ table terminator
        .int    service_codetable
service_pre:
        mov     r0, r0          @ magic instruction, pointer to service table at service_pre-4
        teq     r1, #Service_Reset
        teqne   r1, #Service_StartFiler
        teqne   r1, #Service_StartedFiler
        teqne   r1, #Service_FilerDying
        movne   pc, lr

service_main:
        stmfd   sp!, {lr}

        ldr     wp, [r12]

        teq     r1, #Service_Reset
        beq     service_reset
        teq     r1, #Service_StartFiler
        beq     service_startfiler
        teq     r1, #Service_StartedFiler
        beq     service_startedfiler
        teq     r1, #Service_FilerDying
        beq     service_filerdying

        @ Should never reach here
        ldmfd   sp!, {pc}



service_reset:
	@ Zero the Task Handle
	mov	r14, #0
	str	r14, [wp, #WS_MY_TASK_HANDLE]
	ldmfd	sp!, {pc}



service_startfiler:
	ldr	r14, [wp, #WS_MY_TASK_HANDLE]
	teq	r14, #0                                 @ Am I already active?
	moveq	r14, #-1                                @ No, so set handle to -1
	streq	r14, [wp, #WS_MY_TASK_HANDLE]
	streq	r1,  [wp, #WS_FILER_TASK_HANDLE]        @ store Filer's task handle
	adreq	r0,  desktop_hostfsfiler                @ r0 points to command to start task
	moveq	r1,  #0                                 @ claim the service
	ldmfd	sp!, {pc}



service_startedfiler:
	@ Zero the Task Handle if it is -1
	ldr	r14, [wp, #WS_MY_TASK_HANDLE]
	cmp	r14, #-1
	moveq	r14, #0
	streq	r14, [wp, #WS_MY_TASK_HANDLE]
	ldmfd	sp!, {pc}



service_filerdying:
        @ Shut down task if active

        stmfd	sp!, {r0-r1}

        ldr	r0, [wp, #WS_MY_TASK_HANDLE]
        cmp	r0, #0

        @ Zero the Task Handle if non-zero
        movne	r14, #0
        strne	r14, [wp, #WS_MY_TASK_HANDLE]

        @ Shut down task if Task Handle was positive
        ldrgt	r1, TASK
        swigt	XWimp_CloseDown

        ldmfd	sp!, {r0-r1}
        ldmfd	sp!, {pc}



command_desktop_hostfsfiler:
	stmfd	sp!, {lr}
	mov	r2, r0
	adr	r1, modtitle
	mov	r0, #Module_Enter
	swi	XOS_Module
	ldmfd	sp!, {pc}



TASK:
	.ascii	"TASK"

task_modtitle:
	.string	"HostFS Filer"
	.align

default_disc_name:
	.string	"HostFS"
	.align

icon_bar_block:
	.int	-5		@ Left side of icon bar, scan from left (RO3+)
	.int	0		@ Minimum X
	.int	-16		@ Minimum Y
	.int	96		@ Maximum X
	.int	20		@ Maximum Y (excludes Sprite - added later)
	.int	0x1700310b	@ Flags (includes Indirected Text and Sprite)
	.int	0		@ Gap for pointer to Text
	.int	0		@ Gap for pointer to Validation String
	.int	12		@ Length of Text buffer

icon_bar_validation:
	.ascii	"S"		@ Unterminated - continues below...
icon_bar_icon_name:
	.string	"harddisc"

	.align

@ Second icon data for Shared drive
icon_bar_block2:
	.int	-5		@ Left side of icon bar, scan from left (RO3+)
	.int	0		@ Minimum X
	.int	-16		@ Minimum Y
	.int	96		@ Maximum X
	.int	20		@ Maximum Y (excludes Sprite - added later)
	.int	0x1700310b	@ Flags (includes Indirected Text and Sprite)
	.int	0		@ Gap for pointer to Text
	.int	0		@ Gap for pointer to Validation String
	.int	6		@ Length of Text buffer

icon_bar_text2:
	.string	"Shared"

icon_bar_validation2:
	.ascii	"S"		@ Unterminated - continues below...
icon_bar_icon_name2:
	.string	"harddisc"

	.align

@ Menu template for the Shared drive (static)
menu_shared:
	.string	"Shared"	@ Menu Title, padded to 12 bytes
	.align
	.int	0

	.byte	7, 2, 7, 0	@ Title colours
	.int	16 * 6		@ Width
	.int	44		@ Height
	.int	0		@ Vertical gap
	@ Menu items
	.int	(1 << 7)	@ Flags: last item
	.int	-1		@ Submenu pointer
	.int	0x07000001	@ Menu item icon flags, Text
	.string	"Free"		@ Menu item icon data, padded to 12 bytes
	.align
	.int	0

@ Static strings
str_filer_prefix:
	.string	"Filer_OpenDir HostFS::"
str_free_prefix:
	.string	"ShowFree -fs HostFS "
str_root_suffix:
	.string	".$"
str_namedisc_prefix:
	.string	"HostFS_NameDisc "
str_name_disc_item:
	.string	"Name disc"
str_disc_name_title:
	.string	"Disc name"
menu_name_validation:
	.ascii	"A~$&%@\\^:.#*\"|"
	.byte	' ', 0
	.align



	@ "Start" entry point
	@ Entered in User Mode
	@ Therefore no need to preserve link register before calling SWIs
start:
        ldr     wp, [r12]               @ Get workspace pointer
	add	sp, wp, #WORKSPACE_SIZE	@ Set up USR stack in workspace
        ldr     r0, [wp, #WS_MY_TASK_HANDLE]
        cmp     r0, #0                  @ Am I already active?
        ble     start_skipclosedown     @ No then skip following instructions
        ldr     r1, TASK                @ Yes, so close down first
        swi     XWimp_CloseDown
        mov     r0, #0                  @ Mark as inactive
        str     r0, [wp, #WS_MY_TASK_HANDLE]

start_skipclosedown:
	bl	query_disc_name
	bl	build_filer_strings
	bl	build_menu

	ldr	r0, = WIMP_VERSION	@ (re)start the task
	ldr	r1, TASK
	adr	r2, task_modtitle
	swi	XWimp_Initialise
	swivs	XOS_Exit		@ Exit if error

	str	r0, [wp, #WS_WIMP_VERSION]	@ store Wimp version
	str	r1, [wp, #WS_MY_TASK_HANDLE]	@ store Task handle

	@ Prepare block for Icon Bar icon
	adrl	r0, icon_bar_block
	add	r1, wp, #WS_ICON_BAR_BLOCK

	ldmia	r0, {r2-r10}
	add	r8, wp, #WS_ICON_TEXT		@ Text -> workspace
	adrl	r9, icon_bar_validation
	stmia	r1, {r2-r10}

	@ Calculate size of Icon Bar Icon
	mov	r0, #SpriteOp_ReadSpriteInfo
	adrl	r2, icon_bar_icon_name
	swi	XWimp_SpriteOp
	movvc	r0, r6
	movvc	r1, #ModeVariable_YEig
	swivc	XOS_ReadModeVariable
	bvs	close_down

	@ Add sprite height to Maximum Y of icon's Bounding Box
	ldr	r0, [r12, #WS_ICON_BAR_BLOCK + 16]
	add	r0, r0, r4, lsl r2	@ += Pixels << YEig
	str	r0, [r12, #WS_ICON_BAR_BLOCK + 16]

	@ Create HostFS Icon on Icon Bar
	mov	r0, #0x71000000		@ Priority higher than ADFS Hard Disc but lower than CD-ROM discs
	add	r1, wp, #WS_ICON_BAR_BLOCK
	swi	XWimp_CreateIcon
	bvs	close_down
	str	r0, [wp, #WS_HOSTFS_ICON_HANDLE]	@ Save icon handle

	@ Prepare block for second Icon Bar icon (Shared)
	adrl	r0, icon_bar_block2
	add	r1, wp, #WS_ICON_BAR_BLOCK2

	ldmia	r0, {r2-r10}
	adrl	r8, icon_bar_text2		@ Fill in pointers
	adrl	r9, icon_bar_validation2
	stmia	r1, {r2-r10}

	@ Calculate size of second Icon Bar Icon
	mov	r0, #SpriteOp_ReadSpriteInfo
	adrl	r2, icon_bar_icon_name2
	swi	XWimp_SpriteOp
	movvc	r0, r6
	movvc	r1, #ModeVariable_YEig
	swivc	XOS_ReadModeVariable
	bvs	close_down

	@ Add sprite height to Maximum Y of second icon's Bounding Box
	ldr	r0, [r12, #WS_ICON_BAR_BLOCK2 + 16]
	add	r0, r0, r4, lsl r2	@ += Pixels << YEig
	str	r0, [r12, #WS_ICON_BAR_BLOCK2 + 16]

	@ Create Shared Icon on Icon Bar
	mov	r0, #0x71000000		@ Priority just after HostFS icon
	add	r0, r0, #1
	add	r1, wp, #WS_ICON_BAR_BLOCK2
	swi	XWimp_CreateIcon
	bvs	close_down
	str	r0, [wp, #WS_SHARED_ICON_HANDLE]	@ Save icon handle


	@ Main poll loop
re_poll:
	swi	XOS_ReadMonotonicTime	@ returns time in r0
	add	r2, r0, #100		@ poll no sooner than 1 sec unless event
	ldr	r0, = WIMP_POLL_MASK
	add	r1, wp, #WS_WIMP_BLOCK	@ point to Wimp block within workspace
	swi	XWimp_PollIdle
	bvs	close_down

	teq	r0, #6			@ 6 = Mouse Click
	beq	mouse_click
	teq	r0, #9			@ 9 = Menu Selection
	beq	menu_selection
	teq	r0, #17			@ 17 = User Message
	teqne	r0, #18			@ 18 = User Message Recorded
	beq	user_message
	b	re_poll


mouse_click:
	@ Check if click is on icon bar (window handle -2)
	ldr	r2, [r1, #12]		@ Window handle
	cmp	r2, #-2
	bne	re_poll

	@ Check which icon was clicked
	ldr	r3, [r1, #16]		@ Icon handle from click
	ldr	r4, [wp, #WS_SHARED_ICON_HANDLE]
	cmp	r3, r4
	beq	mouse_click_shared

	ldr	r4, [wp, #WS_HOSTFS_ICON_HANDLE]
	cmp	r3, r4
	beq	mouse_click_hostfs

	b	re_poll

mouse_click_hostfs:
	ldr	r0, [r1, #8]		@ Buttons

	cmp	r0, #4			@ Select
	cmpne	r0, #1			@ Adjust
	bne	1f
	add	r0, wp, #WS_FILER_CMD
	swi	XOS_CLI
	b	re_poll
1:
	cmp	r0, #2			@ Menu
	bne	re_poll

	mov	r0, #0			@ Flag: HostFS menu
	str	r0, [wp, #WS_CURRENT_MENU]

	ldr	r2, [r1, #0]		@ X coordinate of click
	sub	r2, r2, #64
	mov	r3, #(96 + 44)
	add	r1, wp, #WS_MENU
	swi	XWimp_CreateMenu

	b	re_poll

mouse_click_shared:
	ldr	r0, [r1, #8]		@ Buttons

	cmp	r0, #4			@ Select
	cmpne	r0, #1			@ Adjust
	adreq	r0, cli_command_shared
	swieq	XOS_CLI
	beq	re_poll

	cmp	r0, #2			@ Menu
	bne	re_poll

	mov	r0, #1			@ Flag: Shared menu
	str	r0, [wp, #WS_CURRENT_MENU]

	ldr	r2, [r1, #0]		@ X coordinate of click
	sub	r2, r2, #64
	mov	r3, #(96 + 44)
	adrl	r1, menu_shared
	swi	XWimp_CreateMenu

	b	re_poll

cli_command_shared:
	.string	"Filer_OpenDir HostFS::Shared.$"
	.align


menu_selection:
	ldr	r0, [wp, #WS_CURRENT_MENU]
	cmp	r0, #1
	beq	menu_selection_shared

	@ HostFS menu: check item index
	ldr	r0, [wp, #WS_WIMP_BLOCK]
	cmp	r0, #0
	beq	menu_selection_name
	cmp	r0, #1
	bne	re_poll

	@ Item 1: Free
	add	r0, wp, #WS_FREE_CMD
	swi	XOS_CLI
	b	re_poll

menu_selection_shared:
	adrl	r0, free_cli_command_shared
	swi	XOS_CLI
	b	re_poll

menu_selection_name:
	@ Build "HostFS_NameDisc <name>" at WS_NAMEDISC_CMD
	adrl	r0, str_namedisc_prefix
	add	r1, wp, #WS_NAMEDISC_CMD
	bl	copy_string
	sub	r1, r1, #1		@ back over NUL
	add	r0, wp, #WS_MENU_NAME_BUF
	bl	copy_string

	add	r0, wp, #WS_NAMEDISC_CMD
	swi	XOS_CLI

	@ Re-query and rebuild everything
	bl	query_disc_name
	bl	build_filer_strings
	bl	build_menu

	@ Delete old icon and recreate with updated text
	add	r1, wp, #WS_WIMP_BLOCK
	mvn	r0, #1			@ -2 = icon bar
	str	r0, [r1]
	ldr	r0, [wp, #WS_HOSTFS_ICON_HANDLE]
	str	r0, [r1, #4]
	swi	XWimp_DeleteIcon

	mov	r0, #0x71000000		@ same priority as original
	add	r1, wp, #WS_ICON_BAR_BLOCK
	swi	XWimp_CreateIcon
	str	r0, [wp, #WS_HOSTFS_ICON_HANDLE]

	b	re_poll

free_cli_command_shared:
	.string	"ShowFree -fs HostFS Shared"
	.align


user_message:
	ldr	r0, [r1, #16]		@ Contains message code
	teq	r0, #Message_Quit	@ Is it Quit message...?
	bne	re_poll			@ ...no so re-poll
					@ otherwise continue to...
close_down:
	@ Close down Wimp task
	ldr	r0, [wp, #WS_MY_TASK_HANDLE]
	ldr	r1, TASK
	swi	XWimp_CloseDown

	@ Zero the Task Handle
	mov	r0, #0
	str	r0, [wp, #WS_MY_TASK_HANDLE]

	swi	XOS_Exit


@ -----------------------------------------------------------------------
@ Query the disc name from the emulator into WS_DISC_NAME.
@ Falls back to "HostFS" if the emulator does not support the query.
@ -----------------------------------------------------------------------
query_disc_name:
	stmfd	sp!, {r0-r3, r9, lr}

	add	r0, wp, #WS_DISC_NAME
	mov	r1, #12
	mov	r9, #HOSTFS_OP_QUERY_DISC_NAME
	swi	ArcEm_HostFS

	@ Check non-empty
	ldrb	r0, [wp, #WS_DISC_NAME]
	cmp	r0, #0
	bne	1f

	@ Fall back to default
	adrl	r0, default_disc_name
	add	r1, wp, #WS_DISC_NAME
	bl	copy_string
1:
	@ Copy disc name to icon text buffer too
	add	r0, wp, #WS_DISC_NAME
	add	r1, wp, #WS_ICON_TEXT
	bl	copy_string

	ldmfd	sp!, {r0-r3, r9, pc}

@ -----------------------------------------------------------------------
@ Build "Filer_OpenDir HostFS::<name>.$" at WS_FILER_CMD and
@ "ShowFree -fs HostFS <name>" at WS_FREE_CMD.
@ -----------------------------------------------------------------------
build_filer_strings:
	stmfd	sp!, {r0-r3, lr}

	@ "Filer_OpenDir HostFS::<name>.$"
	adrl	r0, str_filer_prefix
	add	r1, wp, #WS_FILER_CMD
	bl	copy_string
	sub	r1, r1, #1		@ back over NUL
	add	r0, wp, #WS_DISC_NAME
	bl	copy_string
	sub	r1, r1, #1
	adrl	r0, str_root_suffix
	bl	copy_string

	@ "ShowFree -fs HostFS <name>"
	adrl	r0, str_free_prefix
	add	r1, wp, #WS_FREE_CMD
	bl	copy_string
	sub	r1, r1, #1
	add	r0, wp, #WS_DISC_NAME
	bl	copy_string

	ldmfd	sp!, {r0-r3, pc}

@ -----------------------------------------------------------------------
@ Build two Wimp menu blocks for the HostFS drive:
@   WS_MENU      — main menu (title = disc name): "Name disc" + "Free"
@   WS_MENU_NAME — submenu (title = "Disc name"): writable name field
@ Also copies the current disc name to WS_MENU_NAME_BUF.
@ -----------------------------------------------------------------------
build_menu:
	stmfd	sp!, {r0-r4, lr}

	@ Copy current disc name into writable buffer
	add	r0, wp, #WS_DISC_NAME
	add	r1, wp, #WS_MENU_NAME_BUF
	bl	copy_string

	@ === Main menu at WS_MENU ===
	add	r3, wp, #WS_MENU

	@ Title: disc name, NUL-padded to 12 bytes
	add	r0, wp, #WS_DISC_NAME
	mov	r1, r3
	bl	copy_string
	sub	r2, r1, r3
1:	cmp	r2, #12
	movlt	r4, #0
	strltb	r4, [r1], #1
	addlt	r2, r2, #1
	blt	1b

	@ Title colours
	mov	r0, #7
	strb	r0, [r1], #1
	mov	r0, #2
	strb	r0, [r1], #1
	mov	r0, #7
	strb	r0, [r1], #1
	mov	r0, #0
	strb	r0, [r1], #1

	@ Width, height, gap
	mov	r0, #(16 * 12)
	str	r0, [r1], #4
	mov	r0, #44
	str	r0, [r1], #4
	mov	r0, #0
	str	r0, [r1], #4

	@ Item 0: "Name disc" with submenu arrow
	mov	r0, #0
	str	r0, [r1], #4		@ Flags: 0
	add	r0, wp, #WS_MENU_NAME
	str	r0, [r1], #4		@ Submenu: pointer to name submenu
	ldr	r0, =0x07000001
	str	r0, [r1], #4		@ Icon flags: Text
	mov	r4, r1			@ save icon data start
	adrl	r0, str_name_disc_item
	bl	copy_string
	add	r1, r4, #12		@ advance past 12-byte icon data

	@ Item 1: "Free" (last item)
	mov	r0, #(1 << 7)
	str	r0, [r1], #4
	mvn	r0, #0
	str	r0, [r1], #4
	ldr	r0, =0x07000001
	str	r0, [r1], #4
	adrl	r0, str_free_item
	bl	copy_string

	@ === Name submenu at WS_MENU_NAME ===
	add	r3, wp, #WS_MENU_NAME

	@ Title: "Disc name", NUL-padded to 12 bytes
	adrl	r0, str_disc_name_title
	mov	r1, r3
	bl	copy_string
	sub	r2, r1, r3
2:	cmp	r2, #12
	movlt	r4, #0
	strltb	r4, [r1], #1
	addlt	r2, r2, #1
	blt	2b

	@ Title colours
	mov	r0, #7
	strb	r0, [r1], #1
	mov	r0, #2
	strb	r0, [r1], #1
	mov	r0, #7
	strb	r0, [r1], #1
	mov	r0, #0
	strb	r0, [r1], #1

	@ Width, height, gap
	mov	r0, #(16 * 12)
	str	r0, [r1], #4
	mov	r0, #44
	str	r0, [r1], #4
	mov	r0, #0
	str	r0, [r1], #4

	@ Item 0: writable name field (last item)
	mov	r0, #((1 << 7) | (1 << 2))
	str	r0, [r1], #4		@ Flags: last + writable
	mvn	r0, #0
	str	r0, [r1], #4		@ Submenu: -1
	ldr	r0, =0x07000121
	str	r0, [r1], #4		@ Icon flags: text | fill | indirected
	add	r0, wp, #WS_MENU_NAME_BUF
	str	r0, [r1], #4		@ buffer pointer
	adrl	r0, menu_name_validation
	str	r0, [r1], #4		@ validation pointer
	mov	r0, #11
	str	r0, [r1], #4		@ buffer length (10 chars + NUL)

	ldmfd	sp!, {r0-r4, pc}

str_free_item:
	.string	"Free"
	.align

@ -----------------------------------------------------------------------
@ Copy a NUL-terminated string.
@ Entry: r0 = src, r1 = dst
@ Exit:  r1 = one past the NUL written (r0, r2 corrupted)
@ -----------------------------------------------------------------------
copy_string:
0:	ldrb	r2, [r0], #1
	strb	r2, [r1], #1
	cmp	r2, #0
	bne	0b
	mov	pc, lr
