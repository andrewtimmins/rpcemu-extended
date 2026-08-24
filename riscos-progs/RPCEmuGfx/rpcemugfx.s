@ RPCEmuGfx - display driver for the RPCEmu graphics card
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ The guest half of the emulated graphics card. The card carries its own
@ framestore, so the modes it can show are limited by the card's memory rather
@ than by how much VRAM is fitted to the machine: 2560x1440 in full colour, where
@ VIDC20 with 2MB of VRAM stops at 800x600.
@
@ This is an ordinary GraphicsV driver. It registers with OS_ScreenMode 64,
@ claims GraphicsV, and answers the calls the kernel makes to drive a display:
@ what the hardware can do, where its framestore is, what mode to program, where
@ to scan out from, and the palette. Everything it needs to know about the card
@ it reads from the card's own registers, which it finds through the address the
@ Podule manager reports for the slot. There is nothing machine-specific here and
@ nothing patched.
@
@ The card does not become the display just by being present: RISC OS keeps using
@ VIDC20 until asked to switch. *GfxCardOn switches to the card and *GfxCardOff
@ switches back, and *GfxCardStatus shows what is going on.
@
@ The approach follows the precedent set by ViewFinder, John Kortink's graphics
@ expansion card for the Acorn Risc PC, which showed that a card-hosted
@ framestore driven by its own display driver could take the machine well beyond
@ VIDC20's limits. No ViewFinder code, firmware or programming interface is used;
@ this driver is written from the GraphicsV documentation in the RISC OS sources.
@
@ GraphicsV and OS_ScreenMode 64 are RISC OS 5 features, so unlike the other
@ modules here this one is 32-bit only and uses mrs/msr where it needs the mode.
@ On an older RISC OS the driver declines to initialise and says why.

	@ ARMv3 is the RiscPC floor (ARM610/710); mrs/msr arrived with it.
	.arch	armv3

	ws	.req	r12		@ workspace, in module entries via [r12]

	@ ARM
	NBIT		= 1 << 31
	SVC32_MODE	= 0x13

	@ RISC OS SWIs (X-form: bit 17 set so errors return via V + R0)
	XOS_WriteC		= 0x20000
	XOS_Write0		= 0x20002
	XOS_NewLine		= 0x20003
	XOS_Module		= 0x2001e
	XOS_Claim		= 0x2001f
	XOS_Release		= 0x20020
	XOS_ClaimDeviceVector	= 0x2004b
	XOS_ReleaseDeviceVector	= 0x2004c
	XOS_CallAVector		= 0x20034
	XOS_IntOff		= 0x20013
	XOS_IntOn		= 0x20014
	XOS_ScreenMode		= 0x20065
	XOS_CLI			= 0x20005
	XOS_ReadModeVariable	= 0x20035
	XOS_ReadVarVal		= 0x20023
	XOS_ReadSysInfo		= 0x20058
	XOS_SetVarVal		= 0x20024
	XOS_ChangeDynamicArea	= 0x2002a
	XOS_ReadDynamicArea	= 0x2005c

	@ OS_SetVarVal / OS_ReadVarVal variable types (Kernel hdr/Variables)
	VarType_String		= 0
	VarType_Code		= 16
	XResourceFS_RegisterFiles	= 0x61b40
	XResourceFS_DeregisterFiles	= 0x61b41

	Service_ResourceFSStarting	= 0x60
	XOS_ConvertCardinal4	= 0x200d8
	XPodule_ReadInfo	= 0x6028d

	@ OS_Module reason codes
	Module_Claim	= 6
	Module_Free	= 7

	@ Dynamic area numbers
	ChangeDyn_Screen	= 2

	@ OS_ScreenMode reason codes
	ScreenMode_SelectMode		= 0
	ScreenMode_SelectMonitorType	= 3
	ScreenMode_SelectDevice		= 11
	ScreenMode_RegisterDriver	= 64
	ScreenMode_StartDriver		= 65
	ScreenMode_StopDriver		= 66
	ScreenMode_DeregisterDriver	= 67

	@ Podule_ReadInfo items we ask for. The words come back in ascending bit
	@ order, which is what the loads after the call assume.
	Podule_ReadInfo_SyncBase	= 1 << 1
	Podule_ReadInfo_EASILogical	= 1 << 9
	Podule_ReadInfo_IntMask		= 1 << 15
	Podule_ReadInfo_IntValue	= 1 << 16
	Podule_ReadInfo_IntDeviceVector	= 1 << 17
	READINFO_ITEMS = Podule_ReadInfo_EASILogical | Podule_ReadInfo_IntMask | Podule_ReadInfo_IntValue | Podule_ReadInfo_IntDeviceVector

	@ For finding our own card by number when it cannot be found by address:
	@ the two ways a card's base can be recorded, asked for together so one
	@ call answers whichever form initialisation handed us.
	READINFO_BASES = Podule_ReadInfo_SyncBase | Podule_ReadInfo_EASILogical

	@ Expansion card slots to look through. Every machine RISC OS runs on has
	@ eight, and asking about one that is empty is not an error.
	PODULE_COUNT = 8

	@ The monitor's EDID lives in an EEPROM at IIC address 0x50; RISC OS asks
	@ for it as 0xa1, that address shifted up with the read bit set, in bits
	@ 16-23 of the IICOp request (see readedidblock() in ScreenModes).
	EDID_IIC_READ		= 0xa1

	@ Monitor types, as RISC OS numbers them (see monitors.h in ScreenModes).
	@ Only EDID makes it read the monitor's own EDID - which is the one this card
	@ serves. Under any other type the modes on offer are whatever that type
	@ defines, however much the card could show.
	MONITOR_FILE		= 7	@ a loaded monitor definition file
	MONITOR_EDID		= 16
	MONITOR_AUTO		= 31

	@ GraphicsV
	GraphicsV		= 0x2a
	GraphicsV_VSync		= 1

	@ GraphicsV_ReadInfo items (r0)
	GVReadInfo_Version		= 0
	GVReadInfo_ModuleName		= 1
	GVReadInfo_DriverName		= 2
	GVReadInfo_HardwareName		= 3
	GVReadInfo_ControlListItems	= 4

	@ GraphicsV_DisplayFeatures flags
	GVFeature_HardwareScroll	= 1 << 0
	GVFeature_SeparateFramestore	= 1 << 3
	GVFeature_HardwarePointer	= 1 << 1
	GVFeature_NoVsyncIRQ		= 1 << 4
	GVFeature_VariableFramestore	= 1 << 5
	GVFeature_CopyRectangleIsFast	= 1 << 6

	@ Depths we can scan out, as GraphicsV states them: bit n set means 2^n
	@ bits per pixel is available, so 8bpp, 16bpp and 32bpp.
	GVPixelDepths	= (1 << 3) | (1 << 4) | (1 << 5)

	@ Buffer alignment we need for the framestore, in bytes.
	GVAlignment	= 16

	@ Mode flags (hdr:VduExt)
	ModeFlag_FullPalette	= 1 << 7
	ModeFlag_64k		= 1 << 7	@ same bit; means 565 at log2bpp 4

	@ Mode variables, as OS_ReadModeVariable numbers them
	ModeVar_ScreenSize	= 7
	ModeVar_Log2BPP		= 9
	ModeVar_XWindLimit	= 11
	ModeVar_YWindLimit	= 12

	@ VIDC list type 3 (hdr:VIDCList)
	VIDCList3_Type			= 0
	VIDCList3_PixelDepth		= 4
	VIDCList3_HorizDisplaySize	= 20
	VIDCList3_VertiDisplaySize	= 44
	VIDCList3_SyncPol		= 60
	VIDCList3_ControlList		= 64

	ControlList_ExtraBytes		= 14
	ControlList_Terminator		= -1

	SyncPol_InterlaceFields		= 16

	@ Card registers, as offsets from the register window. These must match
	@ the register map in src/gfxcard.h.
	REG_ID		= 0x00
	REG_VERSION	= 0x04
	REG_CAPS	= 0x08
	REG_FB_PHYS	= 0x0c
	REG_FB_SIZE	= 0x10
	REG_CTRL	= 0x14
	REG_STATUS	= 0x18
	REG_WIDTH	= 0x1c
	REG_HEIGHT	= 0x20
	REG_BPP		= 0x24
	REG_STRIDE	= 0x28
	REG_START	= 0x2c
	REG_PAL_INDEX	= 0x30
	REG_PAL_ENTRY	= 0x34
	REG_MAX_WIDTH	= 0x38
	REG_MAX_HEIGHT	= 0x3c
	REG_FRAMES	= 0x40
	REG_EDID_SIZE	= 0x44
	REG_EDID_INDEX	= 0x48
	REG_EDID_DATA	= 0x4c
	REG_PTR_CTRL	= 0x50
	REG_PTR_X	= 0x54
	REG_PTR_Y	= 0x58
	REG_PTR_WIDTH	= 0x5c
	REG_PTR_HEIGHT	= 0x60
	REG_PTR_PHYS	= 0x64
	REG_PTR_PAL_IDX	= 0x68
	REG_PTR_PAL	= 0x6c
	REG_OPTIONS	= 0x70
	REG_RENDER_OP	= 0x74
	REG_RENDER_X	= 0x78
	REG_RENDER_Y	= 0x7c
	REG_RENDER_W	= 0x80
	REG_RENDER_H	= 0x84
	REG_RENDER_SRC_X = 0x88
	REG_RENDER_SRC_Y = 0x8c
	REG_RENDER_PAT_IDX = 0x90
	REG_RENDER_PAT	= 0x94

	@ Operations REG_RENDER_OP performs, as GraphicsV_Render names them.
	RENDER_COPY	= 1
	RENDER_FILL	= 2
	RENDER_PATTERN_WORDS = 16

	@ Where the register window sits in the card's EASI space, and what the
	@ card's registers mean.
	GFXCARD_REG_BASE	= 0x00080000
	GFXCARD_ID		= 0x52504778	@ "RPGx"
	GFXCARD_VERSION		= 1

	GFXCARD_CAP_8BPP	= 0x0001
	GFXCARD_CAP_32BPP	= 0x0004
	GFXCARD_CAP_HW_SCROLL	= 0x0100
	GFXCARD_CAP_VSYNC	= 0x0200
	GFXCARD_CAP_EDID	= 0x0400
	GFXCARD_CAP_HW_POINTER	= 0x0800
	GFXCARD_CAP_RENDER	= 0x1000

	GFXCARD_CTRL_ENABLE	= 0x0001
	GFXCARD_CTRL_BLANK	= 0x0002
	GFXCARD_CTRL_VSYNC_IRQ	= 0x0004

	GFXCARD_STATUS_VSYNC	= 0x0001

	@ What the user asked the card to do, as opposed to what it can do.
	GFXCARD_OPT_BOOT_DISPLAY = 0x0001

	@ Workspace
	WS_REGS		= 0	@ logical address of the card's registers
	WS_PODULE	= 4	@ podule base, as we were given it at init
	WS_DRIVER	= 8	@ our GraphicsV driver number
	WS_FB_PHYS	= 12	@ framestore, as the card reports it
	WS_FB_SIZE	= 16
	WS_MAX_WIDTH	= 20	@ largest mode the card will accept
	WS_MAX_HEIGHT	= 24
	WS_FEATURES	= 28	@ what GraphicsV_DisplayFeatures answers
	WS_INTMASK	= 32	@ address of the podule interrupt mask register
	WS_INTBIT	= 36	@ our bit within it
	WS_DEVNO	= 40	@ device vector number for our interrupt
	WS_FLAGS	= 44	@ FLAG_*: what has been claimed, for finalisation
	WS_VSYNCS	= 48	@ vsync interrupts handled (diagnostics)
	WS_MODE_W	= 52	@ geometry last programmed (diagnostics)
	WS_MODE_H	= 56
	WS_MODE_BPP	= 60
	WS_GVHANDLER	= 64	@ our GraphicsV handler, as OS_Claim was given it
	WS_VSHANDLER	= 68	@ our vsync handler, likewise
	WS_SCRATCH	= 72	@ 4 words: Podule_ReadInfo results, number buffer
	WS_EDID_SIZE	= 96	@ EDID bytes the card can serve, 0 if none
	WS_VET_W	= 100	@ the last mode vetted, and what came of it, so a
	WS_VET_H	= 104	@ refusal can be explained rather than guessed at
	WS_VET_STRIDE	= 108
	WS_VET_L2BPP	= 112
	WS_VET_REASON	= 116
	WS_SAVE_W	= 84	@ geometry in use before a switch, so it survives it
	WS_SAVE_H	= 88
	WS_SAVE_L2BPP	= 92
	WS_CMD		= 120	@ CMD_SIZE bytes: *WimpMode command builder
	@ The trampoline the GfxCard$Display code variable is entered through. It
	@ has to live somewhere writable that stays put for the module's lifetime,
	@ and it has to hand the read code its workspace, which the kernel does not
	@ pass. See make_display_var.
	WS_VARTHUNK	= WS_CMD + CMD_SIZE	@ VARTHUNK_SIZE bytes
	WORKSPACE_SIZE	= WS_VARTHUNK + VARTHUNK_SIZE

	VARTHUNK_SIZE	= 16

	CMD_SIZE	= 64	@ "WimpMode X2560 Y1440 C16M" and room

	@ Why the last mode was refused (WS_VET_REASON)
	VET_OK		= 0
	VET_NOT_TYPE3	= 1
	VET_DEPTH	= 2
	VET_INTERLACE	= 3
	VET_NO_PIXELS	= 4
	VET_TOO_WIDE	= 5
	VET_TOO_TALL	= 6
	VET_TOO_BIG	= 7

	FLAG_VECTOR	= 1 << 0	@ GraphicsV claimed
	FLAG_REGISTERED	= 1 << 1	@ driver number allocated
	FLAG_STARTED	= 1 << 2	@ driver started
	FLAG_DEVICE	= 1 << 3	@ device vector claimed
	FLAG_RESOURCES	= 1 << 4	@ app registered with ResourceFS
	FLAG_DISPLAYVAR	= 1 << 5	@ GfxCard$Display code variable created


	.global	_start
