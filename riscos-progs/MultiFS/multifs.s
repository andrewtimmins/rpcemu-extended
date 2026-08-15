@ MultiFS - a filing system for FAT media
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ RISC OS can drive a USB stick but it cannot read one. The card's ROM carries
@ enough of RISC OS's own stack to enumerate the device and move sectors - see
@ docs/usb.md - and then FileCore declines to mount it, correctly, because almost
@ every stick in the world is FAT and FileCore only mounts FileCore discs. This
@ module is the missing half: it reads the media.
@
@ It is called MultiFS rather than FAT-something because FAT is where it starts
@ and not where it stops. The volume scan below already tells the formats apart,
@ and exFAT is the next one.
@
@ Naming note: our guest modules are otherwise prefixed RPCEmu, deliberately, so
@ that nothing of ours can be mistaken for part of RISC OS or collide with a name
@ ROOL might later use (see riscos-progs/RPCEmuUSBSupport). A filing system is the
@ one place that convention costs more than it earns: its name appears in every
@ path a user ever types, and "RPCEmuFAT::USB STICK.$" is a poor thing to make
@ somebody type. The module is still ours and says so in its help string.
@
@ Sectors come from SCSIFS, whose interface was established by reading the SWI
@ decoding tables out of the modules we ship rather than from any documentation:
@
@   SCSIFS_DiscOp  (&40980)  R0 = drive
@                            R1 = reason, 1 = read sectors
@                            R2 = disc address IN BYTES
@                            R3 = buffer
@                            R4 = length in bytes
@                            exit R4 = bytes NOT transferred
@
@ That disc address is a byte offset in a 32-bit register, so it reaches 4GB and
@ no further. SCSIFS offers SectorDiscOp and DiscOp64, either of which would lift
@ that, but both answer "FileCore in use" for every register layout tried so far,
@ which is one shared code path refusing rather than a parameter mistake. Until
@ that is settled, read_sector refuses an address it cannot express instead of
@ silently reading the wrong sector - a wrapped address is indistinguishable from
@ real data, and would corrupt a volume the moment writing is added.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	@ Register naming
	wp	.req	r12		@ private word on entry, workspace once loaded

	@ ARM constants
	NBIT = 1 << 31

	@ RISC OS SWIs, X form throughout: errors come back in R0 with V set
	XOS_Write0		= 0x20002
	XOS_NewLine		= 0x20003
	XOS_Module		= 0x2001e
	XOS_FSControl		= 0x20029
	XOS_ConvertCardinal4	= 0x200d8

	XSCSIFS_DiscOp		= 0x60980
	XSCSIFS_TestReady	= 0x60986
	XSCSIFS_SectorDiscOp	= 0x6098d

	@ OS_Module reason codes
	Module_Claim	= 6
	Module_Free	= 7

	@ OS_FSControl reason codes
	FSControl_AddFS		= 12
	FSControl_SelectFS	= 14
	FSControl_RemoveFS	= 16

	Service_FSRedeclare	= 0x40

	@ Filing system properties.
	@
	@ HostFS took 0x99 with a "TODO choose unique value" against it, so ours
	@ sits next to it on the same footing: neither is allocated by ROOL. A
	@ clash shows up as OS_FSControl 12 returning an error, which init below
	@ reports rather than swallowing.
	FILING_SYSTEM_NUMBER	= 0x9a
	MAX_OPEN_FILES		= 32

	@ SCSI drives to look at. SCSIFS_Drives would tell us, but a drive that is
	@ not there simply fails its first read, which is the same answer for less
	@ machinery.
	MAX_DRIVES	= 4

	@ Volume table. Eight is four drives with a full partition table each,
	@ which is more than the emulated bus can present.
	MAX_VOLUMES	= 8

	@ Volume record, one per mountable partition found
	VOL_DRIVE	= 0	@ SCSI drive it lives on
	VOL_START	= 4	@ first sector of the partition, LBA
	VOL_TOTSEC	= 8	@ sectors in the volume
	VOL_BPS		= 12	@ bytes per sector
	VOL_SPC		= 16	@ sectors per cluster
	VOL_RSVD	= 20	@ reserved sectors before the first FAT
	VOL_NFATS	= 24	@ number of FATs
	VOL_FATSZ	= 28	@ sectors in one FAT
	VOL_ROOTCLUS	= 32	@ FAT32: first cluster of the root directory
	VOL_ROOTENTS	= 36	@ FAT12/16: entries in the fixed root directory
	VOL_FIRSTDATA	= 40	@ LBA of the first data sector, relative to VOL_START
	VOL_CLUSTERS	= 44	@ count of data clusters, which fixes the FAT width
	VOL_TYPE	= 48	@ 12, 16 or 32; 0 means the slot is empty
	VOL_LABEL	= 52	@ 11 characters and a terminator
	VOL_SPCLOG	= 64	@ log2 of sectors per cluster, so cluster maths shifts
	VOL_FATSEC0	= 68	@ absolute LBA of the first FAT
	VOL_ROOTSEC0	= 72	@ absolute LBA of the FAT12/16 fixed root
	VOL_ROOTSECS	= 76	@ how many sectors that root occupies
	VOL_DATASEC0	= 80	@ absolute LBA of the first data sector
	VOL_SIZE	= 128
	VOL_SHIFT	= 7	@ log2(VOL_SIZE), for indexing the table

	@ Workspace.
	@
	@ Three sector buffers rather than one, because reading a directory means
	@ holding a directory sector while consulting the FAT, and the volume scan
	@ wants a third that neither of those disturbs.
	WS_SECTOR	= 0			@ scanning: boot records
	WS_DIRSEC	= 512			@ the directory sector being walked
	WS_FATSEC	= 1024			@ the FAT sector last consulted

	@ 1536 and 2560 are not arbitrary: they are what an ADD can carry as an
	@ immediate, and the table's address is worked out that way constantly.
	WS_VOLUMES	= 1536
	WS_NVOLUMES	= WS_VOLUMES + (MAX_VOLUMES * VOL_SIZE)
	WS_REC		= WS_NVOLUMES + 4	@ record add_volume is filling in
	WS_FATSEC_LBA	= WS_REC + 4		@ which sector WS_FATSEC holds, 0 for none

	@ Directory iterator state
	WS_IT_REC	= WS_FATSEC_LBA + 4
	WS_IT_CLUSTER	= WS_IT_REC + 4
	WS_IT_SECTOR	= WS_IT_CLUSTER + 4
	WS_IT_SECLEFT	= WS_IT_SECTOR + 4
	WS_IT_OFF	= WS_IT_SECLEFT + 4
	WS_IT_FIXED	= WS_IT_OFF + 4		@ non-zero for a FAT12/16 root
	WS_NAME		= WS_IT_FIXED + 4	@ a name built from a directory entry
	WS_FOUND	= WS_NAME + 16		@ a copy of the entry a lookup found
	WS_COMP		= WS_FOUND + 32		@ one component of a path
	WS_SIZE		= WS_COMP + 16

	@ Partition types worth looking at. Anything else in the table is skipped
	@ rather than probed, so a Linux or FileCore partition is left alone.
	PART_FAT12	= 0x01
	PART_FAT16_32M	= 0x04
	PART_FAT16	= 0x06
	PART_FAT32	= 0x0b
	PART_FAT32_LBA	= 0x0c
	PART_FAT16_LBA	= 0x0e

	.global	_start

_start:

module_start:

	.int	0		@ Start
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
	.int	1		@ 32 bit compatible

title:
	.string	"MultiFS"

	.include "multifs-version.inc"


	@ Help and Command keyword table
table:
	.string	"MultiFS"
	.align
	.int	command_multifs
	.int	0x00000000
	.int	0
	.int	command_multifs_help

	.string	"MultiFSDiscs"
	.align
	.int	command_discs
	.int	0x00000000
	.int	0
	.int	command_discs_help

	.string	"MultiFSProbe"
	.align
	.int	command_probe
	.int	0x00000000
	.int	0
	.int	command_discs_help

	.string	"MultiFSDir"
	.align
	.int	command_dir
	.int	0x00000000
	.int	0
	.int	command_discs_help

	.string	"MultiFSFind"
	.align
	.int	command_find
	.int	0x00000000
	.int	0
	.int	command_discs_help

	.byte	0	@ Table terminator

	.align

