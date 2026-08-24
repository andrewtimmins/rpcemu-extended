@ RPCEmuMonitor - a monitor definition for the RISC OS versions that cannot be
@ told about the display any other way
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ RISC OS 5 is told what the monitor can do through a synthesised EDID block,
@ patched into the video driver's own table. Earlier versions have no EDID
@ block to patch and no EDID support to read one, so MonitorType Auto falls back
@ to the monitor ID bit on the VIDC connector - which this emulator holds at 0,
@ meaning a plain VGA monitor. The result is a machine that boots at 800x600 and
@ refuses 1152x864 even at 256 colours, whatever VRAM is fitted. The VRAM was
@ never the limit; the monitor was.
@
@ So this carries a monitor definition file and loads it. The definition lists
@ the same modes the emulator offers everywhere else - it is generated from
@ src/display_mode.c at build time, so the two cannot drift - and RISC OS then
@ vets that list against the screen memory actually fitted, which is exactly
@ what it should do and needs no help from here.
@
@ The file is registered with ResourceFS rather than written anywhere, so
@ nothing is installed on a disc, nothing is edited in the user's !Boot, and the
@ definition exists only while this expansion card is present.
@
@ Deliberately silent on RISC OS 5: EDID already describes the monitor there,
@ and replacing that with this would be a downgrade.

	.arch	armv3

	ws	.req	r12

	@ SWIs (X form where failure is survivable)
	OS_Module			= 0x0001e
	XOS_CLI				= 0x20005
	XOS_Module			= 0x2001e
	XOS_ReadVarVal			= 0x20023
	XOS_ReadSysInfo			= 0x20058
	XResourceFS_RegisterFiles	= 0x61b40
	XResourceFS_DeregisterFiles	= 0x61b41

	Service_ResourceFSStarting	= 0x60
	Service_StartWimp		= 0x49

	Module_Claim			= 6
	Module_Free			= 7

	@ Workspace
	WS_FLAGS	= 0
	WS_VARBUF	= 4		@ scratch for reading a system variable
	WS_SIZE		= 4 + 64

	FLAG_RESOURCES	= 1 << 0	@ definition registered with ResourceFS
	FLAG_LOADED	= 1 << 1	@ and successfully loaded
	FLAG_NO_SCRAP	= 1 << 2	@ no Wimp$ScrapDir, so nothing was attempted

	NBIT		= 1 << 31


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
	.string	"RPCEmuMonitor"
	.balign	4

	@ Defines "help", generated from VersionNum by the Makefile.
	.include "version.inc"

	@ Help and Command keyword table
table:
	.string	"RPCEmuMonitorLoad"
	.balign	4
	.int	cmd_load
	.int	0x00000000	@ min 0 args, max 0
	.int	0
	.int	help_load

	.string	"RPCEmuMonitorStatus"
	.balign	4
	.int	cmd_status
	.int	0x00000000
	.int	0
	.int	help_status

	.int	0		@ table terminator

help_load:
	.string	"*RPCEmuMonitorLoad loads the emulator's monitor definition, making its screen modes the ones on offer.\rSyntax: *RPCEmuMonitorLoad"
	.balign	4
help_status:
	.string	"*RPCEmuMonitorStatus reports whether the emulator's monitor definition is in use.\rSyntax: *RPCEmuMonitorStatus"
	.balign	4


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