_start:

module_start:
	.int	0		@ Start (not an application)
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	service		@ Service Call
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
	.string	"RPCEmuGfx"
	.align

	@ Defines "help", the *Help / *Modules string, generated from VersionNum by
	@ the Makefile so the version and date live in one place.
	.include "version.inc"

	@ Help and Command keyword table
table:
	.string	"GfxCardStatus"
	.align
	.int	command_status
	.int	0x00000000
	.int	0
	.int	command_status_help

	.string	"GfxCardOn"
	.align
	.int	command_on
	.int	0x00000000
	.int	0
	.int	command_on_help

	.string	"GfxCardOff"
	.align
	.int	command_off
	.int	0x00000000
	.int	0
	.int	command_off_help

	.string	"GfxCardVars"
	.align
	.int	command_vars
	.int	0x00000000
	.int	0
	.int	command_vars_help

	.byte	0	@ Table terminator

command_status_help:
	.string	"*GfxCardStatus shows the graphics card and its display state.\rSyntax: *GfxCardStatus"
	.align

command_vars_help:
	.string	"*GfxCardVars puts the card's state into GfxCard$Mode, GfxCard$Store, GfxCard$Frames and GfxCard$Display, for anything that cannot reach the card's registers itself.\rSyntax: *GfxCardVars"
	.align

command_on_help:
	.string	"*GfxCardOn makes the graphics card the display, so modes larger than VRAM allows become available.\rSyntax: *GfxCardOn"
	.align

command_off_help:
	.string	"*GfxCardOff returns the display to VIDC20.\rSyntax: *GfxCardOff"
	.align

driver_name:
	.string	"RPCEmu graphics card"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ GraphicsV
@
@ Entered on the vector with r4 = reason | head << 16 | driver << 24, and r12 the
@ workspace pointer we gave OS_Claim. A call we have handled returns with r4 = 0;
@ anything else is passed on untouched, which is how the calls we do not
@ implement reach whoever else can serve them.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