command_multifs_help:
	.string	"*MultiFS selects the MultiFS filing system\rSyntax: *MultiFS"
	.align

command_discs_help:
	.string	"*MultiFSDiscs lists the FAT volumes MultiFS can see\rSyntax: *MultiFSDiscs"
	.align


	@ Filing System Information Block
fs_info_block:
	.int	fs_name		@ Filing System name
	.int	fs_text		@ Filing System startup text
	.int	fs_open		@ To Open files (FSEntry_Open)
	.int	fs_getbytes	@ To Get Bytes (FSEntry_GetBytes)
	.int	fs_putbytes	@ To Put Bytes (FSEntry_PutBytes)
	.int	fs_args		@ To Control open files (FSEntry_Args)
	.int	fs_close	@ To Close open files (FSEntry_Close)
	.int	fs_file		@ To perform whole-file ops (FSEntry_File)
	.int	FILING_SYSTEM_NUMBER | (MAX_OPEN_FILES << 8)
				@ Filing System Information Word
	.int	fs_func		@ To perform various ops (FSEntry_Func)
	.int	fs_gbpb		@ To perform multi-byte ops (FSEntry_GBPB)
	.int	0		@ Extra Filing System Information Word

fs_name:
	.string	"MultiFS"

fs_text:
	.string	"RPCEmu MultiFS"
	.align


@ ---------------------------------------------------------------------------
@ Module initialisation and finalisation
@ ---------------------------------------------------------------------------

	/* Entry:
	 *   r10 = pointer to environment string
	 *   r11 = I/O base or instantiation number
	 *   r12 = pointer to private word for this instantiation
	 *   r13 = stack pointer (supervisor)
	 * Exit:
	 *   r7-r11, r13 preserved
	 */
init:
	stmfd	sp!, {r7-r11, lr}

	@ Workspace, zeroed. The volume table is read as soon as anything asks
	@ for a disc, and a half-filled record is worse than an empty one.
	mov	r0, #Module_Claim
	ldr	r3, =WS_SIZE
	swi	XOS_Module
	bvs	init_out

	str	r2, [wp]		@ private word holds the workspace

	mov	r0, #0
	mov	r1, r2
	ldr	r3, =WS_SIZE
	add	r3, r2, r3
init_zero:
	str	r0, [r1], #4
	cmp	r1, r3
	blo	init_zero

	@ Declare the filing system. If the number below is already taken this
	@ is what says so.
	mov	r0, #FSControl_AddFS
	adr	r1, module_start
	mov	r2, #(fs_info_block - module_start)
	mov	r3, wp
	swi	XOS_FSControl
	bvs	init_free_and_out

	@ Look at what is attached now. A stick plugged in later is the filer's
	@ business; this is so that a machine which booted with one already in
	@ has something to show.
	@
	@ wp has been the private word up to here, which is what FSControl_AddFS
	@ wanted; everything below works on the workspace itself.
	ldr	wp, [wp]
	bl	scan_all

	cmp	pc, #0			@ clear V for a successful return
	ldmfd	sp!, {r7-r11, pc}

init_free_and_out:
	@ Registration failed, so give the workspace back rather than leak it -
	@ init returning an error means finalisation is never called.
	stmfd	sp!, {r0}
	mov	r0, #Module_Free
	ldr	r2, [wp]
	swi	XOS_Module
	mov	r0, #0
	str	r0, [wp]
	ldmfd	sp!, {r0}

init_out:
	ldmfd	sp!, {r7-r11, pc}


	/* Entry:
	 *   r10 = fatality indication: 0 is non-fatal, 1 is fatal
	 *   r11 = instantiation number
	 *   r12 = pointer to private word
	 */
final:
	stmfd	sp!, {r7-r11, lr}

	mov	r0, #FSControl_RemoveFS
	adr	r1, fs_name
	swi	XOS_FSControl

	ldr	r2, [wp]
	cmp	r2, #0
	beq	final_out

	mov	r0, #Module_Free
	swi	XOS_Module
	mov	r0, #0
	str	r0, [wp]

final_out:
	cmp	pc, #0
	ldmfd	sp!, {r7-r11, pc}


	/* Service call handler.
	 *
	 * Service_FSRedeclare is FileSwitch asking every filing system to say it
	 * is still here, which happens when another one is removed.
	 */
service:
	teq	r1, #Service_FSRedeclare
	movnes	pc, lr

	stmfd	sp!, {r0-r3, lr}
	mov	r0, #FSControl_AddFS
	adr	r1, module_start
	mov	r2, #(fs_info_block - module_start)
	mov	r3, wp
	swi	XOS_FSControl
	ldmfd	sp!, {r0-r3, pc}


@ ---------------------------------------------------------------------------
@ Commands
@ ---------------------------------------------------------------------------

	/* *MultiFS - select this filing system */
command_multifs:
	stmfd	sp!, {lr}
	mov	r0, #FSControl_SelectFS
	adr	r1, fs_name
	swi	XOS_FSControl
	ldmfd	sp!, {pc}


	/* *MultiFSDiscs - say what the volume scan found.
	 *
	 * The scan is redone here rather than reported from the table, so that
	 * the command answers "what is attached now" rather than "what was
	 * attached when the module loaded".
	 */