init:
	stmfd	sp!, {r0-r6, lr}

	mov	r0, #Module_Claim
	mov	r3, #WS_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {r0-r6, pc}		@ no workspace, no module
	str	r2, [ws]
	mov	r5, r2
	mov	r0, #0
	str	r0, [r5, #WS_FLAGS]

	@ Whether this module belongs here at all is decided by the emulator, which
	@ knows the ROM's version for certain and leaves it out of the expansion
	@ card entirely on RISC OS 5 - see podulerom.c. Asking the guest meant
	@ guessing a version out of an OS_Byte result, which is a poor way to settle
	@ something the host already knows.
	@
	@ The definition goes into ResourceFS, so it is a file RISC OS can open
	@ without one existing on any disc.
	adrl	r0, resfs_files
	swi	XResourceFS_RegisterFiles
	bvs	init_done
	mov	r0, #FLAG_RESOURCES
	str	r0, [r5, #WS_FLAGS]

	@ Not loaded here. Two reasons, and the first is fatal on its own:
	@ *LoadModeFile on RISC OS 3.71 will not read a file out of ResourceFS. It
	@ reports success and does nothing - the same bytes copied to an ordinary
	@ filing system load immediately. So the definition has to be copied
	@ somewhere real first, and the place for that is the Wimp's scrap
	@ directory, which does not exist until !Boot has run and set
	@ Wimp$ScrapDir. Waiting for the desktop to start is therefore both the
	@ workaround and the only moment it could work anyway.

init_done:
	ldmfd	sp!, {r0-r6, pc}


@ Copy the definition into the Wimp's scrap directory and load it from there.
@
@ The copy is left behind on purpose. RISC OS holds on to the monitor definition
@ it was given rather than taking a copy of the contents, so deleting the file
@ afterwards takes the mode list with it.
@
@ No scrap directory means no !Boot has run - a freshly installed OS, most
@ likely - and nothing is done at all. Somewhere to put the file is the one
@ thing this cannot do without, and inventing a location would be writing into
@ a machine whose layout we do not know.
@
@ Every error here is survivable: a machine with a poor mode list is better than
@ one that will not finish starting, and *RPCEmuMonitorStatus says what happened.
@
@ Corrupts r0-r4.
load_mode_file:
	stmfd	sp!, {r5, lr}
	ldr	r5, [ws]

	@ Is there a scrap directory? Read it rather than assume: OS_CLI would
	@ expand <Wimp$ScrapDir> to nothing and copy the file to a name in the
	@ current directory, which is somebody else's disc.
	adrl	r0, var_scrapdir
	add	r1, r5, #WS_VARBUF
	mov	r2, #64
	mov	r3, #0
	mov	r4, #0
	swi	XOS_ReadVarVal
	bvs	load_no_scrap
	cmp	r2, #0			@ set but empty is no better than unset
	beq	load_no_scrap

	adrl	r0, cmd_copy
	swi	XOS_CLI
	bvs	load_done

	adrl	r0, cmd_loadmodefile
	swi	XOS_CLI
	ldrvc	r0, [r5, #WS_FLAGS]
	orrvc	r0, r0, #FLAG_LOADED
	strvc	r0, [r5, #WS_FLAGS]

load_done:
	ldmfd	sp!, {r5, pc}

load_no_scrap:
	ldr	r0, [r5, #WS_FLAGS]
	orr	r0, r0, #FLAG_NO_SCRAP
	str	r0, [r5, #WS_FLAGS]
	ldmfd	sp!, {r5, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

final:
	stmfd	sp!, {r0-r5, lr}
	ldr	r5, [ws]
	cmp	r5, #0
	beq	final_done

	ldr	r0, [r5, #WS_FLAGS]
	tst	r0, #FLAG_RESOURCES
	beq	final_free

	adrl	r0, resfs_files
	swi	XResourceFS_DeregisterFiles

final_free:
	mov	r0, #Module_Free
	mov	r2, r5
	swi	XOS_Module

final_done:
	ldmfd	sp!, {r0-r5, pc}
	mov	pc, lr


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Service calls
@
@ Two of them.
@
@ Service_ResourceFSStarting: ResourceFS has restarted and wants its files back.
@ It is not on the module chain at that point, so the SWI cannot be used - the
@ service passes the address of a routine to call instead.
@
@ Service_StartWimp: the desktop is starting, which is the first moment !Boot
@ has certainly run and Wimp$ScrapDir therefore exists. This is where the
@ definition is actually loaded - see load_mode_file for why it cannot be done
@ at initialisation. Done once: the service comes round again every time the
@ desktop is re-entered, and repeating it would undo a definition the user had
@ chosen for themselves in the meantime.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

service_codetable:
	.int	0			@ flags
	.int	service_main
	.int	Service_ResourceFSStarting
	.int	Service_StartWimp
	.int	0			@ table terminator

	.int	service_codetable	@ lives at service-4
service:
	mov	r0, r0			@ magic: "my service table is at [service-4]"
	teq	r1, #Service_ResourceFSStarting
	teqne	r1, #Service_StartWimp
	movne	pc, lr

service_main:
	stmfd	sp!, {r0, r5, lr}
	ldr	r5, [ws]
	cmp	r5, #0
	ldmeqfd	sp!, {r0, r5, pc}

	teq	r1, #Service_StartWimp
	beq	service_startwimp

	ldr	r0, [r5, #WS_FLAGS]
	tst	r0, #FLAG_RESOURCES	@ never registered, so nothing to restore
	ldmeqfd	sp!, {r0, r5, pc}

	adrl	r0, resfs_files
	mov	lr, pc
	mov	pc, r2			@ ResourceFS's own registration routine
	ldmfd	sp!, {r0, r5, pc}

service_startwimp:
	ldr	r0, [r5, #WS_FLAGS]
	tst	r0, #FLAG_RESOURCES	@ nothing registered, nothing to copy
	ldmeqfd	sp!, {r0, r5, pc}
	tst	r0, #FLAG_LOADED	@ already done, and once is right
	ldmnefd	sp!, {r0, r5, pc}

	stmfd	sp!, {r1, r2, r3, r4}
	bl	load_mode_file
	ldmfd	sp!, {r1, r2, r3, r4}
	ldmfd	sp!, {r0, r5, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Commands
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

cmd_load:
	stmfd	sp!, {r0-r4, lr}
	bl	load_mode_file
	ldmfd	sp!, {r0-r4, pc}

cmd_status:
	stmfd	sp!, {r0-r5, lr}
	ldr	r5, [ws]
	ldr	r5, [r5, #WS_FLAGS]

	tst	r5, #FLAG_LOADED
	adrne	r0, msg_in_use
	bne	cmd_status_say
	tst	r5, #FLAG_NO_SCRAP
	adrne	r0, msg_no_scrap
	adreq	r0, msg_not_in_use
cmd_status_say:
	swi	XOS_CLI

	ldmfd	sp!, {r0-r5, pc}

var_scrapdir:
	.string	"Wimp$ScrapDir"
	.balign	4

	@ F forces the copy over an older one; the rest keep it silent.
cmd_copy:
	.string	"Copy Resources:$.RPCEmu.Monitors.RPCEmu <Wimp$ScrapDir>.RPCEmuMDF F~C~V~P"
	.balign	4
cmd_loadmodefile:
	.string	"LoadModeFile <Wimp$ScrapDir>.RPCEmuMDF"
	.balign	4
msg_in_use:
	.string	"Echo RPCEmu monitor definition loaded - its screen modes are the ones on offer."
	.balign	4
msg_no_scrap:
	.string	"Echo RPCEmu monitor definition not loaded: there is no Wimp$ScrapDir to put it in, so no boot sequence has run."
	.balign	4
msg_not_in_use:
	.string	"Echo RPCEmu monitor definition not in use yet - it is loaded when the desktop starts."
	.balign	4


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The definition, as a ResourceFS file
@
@ The format ResourceFS documents: an offset to the next entry, the load and
@ exec addresses (the load address carries the filetype), the size, the
@ attributes, the path, then the data preceded by its own length. A zero offset
@ ends the list.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	ATTR_READ	= 3

resfs_files:
	.int	resfs_end - resfs_files
	.int	0xffffff00		@ filetype &FFF, text
	.int	0
	.int	mdf_end - mdf_text
	.int	ATTR_READ
	.string	"RPCEmu.Monitors.RPCEmu"
	.balign	4, 0
	.int	mdf_end - mdf_text + 4

	@ Generated from src/display_mode.c by mkmdf.py, so the modes offered here
	@ are the modes the emulator offers everywhere else.
	.include "mdf.inc"

resfs_end:
	.int	0			@ end of the file list

	.end