graphicsv:
	stmfd	sp!, {lr}
	ldr	lr, [ws, #WS_DRIVER]
	eor	lr, r4, lr, lsl #24	@ 0 in the top byte if this call is ours
	cmp	lr, #(gv_table_end - gv_table) / 4
	addlo	pc, pc, lr, lsl #2
	ldmfd	sp!, {pc}
gv_table:
	ldmfd	sp!, {pc}	@  0 Complete
	ldmfd	sp!, {pc}	@  1 VSync (driver to kernel, not to us)
	b	gv_setmode	@  2 SetMode
	ldmfd	sp!, {pc}	@  3 SetInterlace (deprecated)
	b	gv_setblank	@  4 SetBlank
	b	gv_updatepointer @  5 UpdatePointer
	b	gv_setdma	@  6 SetDMAAddress
	b	gv_vetmode	@  7 VetMode
	b	gv_features	@  8 DisplayFeatures
	b	gv_framestore	@  9 FramestoreAddress
	b	gv_writepal	@ 10 WritePaletteEntry
	b	gv_writepals	@ 11 WritePaletteEntries
	b	gv_readpal	@ 12 ReadPaletteEntry
	b	gv_render	@ 13 Render
	b	gv_iicop	@ 14 IICOp
	ldmfd	sp!, {pc}	@ 15 SelectHead (one head)
	ldmfd	sp!, {pc}	@ 16 StartupMode
	b	gv_pixelformats	@ 17 PixelFormats
	b	gv_readinfo	@ 18 ReadInfo
gv_table_end:

	@ 19 VetMode2 is deliberately not claimed. The kernel asks for it first and
	@ falls back to VetMode when nobody answers, and the fallback is not a
	@ legacy path: DoFullVetMode in the kernel rebuilds the whole VetMode2
	@ reply from DisplayFeatures and FramestoreAddress, both of which this
	@ driver answers, and arrives at exactly the ExtFramestore result with this
	@ card's address and size that claiming reason 19 would have returned.
	@ Implementing it would save two vector calls per mode vetted and change
	@ nothing else.

	@ Render: r0 = flags, r1 = operation, r2 -> its operands. Claimed by
	@ returning r4 = 0; left alone otherwise, and the kernel does the work on the
	@ ARM instead, which is what happened before this existed.
	@
	@ Two operations are worth taking. Copy moves a rectangle within the screen,
	@ which is what scrolling a window is, and fill paints one, which is what
	@ clearing a window is. At 1920x1080 either can be megabytes that no longer
	@ have to cross the bus a word at a time.
	@
	@ RISC OS counts pixels up from the bottom of the screen and the card counts
	@ rows down from the top, so every y is turned over here. Both ends of a
	@ RISC OS rectangle are inclusive, hence the +1 on the sizes.
	@
	@	copy:	+0 srcL  +4 srcB  +8 dstL  +12 dstB  +16 W-1  +20 H-1
	@	fill:	+0 left  +4 top   +8 right +12 bottom +16 -> pattern
gv_render:
	stmfd	sp!, {r0, r1, r2, r3, r5, r6, r7}
	ldr	r5, [ws, #WS_REGS]

	teq	r1, #RENDER_COPY
	beq	gv_render_copy
	teq	r1, #RENDER_FILL
	beq	gv_render_fill
	b	gv_render_pass		@ anything else is not ours

gv_render_copy:
	ldr	r6, [r5, #REG_HEIGHT]	@ rows on the display, for turning y over
	ldr	r7, [r2, #16]
	add	r7, r7, #1		@ width
	str	r7, [r5, #REG_RENDER_W]
	ldr	r7, [r2, #20]
	add	r7, r7, #1		@ height
	str	r7, [r5, #REG_RENDER_H]

	ldr	r0, [r2, #0]		@ source left
	str	r0, [r5, #REG_RENDER_SRC_X]
	ldr	r0, [r2, #4]		@ source bottom
	add	r0, r0, r7		@ + height
	sub	r0, r6, r0		@ = rows down to the source's top
	str	r0, [r5, #REG_RENDER_SRC_Y]

	ldr	r0, [r2, #8]		@ destination left
	str	r0, [r5, #REG_RENDER_X]
	ldr	r0, [r2, #12]		@ destination bottom
	add	r0, r0, r7
	sub	r0, r6, r0
	str	r0, [r5, #REG_RENDER_Y]

	mov	r0, #RENDER_COPY
	str	r0, [r5, #REG_RENDER_OP]
	b	gv_render_done

gv_render_fill:
	ldr	r6, [r5, #REG_HEIGHT]
	ldr	r0, [r2, #0]		@ left
	ldr	r1, [r2, #8]		@ right, inclusive
	sub	r1, r1, r0
	add	r1, r1, #1		@ width
	str	r0, [r5, #REG_RENDER_X]
	str	r1, [r5, #REG_RENDER_W]

	ldr	r0, [r2, #4]		@ top, counting up from the bottom
	ldr	r1, [r2, #12]		@ bottom, likewise
	sub	r3, r0, r1
	add	r3, r3, #1		@ height
	str	r3, [r5, #REG_RENDER_H]
	add	r0, r0, #1
	sub	r0, r6, r0		@ rows down to the rectangle's top
	str	r0, [r5, #REG_RENDER_Y]

	@ The pattern: eight (ora, eor) pairs the card applies as
	@ (destination OR ora) EOR eor, a pair per row of the pattern.
	ldr	r0, [r2, #16]
	teq	r0, #0
	beq	gv_render_pass		@ no pattern, so nothing we can paint with
	mov	r1, #0
	str	r1, [r5, #REG_RENDER_PAT_IDX]
	mov	r3, #RENDER_PATTERN_WORDS
1:
	ldr	r1, [r0], #4
	str	r1, [r5, #REG_RENDER_PAT]
	subs	r3, r3, #1
	bne	1b

	mov	r0, #RENDER_FILL
	str	r0, [r5, #REG_RENDER_OP]

gv_render_done:
	ldmfd	sp!, {r0, r1, r2, r3, r5, r6, r7}
	mov	r4, #0			@ done, do not do it again in software
	ldmfd	sp!, {pc}

gv_render_pass:
	ldmfd	sp!, {r0, r1, r2, r3, r5, r6, r7}
	ldmfd	sp!, {pc}		@ r4 untouched: not ours

	@ DisplayFeatures: out r0 = features, r1 = depths, r2 = alignment
gv_features:
	ldr	r0, [ws, #WS_FEATURES]
	mov	r1, #GVPixelDepths
	mov	r2, #GVAlignment
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ FramestoreAddress: out r0 = physical base, r1 = size. The kernel maps
	@ this itself, rounded out to megabytes, which is why the card puts its
	@ framestore on a megabyte boundary.
gv_framestore:
	ldr	r0, [ws, #WS_FB_PHYS]
	ldr	r1, [ws, #WS_FB_SIZE]
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ ReadInfo: r0 = which item, r1 -> buffer, r2 = its size.
	@
	@ ScreenModes asks for ControlListItems every time the display driver
	@ changes, and a driver that does not answer is recorded as supporting no
	@ control list items at all. That matters here: it then refuses any mode
	@ wanting ExtraBytes with "ExtraBytes wanted by user but not supported by
	@ driver", even though this driver has always honoured ExtraBytes - see
	@ mode_geometry, which adds it to the line length for vetting and for
	@ setting alike. The list below is what makes that support visible.
	@
	@ Out: the item in the buffer, r2 = bytes of the buffer left over (negative
	@ if it was too small, which is how the caller measures the item), r4 = 0
	@ to claim. An item we do not know is left unclaimed.
gv_readinfo:
	stmfd	sp!, {r0, r1, r3, r5, r6}

	cmp	r0, #(gv_ri_table_end - gv_ri_table) / 4
	addlo	pc, pc, r0, lsl #2
	b	gv_readinfo_pass
gv_ri_table:
	b	gv_ri_version		@ 0 Version
	b	gv_ri_name		@ 1 ModuleName
	b	gv_ri_name		@ 2 DriverName
	b	gv_ri_name		@ 3 HardwareName
	b	gv_ri_ctrllist		@ 4 ControlListItems
gv_ri_table_end:

gv_ri_version:
	adrl	r3, gv_ri_version_word
	mov	r5, #4
	b	gv_ri_copy

	@ The module, the driver and the hardware are all this one thing, so the
	@ module title answers all three. Length includes the terminator, as the
	@ stock driver's does.
gv_ri_name:
	adrl	r3, title
	mov	r5, #0
1:
	ldrb	r6, [r3, r5]
	add	r5, r5, #1
	teq	r6, #0
	bne	1b
	b	gv_ri_copy

gv_ri_ctrllist:
	adrl	r3, gv_ri_ctrllist_items
	mov	r5, #(gv_ri_ctrllist_end - gv_ri_ctrllist_items)

	@ Copy min(length, buffer size) bytes, and report the space left over
	@ before that clamp so a caller with too small a buffer learns the size.
gv_ri_copy:
	teq	r5, #0
	beq	gv_readinfo_pass
	sub	r6, r2, r5
	cmp	r5, r2
	movlt	r2, r5
1:
	subs	r2, r2, #1
	ldrgeb	r0, [r3], #1
	strgeb	r0, [r1], #1
	bgt	1b
	mov	r2, r6
	mov	r4, #0
	ldmfd	sp!, {r0, r1, r3, r5, r6}
	ldmfd	sp!, {pc}

gv_readinfo_pass:
	ldmfd	sp!, {r0, r1, r3, r5, r6}
	ldmfd	sp!, {pc}		@ r4 untouched: not ours

gv_ri_version_word:
	.int	VERSION_BCD << 8

	@ Only ExtraBytes: the one control list item this driver acts on. Claiming
	@ others would say the card honours settings it ignores. Terminated the way
	@ the stock driver terminates its list.
gv_ri_ctrllist_items:
	.int	ControlList_ExtraBytes
	.int	ControlList_Terminator
gv_ri_ctrllist_end:

	@ PixelFormats: out r0 -> list of (NColour, ModeFlags, Log2BPP), r1 = count
gv_pixelformats:
	adr	r0, pixel_formats
	mov	r1, #(pixel_formats_end - pixel_formats) / 12
	mov	r4, #0
	ldmfd	sp!, {pc}

pixel_formats:
	.int	255, ModeFlag_FullPalette, 3	@ 8bpp, 256 colours
	@ 16bpp. ModeFlag_64k is what says 565 rather than 555 at this depth, and
	@ it is the same bit as ModeFlag_FullPalette - the kernel reads it one way
	@ at log2bpp 4 and the other below it.
	.int	65535, ModeFlag_64k, 4		@ 16bpp, 64K colours, 565
	.int	-1, 0, 5			@ 32bpp, 16M colours
pixel_formats_end:

	@ UpdatePointer: r0 = flags (bit 0 show, bit 1 the shape has changed),
	@ r1 = x, r2 = y, r3 -> shape descriptor:
	@
	@	+0  width in bytes (2bpp, so four pixels each)
	@	+1  height in rows
	@	+4  logical address of the pixel data
	@	+8  physical address of the same
	@
	@ The card is given the physical address and fetches the shape itself, so
	@ the data stays where RISC OS built it. Claiming this call is what lets the
	@ front-end put the pointer where the host's mouse is: without it RISC OS
	@ plots a pointer into the framestore at a position the emulator's
	@ mouse-following does not drive, and the pointer sits still.
gv_updatepointer:
	stmfd	sp!, {r0, r5, r6}
	ldr	r5, [ws, #WS_REGS]

	tst	r0, #1
	beq	gv_up_hide

	tst	r0, #2			@ new shape?
	beq	gv_up_place
	ldrb	r6, [r3, #0]
	str	r6, [r5, #REG_PTR_WIDTH]
	ldrb	r6, [r3, #1]
	str	r6, [r5, #REG_PTR_HEIGHT]
	ldr	r6, [r3, #8]		@ physical address of the pixel data
	str	r6, [r5, #REG_PTR_PHYS]

gv_up_place:
	str	r1, [r5, #REG_PTR_X]
	str	r2, [r5, #REG_PTR_Y]
	mov	r6, #1
	str	r6, [r5, #REG_PTR_CTRL]
	b	gv_up_done

gv_up_hide:
	mov	r6, #0
	str	r6, [r5, #REG_PTR_CTRL]

gv_up_done:
	mov	r4, #0
	ldmfd	sp!, {r0, r5, r6}
	ldmfd	sp!, {pc}

	@ SetBlank: r0 = 0 unblank / 1 blank, r1 = DPMS state (which we cannot
	@ act on: there is no monitor to put to sleep).
gv_setblank:
	stmfd	sp!, {r2, r3}
	ldr	r2, [ws, #WS_REGS]
	ldr	r3, [r2, #REG_CTRL]
	teq	r0, #0
	biceq	r3, r3, #GFXCARD_CTRL_BLANK
	orrne	r3, r3, #GFXCARD_CTRL_BLANK
	str	r3, [r2, #REG_CTRL]
	mov	r4, #0
	ldmfd	sp!, {r2, r3}
	ldmfd	sp!, {pc}

	@ SetDMAAddress: r0 = which address, r1 = physical address. Only the
	@ display start (0) means anything to this card; the rest are claimed so
	@ the kernel does not go looking elsewhere for them.
gv_setdma:
	teq	r0, #0
	bne	gv_setdma_done
	stmfd	sp!, {r0, r2, r3}
	ldr	r2, [ws, #WS_REGS]
	ldr	r3, [ws, #WS_FB_PHYS]
	subs	r3, r1, r3		@ offset into the framestore
	bcc	gv_setdma_bad		@ below it: ignore rather than scan out
					@ from somewhere we were not given
	ldr	r0, [ws, #WS_FB_SIZE]
	cmp	r3, r0
	strcc	r3, [r2, #REG_START]
gv_setdma_bad:
	ldmfd	sp!, {r0, r2, r3}
gv_setdma_done:
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ WritePaletteEntry: r0 = type, r1 = &BBGGRRSS, r2 = index.
	@ Type 0 is the screen palette and type 2 the pointer's three colours; the
	@ border (type 1) is claimed and dropped, since the card has no border.
gv_writepal:
	stmfd	sp!, {r3}
	ldr	r3, [ws, #WS_REGS]
	teq	r0, #0
	streq	r2, [r3, #REG_PAL_INDEX]
	streq	r1, [r3, #REG_PAL_ENTRY]
	teq	r0, #2
	streq	r2, [r3, #REG_PTR_PAL_IDX]
	streq	r1, [r3, #REG_PTR_PAL]
	ldmfd	sp!, {r3}
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ WritePaletteEntries: r0 = type, r1 -> entries, r2 = first index,
	@ r3 = count. The card's index steps on after each entry, so this is one
	@ write of the index and then one word per colour.
gv_writepals:
	teq	r0, #0
	teqne	r0, #2
	bne	gv_writepals_done
	stmfd	sp!, {r0, r1, r3, r5, r6}
	ldr	r5, [ws, #WS_REGS]
	@ Which index and data register this run goes to.
	teq	r0, #2
	moveq	r6, #REG_PTR_PAL_IDX
	movne	r6, #REG_PAL_INDEX
	str	r2, [r5, r6]
	teq	r0, #2
	moveq	r6, #REG_PTR_PAL
	movne	r6, #REG_PAL_ENTRY
	teq	r3, #0
	beq	gv_writepals_end
1:
	ldr	r0, [r1], #4
	str	r0, [r5, r6]
	subs	r3, r3, #1
	bne	1b
gv_writepals_end:
	ldmfd	sp!, {r0, r1, r3, r5, r6}
gv_writepals_done:
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ ReadPaletteEntry: r0 = type, r2 = index; out r1 = &BBGGRRSS
gv_readpal:
	teq	r0, #0
	bne	gv_readpal_done
	stmfd	sp!, {r3}
	ldr	r3, [ws, #WS_REGS]
	str	r2, [r3, #REG_PAL_INDEX]
	ldr	r1, [r3, #REG_PAL_ENTRY]
	ldmfd	sp!, {r3}
gv_readpal_done:
	mov	r4, #0
	ldmfd	sp!, {pc}

	@ IICOp: r0 = command and device in bits 16-23, byte offset within the
	@ device in bits 0-15; r1 -> buffer; r2 = byte count.
	@ Out: r0 = 0 if the transfer worked, r1 and r2 updated.
	@
	@ Only a read of the monitor's EDID is answered, which is the one IIC
	@ transaction that matters here. RISC OS re-reads the EDID from the display
	@ driver whenever the driver changes (readedid() in ScreenModes); a driver
	@ that cannot answer leaves the machine with a fallback monitor definition
	@ and only the few modes that come with it, however much the card could
	@ show. The card serves the same block the emulator presents to the ROM, so
	@ switching to the card does not change which monitor the machine thinks it
	@ has.
gv_iicop:
	stmfd	sp!, {r3, r5, r6}
	ldr	r5, [ws, #WS_EDID_SIZE]
	teq	r5, #0
	beq	gv_iicop_pass		@ no EDID to serve: let something else try

	mov	r3, r0, lsr #16
	and	r3, r3, #0xff
	teq	r3, #EDID_IIC_READ
	bne	gv_iicop_pass		@ not the monitor, or not a read

	mov	r6, r0, lsl #16
	mov	r6, r6, lsr #16		@ byte offset within the EEPROM
	adds	r3, r6, r2
	bcs	gv_iicop_fail
	cmp	r3, r5
	bhi	gv_iicop_fail		@ past the end of what the card holds

	ldr	r3, [ws, #WS_REGS]
	str	r6, [r3, #REG_EDID_INDEX]
1:
	teq	r2, #0
	beq	2f
	ldr	r6, [r3, #REG_EDID_DATA]
	strb	r6, [r1], #1
	sub	r2, r2, #1
	b	1b
2:
	mov	r0, #0			@ transfer complete, no IIC error
	mov	r4, #0			@ claimed
	ldmfd	sp!, {r3, r5, r6}
	ldmfd	sp!, {pc}

	@ Claimed, but the device did not answer: r0 non-zero says so, and the
	@ untouched r2 says how much did not arrive.
gv_iicop_fail:
	mov	r0, #1
	mov	r4, #0
	ldmfd	sp!, {r3, r5, r6}
	ldmfd	sp!, {pc}

gv_iicop_pass:
	ldmfd	sp!, {r3, r5, r6}
	ldmfd	sp!, {pc}

	@ VetMode: r0 -> VIDC list type 3; out r0 = 0 if the mode is usable.
	@
	@ This is where a mode too big for the card is turned away, so that the
	@ kernel reports "Mode not available" instead of the card being asked to
	@ scan out from beyond its own memory.
gv_vetmode:
	stmfd	sp!, {r1, r2, r3, r5, r6}
	mov	r6, #VET_OK
	str	r6, [ws, #WS_VET_REASON]
	bl	mode_geometry		@ r1 = width, r2 = height, r3 = stride,
					@ r5 = log2bpp; V set if unusable
	@ Note what was asked for either way, so *GfxCardStatus can say what
	@ happened to the last mode the OS offered.
	str	r1, [ws, #WS_VET_W]
	str	r2, [ws, #WS_VET_H]
	str	r3, [ws, #WS_VET_STRIDE]
	str	r5, [ws, #WS_VET_L2BPP]
	bvs	gv_vetmode_bad

	ldr	r6, [ws, #WS_MAX_WIDTH]
	cmp	r1, r6
	movhi	r6, #VET_TOO_WIDE
	bhi	gv_vetmode_reason
	ldr	r6, [ws, #WS_MAX_HEIGHT]
	cmp	r2, r6
	movhi	r6, #VET_TOO_TALL
	bhi	gv_vetmode_reason

	mul	r6, r3, r2		@ bytes the mode needs
	ldr	r1, [ws, #WS_FB_SIZE]
	cmp	r6, r1
	movhi	r6, #VET_TOO_BIG
	bhi	gv_vetmode_reason

	mov	r0, #0			@ usable
	mov	r4, #0
	ldmfd	sp!, {r1, r2, r3, r5, r6}
	ldmfd	sp!, {pc}

gv_vetmode_reason:
	str	r6, [ws, #WS_VET_REASON]
gv_vetmode_bad:
	mvn	r0, #0			@ not usable
	mov	r4, #0
	ldmfd	sp!, {r1, r2, r3, r5, r6}
	ldmfd	sp!, {pc}

	@ SetMode: r0 -> VIDC list type 3. The mode has already been vetted, but
	@ it is read the same way here so the two can never disagree.
gv_setmode:
	stmfd	sp!, {r0, r1, r2, r3, r5, r6}
	bl	mode_geometry
	bvs	gv_setmode_out		@ vetted already, so this should not happen

	ldr	r6, [ws, #WS_REGS]
	str	r1, [r6, #REG_WIDTH]
	str	r2, [r6, #REG_HEIGHT]
	str	r3, [r6, #REG_STRIDE]
	mov	r0, #1
	mov	r0, r0, lsl r5		@ log2bpp -> bits per pixel
	str	r0, [r6, #REG_BPP]
	mov	r0, #0			@ scan out from the base until the
	str	r0, [r6, #REG_START]	@ kernel says otherwise

	str	r1, [ws, #WS_MODE_W]
	str	r2, [ws, #WS_MODE_H]
	mov	r0, #1
	mov	r0, r0, lsl r5
	str	r0, [ws, #WS_MODE_BPP]

	@ Display it. The vsync interrupt is only asked for if we have a device
	@ vector to receive it on; without one the kernel makes its own vsyncs
	@ from the centisecond ticker (see GVFeature_NoVsyncIRQ).
	mov	r0, #GFXCARD_CTRL_ENABLE
	ldr	r1, [ws, #WS_FLAGS]
	tst	r1, #FLAG_DEVICE
	orrne	r0, r0, #GFXCARD_CTRL_VSYNC_IRQ
	str	r0, [r6, #REG_CTRL]

gv_setmode_out:
	mov	r4, #0
	ldmfd	sp!, {r0, r1, r2, r3, r5, r6}
	ldmfd	sp!, {pc}

@ Read the geometry out of a VIDC list type 3.
@
@ in:  r0 -> VIDC list
@ out: r1 = width in pixels, r2 = height in lines, r3 = bytes per line,
@      r5 = log2bpp, V set if the list is one this card cannot show
@ Corrupts r6.
@
@ The line length is computed the way the kernel computes it (see ModeChangeSub
@ in the RISC OS kernel): the pixel width scaled by the depth, rounded up to
@ bytes, plus any ExtraBytes the control list asks for.
mode_geometry:
	stmfd	sp!, {lr}

	ldr	r1, [r0, #VIDCList3_Type]
	teq	r1, #3
	movne	r6, #VET_NOT_TYPE3
	bne	mode_geometry_bad

	ldr	r5, [r0, #VIDCList3_PixelDepth]
	teq	r5, #3			@ 8bpp
	teqne	r5, #4			@ 16bpp, which for this card is 565
	teqne	r5, #5			@ or 32bpp; nothing else is scanned out
	movne	r6, #VET_DEPTH
	bne	mode_geometry_bad

	@ Two distinct interlaced fields would need the card to reorder lines as
	@ it scans out, which it does not do.
	ldr	r1, [r0, #VIDCList3_SyncPol]
	tst	r1, #SyncPol_InterlaceFields
	movne	r6, #VET_INTERLACE
	bne	mode_geometry_bad

	ldr	r1, [r0, #VIDCList3_HorizDisplaySize]
	ldr	r2, [r0, #VIDCList3_VertiDisplaySize]
	teq	r1, #0
	teqne	r2, #0
	moveq	r6, #VET_NO_PIXELS
	beq	mode_geometry_bad

	mov	r3, r1, lsl r5
	add	r3, r3, #7
	mov	r3, r3, lsr #3		@ bytes per line, before ExtraBytes

	add	r6, r0, #VIDCList3_ControlList
1:
	ldr	lr, [r6], #4
	cmn	lr, #1			@ ControlList_Terminator?
	beq	mode_geometry_ok
	teq	lr, #ControlList_ExtraBytes
	ldr	lr, [r6], #4
	addeq	r3, r3, lr
	b	1b

mode_geometry_ok:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {pc}

@ Record why, then fail. r6 = reason.
mode_geometry_bad:
	str	r6, [ws, #WS_VET_REASON]
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ VSync interrupt
@
@ The card raises its interrupt once per displayed frame. RISC OS wants to know
@ because pointer and palette changes are timed to frame boundaries, and because
@ programs wait on the vsync event.
@
@ Entered in IRQ mode on the podule device vector with r12 = the workspace. The
@ GraphicsV call has to be made in SVC mode, as VIDC20Video's own vsync handler
@ does, because it goes through a SWI.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

vsync_handler:
	stmfd	sp!, {r0, r1, r4, r9, lr}
	ldr	r0, [ws, #WS_REGS]
	ldr	r1, [r0, #REG_STATUS]
	tst	r1, #GFXCARD_STATUS_VSYNC
	beq	vsync_not_ours

	mov	r1, #GFXCARD_STATUS_VSYNC	@ acknowledge, write to clear
	str	r1, [r0, #REG_STATUS]

	ldr	r1, [ws, #WS_VSYNCS]
	add	r1, r1, #1
	str	r1, [ws, #WS_VSYNCS]

	@ Prepare r4 before changing mode: r12 is ours until the SWI.
	ldr	r4, [ws, #WS_DRIVER]
	mov	r4, r4, lsl #24
	orr	r4, r4, #GraphicsV_VSync

	mrs	r9, cpsr
	orr	lr, r9, #SVC32_MODE
	msr	cpsr_c, lr
	stmfd	sp!, {r9, lr}		@ on the SVC stack: the SWI corrupts lr
	mov	r9, #GraphicsV
	swi	XOS_CallAVector
	ldmfd	sp!, {r9, lr}
	msr	cpsr_c, r9		@ back to IRQ mode, where our lr is intact

vsync_not_ours:
	ldmfd	sp!, {r0, r1, r4, r9, lr}
	mov	pc, lr


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ in: r11 = podule base, r12 -> private word
init:
	stmfd	sp!, {r4, r5, r6, lr}

	@ Claim zeroed workspace and record it in the private word.
	ldr	r0, [r12]
	teq	r0, #0
	bne	1f
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {r4, r5, r6, pc}	@ no memory -> refuse to initialise
	str	r2, [r12]
1:
	ldr	r5, [r12]		@ r5 = workspace for the rest of init
	mov	r0, #0
	mov	r1, #0
2:
	str	r0, [r5, r1]
	add	r1, r1, #4
	cmp	r1, #WORKSPACE_SIZE
	blo	2b

	str	r11, [r5, #WS_PODULE]

	@ Ask the Podule manager where this card is and how its interrupt works.
	@ Doing it this way rather than assuming an address means the card works
	@ in whichever slot it is fitted to.
	bl	podule_read_info
	bvs	init_failed

	add	r1, r5, #WS_SCRATCH
	ldmia	r1, {r2, r3, r4, r6}	@ EASI base, int mask addr, bit, devno
	str	r3, [r5, #WS_INTMASK]
	str	r4, [r5, #WS_INTBIT]
	str	r6, [r5, #WS_DEVNO]

	ldr	r0, =GFXCARD_REG_BASE
	add	r2, r2, r0
	str	r2, [r5, #WS_REGS]

	@ Check we are talking to the card we think we are. A stock RPCEmu with
	@ the card switched off does not present one at all, in which case the
	@ ROM this module came from is not there either - but a future card with
	@ a different interface might be, so the version is checked too.
	ldr	r0, [r2, #REG_ID]
	ldr	r1, =GFXCARD_ID
	teq	r0, r1
	bne	init_no_card
	ldr	r0, [r2, #REG_VERSION]
	teq	r0, #GFXCARD_VERSION
	bne	init_bad_version

	@ Everything else we need to know, the card tells us.
	ldr	r0, [r2, #REG_FB_PHYS]
	str	r0, [r5, #WS_FB_PHYS]
	ldr	r0, [r2, #REG_FB_SIZE]
	str	r0, [r5, #WS_FB_SIZE]
	ldr	r0, [r2, #REG_MAX_WIDTH]
	str	r0, [r5, #WS_MAX_WIDTH]
	ldr	r0, [r2, #REG_MAX_HEIGHT]
	str	r0, [r5, #WS_MAX_HEIGHT]
	ldr	r0, [r2, #REG_EDID_SIZE]
	str	r0, [r5, #WS_EDID_SIZE]

	@ The features we report. VariableFramestore is deliberately NOT claimed:
	@ this card's framestore is fixed, and claiming otherwise makes the kernel
	@ stop reading its address and size at initialisation without lifting the
	@ limit that matters (the video memory figure ScreenModes vets modes
	@ against comes from the fitted VRAM, which no driver flag changes).
	ldr	r0, [r2, #REG_CAPS]
	mov	r1, #GVFeature_SeparateFramestore
	tst	r0, #GFXCARD_CAP_HW_SCROLL
	orrne	r1, r1, #GVFeature_HardwareScroll
	tst	r0, #GFXCARD_CAP_HW_POINTER
	orrne	r1, r1, #GVFeature_HardwarePointer
	@ Say that copying a rectangle is quicker than plotting the same pixels
	@ again, which is what it is now the card does it: OS_SpriteOp 65 reads this
	@ to decide between the two.
	tst	r0, #GFXCARD_CAP_RENDER
	orrne	r1, r1, #GVFeature_CopyRectangleIsFast
	str	r1, [r5, #WS_FEATURES]

	@ Start from a known state: not displaying, not blanked, no interrupt.
	mov	r0, #0
	str	r0, [r2, #REG_CTRL]
	mov	r0, #GFXCARD_STATUS_VSYNC
	str	r0, [r2, #REG_STATUS]

	@ Claim GraphicsV before registering, so that a call arriving the moment
	@ we are registered finds a handler.
	adrl	r1, graphicsv
	str	r1, [r5, #WS_GVHANDLER]
	adrl	r0, vsync_handler
	str	r0, [r5, #WS_VSHANDLER]

	mov	r0, #GraphicsV
	mov	r2, r5
	swi	XOS_Claim
	bvs	init_failed
	mov	r0, #FLAG_VECTOR
	str	r0, [r5, #WS_FLAGS]

	@ Register as a display driver. This is where an older RISC OS, which
	@ has no such reason code, turns us away.
	mov	r0, #ScreenMode_RegisterDriver
	mov	r1, #0
	adrl	r2, driver_name
	swi	XOS_ScreenMode
	bvs	init_no_graphicsv
	str	r0, [r5, #WS_DRIVER]
	ldr	r0, [r5, #WS_FLAGS]
	orr	r0, r0, #FLAG_REGISTERED
	str	r0, [r5, #WS_FLAGS]

	@ Take the card's interrupt if we can. If we cannot, the kernel is told
	@ that no vsync interrupt is generated and makes its own, so a failure
	@ here costs smoothness rather than the whole card.
	ldr	r0, [r5, #WS_DEVNO]
	ldr	r1, [r5, #WS_VSHANDLER]
	mov	r2, r5
	ldr	r3, [r5, #WS_PODULE]
	mov	r4, #1
	swi	XOS_ClaimDeviceVector
	bvs	init_no_vsync_irq

	ldr	r0, [r5, #WS_FLAGS]
	orr	r0, r0, #FLAG_DEVICE
	str	r0, [r5, #WS_FLAGS]

	@ Let podule interrupts through. Read-modify-write of a register shared
	@ with every other card, so interrupts stay off across it.
	swi	XOS_IntOff
	ldr	r0, [r5, #WS_INTMASK]
	ldr	r1, [r5, #WS_INTBIT]
	ldrb	r2, [r0]
	orr	r2, r2, r1
	strb	r2, [r0]
	swi	XOS_IntOn
	b	init_start

init_no_vsync_irq:
	ldr	r0, [r5, #WS_FEATURES]
	orr	r0, r0, #GVFeature_NoVsyncIRQ
	str	r0, [r5, #WS_FEATURES]

init_start:
	@ Announce that the driver is ready. This does not switch the display to
	@ the card: RISC OS already has VIDC20 and keeps it. *GfxCardOn switches.
	mov	r0, #ScreenMode_StartDriver
	ldr	r1, [r5, #WS_DRIVER]
	swi	XOS_ScreenMode
	bvs	init_failed
	ldr	r0, [r5, #WS_FLAGS]
	orr	r0, r0, #FLAG_STARTED
	str	r0, [r5, #WS_FLAGS]

	@ Put the desktop app into Resources:$.Apps, out of this card's own ROM, so
	@ there is nothing to install and it is there only while the card is.
	adrl	r0, resfs_files
	swi	XResourceFS_RegisterFiles
	ldrvc	r0, [r5, #WS_FLAGS]
	orrvc	r0, r0, #FLAG_RESOURCES
	strvc	r0, [r5, #WS_FLAGS]

	@ And tell the app where to find the card. A system variable rather than a
	@ SWI: it is one word of information, and this way anything at all can read
	@ the card's registers - a BASIC program, an Obey file, a debugger.
	adrl	r0, var_base_name
	add	r1, r5, #WS_CMD
	mov	r2, #8
	ldr	r3, [r5, #WS_REGS]
	bl	hex_word		@ r1 -> 8 hex digits, NUL terminated
	adrl	r0, var_base_name
	add	r1, r5, #WS_CMD
	mov	r2, #8
	mov	r3, #0
	mov	r4, #VarType_String
	swi	XOS_SetVarVal

	@ GfxCard$Display, which reads "Card" or "VIDC20" and answers from the kernel
	@ every time it is read. Created here rather than by *GfxCardVars so it exists
	@ from the moment the driver does, and so nothing has to run a command before
	@ asking. Failing to create it is not worth refusing to initialise over - the
	@ card still works - so the error is discarded and the flag simply not set.
	bl	make_display_var
	ldrvc	r0, [r5, #WS_FLAGS]
	orrvc	r0, r0, #FLAG_DISPLAYVAR
	strvc	r0, [r5, #WS_FLAGS]

	@ Take the display now, if that is how the card is configured. Doing it here
	@ rather than leaving it to *GfxCardOn means the machine boots onto the card -
	@ splash, desktop and all - instead of changing mode once the desktop is up
	@ and reflowing every window.
	@
	@ The mode list is the monitor's, so the EDID monitor type is selected too
	@ when the configured one would not offer what the card can show. That is a
	@ selection for this session only (OS_ScreenMode 3), not a change to the
	@ configuration: a boot without the card behaves exactly as before.
	@ From the workspace, not r2: the registration calls above have had r2 for
	@ their own purposes since it last held the register base.
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_OPTIONS]
	tst	r0, #GFXCARD_OPT_BOOT_DISPLAY
	beq	init_banner

	bl	monitor_type
	teq	r1, #MONITOR_EDID
	teqne	r1, #MONITOR_FILE
	beq	init_take_display
	mov	r0, #ScreenMode_SelectMonitorType
	mov	r1, #MONITOR_EDID
	swi	XOS_ScreenMode		@ an error here is not fatal: the card still
					@ works, with fewer modes on offer

init_take_display:
	mov	r0, #ScreenMode_SelectDevice
	ldr	r1, [r5, #WS_DRIVER]
	swi	XOS_ScreenMode
	@ Also not fatal. If the display cannot be handed over this early the card
	@ is still registered and *GfxCardOn will do it later, so say so rather than
	@ leaving the user to wonder.
	bvc	init_banner
	adrl	r0, msg_boot_failed
	swi	XOS_Write0
	swi	XOS_NewLine

init_banner:
	@ Say hello on the startup screen, among the lines RISC OS prints as it finds
	@ what the machine has. Expansion cards conventionally announce themselves
	@ here, and it is the one place the card is visible without going looking for
	@ it. Only on success: there is nothing to announce if the card could not be
	@ taken on.
	adrl	r0, msg_banner
	swi	XOS_Write0
	swi	XOS_NewLine

	cmp	pc, #0			@ clear V for a successful return
	ldmfd	sp!, {r4, r5, r6, pc}

	@ A literal pool here: initialisation's ldr= constants cannot reach the one
	@ at the end of the module now that the app's files are carried in it.
	.ltorg

msg_banner:
	.string	"High Resolution Graphics Card Installed..."
	.align

msg_boot_failed:
	.string	"High Resolution Graphics Card: could not take the display this early - use *GfxCardOn"
	.align

@ Where this card is and how its interrupt works, into WS_SCRATCH.
@
@ Asking by base address, which is the obvious thing since that is what
@ initialisation hands us in R11, does not work at every RAM size. The Podule
@ manager resolves an address by masking it and then ORing in &180000, promoting
@ whatever access speed was asked for to "sync", before comparing against the
@ card's recorded sync base (ConvertR3ToPoduleNode in HWSupport.Podule). Bits 19
@ and 20 are that speed field, so the comparison can only succeed when the card's
@ recorded base already has both set - and the logical address the kernel maps
@ expansion card space at moves with the amount of RAM fitted. A 256MB machine
@ gets an address where both bits are set and the promotion is a no-op; a 128MB
@ machine gets one where bit 20 is clear and the promoted value can never match.
@ The call then fails with &500, "Bad expansion card identifier", and a driver
@ that treats that as fatal does not start at all on a machine with less RAM.
@
@ So fall back to asking by expansion card number, which takes the manager's
@ list-scan path and never goes near the speed bits. Our own card is found by
@ asking each slot for its base and looking for the one we were handed - both
@ forms of base, because which one initialisation gives is not ours to assume.
@ EtherRPCEm carries the same fallback, for the same reason, and Acorn's own
@ Econet driver finds its card by number too.
@
@ in:  r5 = workspace, r11 = the address initialisation handed us
@ out: WS_SCRATCH holds the four words READINFO_ITEMS asks for.
@      V set and r0 -> error if the card could not be found at all.
podule_read_info:
	stmfd	sp!, {r1, r2, r3, r4, r6, lr}

	ldr	r0, =READINFO_ITEMS
	add	r1, r5, #WS_SCRATCH
	mov	r2, #16
	mov	r3, r11
	swi	XPodule_ReadInfo
	ldmvcfd	sp!, {r1, r2, r3, r4, r6, pc}	@ the direct way worked

	mov	r4, #0			@ slot being tried
1:
	ldr	r0, =READINFO_BASES
	add	r1, r5, #WS_SCRATCH
	mov	r2, #8
	mov	r3, r4
	swi	XPodule_ReadInfo
	bvs	2f			@ empty slot, or no such slot
	ldr	r0, [r5, #WS_SCRATCH]		@ sync base
	teq	r0, r11
	ldrne	r0, [r5, #WS_SCRATCH + 4]	@ EASI base
	teqne	r0, r11
	bne	2f

	@ This card is us: ask the same question by number.
	ldr	r0, =READINFO_ITEMS
	add	r1, r5, #WS_SCRATCH
	mov	r2, #16
	mov	r3, r4
	swi	XPodule_ReadInfo
	ldmfd	sp!, {r1, r2, r3, r4, r6, pc}	@ V as the SWI left it
2:
	add	r4, r4, #1
	cmp	r4, #PODULE_COUNT
	blo	1b

	@ Not in the list at all, which should not be possible: this module is
	@ running out of that card's ROM.
	adrl	r0, err_no_card
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r2, r3, r4, r6, pc}

	.ltorg

	@ Failure paths. Each undoes exactly what had been done, then returns the
	@ error it was given (or one of ours) with V set.
init_no_card:
	adrl	r0, err_no_card
	b	init_failed

init_bad_version:
	adrl	r0, err_bad_version
	b	init_failed

init_no_graphicsv:
	adrl	r0, err_no_graphicsv
	b	init_failed

	@ Anything already claimed is given back, whatever stage we reached, and
	@ the error in r0 is returned to the module system.
init_failed:
	stmfd	sp!, {r0}
	bl	teardown
	mov	r0, #Module_Free
	mov	r2, r5
	swi	XOS_Module
	mov	r0, #0
	str	r0, [r12]
	ldmfd	sp!, {r0}
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r4, r5, r6, pc}
err_no_card:
	.int	0
	.string	"RPCEmuGfx: no graphics card found in this slot"
	.align

err_bad_version:
	.int	0
	.string	"RPCEmuGfx: the graphics card uses an interface this driver does not know"
	.align

err_no_graphicsv:
	.int	0
	.string	"RPCEmuGfx: this RISC OS has no display driver interface (RISC OS 5 is needed)"
	.align


@ Give everything back, in the reverse order it was taken. Called from
@ finalisation and from a failed initialisation, so it works from a partly
@ claimed state: WS_FLAGS says what there is to undo.
@
@ in: r5 = workspace
teardown:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	ldr	r4, [r5, #WS_FLAGS]

	@ Stop displaying, and stop interrupting. The registers are not known
	@ until initialisation has got that far, and this runs on that path too.
	ldr	r0, [r5, #WS_REGS]
	teq	r0, #0
	movne	r1, #0
	strne	r1, [r0, #REG_CTRL]
	movne	r1, #GFXCARD_STATUS_VSYNC
	strne	r1, [r0, #REG_STATUS]

	tst	r4, #FLAG_DEVICE
	beq	1f
	ldr	r0, [r5, #WS_DEVNO]
	ldr	r1, [r5, #WS_VSHANDLER]
	mov	r2, r5
	ldr	r3, [r5, #WS_PODULE]
	mov	r4, #1
	swi	XOS_ReleaseDeviceVector
	ldr	r4, [r5, #WS_FLAGS]
1:
	@ If the card is the display, hand back to whichever driver was there
	@ first (VIDC20, driver 0) so the machine still has a screen. An error
	@ here is not ours to report: we are being taken away either way.
	@
	@ The screen memory has to be there before the display moves onto it, for
	@ the reasons restore_screen_memory sets out. If it cannot be had then the
	@ switch is made anyway - refusing would leave the machine with a display
	@ driver that is about to be unplugged, which is worse.
	tst	r4, #FLAG_STARTED
	beq	2f
	mov	r0, #ScreenMode_SelectDevice
	mvn	r1, #0			@ read the current driver
	swi	XOS_ScreenMode
	ldr	r2, [r5, #WS_DRIVER]
	teq	r1, r2
	bne	15f
	bl	restore_screen_memory
	mov	r0, #ScreenMode_SelectDevice
	mov	r1, #0
	swi	XOS_ScreenMode
15:

	mov	r0, #ScreenMode_StopDriver
	ldr	r1, [r5, #WS_DRIVER]
	swi	XOS_ScreenMode
2:
	tst	r4, #FLAG_REGISTERED
	beq	3f
	mov	r0, #ScreenMode_DeregisterDriver
	ldr	r1, [r5, #WS_DRIVER]
	swi	XOS_ScreenMode
3:
	tst	r4, #FLAG_RESOURCES
	beq	35f
	adrl	r0, resfs_files
	swi	XResourceFS_DeregisterFiles
	adrl	r0, var_base_name
	mov	r1, #0			@ unset the variable
	mov	r2, #-1
	mov	r3, #0
	mov	r4, #VarType_String
	swi	XOS_SetVarVal
	ldr	r4, [r5, #WS_FLAGS]
35:
	@ GfxCard$Display must go, and this is not tidiness: its value is a pair of
	@ jumps into this module's code, and the kernel would keep calling them after
	@ the module and its workspace had been freed. Deleting a code variable needs
	@ the type as well - the kernel refuses to delete a code node when told any
	@ other type (Kernel s/Arthur2, SetVarVal_DeleteIt) - which is why the base
	@ variable above passes VarType_String and this one does not.
	tst	r4, #FLAG_DISPLAYVAR
	beq	36f
	adrl	r0, var_display
	mov	r1, #0
	mov	r2, #-1
	mov	r3, #0
	mov	r4, #VarType_Code
	swi	XOS_SetVarVal
	ldr	r4, [r5, #WS_FLAGS]
36:
	@ The other three are plain strings written by *GfxCardVars. Left behind they
	@ would describe a card that is no longer there, which is what anything
	@ reading them would believe. Unset whether or not they were ever written;
	@ "not found" is not an error worth acting on here.
	adrl	r0, var_mode
	bl	unset_var
	adrl	r0, var_store
	bl	unset_var
	adrl	r0, var_frames
	bl	unset_var
	ldr	r4, [r5, #WS_FLAGS]
	tst	r4, #FLAG_VECTOR
	beq	4f
	mov	r0, #GraphicsV
	ldr	r1, [r5, #WS_GVHANDLER]
	mov	r2, r5
	swi	XOS_Release
4:
	mov	r0, #0
	str	r0, [r5, #WS_FLAGS]
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

@ in: r12 -> private word
final:
	stmfd	sp!, {r5, lr}
	ldr	r5, [r12]
	teq	r5, #0
	beq	final_done

	bl	teardown

	mov	r0, #Module_Free
	mov	r2, r5
	swi	XOS_Module
	mov	r0, #0
	str	r0, [r12]

final_done:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r5, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Service handler
@
@ Only Service_ResourceFSStarting: ResourceFS has restarted and wants its files
@ back. It is not on the module chain at that point, so the SWI cannot be used -
@ the service passes the address of a routine to call instead.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

service_codetable:
	.int	0			@ flags
	.int	service_main
	.int	Service_ResourceFSStarting
	.int	0			@ table terminator

	.int	service_codetable	@ lives at service-4
service:
	mov	r0, r0			@ magic: "my service table is at [service-4]"
	teq	r1, #Service_ResourceFSStarting
	movne	pc, lr

service_main:
	stmfd	sp!, {r0, lr}
	adrl	r0, resfs_files
	mov	lr, pc
	mov	pc, r2			@ ResourceFS's own registration routine
	ldmfd	sp!, {r0, pc}

@ Write r3 as eight hex digits at r1, NUL terminated. r2 = 8, as given.
hex_word:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	add	r1, r1, #8
	mov	r0, #0
	strb	r0, [r1]		@ terminator
1:
	and	r4, r3, #0xf
	cmp	r4, #9
	addls	r4, r4, #'0'
	addhi	r4, r4, #'a' - 10
	strb	r4, [r1, #-1]!
	mov	r3, r3, lsr #4
	subs	r2, r2, #1
	bne	1b
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

var_base_name:
	.string	"RPCEmuGfx$Base"
	.balign	4, 0


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The desktop app, carried in this card's ROM
@
@ Registered with ResourceFS so it appears as Resources:$.Apps.!GfxCard, which is
@ where the Apps icon on the icon bar looks. Nothing is installed on a disc, and
@ the app is present exactly when the card is.
@
@ The format is the one ResourceFS documents: per file, an offset to the next
@ entry, the load and exec addresses and size as OS_File would give them, the
@ attributes, the name without "$.", then (word-aligned) the size again plus four,
@ then the data. A single zero word ends the list.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	@ The application directory, generated by mkresfs.py from whatever files are
	@ in riscos-progs/RPCEmuGfx: drop a !Sprites, Templates or Messages file in
	@ there and it appears in Resources:$.Apps.!GfxCard without a change here.
	.include "resfs.inc"


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ * Commands
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Switch the display to the card. OS_ScreenMode reason 11 does the work,
@ including picking a mode and putting the previous driver back if that fails.
@
@ It does not, however, reach the Wimp. Changing driver re-initialises the mode
@ to the configured one and clears the screen, but the desktop is never told and
@ so never repaints: the screen is left blank, at whatever mode the CMOS asked
@ for, however large the mode was beforehand. So the geometry in use is noted
@ first and asked for again through *WimpMode once the switch has happened, which
@ both keeps the resolution and gives the desktop a mode change it acts on.
@ Outside the desktop there is nothing to repaint and the step is skipped.
command_on:
	stmfd	sp!, {r5, lr}
	ldr	r5, [r12]
	teq	r5, #0
	beq	command_on_no_ws

	bl	save_mode
	mov	r0, #ScreenMode_SelectDevice
	ldr	r1, [r5, #WS_DRIVER]
	swi	XOS_ScreenMode
	ldmvsfd	sp!, {r5, pc}		@ refused: report it as it stands

	bl	restore_wimp_mode

	@ The modes on offer are the monitor's, not the card's, so a monitor type
	@ that does not read the card's EDID quietly limits what can be selected.
	@ Worth saying at the moment of switching rather than leaving it to be
	@ discovered by a mode that will not take.
	bl	monitor_type
	teq	r1, #MONITOR_EDID
	teqne	r1, #MONITOR_FILE
	beq	1f
	adrl	r0, msg_mon_hint
	swi	XOS_Write0
	swi	XOS_NewLine
1:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r5, pc}

command_on_no_ws:
	adrl	r0, err_no_workspace
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r5, pc}

@ The configured monitor type.
@ out: r1 = type, as monitors.h numbers them
monitor_type:
	stmfd	sp!, {r0, r2, lr}
	mov	r0, #1
	swi	XOS_ReadSysInfo		@ out: r1 = monitor type, r2 = sync
	movvs	r1, #MONITOR_AUTO	@ unreadable: assume the default
	ldmfd	sp!, {r0, r2, pc}

@ Note the geometry of the mode in use, so it can be asked for again after the
@ switch. A variable that cannot be read leaves zero, which restore_wimp_mode
@ takes as "nothing to restore".
@
@ in: r5 = workspace
save_mode:
	stmfd	sp!, {r0, r1, r2, lr}
	mov	r0, #0
	str	r0, [r5, #WS_SAVE_W]
	str	r0, [r5, #WS_SAVE_H]
	mov	r0, #3			@ 256 colours if the depth is unreadable
	str	r0, [r5, #WS_SAVE_L2BPP]

	mvn	r0, #0			@ the mode in use
	mov	r1, #ModeVar_XWindLimit
	swi	XOS_ReadModeVariable
	addvc	r2, r2, #1		@ a limit is the last pixel, not the count
	strvc	r2, [r5, #WS_SAVE_W]

	mvn	r0, #0
	mov	r1, #ModeVar_YWindLimit
	swi	XOS_ReadModeVariable
	addvc	r2, r2, #1
	strvc	r2, [r5, #WS_SAVE_H]

	mvn	r0, #0
	mov	r1, #ModeVar_Log2BPP
	swi	XOS_ReadModeVariable
	strvc	r2, [r5, #WS_SAVE_L2BPP]

	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, r2, pc}

@ Ask the Wimp for the mode noted by save_mode, if the desktop is running.
@ Nothing here is fatal: the display has already been switched, and a desktop
@ that will not take the mode says so itself.
@
@ in: r5 = workspace
restore_wimp_mode:
	stmfd	sp!, {r0, r1, r2, r3, r4, r6, lr}
	ldr	r0, [r5, #WS_SAVE_W]
	ldr	r1, [r5, #WS_SAVE_H]
	teq	r0, #0
	teqne	r1, #0
	beq	rwm_done

	@ Wimp$State reads "desktop" while the desktop is running, and "commands"
	@ when it is not; the first character is enough to tell them apart.
	adrl	r0, var_wimp_state
	add	r1, r5, #WS_CMD
	mov	r2, #CMD_SIZE
	mov	r3, #0
	mov	r4, #3			@ expanded, as a string
	swi	XOS_ReadVarVal
	bvs	rwm_done
	cmp	r2, #0
	beq	rwm_done
	ldrb	r0, [r5, #WS_CMD]
	teq	r0, #'d'
	bne	rwm_done

	add	r4, r5, #WS_CMD		@ write cursor
	adrl	r6, mode_prefix
	bl	copy_string

	ldr	r0, [r5, #WS_SAVE_W]
	mov	r1, r4
	mov	r2, #12
	swi	XOS_ConvertCardinal4	@ decimal at r1; r1 -> the terminating NUL
	mov	r4, r1

	adrl	r6, mode_y
	bl	copy_string
	ldr	r0, [r5, #WS_SAVE_H]
	mov	r1, r4
	mov	r2, #12
	swi	XOS_ConvertCardinal4
	mov	r4, r1

	@ The depth we came from, of the two this card scans out.
	ldr	r0, [r5, #WS_SAVE_L2BPP]
	teq	r0, #5
	bne	1f
	adrl	r6, mode_c16m
	b	2f
1:
	adrl	r6, mode_c256
2:
	bl	copy_string

	mov	r0, #0
	strb	r0, [r4]		@ terminate
	add	r0, r5, #WS_CMD
	swi	XOS_CLI
	bvc	rwm_done

	@ The desktop would not take that mode. The display has already been
	@ switched and is showing whatever the OS re-initialised to, so this is not
	@ an error - but it is the one thing the user needs told, since otherwise
	@ they are looking at a screen they did not ask for wondering why. Both
	@ directions come through here, so the message does not name one: the user
	@ has just typed the command and knows which way they went.
	adrl	r0, msg_mode_lost
	swi	XOS_Write0
	add	r0, r5, #WS_CMD
	swi	XOS_Write0
	swi	XOS_NewLine
	adrl	r0, msg_mode_hint
	swi	XOS_Write0
	swi	XOS_NewLine

rwm_done:
	cmp	pc, #0			@ clear V: a failure here is not the caller's
	ldmfd	sp!, {r0, r1, r2, r3, r4, r6, pc}

msg_mode_lost:
	.string	"The display was switched, but the desktop would not take the mode it was in: "
	.align
msg_mode_hint:
	.string	"Set a mode the display can show, e.g. *WimpMode X1280 Y1024 C256"
	.align

@ copy_string - append the NUL-terminated string at r6 to r4, leaving r4 after
@ the last byte written and r6 after the terminator.
copy_string:
	stmfd	sp!, {r0, lr}
1:
	ldrb	r0, [r6], #1
	teq	r0, #0
	strneb	r0, [r4], #1
	bne	1b
	ldmfd	sp!, {r0, pc}

var_wimp_state:
	.string	"Wimp$State"
	.align
mode_prefix:
	.string	"WimpMode X"
	.align
mode_y:
	.string	" Y"
	.align
mode_c256:
	.string	" C256"
	.align
mode_c16m:
	.string	" C16M"
	.align

@ ...and back to VIDC20, which is driver 0: it registers from ROM before
@ anything in an expansion card can.
@
@ The card is then told to stop displaying. RISC OS has no call for this - there
@ is no way for the kernel to tell a driver it is no longer the one in use (see
@ the GVTODO in ScreenMode_SelectDevice) - so a card left scanning out would keep
@ putting its own framestore on the screen after the OS had moved on.
@
@ The mode is dropped to something small first, and that is not tidiness. Taking
@ the display off a driver re-initialises the mode, and the mode the kernel picks
@ is the configured one - which, whilst this card is the display, is the card's
@ own preferred mode. VIDC20 cannot show it, so the mode change fails; the
@ kernel's fallback to mode 0 does not take either; and the VDU variables are
@ left describing a mode of the card's size whilst the screen pointer has been
@ moved to the screen dynamic area, which is far smaller and may by then be
@ empty. The next thing to paint the desktop - the Wimp, answering the display
@ changed service call the switch itself issues - walks off the end of screen
@ memory, and the machine takes a string of data aborts and stops.
@
@ Nothing here can make the kernel's mode change succeed. What it can do is make
@ sure that whatever the kernel leaves behind is a mode VIDC20 can hold and that
@ there is the memory to hold it, so that a failed mode change is merely a failed
@ mode change. The mode in use is asked for again afterwards, which both puts the
@ user back where they were and gives the desktop a mode change to repaint from.
command_off:
	stmfd	sp!, {r5, lr}
	ldr	r5, [r12]
	teq	r5, #0
	beq	command_no_ws

	bl	save_mode
	bl	safe_mode
	bl	restore_screen_memory
	ldmvsfd	sp!, {r5, pc}		@ nowhere to move the display to: stay put

	mov	r0, #ScreenMode_SelectDevice
	mov	r1, #0
	swi	XOS_ScreenMode
	ldmvsfd	sp!, {r5, pc}		@ still the display: leave it scanning out
	ldr	r0, [r5, #WS_REGS]
	mov	r1, #0
	str	r1, [r0, #REG_CTRL]

	bl	restore_wimp_mode
	ldmfd	sp!, {r5, pc}

@ A mode any display can show, asked for directly rather than through the Wimp:
@ this is a step on the way to somewhere else, and the desktop is told where it
@ has ended up once the display has finished moving. 640 x 480 in 256 colours is
@ the mode every monitor definition and every EDID carries, and at 300K it is
@ within any screen memory a machine can be configured with.
@
@ A mode selector rather than a mode number, because a numbered mode has to be in
@ the list the monitor defines and a selector does not, and with the frame rate
@ left unstated so that whatever the monitor offers at that size will do.
@
@ out: V clear - a display that will not take this is no worse off for having
@      been asked, and the caller has no better answer than to carry on
safe_mode:
	stmfd	sp!, {r0, r1, lr}
	mov	r0, #ScreenMode_SelectMode
	adrl	r1, safe_mode_selector
	swi	XOS_ScreenMode
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0, r1, pc}

	.align
safe_mode_selector:
	.int	1			@ a mode selector, with no extra variables
	.int	640			@ pixels across
	.int	480			@ pixels down
	.int	3			@ log2bpp: 256 colours
	.int	-1			@ frame rate: whatever the monitor offers
	.int	-1			@ end of the mode variable list

@ Give the screen dynamic area its configured size back, before anything is
@ asked to display out of it.
@
@ Whilst the card is the display RISC OS marks the framestore as external, and
@ the screen area's shrink handler then has no minimum left to defend: the area
@ can be emptied to nothing and its pages handed to the free pool. That is the
@ right thing to do with memory nothing is scanning out of, and on a machine with
@ any memory pressure at all it is what happens. Nothing puts it back.
@
@ Switching to a driver that does read from it therefore arrives at an area of no
@ size. The kernel records a total screen size of zero, every mode is then
@ refused for want of memory - including the mode 0 it falls back on - and the
@ VDU variables are left describing the mode the card was in whilst the screen
@ pointer addresses an empty area. The first thing to paint the screen runs off
@ the end of it and aborts, which is where the machine stops.
@
@ OS_ReadSysInfo 0 gives the size the kernel itself used when it created the area
@ at reset, already clamped to what this machine can offer - the whole of VRAM
@ where VRAM is not available for general use. The mode in use is asked for as
@ well and the larger of the two taken, because the configured size is what the
@ machine booted with and not necessarily enough for the mode it is being left
@ in: a mode change that fails leaves its mode variables behind, and those
@ variables addressing more memory than the area holds is the fault this exists
@ to prevent.
@
@ out: V set and r0 -> error if the memory could not be had back
restore_screen_memory:
	stmfd	sp!, {r1, r2, r6, lr}

	mov	r0, #0
	swi	XOS_ReadSysInfo		@ out: r0 = configured screen size, bytes
	ldmvsfd	sp!, {r1, r2, r6, pc}
	movs	r6, r0
	beq	rsm_done		@ configured as none: not ours to argue with

	mvn	r0, #0			@ the mode in use
	mov	r1, #ModeVar_ScreenSize
	swi	XOS_ReadModeVariable	@ out: r2 = bytes the mode occupies
	cmpvc	r2, r6
	movhi	r6, r2

	mov	r0, #ChangeDyn_Screen
	swi	XOS_ReadDynamicArea	@ out: r0 = base, r1 = current size
	ldmvsfd	sp!, {r1, r2, r6, pc}
	subs	r1, r6, r1
	ble	rsm_done		@ already at least that big

	mov	r0, #ChangeDyn_Screen
	swi	XOS_ChangeDynamicArea
	ldmvsfd	sp!, {r1, r2, r6, pc}

	@ A short grow leaves the same fault a failed one would, and the SWI is
	@ within its rights to move less than it was asked for, so the size is read
	@ back rather than assumed.
	mov	r0, #ChangeDyn_Screen
	swi	XOS_ReadDynamicArea
	ldmvsfd	sp!, {r1, r2, r6, pc}
	cmp	r1, r6
	blo	rsm_short
rsm_done:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r1, r2, r6, pc}

rsm_short:
	adrl	r0, err_no_screen_memory
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r2, r6, pc}

command_no_ws:
	adrl	r0, err_no_workspace
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r5, pc}

err_no_workspace:
	.int	0
	.string	"RPCEmuGfx: the driver is not initialised"
	.align

err_no_screen_memory:
	.int	0
	.string	"RPCEmuGfx: not enough free memory to give the display back to VIDC20"
	.align

@ Put the card's state into system variables.
@
@ The card's registers are in expansion card space, which is mapped for
@ privileged access only: a desktop application cannot read them however much it
@ knows about the card. So the driver, which can, formats what there is to say and
@ leaves it where anything at all can pick it up.
command_vars:
	stmfd	sp!, {r4, r5, r6, lr}
	ldr	r5, [r12]
	teq	r5, #0
	beq	command_vars_no_ws

	@ GfxCard$Mode - what it is displaying
	add	r4, r5, #WS_CMD
	bl	vars_displaying
	bne	1f
	adrl	r6, vtext_idle
	bl	copy_string
	b	2f
1:
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_WIDTH]
	bl	append_decimal
	adrl	r6, vtext_by
	bl	copy_string
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_HEIGHT]
	bl	append_decimal
	adrl	r6, vtext_comma
	bl	copy_string
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_BPP]
	bl	append_decimal
	adrl	r6, vtext_bpp
	bl	copy_string
2:
	adrl	r0, var_mode
	bl	set_var

	@ GfxCard$Store - how much of the framestore the mode uses
	add	r4, r5, #WS_CMD
	bl	vars_displaying
	moveq	r0, #0			@ not displaying: none of it is in use
	beq	3f
	ldr	r0, [r5, #WS_REGS]
	ldr	r1, [r0, #REG_STRIDE]
	ldr	r2, [r0, #REG_HEIGHT]
	mul	r0, r1, r2
	mov	r0, r0, lsr #10
3:
	bl	append_decimal
	adrl	r6, vtext_of
	bl	copy_string
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_FB_SIZE]
	mov	r0, r0, lsr #10
	bl	append_decimal
	adrl	r6, vtext_k
	bl	copy_string
	adrl	r0, var_store
	bl	set_var

	@ GfxCard$Frames - frames the card has put out
	add	r4, r5, #WS_CMD
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_FRAMES]
	bl	append_decimal
	adrl	r0, var_frames
	bl	set_var

	@ GfxCard$Display is not written here. It is a code variable, created at
	@ initialisation, and reading it asks the kernel which driver is in use at
	@ that moment - so there is nothing for this command to keep up to date, and
	@ writing a string over it would replace a live answer with a stale one.
	@ See make_display_var.

	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r4, r5, r6, pc}

command_vars_no_ws:
	adrl	r0, err_no_workspace
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r4, r5, r6, pc}

@ Is the card scanning out? Z set if it is not. in: r5 = workspace
vars_displaying:
	stmfd	sp!, {r0, lr}
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_CTRL]
	tst	r0, #GFXCARD_CTRL_ENABLE
	ldmfd	sp!, {r0, pc}

@ Append r0 in decimal at r4, leaving r4 after the digits.
append_decimal:
	stmfd	sp!, {r0, r1, r2, lr}
	mov	r1, r4
	mov	r2, #12
	swi	XOS_ConvertCardinal4
	movvc	r4, r1
	ldmfd	sp!, {r0, r1, r2, pc}

@ Set the variable named at r0 to the string built at WS_CMD.
@ GfxCard$Display as a code variable, so reading it asks the kernel rather than
@ repeating whatever *GfxCardVars last wrote.
@
@ Why a code variable at all: the value is "which driver is the display", which
@ changes whenever anything switches it - *GfxCardOn, *GfxCardOff, the desktop
@ utility, or another driver entirely. A plain string would be a snapshot, correct
@ only until the next switch, and a stale one is worse than none: ADFFS asked for
@ a way to tell whether this card is driving the screen, and would act on the
@ answer. Reading a code variable calls us, so the answer cannot be out of date.
@
@ The kernel copies the value into its own heap, so the value cannot contain
@ PC-relative references to anything of ours. It is four words - two "LDR PC,
@ [PC, #0]" instructions and two absolute addresses - exactly as the kernel's own
@ Sys$Time and Sys$ReturnCode are built (Kernel s/Arthur2, SystemVarList).
@
@ The read entry needs our workspace and the kernel passes none, so it goes
@ through a trampoline in the workspace which loads r12 and jumps. r12 is one of
@ the registers the kernel documents as corruptible by the read code, so using it
@ is legitimate.
@
@ in: r5 = workspace
@ out: V set on failure, r0 = error
make_display_var:
	stmfd	sp!, {r1, r2, r3, r4, r6, lr}

	@ The trampoline, in workspace:
	@   LDR r12, [PC, #0]   -> the word at +8, our workspace
	@   LDR pc,  [PC, #0]   -> the word at +12, read_display_var
	add	r6, r5, #WS_VARTHUNK
	ldr	r0, thunk_ldr_r12
	str	r0, [r6, #0]
	ldr	r0, thunk_ldr_pc
	str	r0, [r6, #4]
	str	r5, [r6, #8]
	adrl	r0, read_display_var
	str	r0, [r6, #12]

	@ The value handed to the kernel, built in the command buffer: two jumps,
	@ then the addresses they load. Set goes straight to module code, which
	@ needs no workspace; read goes through the trampoline above.
	add	r4, r5, #WS_CMD
	ldr	r0, thunk_ldr_pc
	str	r0, [r4, #0]
	str	r0, [r4, #4]
	adrl	r0, set_display_var
	str	r0, [r4, #8]
	str	r6, [r4, #12]

	adrl	r0, var_display
	mov	r1, r4
	mov	r2, #16			@ length of the code, including alignment
	mov	r3, #0
	mov	r4, #VarType_Code
	swi	XOS_SetVarVal
	ldmfd	sp!, {r1, r2, r3, r4, r6, pc}

thunk_ldr_r12:
	ldr	r12, [pc, #0]
thunk_ldr_pc:
	ldr	pc, [pc, #0]

@ Read entry for GfxCard$Display.
@
@ out: r0 -> the value, r2 = its length. Anything else must come back unchanged.
@
@ Entered from the kernel with "MOV lr, pc", so lr is the only way back and any
@ SWI would overwrite it - hence lr is pushed before XOS_ScreenMode is called.
@ r12 = workspace, from the trampoline.
read_display_var:
	stmfd	sp!, {r1, r3, r4, r12, lr}

	@ Our driver number is read before the SWI, so nothing after it depends on
	@ r12 still holding the workspace.
	ldr	r4, [r12, #WS_DRIVER]

	@ Which driver the kernel is using, not the card's own enable bit: what
	@ decides what you are looking at is the kernel's choice of driver, and only
	@ the kernel can say. This is the same question *GfxCardStatus asks, so the
	@ two cannot disagree.
	mov	r0, #ScreenMode_SelectDevice
	mvn	r1, #0			@ read it, do not change it
	swi	XOS_ScreenMode
	@ An OS that cannot answer is not using this card either way, so it reads
	@ as the built-in display rather than failing. A variable read that returned
	@ an error would break *Show for every other variable too.
	movvs	r1, #0

	teq	r1, r4
	bne	1f
	adrl	r0, vtext_card
	mov	r2, #4			@ "Card"
	b	2f
1:
	adrl	r0, vtext_vidc
	mov	r2, #6			@ "VIDC20"
2:
	cmp	pc, #0			@ clear V: we always have an answer
	ldmfd	sp!, {r1, r3, r4, r12, lr}
	mov	pc, lr

@ Set entry for GfxCard$Display.
@
@ in: r1 -> value, r2 = length
@
@ There is nothing sensible to do with a write. The value is a fact about which
@ driver the kernel is using, and assigning to it cannot make that fact different
@ - *GfxCardOn and *GfxCardOff are how it is changed. Refused rather than
@ silently ignored, so a script that tries gets told instead of believing it
@ worked.
set_display_var:
	adrl	r0, err_display_readonly
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	mov	pc, lr

err_display_readonly:
	.int	0
	.string	"GfxCard$Display says which display the machine is using and cannot be set. Use *GfxCardOn or *GfxCardOff."
	.align

@ Remove a plain string variable, ignoring "not found".
@ in: r0 -> name
unset_var:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	mov	r1, #0
	mov	r2, #-1
	mov	r3, #0
	mov	r4, #VarType_String
	swi	XOS_SetVarVal
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

set_var:
	stmfd	sp!, {r0, r1, r2, r3, r4, lr}
	mov	r1, #0
	strb	r1, [r4]		@ terminate
	add	r1, r5, #WS_CMD
	sub	r2, r4, r1		@ length
	mov	r3, #0
	mov	r4, #0			@ a plain string
	swi	XOS_SetVarVal
	ldmfd	sp!, {r0, r1, r2, r3, r4, pc}

var_mode:
	.string	"GfxCard$Mode"
	.align
var_store:
	.string	"GfxCard$Store"
	.align
var_frames:
	.string	"GfxCard$Frames"
	.align
var_display:
	.string	"GfxCard$Display"
	.align
vtext_card:
	.string	"Card"
	.align
vtext_vidc:
	.string	"VIDC20"
	.align
vtext_idle:
	.string	"Not displaying"
	.align
vtext_by:
	.string	" x "
	.align
vtext_comma:
	.string	", "
	.align
vtext_bpp:
	.string	"bpp"
	.align
vtext_of:
	.string	"K of "
	.align
vtext_k:
	.string	"K"
	.align

	.ltorg

command_status:
	stmfd	sp!, {r4, r5, r6, lr}
	ldr	r5, [r12]
	teq	r5, #0
	beq	command_no_ws_4

	adrl	r0, msg_card
	swi	XOS_Write0
	ldr	r0, [r5, #WS_FB_SIZE]
	mov	r0, r0, lsr #20
	bl	print_number
	adrl	r0, msg_card_end
	swi	XOS_Write0

	@ Largest mode the card will take.
	adrl	r0, msg_largest
	swi	XOS_Write0
	ldr	r0, [r5, #WS_MAX_WIDTH]
	bl	print_number
	adrl	r0, msg_by
	swi	XOS_Write0
	ldr	r0, [r5, #WS_MAX_HEIGHT]
	bl	print_number
	swi	XOS_NewLine

	@ Whether the card can answer for the monitor. This decides which modes
	@ RISC OS believes exist, so it belongs in the status.
	ldr	r0, [r5, #WS_EDID_SIZE]
	teq	r0, #0
	bne	1f
	adrl	r0, msg_no_edid
	b	2f
1:
	adrl	r0, msg_edid
2:
	swi	XOS_Write0

	@ ...and whether RISC OS is set up to read it. This is the one piece of
	@ configuration that decides whether the card's modes are reachable.
	bl	monitor_type
	adrl	r0, msg_monitor
	swi	XOS_Write0
	mov	r0, r1
	bl	print_number
	teq	r1, #MONITOR_EDID
	bne	3f
	adrl	r0, msg_mon_edid
	b	5f
3:
	teq	r1, #MONITOR_FILE
	bne	4f
	adrl	r0, msg_mon_file
	b	5f
4:
	adrl	r0, msg_mon_other
5:
	swi	XOS_Write0

	@ Driver number, and whether it is the one in use.
	adrl	r0, msg_driver
	swi	XOS_Write0
	ldr	r0, [r5, #WS_DRIVER]
	bl	print_number
	mov	r0, #ScreenMode_SelectDevice
	mvn	r1, #0
	swi	XOS_ScreenMode
	ldr	r0, [r5, #WS_DRIVER]
	teq	r0, r1
	bne	1f
	adrl	r0, msg_in_use
	b	2f
1:
	adrl	r0, msg_not_in_use
2:
	swi	XOS_Write0

	@ What the card was last told to display.
	ldr	r0, [r5, #WS_MODE_W]
	teq	r0, #0
	beq	command_status_vsync
	adrl	r0, msg_mode
	swi	XOS_Write0
	ldr	r0, [r5, #WS_MODE_W]
	bl	print_number
	adrl	r0, msg_by
	swi	XOS_Write0
	ldr	r0, [r5, #WS_MODE_H]
	bl	print_number
	adrl	r0, msg_at
	swi	XOS_Write0
	ldr	r0, [r5, #WS_MODE_BPP]
	bl	print_number
	adrl	r0, msg_bpp
	swi	XOS_Write0

	@ What became of the last mode the OS offered. This is what says whether a
	@ "Screen mode not available" came from this card or from somewhere else.
	ldr	r0, [r5, #WS_VET_W]
	teq	r0, #0
	beq	command_status_vsync
	adrl	r0, msg_vetted
	swi	XOS_Write0
	ldr	r0, [r5, #WS_VET_W]
	bl	print_number
	adrl	r0, msg_by
	swi	XOS_Write0
	ldr	r0, [r5, #WS_VET_H]
	bl	print_number
	adrl	r0, msg_at
	swi	XOS_Write0
	mov	r0, #1
	ldr	r1, [r5, #WS_VET_L2BPP]
	mov	r0, r0, lsl r1
	bl	print_number
	adrl	r0, msg_bpp_stride
	swi	XOS_Write0
	ldr	r0, [r5, #WS_VET_STRIDE]
	bl	print_number
	adrl	r0, msg_arrow
	swi	XOS_Write0
	ldr	r0, [r5, #WS_VET_REASON]
	teq	r0, #VET_OK
	bne	1f
	adrl	r0, msg_vet_ok
	b	2f
1:
	ldr	r0, [r5, #WS_VET_REASON]
	adrl	r1, vet_reasons
	sub	r0, r0, #1
	mov	r0, r0, lsl #5		@ 32 bytes per reason
	add	r0, r1, r0
2:
	swi	XOS_Write0
	swi	XOS_NewLine

command_status_vsync:
	@ Frames the card has displayed, and vsyncs we have handled. The two
	@ tracking each other is what says the interrupt path is alive.
	adrl	r0, msg_frames
	swi	XOS_Write0
	ldr	r0, [r5, #WS_REGS]
	ldr	r0, [r0, #REG_FRAMES]
	bl	print_number
	adrl	r0, msg_vsyncs
	swi	XOS_Write0
	ldr	r0, [r5, #WS_VSYNCS]
	bl	print_number
	ldr	r0, [r5, #WS_FEATURES]
	tst	r0, #GVFeature_NoVsyncIRQ
	beq	1f
	adrl	r0, msg_vsync_faked
	b	2f
1:
	adrl	r0, msg_vsync_irq
2:
	swi	XOS_Write0

	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r4, r5, r6, pc}

command_no_ws_4:
	adrl	r0, err_no_workspace
	cmp	r0, #NBIT		@ set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r4, r5, r6, pc}

@ Print r0 as a decimal number. Uses the workspace scratch space, so it is only
@ called from the * commands (a foreground context), never from the vector.
print_number:
	stmfd	sp!, {r0, r1, r2, lr}
	add	r1, r5, #WS_SCRATCH	@ buffer
	mov	r2, #16
	swi	XOS_ConvertCardinal4	@ r0 = value in, -> string out
	swi	XOS_Write0
	ldmfd	sp!, {r0, r1, r2, pc}

msg_card:
	.string	"RPCEmu graphics card present, "
	.align
msg_card_end:
	.string	"MB of display memory\r\n"
	.align
msg_largest:
	.string	"  Largest mode: "
	.align
msg_by:
	.string	" x "
	.align
msg_at:
	.string	" at "
	.align
msg_bpp:
	.string	" bpp\r\n"
	.align
msg_edid:
	.string	"  Monitor EDID: served by the card\r\n"
	.align
msg_no_edid:
	.string	"  Monitor EDID: not available from the card, so RISC OS will fall back to a default monitor\r\n"
	.align
msg_monitor:
	.string	"  Monitor type: "
	.align
msg_mon_edid:
	.string	" (EDID - RISC OS reads the card's, so every mode it can show is offered)\r\n"
	.align
msg_mon_file:
	.string	" (a monitor definition file - the modes on offer are the ones it defines)\r\n"
	.align
msg_mon_other:
	.string	" - RISC OS is not reading the card's EDID, so the modes on offer are whatever this monitor type defines. *Configure MonitorType EDID and reset for the full set, or load a monitor definition file.\r\n"
	.align
msg_mon_hint:
	.string	"Note: the configured monitor type does not read the card's EDID, so only that monitor's modes can be selected. *Configure MonitorType EDID and reset, or *LoadModeFile a definition."
	.align
msg_driver:
	.string	"  Display driver "
	.align
msg_in_use:
	.string	", in use\r\n"
	.align
msg_not_in_use:
	.string	", not in use (*GfxCardOn to switch to it)\r\n"
	.align
msg_mode:
	.string	"  Displaying "
	.align
msg_vetted:
	.string	"  Last mode offered: "
	.align
msg_bpp_stride:
	.string	" bpp, stride "
	.align
msg_arrow:
	.string	" - "
	.align
msg_vet_ok:
	.string	"accepted"
	.align

	@ Fixed 32-byte slots so the reason can be indexed, in VET_* order.
vet_reasons:
	.ascii	"not a type 3 mode list\0"
	.space	32 - 23
	.ascii	"a depth this card cannot show\0"
	.space	32 - 30
	.ascii	"interlaced with two fields\0"
	.space	32 - 27
	.ascii	"no pixels\0"
	.space	32 - 10
	.ascii	"wider than the card allows\0"
	.space	32 - 27
	.ascii	"taller than the card allows\0"
	.space	32 - 28
	.ascii	"too large for the framestore\0"
	.space	32 - 29

msg_frames:
	.string	"  Frames displayed: "
	.align
msg_vsyncs:
	.string	", vsyncs handled: "
	.align
msg_vsync_irq:
	.string	" (card interrupt)\r\n"
	.align
msg_vsync_faked:
	.string	" (no card interrupt; the OS is making its own)\r\n"
	.align

	.end