command_discs:
	stmfd	sp!, {r0-r6, lr}

	ldr	wp, [wp]
	bl	scan_all

	ldr	r6, [wp, #WS_NVOLUMES]
	cmp	r6, #0
	beq	discs_none

	adr	r0, msg_heading
	swi	XOS_Write0

	add	r4, wp, #WS_VOLUMES
	mov	r5, #0

discs_loop:
	ldr	r0, [r4, #VOL_TYPE]
	cmp	r0, #0
	beq	discs_next

	@ drive and partition start
	adr	r0, msg_drive
	swi	XOS_Write0
	ldr	r0, [r4, #VOL_DRIVE]
	bl	print_cardinal

	adr	r0, msg_fat
	swi	XOS_Write0
	ldr	r0, [r4, #VOL_TYPE]
	bl	print_cardinal

	adr	r0, msg_clusters
	swi	XOS_Write0
	ldr	r0, [r4, #VOL_CLUSTERS]
	bl	print_cardinal

	adr	r0, msg_start
	swi	XOS_Write0
	ldr	r0, [r4, #VOL_START]
	bl	print_cardinal

	adr	r0, msg_label
	swi	XOS_Write0
	add	r0, r4, #VOL_LABEL
	swi	XOS_Write0

	swi	XOS_NewLine

discs_next:
	add	r4, r4, #VOL_SIZE
	add	r5, r5, #1
	cmp	r5, #MAX_VOLUMES
	blo	discs_loop

	ldmfd	sp!, {r0-r6, pc}

discs_none:
	adr	r0, msg_no_discs
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r6, pc}

msg_heading:
	.string	"Drive  Format  Clusters  Start  Name\n"
	.align
msg_drive:
	.string	"    "
	.align
msg_fat:
	.string	"    FAT"
	.align
msg_clusters:
	.string	"    "
	.align
msg_start:
	.string	"    "
	.align
msg_label:
	.string	"  "
	.align
msg_no_discs:
	.string	"No FAT volumes found"
	.align


	/* *MultiFSFind - look a few paths up and say what came back. */
command_find:
	stmfd	sp!, {r0-r8, lr}
	ldr	wp, [wp]

	adr	r8, find_paths
find_loop:
	ldrb	r0, [r8]
	cmp	r0, #0
	beq	find_out

	mov	r0, r8
	swi	XOS_Write0
	adr	r0, msg_gap
	swi	XOS_Write0

	bl	current_volume
	bvs	find_err
	mov	r1, r8
	bl	path_lookup
	bvs	find_err
	bl	print_cardinal
	b	find_eol

find_err:
	add	r0, r0, #4
	swi	XOS_Write0

find_eol:
	swi	XOS_NewLine
find_skip:
	ldrb	r0, [r8], #1
	cmp	r0, #0
	bne	find_skip
	b	find_loop

find_out:
	ldmfd	sp!, {r0-r8, pc}

find_paths:
	.string	"$"
	.string	"$.RPCTEST"
	.string	":0.$"
	.string	""
	.align


	/* *MultiFSDir - list the root directory of the first volume.
	 *
	 * A diagnostic, and the thing that proves the cluster and directory
	 * walking before any of it is wired to FileSwitch.
	 */
command_dir:
	stmfd	sp!, {r0-r8, lr}
	ldr	wp, [wp]

	bl	scan_all

	ldr	r0, [wp, #WS_NVOLUMES]
	cmp	r0, #0
	beq	dir_cmd_none

	add	r8, wp, #WS_VOLUMES	@ volume 0
	mov	r0, r8
	mov	r1, #0			@ its root
	bl	dir_open
	bvs	dir_cmd_out

dir_cmd_loop:
	bl	dir_next
	bvs	dir_cmd_out
	cmp	r0, #0
	beq	dir_cmd_out

	mov	r7, r0
	ldrb	r1, [r7, #11]		@ attributes

	@ A long-name entry is a fragment of the name of the entry after it, not
	@ an entry in its own right.
	and	r2, r1, #0x0f
	cmp	r2, #0x0f
	beq	dir_cmd_loop

	@ The volume label lives in the root as an entry with no data.
	tst	r1, #0x08
	bne	dir_cmd_loop

	@ Directory or file
	tst	r1, #0x10
	adrne	r0, msg_isdir
	adreq	r0, msg_isfile
	swi	XOS_Write0

	mov	r0, r7
	bl	print_short_name

	@ Size, which is meaningless for a directory
	ldrb	r1, [r7, #11]
	tst	r1, #0x10
	bne	dir_cmd_eol
	adr	r0, msg_gap
	swi	XOS_Write0
	mov	r0, r7
	mov	r1, #28
	bl	ld32
	bl	print_cardinal

dir_cmd_eol:
	swi	XOS_NewLine
	b	dir_cmd_loop

dir_cmd_none:
	adr	r0, msg_no_discs
	swi	XOS_Write0
	swi	XOS_NewLine

dir_cmd_out:
	ldmfd	sp!, {r0-r8, pc}

msg_isdir:
	.string	"D  "
	.align
msg_isfile:
	.string	"   "
	.align
msg_gap:
	.string	"  "
	.align


	/* Print the 8.3 name of a directory entry.
	 *
	 * RISC OS uses '.' to separate directories, so a FAT name's dot cannot
	 * survive as itself; '/' is what RISC OS has always used in its place.
	 *
	 * Entry: R0 = directory entry.
	 */
print_short_name:
	stmfd	sp!, {r0-r5, lr}
	bl	build_short_name
	swi	XOS_Write0
	ldmfd	sp!, {r0-r5, pc}


	/* Build the 8.3 name of a directory entry into the workspace.
	 *
	 * RISC OS uses '.' to separate directories, so a FAT name's dot cannot
	 * survive as itself; '/' is what RISC OS has always used in its place.
	 *
	 * Entry: R0 = directory entry.  Exit: R0 = the name, NUL terminated.
	 */
build_short_name:
	stmfd	sp!, {r1-r5, lr}

	mov	r4, r0
	ldr	r5, =WS_NAME
	add	r5, wp, r5
	mov	r3, #0

	@ The name, up to eight characters, trailing spaces dropped
	mov	r2, #8
	bl	copy_field
	@ The extension, if there is one
	ldrb	r0, [r4, #8]
	cmp	r0, #' '
	beq	print_short_done
	mov	r0, #'/'
	strb	r0, [r5, r3]
	add	r3, r3, #1
	add	r4, r4, #8
	mov	r2, #3
	bl	copy_field
	sub	r4, r4, #8

print_short_done:
	mov	r0, #0
	strb	r0, [r5, r3]
	mov	r0, r5
	ldmfd	sp!, {r1-r5, pc}

	.ltorg

	/* R4 = source, R2 = length, R5 = destination, R3 = offset so far. */
copy_field:
	stmfd	sp!, {r0-r2, lr}
	mov	r1, #0
copy_field_loop:
	ldrb	r0, [r4, r1]
	cmp	r0, #' '
	beq	copy_field_done		@ padding, and nothing follows it
	strb	r0, [r5, r3]
	add	r3, r3, #1
	add	r1, r1, #1
	cmp	r1, r2
	blo	copy_field_loop
copy_field_done:
	ldmfd	sp!, {r0-r2, pc}


	/* *MultiFSProbe - walk drive 0 aloud: what the boot sector holds, what
	   each partition entry says, and whether each was accepted.
	   The command that localises a fault to a step. */
command_probe:
	stmfd	sp!, {r0-r7, lr}
	ldr	wp, [wp]

	mov	r0, #0
	mov	r1, #0
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvc	probe_read_ok
	mov	r1, r0
	adr	r0, pmsg_readfail
	swi	XOS_Write0
	ldr	r0, [r1]		@ error number
	bl	print_cardinal
	adr	r0, msg_drive
	swi	XOS_Write0
	add	r0, r1, #4		@ error text
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r7, pc}

probe_read_ok:
	adr	r0, pmsg_sig
	swi	XOS_Write0
	add	r2, wp, #WS_SECTOR
	ldrb	r0, [r2, #510]
	bl	print_cardinal
	adr	r0, msg_drive
	swi	XOS_Write0
	add	r2, wp, #WS_SECTOR
	ldrb	r0, [r2, #511]
	bl	print_cardinal
	swi	XOS_NewLine

	mov	r4, #0
probe_part:
	adr	r0, pmsg_part
	swi	XOS_Write0
	mov	r0, r4
	bl	print_cardinal

	mov	r0, r4, lsl #4
	add	r0, r0, #0x1b0
	add	r0, r0, #0x0e
	add	r6, r0, #4

	adr	r0, pmsg_type
	swi	XOS_Write0
	add	r0, wp, #WS_SECTOR
	add	r0, r0, r6
	ldrb	r0, [r0]
	bl	print_cardinal

	adr	r0, pmsg_lba
	swi	XOS_Write0
	add	r0, wp, #WS_SECTOR
	add	r1, r6, #4
	bl	ld32
	mov	r7, r0
	bl	print_cardinal
	swi	XOS_NewLine

	cmp	r7, #0
	beq	probe_next

	mov	r0, #0
	mov	r1, r7
	bl	add_volume
	adr	r0, pmsg_added
	swi	XOS_Write0
	bvc	probe_added_ok
	swi	XOS_Write0
	swi	XOS_NewLine
	b	probe_next
probe_added_ok:
	adr	r0, pmsg_ok
	swi	XOS_Write0
	swi	XOS_NewLine

probe_next:
	add	r4, r4, #1
	cmp	r4, #4
	blo	probe_part

	ldmfd	sp!, {r0-r7, pc}

pmsg_readfail:
	.string	"probe: sector 0 unreadable, err "
	.align
pmsg_sig:
	.string	"probe: sig "
	.align
pmsg_part:
	.string	"probe: part "
	.align
pmsg_type:
	.string	"  type "
	.align
pmsg_lba:
	.string	"  lba "
	.align
pmsg_added:
	.string	"probe: add_volume -> "
	.align
pmsg_ok:
	.string	"added"
	.align


	/* Print an unsigned number in R0, no padding. */
print_cardinal:
	stmfd	sp!, {r0-r3, lr}
	sub	sp, sp, #16
	mov	r1, sp
	mov	r2, #16
	swi	XOS_ConvertCardinal4
	swi	XOS_Write0
	add	sp, sp, #16
	ldmfd	sp!, {r0-r3, pc}


@ ---------------------------------------------------------------------------
@ Reading sectors
@ ---------------------------------------------------------------------------

	/* Read one 512-byte sector.
	 *
	 * Entry: R0 = drive, R1 = sector (LBA), R2 = buffer
	 * Exit:  VC, or VS with R0 = error
	 *
	 * The call is FileCore's DiscOp shape and two details of it are easy to
	 * get wrong, both of which cost time here:
	 *
	 *   - The drive is NOT a register of its own. It goes in the top three
	 *     bits of the disc address in R2, which leaves 29 bits for the
	 *     address itself.
	 *   - R5 and R6 are part of the call and must be zero. Leaving whatever
	 *     happened to be in them gets "FileCore in use", which reads like a
	 *     busy device and is nothing of the sort.
	 *
	 * Those 29 bits count SECTORS here, because this uses SectorDiscOp rather
	 * than DiscOp - 256GB rather than the 512MB a byte address would reach.
	 */
read_sector:
	stmfd	sp!, {r1-r10, lr}

	@ Refuse an address that will not fit rather than let it wrap. A wrapped
	@ address reads a real sector somewhere else on the disc, and nothing
	@ downstream could tell that from the sector it asked for.
	cmp	r1, #(1 << 29)
	bcs	read_sector_too_far

	mov	r7, r0			@ drive
	mov	r8, r1			@ sector
	mov	r9, r2			@ buffer
	mov	r10, #1			@ retries left

	@ Clear the buffer first, so that a read which quietly transfers nothing
	@ cannot be mistaken for a successful one. Without this a drive that does
	@ not exist appears to hold whatever the last real drive did - which is
	@ exactly how three phantom copies of a stick turned up in the table.
	mov	r0, #0
	mov	r1, r9
	add	r2, r9, #512
read_sector_clear:
	str	r0, [r1], #4
	cmp	r1, r2
	blo	read_sector_clear

read_sector_try:
	mov	r2, r7, lsl #29		@ drive in the top three bits...
	orr	r2, r2, r8		@ ...and the sector number below it
	mov	r1, #1			@ reason: read sectors
	mov	r3, r9
	mov	r4, #512
	mov	r5, #0
	mov	r6, #0
	swi	XSCSIFS_SectorDiscOp
	bvc	read_sector_check

	@ A drive nothing has touched since it appeared answers "the disc drive
	@ is empty" until it is asked whether it is ready. Ask once and try
	@ again; if it still says empty, it means it.
	cmp	r10, #0
	beq	read_sector_out
	sub	r10, r10, #1

	mov	r1, r7			@ drive
	swi	XSCSIFS_TestReady
	b	read_sector_try

read_sector_check:
	@ R4 comes back as the number of bytes NOT transferred.
	cmp	r4, #0
	bne	read_sector_short

read_sector_out:
	ldmfd	sp!, {r1-r10, pc}

read_sector_too_far:
	adr	r0, err_too_far
	b	set_v_return

read_sector_short:
	adr	r0, err_short_read

set_v_return:
	cmp	r0, #NBIT		@ the idiom hostfs.s uses to set V
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r10, pc}

err_too_far:
	.int	0
	.string	"MultiFS cannot reach that far into the disc yet"
	.align

err_short_read:
	.int	0
	.string	"MultiFS could not read a whole sector"
	.align


	/* Is there a boot signature at the end of the sector buffer?
	 *
	 * Exit: Z set if the signature is there, Z clear if not.
	 */
boot_signature:
	stmfd	sp!, {r0-r2, lr}
	add	r2, wp, #WS_SECTOR
	ldrb	r0, [r2, #510]
	ldrb	r1, [r2, #511]
	cmp	r0, #0x55
	cmpeq	r1, #0xaa
	ldmfd	sp!, {r0-r2, pc}


	/* Read a 16-bit little-endian value from an unaligned offset.
	 *
	 * FAT lays its fields out at whatever offset it likes and half of them
	 * are odd, so these cannot be LDR/LDRH: an unaligned LDR on ARM rotates
	 * rather than faults, which would be wrong quietly.
	 *
	 * Entry: R0 = base, R1 = offset.  Exit: R0 = value.
	 */
ld16:
	stmfd	sp!, {r2, lr}
	add	r2, r0, r1
	ldrb	r0, [r2]
	ldrb	r1, [r2, #1]
	orr	r0, r0, r1, lsl #8
	ldmfd	sp!, {r2, pc}

	/* As ld16, for 32 bits. */
ld32:
	stmfd	sp!, {r2-r3, lr}
	add	r2, r0, r1
	ldrb	r0, [r2]
	ldrb	r3, [r2, #1]
	orr	r0, r0, r3, lsl #8
	ldrb	r3, [r2, #2]
	orr	r0, r0, r3, lsl #16
	ldrb	r3, [r2, #3]
	orr	r0, r0, r3, lsl #24
	ldmfd	sp!, {r2-r3, pc}


	/* Base-2 logarithm of a power of two.
	 *
	 * Every divisor FAT needs - bytes per sector, sectors per cluster - is a
	 * power of two, so the whole filing system can be built out of shifts and
	 * needs no division routine at all.
	 *
	 * Entry: R0 = value.  Exit: R0 = log2, or -1 if not a power of two.
	 */
log2:
	stmfd	sp!, {r1-r2, lr}
	cmp	r0, #0
	beq	log2_bad
	sub	r1, r0, #1
	tst	r0, r1			@ more than one bit set?
	bne	log2_bad
	mov	r1, #0
log2_loop:
	movs	r0, r0, lsr #1
	beq	log2_done
	add	r1, r1, #1
	b	log2_loop
log2_done:
	mov	r0, r1
	ldmfd	sp!, {r1-r2, pc}
log2_bad:
	mvn	r0, #0
	ldmfd	sp!, {r1-r2, pc}


@ ---------------------------------------------------------------------------
@ Finding volumes
@ ---------------------------------------------------------------------------

	/* Look at every drive and fill the volume table.
	 *
	 * Entry: wp = workspace.  Errors from a drive that is not there are
	 * expected and swallowed; the table simply gets no entry for it.
	 */
scan_all:
	stmfd	sp!, {r0-r6, lr}

	@ Empty the table first, so a stick that has gone does not linger.
	mov	r0, #0
	str	r0, [wp, #WS_NVOLUMES]
	add	r1, wp, #WS_VOLUMES
	mov	r2, #MAX_VOLUMES
scan_all_clear:
	str	r0, [r1, #VOL_TYPE]
	add	r1, r1, #VOL_SIZE
	subs	r2, r2, #1
	bne	scan_all_clear

	mov	r6, #0
scan_all_loop:
	mov	r0, r6
	bl	scan_drive
	add	r6, r6, #1
	cmp	r6, #MAX_DRIVES
	blo	scan_all_loop

	cmp	pc, #0
	ldmfd	sp!, {r0-r6, pc}


	/* Look at one drive.
	 *
	 * Sector 0 is either a partition table or a volume in its own right, and
	 * both are common on removable media - a stick from a camera is often a
	 * bare volume with no partition table at all. The signature alone does
	 * not tell them apart, because a FAT boot record carries it too, so the
	 * partition table is tried first and the whole device is only treated as
	 * one volume if no usable entry was found.
	 *
	 * Entry: R0 = drive.
	 */
scan_drive:
	stmfd	sp!, {r0-r7, lr}

	mov	r7, r0			@ drive
	mov	r1, #0			@ sector 0
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	scan_drive_out		@ no such drive, or nothing readable

	@ Boot signature. Without it this is not a partition table and not a
	@ boot record either.
	bl	boot_signature
	bne	scan_drive_out

	@ Try the four partition entries.
	mov	r5, #0			@ entries accepted
	mov	r4, #0			@ entry index
scan_drive_part:
	@ Entry is 16 bytes at &1BE + index*16; type at +4, first LBA at +8.
	mov	r0, r4, lsl #4
	add	r0, r0, #0x1b0
	add	r0, r0, #0x0e		@ &1BE + index*16
	add	r6, r0, #4		@ offset of the type byte

	add	r0, wp, #WS_SECTOR
	add	r0, r0, r6
	ldrb	r0, [r0]
	bl	partition_is_fat
	beq	scan_drive_next

	@ First LBA of the partition
	add	r0, wp, #WS_SECTOR
	add	r1, r6, #4
	bl	ld32
	mov	r1, r0
	mov	r0, r7
	bl	add_volume
	addvc	r5, r5, #1

scan_drive_next:
	add	r4, r4, #1
	cmp	r4, #4
	blo	scan_drive_part

	@ Nothing in the table looked like FAT, so try the device as one volume.
	cmp	r5, #0
	bne	scan_drive_out
	mov	r0, r7
	mov	r1, #0
	bl	add_volume

scan_drive_out:
	cmp	pc, #0
	ldmfd	sp!, {r0-r7, pc}


	/* Is this partition type one we should look at?
	 *
	 * Entry: R0 = type byte.  Exit: Z set if not FAT, Z clear if it is.
	 */
partition_is_fat:
	teq	r0, #PART_FAT12
	teqne	r0, #PART_FAT16_32M
	teqne	r0, #PART_FAT16
	teqne	r0, #PART_FAT32
	teqne	r0, #PART_FAT32_LBA
	teqne	r0, #PART_FAT16_LBA
	@ TEQ leaves Z set when one matched, so invert for the caller's sense.
	moveq	r0, #1
	movne	r0, #0
	teq	r0, #0
	mov	pc, lr


	/* Read a boot record and, if it is a FAT volume, add it to the table.
	 *
	 * Entry: R0 = drive, R1 = first sector of the volume.
	 * Exit:  VC if added, VS if it was not a usable FAT volume.
	 */
add_volume:
	stmfd	sp!, {r1-r11, lr}

	@ Somewhere to put it
	ldr	r2, [wp, #WS_NVOLUMES]
	cmp	r2, #MAX_VOLUMES
	bhs	add_volume_full

	mov	r10, r0			@ drive
	mov	r11, r1			@ start LBA

	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	add_volume_out

	@ Boot signature
	bl	boot_signature
	bne	add_volume_bad

	@ Bytes per sector. Only 512 is handled for now: everything below reads
	@ whole sectors into a 512-byte buffer, and a 4096-byte sector would
	@ overrun it. Refusing is better than truncating.
	add	r0, wp, #WS_SECTOR
	mov	r1, #11
	bl	ld16
	cmp	r0, #512
	bne	add_volume_bad
	mov	r4, r0			@ bytes per sector

	@ Sectors per cluster, which must be a power of two
	add	r0, wp, #WS_SECTOR
	ldrb	r5, [r0, #13]
	mov	r0, r5
	bl	log2
	cmp	r0, #0
	blt	add_volume_bad
	mov	r9, r0			@ log2(sectors per cluster)

	@ Reserved sectors
	add	r0, wp, #WS_SECTOR
	mov	r1, #14
	bl	ld16
	mov	r6, r0

	@ Number of FATs
	add	r0, wp, #WS_SECTOR
	ldrb	r7, [r0, #16]
	cmp	r7, #0
	beq	add_volume_bad
	cmp	r7, #4
	bhi	add_volume_bad

	@ Root entry count: zero on FAT32, non-zero on FAT12/16
	add	r0, wp, #WS_SECTOR
	mov	r1, #17
	bl	ld16
	mov	r8, r0

	@ FAT size: the 16-bit field, or the 32-bit one when that is zero
	add	r0, wp, #WS_SECTOR
	mov	r1, #22
	bl	ld16
	cmp	r0, #0
	bne	add_volume_have_fatsz
	add	r0, wp, #WS_SECTOR
	mov	r1, #36
	bl	ld32
add_volume_have_fatsz:
	cmp	r0, #0
	beq	add_volume_bad
	mov	r1, r0			@ FAT size in sectors
	stmfd	sp!, {r1}

	@ Total sectors: likewise the 16-bit field, else the 32-bit one
	add	r0, wp, #WS_SECTOR
	mov	r1, #19
	bl	ld16
	cmp	r0, #0
	bne	add_volume_have_totsec
	add	r0, wp, #WS_SECTOR
	mov	r1, #32
	bl	ld32
add_volume_have_totsec:
	ldmfd	sp!, {r1}		@ R1 = FAT size, R0 = total sectors
	cmp	r0, #0
	beq	add_volume_bad

	@ Everything needed is now in registers:
	@   R0 total sectors  R1 FAT size  R4 bytes/sector  R5 sectors/cluster
	@   R6 reserved       R7 FATs      R8 root entries  R9 log2(spc)
	@
	@ Root directory sectors, rounded up. On FAT32 the root is a normal
	@ cluster chain and this is zero.
	stmfd	sp!, {r0-r1}
	mov	r0, r8, lsl #5		@ entries * 32
	add	r0, r0, #0x100
	add	r0, r0, #0xff		@ + 511, in two encodable halves
	mov	r2, r0, lsr #9		@ / 512, the only sector size accepted
	ldmfd	sp!, {r0-r1}

	@ First data sector = reserved + FATs*FATsize + root directory sectors
	mla	r3, r7, r1, r6
	add	r3, r3, r2		@ R3 = first data sector

	@ Data sectors, then clusters
	subs	r2, r0, r3
	bls	add_volume_bad
	mov	r2, r2, lsr r9		@ R2 = count of clusters

	@ Write the record. Its address also goes in the workspace, because the
	@ label copy below is a BL and nothing held in LR would survive it.
	@
	@ R0 is the total sector count here and is wanted, so it is put by while
	@ the record address is worked out.
	stmfd	sp!, {r0}
	ldr	r0, [wp, #WS_NVOLUMES]
	add	lr, wp, #WS_VOLUMES
	add	lr, lr, r0, lsl #VOL_SHIFT
	str	lr, [wp, #WS_REC]
	ldmfd	sp!, {r0}

	str	r10, [lr, #VOL_DRIVE]
	str	r11, [lr, #VOL_START]
	str	r0, [lr, #VOL_TOTSEC]
	str	r4, [lr, #VOL_BPS]
	str	r5, [lr, #VOL_SPC]
	str	r6, [lr, #VOL_RSVD]
	str	r7, [lr, #VOL_NFATS]
	str	r8, [lr, #VOL_ROOTENTS]
	str	r1, [lr, #VOL_FATSZ]
	str	r3, [lr, #VOL_FIRSTDATA]
	str	r2, [lr, #VOL_CLUSTERS]
	str	r9, [lr, #VOL_SPCLOG]

	@ Absolute positions, worked out once here rather than at every read.
	@ R3 is the first data sector relative to the volume; the FATs and the
	@ fixed root sit between the reserved sectors and it.
	add	r0, r11, r6		@ start + reserved
	str	r0, [lr, #VOL_FATSEC0]
	mla	r0, r7, r1, r0		@ + FATs * FAT size
	str	r0, [lr, #VOL_ROOTSEC0]
	add	r0, r11, r3
	str	r0, [lr, #VOL_DATASEC0]
	ldr	r0, [lr, #VOL_ROOTSEC0]
	ldr	lr, [wp, #WS_REC]
	ldr	r0, [lr, #VOL_DATASEC0]
	ldr	r3, [lr, #VOL_ROOTSEC0]
	sub	r0, r0, r3
	str	r0, [lr, #VOL_ROOTSECS]
	ldr	r3, [lr, #VOL_FIRSTDATA]

	@ Which FAT is it? The cluster count decides, and nothing else does -
	@ the "FAT32" string in the boot record is a comment and is not
	@ authoritative.
	mov	r0, #12
	ldr	r3, =4085
	cmp	r2, r3
	blo	add_volume_type_known
	mov	r0, #16
	ldr	r3, =65525
	cmp	r2, r3
	blo	add_volume_type_known
	mov	r0, #32
add_volume_type_known:
	str	r0, [lr, #VOL_TYPE]

	cmp	r0, #32
	bne	add_volume_label

	add	r0, wp, #WS_SECTOR
	mov	r1, #44
	bl	ld32			@ costs us LR, so fetch the record again
	ldr	lr, [wp, #WS_REC]
	str	r0, [lr, #VOL_ROOTCLUS]

add_volume_label:
	ldr	r0, [wp, #WS_REC]
	ldr	r1, [r0, #VOL_TYPE]
	bl	copy_label

	@ Only now is the record real, so only now does the count move on.
	ldr	lr, [wp, #WS_NVOLUMES]
	add	lr, lr, #1
	str	lr, [wp, #WS_NVOLUMES]

	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

add_volume_bad:
	adr	r0, err_not_fat
	b	add_volume_setv

add_volume_full:
	adr	r0, err_too_many

add_volume_setv:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

add_volume_out:
	ldmfd	sp!, {r1-r11, pc}

err_not_fat:
	.int	0
	.string	"Not a FAT volume"
	.align

err_too_many:
	.int	0
	.string	"MultiFS has no room for another volume"
	.align


	/* Copy the volume label out of the boot record, trimming the padding.
	 *
	 * FAT pads the label with spaces to eleven characters, which would show
	 * on the icon bar and in every path, so the trailing ones go. The field
	 * is at a different offset on FAT32 than on FAT12/16.
	 *
	 * Entry: R0 = volume record, R1 = FAT type.
	 */
copy_label:
	stmfd	sp!, {r0-r5, lr}

	mov	r5, r0			@ the record

	cmp	r1, #32
	moveq	r1, #71
	movne	r1, #43

	add	r0, wp, #WS_SECTOR
	add	r0, r0, r1		@ source
	add	r1, r5, #VOL_LABEL	@ destination

	mov	r2, #0
copy_label_in:
	ldrb	r3, [r0, r2]
	strb	r3, [r1, r2]
	add	r2, r2, #1
	cmp	r2, #11
	blo	copy_label_in

	@ Trim trailing spaces, then terminate.
copy_label_trim:
	cmp	r2, #0
	beq	copy_label_done
	sub	r3, r2, #1
	ldrb	r4, [r1, r3]
	cmp	r4, #' '
	bne	copy_label_done
	mov	r2, r3
	b	copy_label_trim

copy_label_done:
	mov	r3, #0
	strb	r3, [r1, r2]

	ldmfd	sp!, {r0-r5, pc}


@ ---------------------------------------------------------------------------
@ Clusters, the FAT, and walking a directory
@ ---------------------------------------------------------------------------

	/* First sector of a cluster.
	 *
	 * Entry: R0 = volume record, R1 = cluster.  Exit: R0 = absolute LBA.
	 *
	 * Clusters are numbered from two: the first two entries of the FAT are
	 * the media descriptor and the end marker, so cluster 2 is the first one
	 * that has any data behind it.
	 */
cluster_sector:
	stmfd	sp!, {r1-r3, lr}
	ldr	r2, [r0, #VOL_SPCLOG]
	ldr	r3, [r0, #VOL_DATASEC0]
	sub	r1, r1, #2
	add	r0, r3, r1, lsl r2
	ldmfd	sp!, {r1-r3, pc}


	/* Follow one link of a cluster chain.
	 *
	 * Entry: R0 = volume record, R1 = cluster.
	 * Exit:  R0 = the next cluster, or 0 for end of chain; VS on a read error.
	 *
	 * FAT16 and FAT32 differ only in how wide an entry is and how much of it
	 * means anything: the top four bits of a FAT32 entry are reserved and are
	 * not part of the cluster number, which is why the mask below is 28 bits
	 * and not 32.
	 */
fat_next:
	stmfd	sp!, {r1-r7, lr}

	mov	r7, r0			@ volume record
	ldr	r2, [r7, #VOL_TYPE]

	cmp	r2, #32
	moveq	r3, r1, lsl #2		@ FAT32: four bytes an entry
	movne	r3, r1, lsl #1		@ FAT16: two

	@ Which FAT sector holds it, and where in that sector.
	ldr	r0, [r7, #VOL_FATSEC0]
	add	r0, r0, r3, lsr #9
	ldr	r6, =511
	and	r6, r3, r6

	@ The FAT is consulted for every link of every chain, so the last sector
	@ read is kept. A directory of any size sits in one or two FAT sectors,
	@ which turns a walk from one read per cluster into one read per file.
	ldr	r4, [wp, #WS_FATSEC_LBA]
	cmp	r4, r0
	beq	fat_next_have_sector

	stmfd	sp!, {r0}
	ldr	r5, [r7, #VOL_DRIVE]
	mov	r1, r0
	mov	r0, r5
	add	r2, wp, #WS_FATSEC
	bl	read_sector
	ldmfd	sp!, {r0}
	bvs	fat_next_out
	str	r0, [wp, #WS_FATSEC_LBA]

fat_next_have_sector:
	add	r0, wp, #WS_FATSEC
	mov	r1, r6
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #32
	beq	fat_next_32

	bl	ld16
	ldr	r1, =0xfff8
	cmp	r0, r1
	movhs	r0, #0			@ end of chain
	b	fat_next_ok

fat_next_32:
	bl	ld32
	bic	r0, r0, #0xf0000000	@ the top four bits are not ours
	ldr	r1, =0x0ffffff8
	cmp	r0, r1
	movhs	r0, #0			@ end of chain

fat_next_ok:
	cmp	pc, #0			@ clear V

fat_next_out:
	ldmfd	sp!, {r1-r7, pc}

	.ltorg


	/* Start walking a directory.
	 *
	 * Entry: R0 = volume record, R1 = first cluster, or 0 for the root.
	 *
	 * On FAT12 and FAT16 the root is not a cluster chain at all but a fixed
	 * run of sectors of a fixed size, which is the single biggest difference
	 * between those and FAT32 and the reason for the flag below.
	 */
dir_open:
	stmfd	sp!, {r0-r4, lr}

	str	r0, [wp, #WS_IT_REC]
	mov	r4, r0

	cmp	r1, #0
	bne	dir_open_chain

	ldr	r2, [r4, #VOL_TYPE]
	cmp	r2, #32
	beq	dir_open_root32

	@ FAT12/16: the fixed root
	mov	r0, #1
	str	r0, [wp, #WS_IT_FIXED]
	ldr	r0, [r4, #VOL_ROOTSEC0]
	str	r0, [wp, #WS_IT_SECTOR]
	ldr	r0, [r4, #VOL_ROOTSECS]
	str	r0, [wp, #WS_IT_SECLEFT]
	mov	r0, #0
	str	r0, [wp, #WS_IT_CLUSTER]
	b	dir_open_done

dir_open_root32:
	ldr	r1, [r4, #VOL_ROOTCLUS]

dir_open_chain:
	mov	r0, #0
	str	r0, [wp, #WS_IT_FIXED]
	str	r1, [wp, #WS_IT_CLUSTER]
	mov	r0, r4
	bl	cluster_sector
	str	r0, [wp, #WS_IT_SECTOR]
	ldr	r0, [r4, #VOL_SPCLOG]
	mov	r1, #1
	mov	r0, r1, lsl r0
	str	r0, [wp, #WS_IT_SECLEFT]

dir_open_done:
	@ Nothing is in the buffer yet, so the first step must fetch a sector.
	mov	r0, #512
	str	r0, [wp, #WS_IT_OFF]

	cmp	pc, #0
	ldmfd	sp!, {r0-r4, pc}


	/* The next directory entry that is worth looking at.
	 *
	 * Exit: R0 = pointer to a 32-byte entry, or 0 when the directory ends.
	 *       VS on a read error.
	 *
	 * Deleted entries are skipped here. Long-file-name entries are not: they
	 * carry the name the user actually gave the file, and the caller wants
	 * them.
	 */
dir_next:
	stmfd	sp!, {r1-r7, lr}

dir_next_again:
	ldr	r0, [wp, #WS_IT_OFF]
	cmp	r0, #512
	blo	dir_next_have_sector

	@ Another sector, from this cluster or the next one.
	ldr	r0, [wp, #WS_IT_SECLEFT]
	cmp	r0, #0
	bne	dir_next_read

	ldr	r0, [wp, #WS_IT_FIXED]
	cmp	r0, #0
	bne	dir_next_end		@ a fixed root simply runs out

	ldr	r0, [wp, #WS_IT_REC]
	ldr	r1, [wp, #WS_IT_CLUSTER]
	bl	fat_next
	bvs	dir_next_out
	cmp	r0, #0
	beq	dir_next_end

	str	r0, [wp, #WS_IT_CLUSTER]
	mov	r1, r0
	ldr	r0, [wp, #WS_IT_REC]
	bl	cluster_sector
	str	r0, [wp, #WS_IT_SECTOR]
	ldr	r0, [wp, #WS_IT_REC]
	ldr	r0, [r0, #VOL_SPCLOG]
	mov	r1, #1
	mov	r0, r1, lsl r0
	str	r0, [wp, #WS_IT_SECLEFT]

dir_next_read:
	ldr	r0, [wp, #WS_IT_REC]
	ldr	r0, [r0, #VOL_DRIVE]
	ldr	r1, [wp, #WS_IT_SECTOR]
	add	r2, wp, #WS_DIRSEC
	bl	read_sector
	bvs	dir_next_out

	ldr	r0, [wp, #WS_IT_SECTOR]
	add	r0, r0, #1
	str	r0, [wp, #WS_IT_SECTOR]
	ldr	r0, [wp, #WS_IT_SECLEFT]
	sub	r0, r0, #1
	str	r0, [wp, #WS_IT_SECLEFT]
	mov	r0, #0
	str	r0, [wp, #WS_IT_OFF]

dir_next_have_sector:
	ldr	r0, [wp, #WS_IT_OFF]
	add	r1, wp, #WS_DIRSEC
	add	r1, r1, r0
	add	r0, r0, #32
	str	r0, [wp, #WS_IT_OFF]

	ldrb	r2, [r1]
	cmp	r2, #0
	beq	dir_next_end		@ a zero name byte ends the directory
	cmp	r2, #0xe5
	beq	dir_next_again		@ deleted

	mov	r0, r1
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

dir_next_end:
	mov	r0, #0
	cmp	pc, #0

dir_next_out:
	ldmfd	sp!, {r1-r7, pc}


@ ---------------------------------------------------------------------------
@ Finding an object by name
@ ---------------------------------------------------------------------------

	/* Fold a character for comparison.
	 *
	 * FAT short names are upper case on the medium whatever the user typed,
	 * and RISC OS does not care about case in a filename either, so both
	 * sides are folded before comparing.
	 */
upper:
	cmp	r0, #'a'
	rsbges	r1, r0, #'z'
	subge	r0, r0, #('a' - 'A')
	mov	pc, lr


	/* Compare a built name against a path component.
	 *
	 * Entry: R0 = name, R1 = component.  Exit: Z set if they match.
	 */
name_compare:
	stmfd	sp!, {r0-r4, lr}
	mov	r3, r0
	mov	r4, r1
	mov	r2, #0
name_compare_loop:
	ldrb	r0, [r3, r2]
	bl	upper
	mov	r1, r0
	ldrb	r0, [r4, r2]
	bl	upper
	cmp	r0, r1
	bne	name_compare_out
	cmp	r0, #0
	beq	name_compare_out	@ both ended together
	add	r2, r2, #1
	b	name_compare_loop
name_compare_out:
	ldmfd	sp!, {r0-r4, pc}


	/* Find an object by path.
	 *
	 * Entry: R0 = volume record, R1 = path within the disc.
	 * Exit:  R0 = 0 if not found, 1 for a file, 2 for a directory.
	 *        WS_FOUND holds a copy of the directory entry when found.
	 *
	 * The path arrives in RISC OS's own shape: an optional ":disc." that has
	 * already served its purpose by the time it gets here, "$" for the root,
	 * and components separated by dots. A component of "" means the root
	 * itself, which is a directory and has no entry of its own - hence the
	 * early return rather than a search for it.
	 */
path_lookup:
	stmfd	sp!, {r1-r11, lr}

	mov	r10, r0			@ volume record
	mov	r11, r1			@ where we are in the path
	mov	r9, #0			@ cluster of the directory being searched

	@ Step over a leading disc name, which the caller has already used.
	ldrb	r0, [r11]
	cmp	r0, #':'
	bne	path_no_disc
path_skip_disc:
	ldrb	r0, [r11], #1
	cmp	r0, #0
	beq	path_is_root
	cmp	r0, #'.'
	bne	path_skip_disc

path_no_disc:
	@ Step over "$" and the dot after it.
	ldrb	r0, [r11]
	cmp	r0, #'$'
	addeq	r11, r11, #1
	ldrb	r0, [r11]
	cmp	r0, #'.'
	addeq	r11, r11, #1

	ldrb	r0, [r11]
	cmp	r0, #0
	beq	path_is_root

path_component:
	@ Copy the next component out of the path.
	ldr	r8, =WS_COMP
	add	r8, wp, r8
	mov	r2, #0
path_copy:
	ldrb	r0, [r11]
	cmp	r0, #0
	beq	path_copy_done
	cmp	r0, #'.'
	beq	path_copy_done
	cmp	r2, #14
	bhs	path_copy_next
	strb	r0, [r8, r2]
	add	r2, r2, #1
path_copy_next:
	add	r11, r11, #1
	b	path_copy
path_copy_done:
	mov	r0, #0
	strb	r0, [r8, r2]
	ldrb	r0, [r11]
	cmp	r0, #'.'
	addeq	r11, r11, #1

	@ Search the directory we are in for it.
	mov	r0, r10
	mov	r1, r9
	bl	dir_open
	bvs	path_out

path_search:
	bl	dir_next
	bvs	path_out
	cmp	r0, #0
	beq	path_not_found

	mov	r7, r0
	ldrb	r1, [r7, #11]
	and	r2, r1, #0x0f
	cmp	r2, #0x0f
	beq	path_search		@ a long-name fragment
	tst	r1, #0x08
	bne	path_search		@ the volume label

	mov	r0, r7
	bl	build_short_name
	ldr	r1, =WS_COMP
	add	r1, wp, r1
	bl	name_compare
	bne	path_search

	@ Found this component. Keep the entry: the directory buffer it lives in
	@ will be reused by the next dir_open.
	ldr	r1, =WS_FOUND
	add	r1, wp, r1
	mov	r2, #0
path_keep:
	ldrb	r0, [r7, r2]
	strb	r0, [r1, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	path_keep

	@ Is that the end of the path?
	ldrb	r0, [r11]
	cmp	r0, #0
	beq	path_found

	@ No, so this one has to be a directory to go on into.
	ldrb	r0, [r7, #11]
	tst	r0, #0x10
	beq	path_not_found

	mov	r0, r7
	mov	r1, #20
	bl	ld16
	mov	r9, r0, lsl #16
	mov	r0, r7
	mov	r1, #26
	bl	ld16
	orr	r9, r9, r0
	b	path_component

path_found:
	ldr	r1, =WS_FOUND
	add	r1, wp, r1
	ldrb	r0, [r1, #11]
	tst	r0, #0x10
	movne	r0, #2
	moveq	r0, #1
	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

path_is_root:
	mov	r0, #2
	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

path_not_found:
	mov	r0, #0
	cmp	pc, #0

path_out:
	ldmfd	sp!, {r1-r11, pc}

	.ltorg


	/* The volume everything works on, for now.
	 *
	 * One stick at a time is what the emulated bus presents, so a lookup
	 * uses the first volume found. Choosing by disc name is what this
	 * becomes when more than one can be attached.
	 *
	 * Exit: R0 = volume record, or VS if there is nothing mounted.
	 */
current_volume:
	stmfd	sp!, {r1, lr}
	ldr	r0, [wp, #WS_NVOLUMES]
	cmp	r0, #0
	bne	current_volume_have

	@ Nothing in the table. That is the ordinary state when the module was
	@ loaded before the drive was ready, or when a stick has just been put
	@ in, so look again rather than declaring there is no disc.
	bl	scan_all
	ldr	r0, [wp, #WS_NVOLUMES]
	cmp	r0, #0
	beq	current_volume_none

current_volume_have:
	add	r0, wp, #WS_VOLUMES
	cmp	pc, #0
	ldmfd	sp!, {r1, pc}
current_volume_none:
	adr	r0, err_no_disc
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, pc}

err_no_disc:
	.int	0xd4			@ FileCore's "disc not found"
	.string	"No USB disc is mounted"
	.align


@ ---------------------------------------------------------------------------
@ FileSwitch entry points
@ ---------------------------------------------------------------------------
@
@ Registered so that the filing system exists and can be selected, and so that
@ the volume scan above can be exercised, but not yet implemented. Each returns
@ an error rather than pretending to succeed: a filing system that silently
@ returns nothing for a directory looks like an empty disc, which is exactly the
@ misdiagnosis this module exists to end.

	/* FSEntry_File - whole-object operations.
	 *
	 *   R0 = reason, R1 = name, R6 = special field
	 *
	 * Only reason 5, read catalogue info, is answered: it is what everything
	 * else asks first, and what tells FileSwitch an object exists at all.
	 *
	 *   Out  R0 = object type, 0 none, 1 file, 2 directory
	 *        R2 = load address, R3 = exec address, R4 = length,
	 *        R5 = attributes
	 */
fs_file:
	stmfd	sp!, {r1, r6-r12, lr}
	ldr	wp, [wp]		@ FileSwitch hands us the private word

	cmp	r0, #5
	bne	fs_file_unsupported

	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_file_out

	bl	path_lookup
	bvs	fs_file_out
	cmp	r0, #0
	beq	fs_file_none

	cmp	r0, #2
	beq	fs_file_dir

	@ A file. Everything is a data file for now: giving a type by extension
	@ is a table, and a table that guesses wrongly is worse than one honest
	@ answer until MimeMap is consulted properly.
	ldr	r6, =WS_FOUND
	add	r6, wp, r6
	mov	r0, r6
	mov	r1, #28
	bl	ld32
	mov	r4, r0			@ length
	ldr	r2, =0xfffffd00		@ load: typed, &FFD data
	mov	r3, #0			@ exec
	ldrb	r0, [r6, #11]
	mov	r5, #3			@ read and write
	tst	r0, #0x01		@ read-only on the medium
	bicne	r5, r5, #2
	mov	r0, #1
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_dir:
	mov	r2, #0
	mov	r3, #0
	mov	r4, #0
	mov	r5, #3
	mov	r0, #2
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_none:
	mov	r0, #0
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_unsupported:
	mov	r0, #0
	cmp	pc, #0

fs_file_out:
	ldmfd	sp!, {r1, r6-r12, pc}

	.ltorg


	/* FSEntry_Func - the general-purpose entry.
	 *
	 *   R0 = reason
	 *
	 * 14 and 15 are what a Filer window and *Cat are built out of: read so
	 * many entries from a directory, starting at a given index.
	 *
	 *   R1 = directory, R2 = buffer, R3 = number wanted, R4 = index to
	 *   start at, R5 = buffer length, R6 = special field
	 *   Out R3 = number read, R4 = next index, or -1 when the end is reached
	 */
fs_func:
	stmfd	sp!, {r0-r2, r5-r12, lr}
	ldr	wp, [wp]		@ FileSwitch hands us the private word

	cmp	r0, #14
	beq	fs_func_dir
	cmp	r0, #15
	beq	fs_func_dir
	cmp	r0, #11
	beq	fs_func_discname

	@ Everything else is accepted and does nothing, which is what most of the
	@ reasons want: refusing them makes FileSwitch give up on the whole
	@ filing system rather than on the one operation.
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

	/* Reason 11: read disc name and boot option, R2 = buffer. */
fs_func_discname:
	bl	current_volume
	bvs	fs_func_out
	mov	r7, r0
	ldmfd	sp, {r0-r2}		@ recover the caller's R2 without popping
	add	r8, r7, #VOL_LABEL
	mov	r3, #0
fs_func_dn_copy:
	ldrb	r0, [r8, r3]
	strb	r0, [r2, r3]
	cmp	r0, #0
	beq	fs_func_dn_done
	add	r3, r3, #1
	cmp	r3, #11
	blo	fs_func_dn_copy
fs_func_dn_done:
	mov	r0, #0
	strb	r0, [r2, r3]
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

	/* Reasons 14 and 15. */
fs_func_dir:
	mov	r9, r0			@ which reason
	mov	r10, r3			@ how many wanted
	mov	r11, r4			@ index to start at

	stmfd	sp!, {r1-r2}
	bl	current_volume
	ldmfd	sp!, {r1-r2}
	bvs	fs_func_out
	mov	r7, r0			@ volume

	mov	r8, r2			@ where the entries go

	stmfd	sp!, {r1}
	mov	r0, r7
	bl	path_lookup
	ldmfd	sp!, {r1}
	bvs	fs_func_out
	cmp	r0, #2
	bne	fs_func_notdir

	@ The root has no entry of its own, so a lookup of it leaves WS_FOUND
	@ untouched; cluster 0 means "the root" to dir_open either way.
	mov	r6, #0
	cmp	r0, #2
	ldr	r0, =WS_FOUND
	add	r0, wp, r0
	ldrb	r1, [r0, #11]
	tst	r1, #0x10
	beq	fs_func_dir_open	@ no entry: it is the root
	mov	r1, #20
	stmfd	sp!, {r0}
	bl	ld16
	mov	r6, r0, lsl #16
	ldmfd	sp!, {r0}
	mov	r1, #26
	stmfd	sp!, {r0}
	bl	ld16
	ldmfd	sp!, {r1}
	orr	r6, r6, r0

fs_func_dir_open:
	mov	r0, r7
	mov	r1, r6
	bl	dir_open
	bvs	fs_func_out

	@ Step over the entries already returned by an earlier call.
	mov	r5, #0
fs_func_skip:
	cmp	r5, r11
	bhs	fs_func_collect
	bl	fs_func_next_real
	bvs	fs_func_out
	cmp	r0, #0
	beq	fs_func_end
	add	r5, r5, #1
	b	fs_func_skip

fs_func_collect:
	mov	r4, #0			@ entries put in the buffer
fs_func_collect_loop:
	cmp	r4, r10
	bhs	fs_func_done
	bl	fs_func_next_real
	bvs	fs_func_out
	cmp	r0, #0
	beq	fs_func_end

	mov	r6, r0
	cmp	r9, #15
	beq	fs_func_with_info

	@ Reason 14: just the name
	mov	r0, r6
	bl	build_short_name
	mov	r1, r0
	bl	copy_name_to_buffer
	b	fs_func_counted

fs_func_with_info:
	@ Reason 15: five words, then the name
	ldrb	r0, [r6, #11]
	tst	r0, #0x10
	ldr	r0, =0xfffffd00
	movne	r0, #0
	str	r0, [r8], #4		@ load address
	mov	r0, #0
	str	r0, [r8], #4		@ exec address
	mov	r0, r6
	mov	r1, #28
	bl	ld32
	ldrb	r1, [r6, #11]
	tst	r1, #0x10
	movne	r0, #0
	str	r0, [r8], #4		@ length
	mov	r0, #3
	str	r0, [r8], #4		@ attributes
	ldrb	r1, [r6, #11]
	tst	r1, #0x10
	movne	r0, #2
	moveq	r0, #1
	str	r0, [r8], #4		@ object type
	mov	r0, r6
	bl	build_short_name
	mov	r1, r0
	bl	copy_name_to_buffer

fs_func_counted:
	add	r4, r4, #1
	add	r11, r11, #1
	b	fs_func_collect_loop

fs_func_done:
	@ R3 = how many, R4 = where to carry on from
	mov	r3, r4
	mov	r4, r11
	str	r3, [sp, #12]		@ the caller's R3
	str	r4, [sp, #16]		@ the caller's R4 (R0-R2 then R5.. are below)
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

fs_func_end:
	mov	r3, r4
	mvn	r4, #0			@ -1: nothing left
	str	r3, [sp, #12]
	str	r4, [sp, #16]
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

fs_func_notdir:
	adr	r0, err_not_dir
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

fs_func_out:
	ldmfd	sp!, {r0-r2, r5-r12, pc}

err_not_dir:
	.int	0xd6
	.string	"Not a directory"
	.align

	/* The next entry that is a real object, skipping name fragments and the
	   volume label. Exit R0 = entry, or 0 at the end. */
fs_func_next_real:
	stmfd	sp!, {r1-r2, lr}
fs_func_next_real_loop:
	bl	dir_next
	bvs	fs_func_next_real_out
	cmp	r0, #0
	beq	fs_func_next_real_out
	ldrb	r1, [r0, #11]
	and	r2, r1, #0x0f
	cmp	r2, #0x0f
	beq	fs_func_next_real_loop
	tst	r1, #0x08
	bne	fs_func_next_real_loop
fs_func_next_real_out:
	ldmfd	sp!, {r1-r2, pc}

	/* Copy a NUL-terminated name to R8, word aligning after it. */
copy_name_to_buffer:
	stmfd	sp!, {r0-r2, lr}
	mov	r2, #0
copy_name_loop:
	ldrb	r0, [r1, r2]
	strb	r0, [r8, r2]
	add	r2, r2, #1
	cmp	r0, #0
	bne	copy_name_loop
	add	r8, r8, r2
	add	r8, r8, #3
	bic	r8, r8, #3
	ldmfd	sp!, {r0-r2, pc}

	.ltorg


fs_open:
fs_getbytes:
fs_putbytes:
fs_args:
fs_close:
fs_gbpb:
	stmfd	sp!, {lr}
	adr	r0, err_not_yet
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {pc}

err_not_yet:
	.int	0
	.string	"MultiFS cannot do that yet"
	.align

	.end
