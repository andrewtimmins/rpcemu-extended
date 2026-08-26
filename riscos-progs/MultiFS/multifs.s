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
	XOS_CLI			= 0x20005
	XOS_GBPB		= 0x2000c
	XOS_Find		= 0x2000d
	XOS_NewLine		= 0x20003
	XOS_Word		= 0x20007
	XOS_Module		= 0x2001e
	XOS_FSControl		= 0x20029
	XOS_ConvertCardinal4	= 0x200d8
	XOS_ReadMonotonicTime	= 0x20042

	XFree_Register		= 0x644c0
	XFree_DeRegister	= 0x644c1

	XTerritory_ReadCurrentTimeZone	= 0x63048
	XTerritory_ConvertTimeToUTCOrdinals = 0x63049
	XTerritory_ConvertOrdinalsToTime = 0x63051

	@ Ordinals block, as both Territory calls lay it out
	ORD_CS		= 0
	ORD_SECOND	= 4
	ORD_MINUTE	= 8
	ORD_HOUR	= 12
	ORD_DAY		= 16
	ORD_MONTH	= 20
	ORD_YEAR	= 24

	@ Where a RISC OS file type lives on FAT media.
	@
	@ FAT has no field for one, so it has to be borrowed from a field that is
	@ there, and where to borrow is not a free choice: the same stick is read on
	@ other machines and by Windows. The layout below is therefore fixed by
	@ convention and MUST NOT be changed - whose convention it is, and why it is
	@ followed here, is recorded in the README under "License and credits".
	@
	@ An impossible CREATION DATE marks the entry and the type is spread across
	@ the creation TIME. The older trick of putting it in the NTRes byte at
	@ offset 12 has been abandoned, Windows 10 having made that byte unsafe to
	@ use; it is still read below for the sake of media written before that, but
	@ nothing here writes it.
	@
	@ The marker is 127<<9 | 1<<5 | 1: the 1st of January 2107, which nothing
	@ writes by accident. Only the CREATION stamp is given up for this. The
	@ last-written stamp, which is the one anybody looks at, is untouched.
	CTIME_FTYPE_MAGIC	= (127 << 9) + (1 << 5) + 1

	@ Data, which is what an untyped file is
	FILETYPE_DATA		= 0xffd

	@ OS_Word 14, sub-reason 3: read the soft copy of the clock, as UTC.
	OSWord_ReadClock	= 14
	ReadClock_UTC		= 3

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

	@ Bit 20 of the information word: have FileSwitch load a whole file by
	@ opening it and reading it, rather than by calling FSEntry_File 255.
	@
	@ Without it, loading a file gives an application an empty buffer and no
	@ error at all. FSEntry_File here answers reason 5 and nothing else, and
	@ an unhandled reason returns "nothing to report" with V clear - which
	@ FileSwitch reads as a load that succeeded and produced no bytes. *Type
	@ was unaffected throughout, because it opens the file and reads it, which
	@ is exactly what this bit makes everything else do too.
	FS_LOAD_BY_STREAM	= 1 << 20

	@ Bit 19, the same idea for saving: FileSwitch creates the file by opening
	@ it, writing it and closing it, rather than through FSEntry_File 0.
	FS_SAVE_BY_STREAM	= 1 << 19

	@ Bit 23 of the information word. Without it FileSwitch never sends the
	@ later FSEntry_Func reasons at all: free space was implemented, correct,
	@ and simply never asked for - the reason codes arriving were 0 and nothing
	@ else. Our own HostFS carries the same bit, under the name
	@ IMAGEFS_EXTENSIONS.
	FS_EXTRA_ENTRIES	= 1 << 23
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
	VOL_FSINFO	= 84	@ FAT32: absolute LBA of the FSInfo sector, else 0
	VOL_FREE	= 88	@ free clusters, or -1 when nobody has counted yet
	VOL_NEXTFREE	= 92	@ where the next allocation search starts
	@ Set the moment anything is allocated or freed. It says the FSInfo sector
	@ on the medium no longer describes this volume, so its free count must
	@ not be believed and has to be written back.
	VOL_FSDIRTY	= 96
	@ exFAT only: where the allocation bitmap lives. exFAT has no free count
	@ anywhere on the medium - the bitmap IS the answer, one bit per cluster.
	VOL_BMP_CLUS	= 100	@ its first cluster
	VOL_BMP_LEN	= 104	@ its length in bytes
	VOL_BMP_CONTIG	= 108	@ and whether it is one unbroken run
	@ NTFS only.
	VOL_MFT_CLUS	= 112	@ cluster the Master File Table starts at
	VOL_MFT_RECSZ	= 116	@ bytes in one MFT record, usually 1024
	VOL_IDX_SZ	= 120	@ bytes in one index block, usually 4096
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
	WS_DIRSEC_LBA	= WS_IT_FIXED + 4	@ which sector WS_DIRSEC actually holds
	WS_NAME		= WS_DIRSEC_LBA + 4	@ a name built from a directory entry
	WS_FOUND	= WS_NAME + 16		@ a copy of the entry a lookup found
	WS_COMP		= WS_FOUND + 32		@ one component of a path, 264 bytes
	WS_CHECKED	= WS_COMP + 264		@ when the medium was last verified
	WS_CMD		= WS_CHECKED + 4	@ a *command being built, 128 bytes
	WS_FOUND_SEC	= WS_CMD + 128		@ the sector WS_FOUND was read from
	WS_FOUND_OFF	= WS_FOUND_SEC + 4	@ and where in it the entry starts
	WS_PARENT	= WS_FOUND_OFF + 4	@ directory a failed lookup stopped in
	WS_LEAF_OK	= WS_PARENT + 4		@ non-zero if that was the last part
	WS_DISMOUNTED	= WS_LEAF_OK + 4	@ set by *MultiFSDismount until remounted

	@ Where a lookup's entry sits in its directory, counting every 32-byte
	@ slot from the start including the deleted ones and the long-name
	@ fragments - which is exactly how dir_entry_at counts. Deleting and
	@ renaming both have to walk BACKWARDS from the 8.3 entry over the
	@ fragments belonging to it, and a sector and an offset cannot do that on
	@ their own once the run crosses a sector boundary. An index can, because
	@ dir_entry_at will map any index back to a sector.
	WS_IT_INDEX	= WS_DISMOUNTED + 4	@ the iterator's count so far
	WS_FOUND_IDX	= WS_IT_INDEX + 4	@ the index a lookup settled on

	@ exFAT. A file there is either a chain through the FAT like FAT32 or a
	@ CONTIGUOUS run whose FAT entries are meaningless and must not be read -
	@ the "NoFatChain" flag in its stream entry says which. It has to travel
	@ with the object from the lookup that found it to whatever opens it.
	WS_FOUND_CONTIG	= WS_FOUND_IDX + 4
	WS_IT_CONTIG	= WS_FOUND_CONTIG + 4	@ the same, for the entry just returned
	WS_IT_DIRCONTIG	= WS_IT_CONTIG + 4	@ and for the directory being walked
	@ Where the entry set of the object just returned begins, and how many
	@ entries it has. Changing an exFAT object means rewriting the whole set,
	@ so a sector and an offset are not enough to find it again.
	WS_IT_SETIDX	= WS_IT_DIRCONTIG + 4
	WS_IT_SETCNT	= WS_IT_SETIDX + 4
	WS_FOUND_SETIDX	= WS_IT_SETCNT + 4
	WS_FOUND_SETCNT	= WS_FOUND_SETIDX + 4
	@ Non-zero asks exfat_create to claim and clear a cluster for a directory
	@ it makes. Making one wants that; RENAMING one does not - it already has
	@ its cluster and giving it another loses the contents.
	WS_X_ALLOC	= WS_FOUND_SETCNT + 4
	@ Non-zero while the volume in use may not be written to. Set from the
	@ volume's type every time one is chosen, and checked by write_sector
	@ itself - a backstop rather than a policy, so that a path nobody thought
	@ to guard still cannot put a byte on the medium.
	WS_READONLY	= WS_X_ALLOC + 4
	WS_XENT		= WS_READONLY + 4	@ an entry synthesised in FAT's shape, 32 bytes
	WS_XSTREAM	= WS_XENT + 32		@ the stream extension entry, 32 bytes
	@ An entry set being built: a File entry, a Stream Extension, and up to
	@ seventeen File Name entries for a 255-character name.
	WS_XSET		= WS_XSTREAM + 32
	WS_XSET_MAX	= 19
	WS_XEND		= WS_XSET + (WS_XSET_MAX * 32)

	@ How long a verified medium is taken on trust, in centiseconds. Short
	@ enough that pulling a stick out takes the icon away promptly, long
	@ enough that a directory listing does not re-read the boot record once
	@ per entry.
	MEDIA_CHECK_CS	= 50


	@ Fixed, and fixed at values an ADD can carry as an immediate: these two
	@ are added to wp constantly, and a chained offset that lands on an
	@ awkward number stops assembling.
	@ 4096 and 4608, like 3072 and 3584 before them, are values an ADD can
	@ carry as an immediate - which is the whole constraint on them. They were
	@ moved up when exFAT's fields filled the block below.
	WS_FILESEC	= 4096			@ a sector while copying file data

	@ The block above grows towards WS_FILESEC and there is very little room
	@ left. Say so at assembly time rather than letting a new field quietly
	@ start overwriting a sector buffer, which would corrupt the medium.
	.if WS_XEND > WS_FILESEC
	.error "MultiFS workspace has grown into WS_FILESEC - move WS_FILESEC and WS_FILES up, keeping both values ones an ADD can carry as an immediate"
	.endif

	WS_FILES	= 4608
	FH_USED		= 0
	FH_VOL		= 4
	FH_CLUSTER	= 8	@ first cluster, 0 while the file is still empty
	FH_SIZE		= 12	@ extent
	FH_DIRSEC	= 16	@ absolute LBA of the sector holding its 8.3 entry
	FH_DIROFF	= 20	@ and where in that sector the entry starts
	FH_FLAGS	= 24	@ FHF_* below
	@ exFAT: which entry set describes this file, and the directory it is in.
	FH_SETIDX	= 28
	FH_SETCNT	= 32
	FH_DIRCLUS	= 36
	FH_ENTRY	= 48

	FHF_WRITE	= 1 << 0	@ opened for writing
	FHF_DIRTY	= 1 << 1	@ the directory entry needs putting back
	@ exFAT only: the file is one contiguous run and its FAT entries mean
	@ nothing. Reading them gets whatever was last there.
	FHF_CONTIG	= 1 << 2
	MAX_OPEN	= 8
	WS_FILEEND	= WS_FILES + (MAX_OPEN * FH_ENTRY)

	@ Long file names. The state is a block of its own, addressed through one
	@ register, because these offsets are far too large to ride in an ADD.
	@ See lfn_take for how VFAT lays the fragments out.
	LFN_MAX_ORD	= 20		@ fragments VFAT allows for one name
	LFN_CHARS	= 13		@ characters each one carries
	LFN_MAX		= LFN_MAX_ORD * LFN_CHARS

	LN_CKSUM	= 0	@ the checksum the fragments agree on
	LN_EXPECT	= 4	@ the ordinal due next, 0 once the run is complete
	LN_OK		= 8	@ non-zero while the run has been consistent
	LN_COUNT	= 12	@ fragments the run says it has
	LN_TEXT		= 16	@ the name being assembled
	LN_SIZE		= LN_TEXT + LFN_MAX + 4

	WS_LFN		= WS_FILEEND
	WS_BUILT	= WS_LFN + LN_SIZE	@ the name handed back to callers
	WS_NEWENT	= WS_BUILT + LFN_MAX + 4	@ a 32-byte entry being built
	WS_FRAG		= WS_NEWENT + 32	@ a long-name fragment being built
	@ NTFS wants two buffers of its own and they are far too big to share with
	@ anything: a whole MFT record, and a whole index block.
	NTFS_MAX_REC	= 1024
	NTFS_MAX_IDX	= 4096
	WS_MFT		= WS_FRAG + 32
	WS_IDX		= WS_MFT + NTFS_MAX_REC

	@ Where the $MFT's own data runs are kept, so that any record can be
	@ found. The table is small on purpose: a Master File Table in more than
	@ sixteen pieces is a volume that wants defragmenting, and refusing is
	@ better than reading the wrong record.
	NTFS_MAX_RUNS	= 16
	WS_MFT_RUNS	= WS_IDX + NTFS_MAX_IDX	@ VCN, LCN, length: three words each
	WS_MFT_NRUNS	= WS_MFT_RUNS + (NTFS_MAX_RUNS * 12)

	@ The directory iterator's NTFS state.
	WS_N_PHASE	= WS_MFT_NRUNS + 4	@ 0 in the index root, 1 in an index block
	WS_N_OFF	= WS_N_PHASE + 4	@ where in the current node
	WS_N_END	= WS_N_OFF + 4		@ and where its entries stop
	WS_N_VCN	= WS_N_END + 4		@ which index block
	WS_N_NVCN	= WS_N_VCN + 4		@ how many there are
	WS_N_DIRREC	= WS_N_NVCN + 4		@ the directory's MFT record number
	WS_N_IRUNS	= WS_N_DIRREC + 4	@ the $INDEX_ALLOCATION run list
	WS_N_NIRUNS	= WS_N_IRUNS + (NTFS_MAX_RUNS * 12)
	WS_N_DATA	= WS_N_NIRUNS + 4	@ the run list of the file being read
	WS_N_NDATA	= WS_N_DATA + (NTFS_MAX_RUNS * 12)

	WS_SIZE		= WS_N_NDATA + 4

	@ Partition types worth looking at. Anything else in the table is skipped
	@ rather than probed, so a Linux or FileCore partition is left alone.
	PART_FAT12	= 0x01
	PART_FAT16_32M	= 0x04
	PART_FAT16	= 0x06
	PART_FAT32	= 0x0b
	PART_FAT32_LBA	= 0x0c
	PART_FAT16_LBA	= 0x0e
	@ What Windows puts on an exFAT partition - and on NTFS and HPFS, so the
	@ type byte only says "look", not "this is exFAT". The boot record decides.
	PART_IFS	= 0x07

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

	.string	"MultiFSFree"
	.align
	.int	command_free
	@ Nought or one parameter, "-r". Maximum lives in bits 16-23; see the note
	@ on MultiFSInfo below.
	.int	0x00010000
	.int	0
	.int	command_free_help

	.string	"MultiFSMount"
	.align
	.int	command_mount
	.int	0x00000000
	.int	0
	.int	command_dismount_help

	.string	"MultiFSDismount"
	.align
	.int	command_dismount
	.int	0x00000000
	.int	0
	.int	command_dismount_help

	.string	"MultiFSInfo"
	.align
	.int	command_info
	@ Nought or one parameter, the one being "-q".
	@
	@ The two halves of this word are the other way round from the obvious
	@ reading: the kernel takes the MINIMUM from bits 0-7 and the MAXIMUM from
	@ bits 16-23 (Kernel/s/Oscli: "MOV R6, R4, LSR #16 ; max no parms" then
	@ "AND R4, R4, #&FF ; min no parms"). Zero here meant max zero, so
	@ *MultiFSInfo -q was refused with "Invalid number of parameters" before
	@ any of our code ran - and &00000001 asks for a minimum of one and a
	@ maximum of none, which can never be satisfied at all.
	.int	0x00010000
	.int	0
	.int	command_info_help

	.string	"MultiFSWriteTest"
	.align
	.int	command_writetest
	.int	0x00000000
	.int	0
	.int	command_alloctest_help

	.string	"MultiFSMakeTest"
	.align
	.int	command_maketest
	.int	0x00000000
	.int	0
	.int	command_alloctest_help

	.string	"MultiFSAllocTest"
	.align
	.int	command_alloctest
	.int	0x00000000
	.int	0
	.int	command_alloctest_help

	.byte	0	@ Table terminator

	.align

command_multifs_help:
	.string	"*MultiFS selects the MultiFS filing system\rSyntax: *MultiFS"
	.align

command_discs_help:
	.string	"*MultiFSDiscs lists the FAT volumes MultiFS can see\rSyntax: *MultiFSDiscs"
	.align

command_free_help:
	.string	"*MultiFSFree reports free space as MultiFS counts it, before FileSwitch and the Free module have had a hand in it\rSyntax: *MultiFSFree"
	.align

command_dismount_help:
	.string	"*MultiFSDismount forgets the disc so the stick can be taken out safely\rSyntax: *MultiFSDismount"
	.align

command_info_help:
	.string	"*MultiFSInfo sets MultiFS$Format, $Label, $Drive, $Cluster, $Size, $Free and $Used to describe the disc\rSyntax: *MultiFSInfo"
	.align

command_alloctest_help:
	.string	"*MultiFSAllocTest takes one cluster and gives it straight back, to prove the write path without putting a file at risk\rSyntax: *MultiFSAllocTest"
	.align


	@ Filing System Information Block
fs_info_offset:
	.int	fs_info_block - module_start

fs_info_block:
	.int	fs_name		@ Filing System name
	.int	fs_text		@ Filing System startup text
	.int	fs_open		@ To Open files (FSEntry_Open)
	.int	fs_getbytes	@ To Get Bytes (FSEntry_GetBytes)
	.int	fs_putbytes	@ To Put Bytes (FSEntry_PutBytes)
	.int	fs_args		@ To Control open files (FSEntry_Args)
	.int	fs_close	@ To Close open files (FSEntry_Close)
	.int	fs_file		@ To perform whole-file ops (FSEntry_File)
	.int	FILING_SYSTEM_NUMBER | (MAX_OPEN_FILES << 8) | FS_LOAD_BY_STREAM | FS_SAVE_BY_STREAM
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
	adrl	r1, module_start
	ldr	r2, fs_info_offset
	mov	r3, wp
	swi	XOS_FSControl
	bvs	init_free_and_out

	@ Register with the Free module, which is what *ShowFree and the filer's
	@ Free menu entry ask. Without this they answer "Unknown filing system"
	@ however well FSEntry_Func 30 works. It is not worth failing to load
	@ over, so the error is dropped: everything else still works.
	mov	r0, #FILING_SYSTEM_NUMBER
	adrl	r1, free_routine
	mov	r2, wp
	swi	XFree_Register

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

	mov	r0, #FILING_SYSTEM_NUMBER
	adrl	r1, free_routine
	mov	r2, wp
	swi	XFree_DeRegister

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
	 *
	 * Note the plain MOV on the way out. MOVS PC, LR is the 26-bit idiom and
	 * writes the SPSR back into the CPSR; in a 32-bit module it hands the
	 * kernel back a processor mode it did not ask for. This path is taken by
	 * every service call the machine broadcasts, so getting it wrong is not
	 * a corner case.
	 */
service:
	teq	r1, #Service_FSRedeclare
	movne	pc, lr

	stmfd	sp!, {r0-r3, lr}
	mov	r0, #FSControl_AddFS
	adrl	r1, module_start
	ldr	r2, fs_info_offset
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
	mov	r0, r4
	bl	format_name
	swi	XOS_Write0

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
	.string	"    "
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


	/* *MultiFSFree - free space as MultiFS itself counts it.
	 *
	 * The same numbers reach the desktop through FileSwitch and then the Free
	 * module, and each of those can present them wrongly on its own. This says
	 * what MultiFS handed over, so the two halves can be told apart.
	 */
command_free:
	stmfd	sp!, {r0-r8, lr}
	ldr	wp, [wp]

	@ "-r" recounts from the FAT instead of believing anything cached.
	@
	@ Worth having as a command of its own. The count kept in memory is only
	@ ever as good as what it started from, and what it starts from is the
	@ FSInfo sector - which any system that wrote to this stick without
	@ maintaining it, ours included before this was fixed, will have left
	@ describing the disc as it used to be. This walks the whole FAT, which
	@ takes a while on a large stick, and writes the true figure back to the
	@ medium so nothing has to do it again.
	mov	r5, #0
	cmp	r1, #0
	beq	command_free_go
	ldrb	r2, [r0]
	cmp	r2, #'-'
	bne	command_free_go
	ldrb	r2, [r0, #1]
	orr	r2, r2, #0x20
	cmp	r2, #'r'
	moveq	r5, #1

command_free_go:
	bl	current_volume
	bvs	free_cmd_out
	mov	r8, r0

	cmp	r5, #0
	mvnne	r0, #0			@ forget the cached count
	strne	r0, [r8, #VOL_FREE]
	movne	r0, #1			@ and do not believe the FSInfo either
	strne	r0, [r8, #VOL_FSDIRTY]

	adr	r0, fmsg_clusters
	swi	XOS_Write0
	ldr	r0, [r8, #VOL_CLUSTERS]
	bl	print_cardinal

	adr	r0, fmsg_percluster
	swi	XOS_Write0
	mov	r0, r8
	bl	cluster_bytes
	mov	r7, r0
	bl	print_cardinal

	adr	r0, fmsg_free
	swi	XOS_Write0
	mov	r0, r8
	bl	volume_free_clusters
	bvs	free_cmd_out
	mov	r6, r0
	bl	print_cardinal

	adr	r0, fmsg_totalbytes
	swi	XOS_Write0
	ldr	r0, [r8, #VOL_CLUSTERS]
	mov	r1, r7
	bl	mul64
	mov	r5, r1
	bl	print_cardinal		@ low word
	adr	r0, fmsg_high
	swi	XOS_Write0
	mov	r0, r5
	bl	print_cardinal

	adr	r0, fmsg_freebytes
	swi	XOS_Write0
	mov	r0, r6
	mov	r1, r7
	bl	mul64
	mov	r5, r1
	bl	print_cardinal
	adr	r0, fmsg_high
	swi	XOS_Write0
	mov	r0, r5
	bl	print_cardinal
	swi	XOS_NewLine

	cmp	pc, #0
free_cmd_out:
	ldmfd	sp!, {r0-r8, pc}

fmsg_clusters:
	.string	"Clusters: "
	.align
fmsg_percluster:
	.string	"  bytes per cluster: "
	.align
fmsg_free:
	.string	"\nFree clusters: "
	.align
fmsg_totalbytes:
	.string	"\nTotal bytes: low "
	.align
fmsg_freebytes:
	.string	"\nFree bytes:  low "
	.align
fmsg_high:
	.string	"  high "
	.align


	/* *MultiFSMount - undo a dismount without unplugging anything. */
command_mount:
	stmfd	sp!, {r0-r1, lr}
	ldr	wp, [wp]
	mov	r0, #0
	ldr	r1, =WS_DISMOUNTED
	str	r0, [wp, r1]
	str	r0, [wp, #WS_NVOLUMES]
	adrl	r0, mmsg_done
	swi	XOS_Write0
	swi	XOS_NewLine
	cmp	pc, #0
	ldmfd	sp!, {r0-r1, pc}

mmsg_done:
	.string	"USB disc mounted again"
	.align


	/* *MultiFSDismount - forget the disc.
	 *
	 * There is nothing to flush: every write this module makes has already
	 * reached the medium by the time the call returns, so dismounting is
	 * simply forgetting - the volume table is emptied and the icon goes with
	 * it. Putting the stick back, or asking for it again, rescans and finds
	 * it as if it had just been plugged in.
	 */
command_dismount:
	stmfd	sp!, {r0-r2, lr}
	ldr	wp, [wp]

	mov	r0, #0
	str	r0, [wp, #WS_NVOLUMES]
	add	r1, wp, #WS_VOLUMES
	mov	r2, #MAX_VOLUMES
command_dismount_clear:
	str	r0, [r1, #VOL_TYPE]
	add	r1, r1, #VOL_SIZE
	subs	r2, r2, #1
	bne	command_dismount_clear

	@ Nothing is cached about it either: the next look starts from scratch.
	str	r0, [wp, #WS_FATSEC_LBA]
	ldr	r1, =WS_CHECKED
	str	r0, [wp, r1]

	mov	r0, #1
	ldr	r1, =WS_DISMOUNTED
	str	r0, [wp, r1]

	adrl	r0, dmsg_done
	swi	XOS_Write0
	swi	XOS_NewLine
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, pc}

dmsg_done:
	.string	"USB disc dismounted - it is safe to remove"
	.align


	/* *MultiFSInfo - publish what this volume is, as system variables.
	 *
	 * The filer wants these to fill in its Disc info window, and system
	 * variables are how one module hands text to another without either of
	 * them needing a SWI chunk allocated to it. They are set with *Set
	 * through OS_CLI, the same way our own HostFS defines Boot$Dir, so there
	 * is nothing here that cannot be inspected by hand with *Show MultiFS$*.
	 *
	 * Sizes are in megabytes: a byte count for a stick of any size does not
	 * fit in the 32 bits OS_ConvertCardinal4 formats.
	 */
command_info:
	stmfd	sp!, {r0-r9, lr}
	ldr	wp, [wp]

	@ "-q" sets the variables and says nothing. MultiFSFiler needs the numbers
	@ to fill its Disc info window and is a Wimp task, which must not write to
	@ the screen: doing it aloud from a task scribbles over whatever the
	@ desktop had there. R0 is the command tail on entry.
	mov	r4, #0			@ non-zero once we are to keep quiet
	cmp	r1, #0
	beq	command_info_go
	ldrb	r2, [r0]
	cmp	r2, #'-'
	bne	command_info_go
	ldrb	r2, [r0, #1]
	orr	r2, r2, #0x20
	cmp	r2, #'q'
	moveq	r4, #1

command_info_go:
	bl	current_volume
	bvs	info_out
	mov	r9, r0

	@ Filing system, as a word rather than a number.
	mov	r0, r9
	bl	format_name
	mov	r1, r0
	adr	r0, info_var_format
	bl	set_var_text

	@ Disc name, as RISC OS sees it - which is the sanitised one.
	add	r1, r9, #VOL_LABEL
	adr	r0, info_var_label
	bl	set_var_text

	adr	r0, info_var_drive
	ldr	r1, [r9, #VOL_DRIVE]
	bl	set_var_number

	adr	r0, info_var_cluster
	mov	r0, r9
	bl	cluster_bytes
	mov	r8, r0			@ bytes per cluster, wanted again below
	mov	r1, r0
	adr	r0, info_var_cluster
	bl	set_var_number

	@ Capacity, free and used, in megabytes.
	ldr	r0, [r9, #VOL_CLUSTERS]
	mov	r1, r8
	bl	mul64
	bl	bytes_to_mb
	mov	r7, r0			@ capacity
	mov	r1, r0
	adr	r0, info_var_size
	bl	set_var_number

	mov	r0, r9
	bl	volume_free_clusters
	movvs	r0, #0
	mov	r1, r8
	bl	mul64
	bl	bytes_to_mb
	mov	r6, r0			@ free
	mov	r1, r0
	adr	r0, info_var_free
	bl	set_var_number

	sub	r1, r7, r6
	adr	r0, info_var_used
	bl	set_var_number

	@ And say it aloud as well as setting the variables, so the command is
	@ worth typing on its own rather than only being plumbing for the filer.
	sub	r5, r7, r6		@ used

	cmp	r4, #0
	beq	info_aloud
	cmp	pc, #0			@ V clear: the variables are set, quietly
	ldmfd	sp!, {r0-r9, pc}

info_aloud:
	adrl	r0, imsg_format
	swi	XOS_Write0
	mov	r0, r9
	bl	format_name
	swi	XOS_Write0

	adrl	r0, imsg_name
	swi	XOS_Write0
	add	r0, r9, #VOL_LABEL
	swi	XOS_Write0

	adrl	r0, imsg_drive
	swi	XOS_Write0
	ldr	r0, [r9, #VOL_DRIVE]
	bl	print_cardinal

	adrl	r0, imsg_size
	swi	XOS_Write0
	mov	r0, r7
	bl	print_cardinal
	adrl	r0, imsg_mb
	swi	XOS_Write0

	adrl	r0, imsg_free
	swi	XOS_Write0
	mov	r0, r6
	bl	print_cardinal
	adrl	r0, imsg_mb
	swi	XOS_Write0

	adrl	r0, imsg_used
	swi	XOS_Write0
	mov	r0, r5
	bl	print_cardinal
	adrl	r0, imsg_mb
	swi	XOS_Write0

	adrl	r0, imsg_cluster
	swi	XOS_Write0
	mov	r0, r8
	bl	print_cardinal
	adrl	r0, imsg_bytes
	swi	XOS_Write0
	swi	XOS_NewLine

	cmp	pc, #0
info_out:
	ldmfd	sp!, {r0-r9, pc}


	/* A 64-bit byte count in R0/R1, as whole megabytes in R0. */
bytes_to_mb:
	mov	r0, r0, lsr #20
	orr	r0, r0, r1, lsl #12
	mov	pc, lr


	/* *Set <name in R0> <text in R1>. */
set_var_text:
	stmfd	sp!, {r0-r4, lr}
	mov	r3, r0
	mov	r4, r1
	ldr	r0, =WS_CMD
	add	r0, wp, r0
	adr	r1, info_set
	bl	append_string
	mov	r1, r3
	bl	append_string
	adr	r1, info_space
	bl	append_string
	mov	r1, r4
	bl	append_string
	mov	r1, #0
	strb	r1, [r0]
	ldr	r0, =WS_CMD
	add	r0, wp, r0
	swi	XOS_CLI
	ldmfd	sp!, {r0-r4, pc}


	/* *Set <name in R0> <number in R1>. */
set_var_number:
	stmfd	sp!, {r0-r4, lr}
	mov	r3, r0
	sub	sp, sp, #16
	mov	r0, r1
	mov	r1, sp
	mov	r2, #16
	swi	XOS_ConvertCardinal4
	mov	r4, r0			@ the formatted digits
	mov	r0, r3
	mov	r1, r4
	bl	set_var_text
	add	sp, sp, #16
	ldmfd	sp!, {r0-r4, pc}


	/* Append the string at R1 to the one at R0, leaving R0 on its
	 * terminator so the next piece carries straight on. */
append_string:
	stmfd	sp!, {r1-r2, lr}
append_string_loop:
	ldrb	r2, [r1], #1
	cmp	r2, #0
	beq	append_string_out
	strb	r2, [r0], #1
	b	append_string_loop
append_string_out:
	ldmfd	sp!, {r1-r2, pc}

imsg_format:
	.string	"Filing system: "
	.align
imsg_name:
	.string	"\nDisc name:     "
	.align
imsg_drive:
	.string	"\nDrive:         "
	.align
imsg_size:
	.string	"\nCapacity:      "
	.align
imsg_free:
	.string	"\nFree:          "
	.align
imsg_used:
	.string	"\nUsed:          "
	.align
imsg_cluster:
	.string	"\nCluster size:  "
	.align
imsg_mb:
	.string	"M"
	.align
imsg_bytes:
	.string	" bytes"
	.align

info_set:
	.string	"Set "
	.align
info_space:
	.string	" "
	.align
	/* What to call this volume's format. Entry: R0 = volume. Exit: R0 = text. */
format_name:
	ldr	r0, [r0, #VOL_TYPE]
	cmp	r0, #128
	adreq	r0, info_ntfs
	moveq	pc, lr
	cmp	r0, #64
	adreq	r0, info_exfat
	moveq	pc, lr
	cmp	r0, #32
	adreq	r0, info_fat32
	moveq	pc, lr
	cmp	r0, #16
	adreq	r0, info_fat16
	adrne	r0, info_fat12
	mov	pc, lr

info_ntfs:
	.string	"NTFS"
	.align
info_exfat:
	.string	"exFAT"
	.align
info_fat32:
	.string	"FAT32"
	.align
info_fat12:
	.string	"FAT12"
	.align
info_fat16:
	.string	"FAT16"
	.align
info_var_format:
	.string	"MultiFS$Format"
	.align
info_var_label:
	.string	"MultiFS$Label"
	.align
info_var_drive:
	.string	"MultiFS$Drive"
	.align
info_var_cluster:
	.string	"MultiFS$Cluster"
	.align
info_var_size:
	.string	"MultiFS$Size"
	.align
info_var_free:
	.string	"MultiFS$Free"
	.align
info_var_used:
	.string	"MultiFS$Used"
	.align


	/* *MultiFSWriteTest - write over the start of a file that already exists.
	 *
	 * Goes the whole way round through FileSwitch - OS_Find to open for
	 * update, OS_GBPB to write, OS_Find to close - rather than calling this
	 * module's own routines, because the point is to prove the entry points
	 * as RISC OS drives them and not just the arithmetic underneath.
	 */
command_writetest:
	stmfd	sp!, {r0-r6, lr}
	ldr	wp, [wp]

	mov	r0, #0xc0		@ open an existing file for update
	adrl	r1, wt_path
	swi	XOS_Find
	bvs	writetest_out
	cmp	r0, #0
	beq	writetest_none
	mov	r6, r0

	mov	r1, r6
	mov	r0, #1			@ OS_GBPB 1: write at the given offset
	adrl	r2, wt_data
	mov	r3, #(wt_data_end - wt_data)
	mov	r4, #0
	swi	XOS_GBPB
	stmfd	sp!, {r0}
	mov	r0, #0			@ close, whatever happened
	mov	r1, r6
	swi	XOS_Find
	ldmfd	sp!, {r0}
	bvs	writetest_out

	adrl	r0, wt_done
	swi	XOS_Write0
	swi	XOS_NewLine
	cmp	pc, #0
	ldmfd	sp!, {r0-r6, pc}

writetest_none:
	adrl	r0, wt_missing
	swi	XOS_Write0
	swi	XOS_NewLine
	cmp	pc, #0

writetest_out:
	ldmfd	sp!, {r0-r6, pc}

wt_path:
	.string	"MultiFS::0.$.WRITEME"
	.align
wt_data:
	.ascii	"MultiFS wrote this."
wt_data_end:
	.byte	0
	.align
wt_done:
	.string	"Wrote over the start of MultiFS::0.$.WRITEME"
	.align
wt_missing:
	.string	"MultiFS::0.$.WRITEME does not exist - make it on the host first"
	.align


	/* *MultiFSMakeTest - make a directory entry directly, with no FileSwitch.
	 *
	 * Creating a file through FileSwitch failed with a message of its own,
	 * which says nothing about which half is wrong. This calls dir_create
	 * straight, so the answer is either "the entry writer is broken" or "the
	 * entry writer is fine and the plumbing above it is not".
	 */
command_maketest:
	stmfd	sp!, {r0-r8, lr}
	ldr	wp, [wp]

	bl	current_volume
	bvs	maketest_err
	mov	r8, r0

	mov	r0, r8
	mov	r1, #0			@ the root
	adrl	r2, mk_name
	mov	r3, #0x20
	bl	dir_create
	bvs	maketest_err

	mov	r7, r0
	adrl	r0, mk_ok
	swi	XOS_Write0
	mov	r0, r7
	bl	print_cardinal
	swi	XOS_NewLine
	cmp	pc, #0
	ldmfd	sp!, {r0-r8, pc}

maketest_err:
	mov	r7, r0
	adrl	r0, mk_bad
	swi	XOS_Write0
	mov	r0, r7
	cmp	r0, #0
	beq	maketest_out
	add	r0, r0, #4
	swi	XOS_Write0
maketest_out:
	swi	XOS_NewLine
	cmp	pc, #0
	ldmfd	sp!, {r0-r8, pc}

mk_name:
	.string	"MakeTest/txt"
	.align
mk_ok:
	.string	"Entry written, in sector "
	.align
mk_bad:
	.string	"dir_create failed: "
	.align


	/* *MultiFSAllocTest - take one cluster and give it straight back.
	 *
	 * The smallest thing that proves sectors can be written at all: it
	 * allocates a cluster, reads the FAT entry back to check the write
	 * landed, frees it again and checks it reads as free. Nothing that any
	 * file depends on is touched, so a failure here costs nothing, which is
	 * the point of having it before anything is built on top.
	 */
command_alloctest:
	stmfd	sp!, {r0-r8, lr}
	ldr	wp, [wp]

	bl	current_volume
	bvs	alloctest_out
	mov	r8, r0

	adr	r0, amsg_before
	swi	XOS_Write0
	mov	r0, r8
	bl	volume_free_clusters
	bvs	alloctest_out
	bl	print_cardinal

	mov	r0, r8
	bl	alloc_cluster
	bvs	alloctest_out
	mov	r7, r0

	adr	r0, amsg_took
	swi	XOS_Write0
	mov	r0, r7
	bl	print_cardinal

	@ Did the write actually land? Read it back rather than assume.
	mov	r0, r8
	mov	r1, r7
	bl	fat_raw
	bvs	alloctest_out
	mov	r6, r0			@ kept: the message pointer below needs R0
	adr	r0, amsg_reads
	swi	XOS_Write0
	mov	r0, r6
	bl	print_cardinal

	mov	r0, r8
	mov	r1, r7
	bl	free_chain
	bvs	alloctest_out

	mov	r0, r8
	mov	r1, r7
	bl	fat_raw
	bvs	alloctest_out
	mov	r6, r0			@ kept: the message pointer below needs R0
	adr	r0, amsg_after
	swi	XOS_Write0
	mov	r0, r6
	bl	print_cardinal

	adr	r0, amsg_free
	swi	XOS_Write0
	mov	r0, r8
	bl	volume_free_clusters
	bvs	alloctest_out
	bl	print_cardinal
	swi	XOS_NewLine

	cmp	pc, #0
alloctest_out:
	ldmfd	sp!, {r0-r8, pc}

amsg_before:
	.string	"Free clusters before: "
	.align
amsg_took:
	.string	"\nAllocated cluster: "
	.align
amsg_reads:
	.string	"\nIts FAT entry now reads: "
	.align
amsg_after:
	.string	"\nAfter freeing it reads: "
	.align
amsg_free:
	.string	"\nFree clusters after: "
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
	mov	r2, #0			@ a root is never a contiguous run
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
	bl	build_entry_name
	swi	XOS_Write0

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
	adrl	r0, msg_no_discs
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


	/* The name to show for a directory entry.
	 *
	 * Entry: R0 = the 8.3 directory entry.
	 * Exit:  R0 = a NUL terminated name.
	 *
	 * The long name when the disc carries a sound one, the 8.3 name when it
	 * does not. Both end up in the same buffer so that a caller has one thing
	 * to hold on to, and the gathered state is cleared afterwards: the next
	 * 8.3 entry along has no claim on this one's fragments.
	 */
build_entry_name:
	stmfd	sp!, {r1-r5, lr}

	mov	r5, r0
	ldr	r4, =WS_BUILT
	add	r4, wp, r4

	@ On exFAT and NTFS the name is already there: the reader put it in
	@ WS_BUILT as it read the entry. There is nothing to assemble and nothing
	@ to fall back on - neither has a short name - so hand it straight back.
	ldr	r0, [wp, #WS_IT_REC]
	ldr	r0, [r0, #VOL_TYPE]
	cmp	r0, #64
	cmpne	r0, #128
	moveq	r0, r4
	ldmeqfd	sp!, {r1-r5, pc}

	mov	r0, r5
	bl	lfn_name
	cmp	r0, #0
	bne	build_entry_copy

	mov	r0, r5
	bl	build_short_name

build_entry_copy:
	mov	r1, #0
build_entry_loop:
	ldrb	r2, [r0, r1]
	strb	r2, [r4, r1]
	cmp	r2, #0
	beq	build_entry_done
	add	r1, r1, #1
	cmp	r1, #LFN_MAX
	blo	build_entry_loop
	mov	r2, #0
	strb	r2, [r4, r1]

build_entry_done:
	bl	lfn_reset
	mov	r0, r4
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
	adrl	r0, msg_drive
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
	adrl	r0, msg_drive
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


	/* Write one 512-byte sector.
	 *
	 * Entry: R0 = drive, R1 = sector (LBA), R2 = buffer.
	 * Exit:  V set on failure.
	 *
	 * The read side's twin, and everything in its header applies here too:
	 * the drive rides in the top three bits of R2's disc address, R5 and R6
	 * must be zero, and the address counts sectors because this is
	 * SectorDiscOp rather than DiscOp. Reason 2 is write where 1 is read.
	 *
	 * There is no retry. A read that fails can be tried again harmlessly; a
	 * write that fails may have put down part of a sector, and repeating it
	 * blindly is how half-written data becomes confidently-wrong data.
	 */
write_sector:
	stmfd	sp!, {r1-r10, lr}

	@ The backstop. Every other guard is about giving a decent message; this
	@ one is about the medium. Without it the FAT entry points wrote FAT
	@ structures onto an NTFS volume and reported success - *CDir, *Delete
	@ and *Rename all "worked", and the volume needed chkdsk afterwards.
	@
	@ R3, NOT R1. R1 is the SECTOR NUMBER on entry here, and the first
	@ version of this loaded the flag into it - so every write on a perfectly
	@ writable volume went to sector zero instead, and took the partition
	@ table with it. Twice.
	ldr	r3, =WS_READONLY
	ldr	r3, [wp, r3]
	cmp	r3, #0
	bne	write_sector_readonly

	cmp	r1, #(1 << 29)
	bcs	write_sector_too_far

	mov	r3, r2			@ the buffer, before R2 becomes the address
	mov	r2, r0, lsl #29		@ drive in the top three bits...
	orr	r2, r2, r1		@ ...and the sector number below it
	mov	r1, #2			@ reason: write sectors
	mov	r4, #512
	mov	r5, #0
	mov	r6, #0
	swi	XSCSIFS_SectorDiscOp
	bvs	write_sector_out

	@ R4 comes back as the number of bytes NOT transferred.
	cmp	r4, #0
	bne	write_sector_short

write_sector_out:
	ldmfd	sp!, {r1-r10, pc}

write_sector_too_far:
	adr	r0, err_too_far
	b	write_sector_setv

write_sector_short:
	adr	r0, err_short_write

write_sector_setv:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r10, pc}

write_sector_readonly:
	adrl	r0, err_readonly_fs
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r10, pc}

err_readonly_fs:
	.int	0xc9
	.string	"MultiFS can read NTFS but not write it"
	.align

err_short_write:
	.int	0
	.string	"MultiFS wrote less than a whole sector"
	.align

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
	teqne	r0, #PART_IFS
	@ TEQ leaves Z set when one matched, so invert for the caller's sense.
	moveq	r0, #1
	movne	r0, #0
	teq	r0, #0
	mov	pc, lr


	/* Do these bytes match?
	 *
	 * Entry: R0 = one, R1 = the other, R2 = how many.
	 * Exit:  Z set if they do.
	 */
mem_equal:
	stmfd	sp!, {r0-r5, lr}
	mov	r3, #0
mem_equal_loop:
	cmp	r3, r2
	beq	mem_equal_yes
	ldrb	r4, [r0, r3]
	ldrb	r5, [r1, r3]
	cmp	r4, r5
	bne	mem_equal_out
	add	r3, r3, #1
	b	mem_equal_loop
mem_equal_yes:
	cmp	r0, r0			@ Z
mem_equal_out:
	ldmfd	sp!, {r0-r5, pc}

exfat_name:
	.ascii	"EXFAT   "
	.align


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

	@ exFAT says so in the eight bytes at offset 3, and shares nothing else
	@ with the BPB below - no bytes per sector at 11, no sectors per cluster
	@ at 13, no reserved count at 14. Reading those from an exFAT boot record
	@ gets zeros and nonsense, so the test has to come first.
	add	r0, wp, #WS_SECTOR
	add	r0, r0, #3		@ the name sits after the three jump bytes
	adrl	r1, exfat_name
	mov	r2, #8
	bl	mem_equal
	beq	add_volume_exfat

	@ NTFS says so in the same eight bytes, and shares no more of the BPB
	@ than exFAT does.
	add	r0, wp, #WS_SECTOR
	add	r0, r0, #3
	adrl	r1, ntfs_name
	mov	r2, #8
	bl	mem_equal
	beq	add_volume_ntfs

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

	mov	r0, #0			@ no FSInfo sector unless FAT32 says otherwise
	str	r0, [lr, #VOL_FSINFO]
	mvn	r0, #0			@ free clusters: not counted yet
	str	r0, [lr, #VOL_FREE]
	mov	r0, #2			@ allocation starts at the first real cluster
	str	r0, [lr, #VOL_NEXTFREE]
	mov	r0, #0
	str	r0, [lr, #VOL_FSDIRTY]

	ldr	r0, [lr, #VOL_TYPE]
	cmp	r0, #32
	bne	add_volume_label

	add	r0, wp, #WS_SECTOR
	mov	r1, #44
	bl	ld32			@ costs us LR, so fetch the record again
	ldr	lr, [wp, #WS_REC]
	str	r0, [lr, #VOL_ROOTCLUS]

	@ The FSInfo sector, which carries a running count of free clusters. It is
	@ only a hint - the specification allows it to be stale or absent - so it
	@ is checked before it is believed, but when it is good it saves reading
	@ the whole of a FAT that on a large stick runs to several megabytes.
	add	r0, wp, #WS_SECTOR
	mov	r1, #48
	bl	ld16
	ldr	lr, [wp, #WS_REC]
	cmp	r0, #0
	ldrne	r1, [lr, #VOL_START]
	addne	r0, r0, r1		@ relative to the volume, not the disc
	str	r0, [lr, #VOL_FSINFO]

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

	add	r0, r5, #VOL_LABEL
	bl	label_legalise

copy_label_out:
	ldmfd	sp!, {r0-r5, pc}


	/* Make a volume label into a name RISC OS can put in a path.
	 *
	 * Entry: R0 = the label, terminated, edited in place.
	 *
	 * FAT and exFAT labels may hold spaces and characters that mean something
	@ else entirely here - a space ends a filename argument, a dot separates
	@ directories, and the rest are wildcards or field separators. A disc
	@ called "USB STICK" therefore canonicalises to a path nothing can parse
	@ back: opening a subdirectory in a Filer window silently listed the root,
	@ because the path fell apart at the space long before it reached this
	@ module. Everything awkward becomes an underscore, so the disc shows as
	@ USB_STICK and its paths hold together.
	 */
label_legalise:
	stmfd	sp!, {r0-r5, lr}
	mov	r1, r0
	mov	r2, #0
copy_label_legal:
	ldrb	r3, [r1, r2]
	cmp	r3, #0
	beq	label_legalise_out
	cmp	r3, #' '		@ space and anything below it
	bls	copy_label_swap
	adr	r4, label_awkward
copy_label_scan:
	ldrb	r5, [r4], #1
	cmp	r5, #0
	beq	copy_label_next
	cmp	r5, r3
	bne	copy_label_scan
copy_label_swap:
	mov	r3, #'_'
	strb	r3, [r1, r2]
copy_label_next:
	add	r2, r2, #1
	b	copy_label_legal

label_legalise_out:
	ldmfd	sp!, {r0-r5, pc}

	@ The characters RISC OS reads as something other than part of a name.
label_awkward:
	.string	".:*#$&@^%\\\"|"
	.align


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

	@ FAT12 entries are a byte and a half and do not divide into a sector, so
	@ none of the arithmetic below fits them. See fat12_get.
	cmp	r2, #12
	beq	fat_next_12

	cmp	r2, #16
	movhi	r3, r1, lsl #2		@ FAT32 and exFAT: four bytes an entry
	movls	r3, r1, lsl #1		@ FAT16: two

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
	cmp	r2, #64
	beq	fat_next_exfat
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
	b	fat_next_ok

	@ Placed here, between one block that ends in a branch and another that
	@ begins with a label, because fat_next_exfat below FALLS THROUGH into
	@ fat_next_ok. Anything put between those two silently becomes part of
	@ the exFAT path.
fat_next_12:
	mov	r0, r7
	bl	fat12_get
	bvs	fat_next_out
	ldr	r1, =0xff8
	cmp	r0, r1
	movhs	r0, #0			@ end of chain
	b	fat_next_ok

	@ exFAT uses all thirty-two bits - there is no "reserved top nibble" to
	@ mask off - and its end marker is &FFFFFFFF exactly. Masking as FAT32
	@ does turns that into &0FFFFFFF, which is a perfectly good cluster
	@ number, and the walk carries on into whatever is there.
fat_next_exfat:
	bl	ld32
	ldr	r1, =0xfffffff7
	cmp	r0, r1
	movhs	r0, #0			@ end of chain, or a bad cluster
	cmp	r0, #1
	movls	r0, #0			@ 0 and 1 are not chain links either

fat_next_ok:
	cmp	pc, #0			@ clear V

fat_next_out:
	ldmfd	sp!, {r1-r7, pc}

	.ltorg


	/* Write one FAT entry, to every copy of the FAT.
	 *
	 * Entry: R0 = volume record, R1 = cluster, R2 = value.
	 * Exit:  V set on failure.
	 *
	 * Both FATs, not just the first. They are redundant copies and every
	 * other operating system that touches the volume is entitled to read
	 * either one; leaving the second stale is the sort of damage that shows
	 * up months later on somebody else's machine.
	 *
	 * The cached FAT sector is invalidated afterwards, because the copy in
	 * WS_FATSEC no longer matches the medium.
	 */
fat_set:
	stmfd	sp!, {r0-r10, lr}

	mov	r7, r0			@ volume record
	mov	r8, r1			@ cluster
	mov	r9, r2			@ value

	ldr	r0, [r7, #VOL_TYPE]

	@ FAT12 has no whole number of bytes to shift by, and the byte it shares
	@ with the entry next door has to be read before it is written. See
	@ fat12_put.
	cmp	r0, #12
	beq	fat_set_12

	cmp	r0, #16
	movhi	r10, #2			@ log2 of the entry width: FAT32 and exFAT
	movls	r10, #1

	@ Where the entry sits: which sector of the first FAT, and where in it.
	mov	r0, r8, lsl r10
	mov	r5, r0, lsr #9		@ sector within a FAT
	ldr	r6, =511
	and	r6, r0, r6		@ offset within that sector

	ldr	r4, [r7, #VOL_NFATS]
	mov	r3, #0			@ which FAT copy

fat_set_copy:
	ldr	r0, [r7, #VOL_FATSEC0]
	ldr	r1, [r7, #VOL_FATSZ]
	mla	r0, r3, r1, r0		@ this copy starts a whole FAT further on
	add	r0, r0, r5
	mov	r1, r0			@ the sector to read, change and put back
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	stmfd	sp!, {r1}
	bl	read_sector
	ldmfd	sp!, {r1}
	bvs	fat_set_out

	@ Change just this entry, leaving the rest of the sector as it was.
	add	r0, wp, #WS_SECTOR
	add	r0, r0, r6
	cmp	r10, #2
	beq	fat_set_32

	strb	r9, [r0]
	mov	r2, r9, lsr #8
	strb	r2, [r0, #1]
	b	fat_set_put

	@ Here rather than beside fat_set_out, because the copy loop above ends by
	@ FALLING THROUGH into it: a block placed there would be entered after
	@ every successful FAT16, FAT32 and exFAT write, and would then write the
	@ entry again as twelve bits.
fat_set_12:
	mov	r0, r7
	mov	r1, r8
	mov	r2, r9
	bl	fat12_put
	b	fat_set_out

fat_set_32:
	@ On exFAT every one of the thirty-two bits is ours, and the end marker
	@ is &FFFFFFFF exactly. FAT32's reserved top nibble must NOT be preserved
	@ here: keeping it turned that marker into &0FFFFFFF, which exFAT reads as
	@ a perfectly ordinary cluster number, and fsck called every file we wrote
	@ a broken chain.
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	beq	fat_set_exfat

	@ The top four bits belong to whoever wrote the volume, so they are kept
	@ rather than overwritten with ours.
	ldrb	r2, [r0, #3]
	and	r2, r2, #0xf0
	strb	r9, [r0]
	mov	r4, r9, lsr #8
	strb	r4, [r0, #1]
	mov	r4, r9, lsr #16
	strb	r4, [r0, #2]
	mov	r4, r9, lsr #24
	and	r4, r4, #0x0f
	orr	r4, r4, r2
	strb	r4, [r0, #3]
	ldr	r4, [r7, #VOL_NFATS]	@ reloaded: the shifts above used R4
	b	fat_set_put

fat_set_exfat:
	strb	r9, [r0]
	mov	r4, r9, lsr #8
	strb	r4, [r0, #1]
	mov	r4, r9, lsr #16
	strb	r4, [r0, #2]
	mov	r4, r9, lsr #24
	strb	r4, [r0, #3]
	ldr	r4, [r7, #VOL_NFATS]

fat_set_put:
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	write_sector
	bvs	fat_set_out

	add	r3, r3, #1
	cmp	r3, r4
	blo	fat_set_copy

	cmp	pc, #0

fat_set_out:
	@ WS_FATSEC now disagrees with the medium wherever it happened to be.
	mov	r1, #0
	str	r1, [wp, #WS_FATSEC_LBA]
	ldmfd	sp!, {r0-r10, pc}


	/* The cluster after this one.
	 *
	 * Entry: R0 = volume, R1 = cluster, R2 = non-zero for a contiguous run.
	 * Exit:  R0 = the next cluster, or 0 at the end of a chain.
	 *
	 * exFAT lets an object say "I am one unbroken run" - the NoFatChain flag
	 * - and when it does, its FAT entries are not maintained and hold
	 * whatever was there before. Following them reads somebody else's data
	 * and reports no error at all, which is exactly what happened before this
	 * existed: a 288KB file read its first cluster correctly and then
	 * returned the contents of another file entirely.
	 *
	 * A contiguous run has no end marker, so it is the caller's length that
	 * stops it. Every caller here already bounds the walk by the extent.
	 */
chain_next:
	cmp	r2, #0
	addne	r0, r1, #1
	movne	pc, lr
	b	fat_next


	/* Read one FAT entry exactly as it stands.
	 *
	 * Entry: R0 = volume record, R1 = cluster.
	 * Exit:  R0 = the entry, end-of-chain markers and all; V set on failure.
	 *
	 * fat_next answers the question "what comes after this cluster" and
	 * flattens every end marker to zero, which is right for walking a chain
	 * and useless for allocating one - a free entry is zero too.
	 */
fat_raw:
	stmfd	sp!, {r1-r7, lr}

	mov	r7, r0
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #12
	beq	fat_raw_12
	cmp	r2, #16
	movhi	r3, r1, lsl #2		@ FAT32 and exFAT
	movls	r3, r1, lsl #1

	ldr	r0, [r7, #VOL_FATSEC0]
	add	r0, r0, r3, lsr #9
	ldr	r6, =511
	and	r6, r3, r6

	ldr	r4, [wp, #WS_FATSEC_LBA]
	cmp	r4, r0
	beq	fat_raw_have

	stmfd	sp!, {r0}
	ldr	r5, [r7, #VOL_DRIVE]
	mov	r1, r0
	mov	r0, r5
	add	r2, wp, #WS_FATSEC
	bl	read_sector
	ldmfd	sp!, {r0}
	bvs	fat_raw_out
	str	r0, [wp, #WS_FATSEC_LBA]

fat_raw_have:
	add	r0, wp, #WS_FATSEC
	mov	r1, r6
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #16
	bhi	fat_raw_32
	bl	ld16
	b	fat_raw_ok
fat_raw_32:
	bl	ld32
	bic	r0, r0, #0xf0000000
	b	fat_raw_ok

	@ Straight out: this is the entry as it stands, and fat12_get has already
	@ set V or cleared it.
fat_raw_12:
	mov	r0, r7
	bl	fat12_get
	b	fat_raw_out

fat_raw_ok:
	cmp	pc, #0
fat_raw_out:
	ldmfd	sp!, {r1-r7, pc}

	.ltorg


	/* Take a free cluster and mark it as the end of a chain.
	 *
	 * Entry: R0 = volume record.
	 * Exit:  R0 = the cluster; V set if there is no room.
	 *
	 * The search starts where the last one finished rather than at cluster
	 * two, so filling a disc does not become quadratic. The hint is only a
	 * hint: it is checked like any other candidate, and the scan wraps.
	 */
alloc_cluster:
	stmfd	sp!, {r1-r8, lr}

	mov	r7, r0
	ldr	r8, [r7, #VOL_CLUSTERS]
	add	r8, r8, #2		@ one past the last usable cluster number

	ldr	r5, [r7, #VOL_NEXTFREE]
	cmp	r5, #2
	movlo	r5, #2
	cmp	r5, r8
	movhs	r5, #2

	mov	r6, #0			@ how many we have looked at

alloc_cluster_try:
	@ On exFAT the FAT does not say what is free - the bitmap does, and for a
	@ contiguous file the FAT entries are not maintained at all, so a zero
	@ there means nothing. Ask the bitmap.
	@
	@ These are two BRANCHES and not a conditional BL pair. "BLEQ bmp_get /
	@ BLNE fat_raw" reads like an if/else and is not one: the first call
	@ returns having set the flags itself - every routine here ends with
	@ "CMP PC, #0", which leaves NE - so the second BL runs as well and its
	@ answer wins. The bitmap said cluster 2 was taken, fat_raw then said it
	@ was free, and the very first file written went straight over the
	@ allocation bitmap.
	mov	r0, r7
	mov	r1, r5
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	beq	alloc_cluster_ask_bitmap
	bl	fat_raw
	b	alloc_cluster_asked

alloc_cluster_ask_bitmap:
	bl	bmp_get

alloc_cluster_asked:
	bvs	alloc_cluster_out
	cmp	r0, #0
	beq	alloc_cluster_found

	add	r5, r5, #1
	cmp	r5, r8
	movhs	r5, #2			@ wrap
	add	r6, r6, #1
	ldr	r0, [r7, #VOL_CLUSTERS]
	cmp	r6, r0
	blo	alloc_cluster_try
	b	alloc_cluster_full

alloc_cluster_found:
	@ Claim it before anything else can be told about it. exFAT needs both:
	@ the bitmap bit, which is what says it is taken, and a FAT entry, because
	@ everything written here is a chain rather than a contiguous run.
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	bne	alloc_cluster_mark
	mov	r0, r7
	mov	r1, r5
	mov	r2, #1
	bl	bmp_set
	bvs	alloc_cluster_out

alloc_cluster_mark:
	mov	r0, r7
	mov	r1, r5
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	mvneq	r2, #0			@ exFAT: all thirty-two bits set
	beq	alloc_cluster_eoc
	cmp	r2, #32
	ldreq	r2, =0x0fffffff
	ldrne	r2, =0xffff
alloc_cluster_eoc:
	bl	fat_set
	bvs	alloc_cluster_out

	add	r0, r5, #1
	str	r0, [r7, #VOL_NEXTFREE]

	@ Only adjust the cached free count if there is one. -1 means nobody has
	@ counted yet, and inventing a number here would be worse than admitting
	@ that: the next enquiry counts properly.
	ldr	r0, [r7, #VOL_FREE]
	cmn	r0, #1
	subne	r0, r0, #1
	strne	r0, [r7, #VOL_FREE]

	mov	r0, #1
	str	r0, [r7, #VOL_FSDIRTY]

	mov	r0, r5
	cmp	pc, #0
	ldmfd	sp!, {r1-r8, pc}

alloc_cluster_full:
	adr	r0, err_disc_full
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

alloc_cluster_out:
	ldmfd	sp!, {r1-r8, pc}

err_disc_full:
	.int	0xc6
	.string	"There is not enough room on the USB disc"
	.align


	/* How many of this volume's clusters are unused.
	 *
	 * Entry: R0 = volume record.
	 * Exit:  R0 = free cluster count; VS on a read error.
	 *
	 * A FAT32 volume carries a running free count in its FSInfo sector, and
	 * one sector is a great deal cheaper than the whole FAT: a 14.5GB stick
	 * has a FAT of nearly eight megabytes, which is some fifteen thousand
	 * reads over the emulated USB stack. The count is only a hint, though -
	 * the specification lets it go stale and lets it be absent - so it is
	 * believed only when both signatures are right and the number it gives
	 * would actually fit on the volume. Otherwise, and always on FAT16, the
	 * FAT is counted entry by entry.
	 *
	 * WS_SECTOR is the buffer used throughout rather than WS_FATSEC, so that
	 * counting the FAT does not throw away the cached FAT sector that every
	 * directory walk depends on.
	 */
volume_free_clusters:
	stmfd	sp!, {r1-r10, lr}

	mov	r7, r0			@ volume record

	@ Counted before, and kept up to date by every allocation since? Then
	@ that is the answer, and a stick with a seven-megabyte FAT is not read
	@ again to confirm what is already known.
	ldr	r0, [r7, #VOL_FREE]
	cmn	r0, #1
	bne	vfc_ok

	@ exFAT keeps no free count anywhere. The allocation bitmap IS the answer
	@ - one bit per cluster, set when it is in use - and counting it is cheap:
	@ a bit per cluster is an eighth of a byte where the FAT spends four, so
	@ a stick whose FAT runs to seven megabytes has a bitmap of sixty
	@ kilobytes. There is no fast path to want.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	vfc_ntfs
	cmp	r0, #64
	beq	vfc_bitmap

	@ Anything written since the volume was found means the count sitting in
	@ the FSInfo sector describes the disc as it WAS. Count the FAT instead -
	@ it is the only thing that is always true - and the answer is written
	@ back below so this is paid for once.
	ldr	r0, [r7, #VOL_FSDIRTY]
	cmp	r0, #0
	bne	vfc_scan

	ldr	r1, [r7, #VOL_FSINFO]
	cmp	r1, #0
	beq	vfc_scan

	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	vfc_scan		@ an unreadable FSInfo just means counting

	add	r0, wp, #WS_SECTOR
	mov	r1, #0
	bl	ld32
	ldr	r1, =0x41615252		@ "RRaA"
	cmp	r0, r1
	bne	vfc_scan

	add	r0, wp, #WS_SECTOR
	ldr	r1, =484
	bl	ld32
	ldr	r1, =0x61417272		@ "rrAa"
	cmp	r0, r1
	bne	vfc_scan

	add	r0, wp, #WS_SECTOR
	ldr	r1, =508
	bl	ld32
	ldr	r1, =0xaa550000
	cmp	r0, r1
	bne	vfc_scan

	add	r0, wp, #WS_SECTOR
	ldr	r1, =488
	bl	ld32

	@ &FFFFFFFF is the documented "not known", and anything larger than the
	@ volume holds is a count that has gone bad.
	ldr	r1, [r7, #VOL_CLUSTERS]
	cmp	r0, r1
	bhi	vfc_scan
	b	vfc_ok

	/* Count the zero entries in the FAT.
	 *
	 * Cluster numbers start at 2: entries 0 and 1 hold the media descriptor
	 * and the end-of-chain marker rather than clusters, so counting starts
	 * past them and a volume with N clusters has entries 2 to N+1.
	 */
vfc_scan:
	ldr	r8, [r7, #VOL_CLUSTERS]
	add	r8, r8, #2		@ one past the last entry that is a cluster

	ldr	r9, [r7, #VOL_TYPE]
	cmp	r9, #12
	beq	vfc_scan_12

	cmp	r9, #32
	moveq	r9, #2			@ log2 of the entry width in bytes
	movne	r9, #1
	mov	r5, #1
	mov	r5, r5, lsl r9		@ the entry width itself

	mov	r10, #0			@ free clusters found so far
	mov	r6, #2			@ the entry being looked at

vfc_sector:
	cmp	r6, r8
	bhs	vfc_counted

	mov	r0, r6, lsl r9		@ byte offset into the FAT
	mov	r1, r0, lsr #9		@ the sector holding it
	ldr	r2, [r7, #VOL_FATSEC0]
	add	r1, r1, r2
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	vfc_out

	@ 512 divides by both of the entry widths that get here, so no entry
	@ straddles two sectors and the walk below can stop at the end of the
	@ buffer with nothing carried over into the next one. FAT12, where that is
	@ not true, went to vfc_scan_12 above.
	mov	r0, r6, lsl r9
	ldr	r1, =511
	and	r4, r0, r1		@ where in the sector this entry sits

vfc_entry:
	add	r0, wp, #WS_SECTOR
	mov	r1, r4
	cmp	r9, #2
	beq	vfc_entry_32
	bl	ld16
	b	vfc_entry_have
vfc_entry_32:
	bl	ld32
	bic	r0, r0, #0xf0000000	@ the top four bits are not part of it
vfc_entry_have:
	cmp	r0, #0
	addeq	r10, r10, #1

	add	r6, r6, #1
	cmp	r6, r8
	bhs	vfc_counted
	add	r4, r4, r5
	cmp	r4, #512
	blo	vfc_entry
	b	vfc_sector

	@ FAT12 entries straddle sectors, so the sector walk above cannot be used
	@ and each entry is asked for on its own. That is one sector read per 341
	@ clusters through fat_raw's cache, and a FAT12 volume holds at most 4,084
	@ clusters, so counting the whole FAT is a dozen reads.
vfc_scan_12:
	mov	r10, #0			@ free clusters found so far
	mov	r6, #2			@ the entry being looked at

vfc_scan_12_entry:
	cmp	r6, r8
	bhs	vfc_counted
	mov	r0, r7
	mov	r1, r6
	bl	fat_raw
	bvs	vfc_out
	cmp	r0, #0
	addeq	r10, r10, #1
	add	r6, r6, #1
	b	vfc_scan_12_entry

	@ The count is the answer. Without the branch this fell into the NTFS
	@ case below, which overwrote it with the volume record and went looking
	@ for an MFT that a FAT volume has not got - so every FAT16 volume, which
	@ has no FSInfo sector to short-circuit the count, reported an error for
	@ its free space rather than a number.
vfc_counted:
	mov	r0, r10
	b	vfc_ok

	/* NTFS keeps its own bitmap, in a file. */
vfc_ntfs:
	mov	r0, r7
	bl	ntfs_free_clusters
	bvs	vfc_out
	str	r0, [r7, #VOL_FREE]
	cmp	pc, #0
	ldmfd	sp!, {r1-r10, pc}

	/* Count the zero bits of an exFAT allocation bitmap. */
vfc_bitmap:
	@ This borrows WS_FATSEC, so the FAT cache must be told it no longer
	@ describes what is in there.
	mov	r0, #0
	str	r0, [wp, #WS_FATSEC_LBA]

	ldr	r0, [r7, #VOL_BMP_CLUS]
	cmp	r0, #0
	beq	vfc_none		@ no bitmap found: say nothing rather than guess
	mov	r8, r0			@ the cluster being read
	ldr	r9, [r7, #VOL_CLUSTERS]
	mov	r10, #0			@ free so far
	mov	r5, #0			@ clusters described so far
	ldr	r6, [r7, #VOL_SPCLOG]
	mov	r0, #1
	mov	r6, r0, lsl r6		@ sectors in a cluster

vfc_bmp_cluster:
	cmp	r5, r9
	bhs	vfc_bmp_done
	cmp	r8, #1
	bls	vfc_bmp_done

	mov	r0, r7
	mov	r1, r8
	bl	cluster_sector
	mov	r4, r0
	mov	r3, r6			@ sectors left in this cluster

vfc_bmp_sector:
	cmp	r5, r9
	bhs	vfc_bmp_done
	cmp	r3, #0
	beq	vfc_bmp_step

	mov	r1, r4
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FATSEC
	bl	read_sector
	bvs	vfc_out

	mov	r2, #0
vfc_bmp_byte:
	cmp	r2, #512
	bhs	vfc_bmp_sector_done
	cmp	r5, r9
	bhs	vfc_bmp_done
	add	r0, wp, #WS_FATSEC
	ldrb	r1, [r0, r2]
	mov	r0, #0
vfc_bmp_bit:
	cmp	r0, #8
	bhs	vfc_bmp_byte_done
	cmp	r5, r9
	bhs	vfc_bmp_done
	tst	r1, #1
	addeq	r10, r10, #1		@ a clear bit is a free cluster
	mov	r1, r1, lsr #1
	add	r0, r0, #1
	add	r5, r5, #1
	b	vfc_bmp_bit
vfc_bmp_byte_done:
	add	r2, r2, #1
	b	vfc_bmp_byte

vfc_bmp_sector_done:
	add	r4, r4, #1
	sub	r3, r3, #1
	b	vfc_bmp_sector

vfc_bmp_step:
	mov	r0, r7
	mov	r1, r8
	ldr	r2, [r7, #VOL_BMP_CONTIG]
	bl	chain_next
	bvs	vfc_out
	cmp	r0, #0
	beq	vfc_bmp_done
	mov	r8, r0
	b	vfc_bmp_cluster

vfc_bmp_done:
	mov	r0, #0
	str	r0, [wp, #WS_FATSEC_LBA]
	mov	r0, r10
	str	r0, [r7, #VOL_FREE]
	cmp	pc, #0
	ldmfd	sp!, {r1-r10, pc}

vfc_none:
	mov	r0, #0
	str	r0, [r7, #VOL_FREE]
	cmp	pc, #0
	ldmfd	sp!, {r1-r10, pc}

vfc_ok:
	str	r0, [r7, #VOL_FREE]

	@ Put the number back on the medium, so the next system to look - which
	@ may well be Windows, and Windows believes this field - is told the
	@ truth rather than whatever was there when the stick was plugged in.
	mov	r1, r0
	ldr	r0, [r7, #VOL_FSDIRTY]
	cmp	r0, #0
	movne	r0, r7
	blne	fsinfo_write

	ldr	r0, [r7, #VOL_FREE]
	cmp	pc, #0			@ clear V

vfc_out:
	ldmfd	sp!, {r1-r10, pc}

	.ltorg


	/* Bytes per cluster: sectors per cluster times bytes per sector.
	 *
	 * Entry: R0 = volume record. Exit: R0 = bytes.
	 */
cluster_bytes:
	stmfd	sp!, {r1-r2, lr}
	ldr	r1, [r0, #VOL_SPC]
	ldr	r2, [r0, #VOL_BPS]
	mul	r0, r1, r2
	ldmfd	sp!, {r1-r2, pc}


	/* Unsigned 32 x 32 into 64.
	 *
	 * Entry: R0, R1 = the two factors.
	 * Exit:  R0 = the low word, R1 = the high word.
	 *
	 * UMULL would be one instruction, but it arrived with ARMv4 and these
	 * modules assemble to the ARMv3 floor so that they run on every machine
	 * the emulator offers. So the factors are split into halves and the four
	 * partial products recombined - which is what UMULL does in hardware.
	 */
mul64:
	stmfd	sp!, {r2-r7, lr}

	mov	r2, r0
	mov	r3, r1
	mov	r4, r2, lsr #16		@ the high half of the first factor
	bic	r5, r2, r4, lsl #16	@ and its low half
	mov	r6, r3, lsr #16
	bic	r7, r3, r6, lsl #16

	mul	r0, r5, r7		@ low x low, the bottom of the result
	mul	r1, r4, r6		@ high x high, the top of it

	@ The two cross terms belong sixteen bits up. Their sum can carry out of
	@ 32 bits, and that carry lands at bit 48 - bit 16 of the high word.
	mul	r2, r5, r6
	mul	r3, r4, r7
	adds	r2, r2, r3
	addcs	r1, r1, #0x10000

	adds	r0, r0, r2, lsl #16
	adc	r1, r1, r2, lsr #16

	ldmfd	sp!, {r2-r7, pc}


	/* Free and total bytes for the mounted volume, as 64-bit pairs.
	 *
	 * Entry: wp = workspace.
	 * Exit:  R0/R1 = free lo/hi, R2/R3 = total lo/hi, V clear.
	 *
	 * Zeroes rather than an error on any failure, because the caller below has
	 * nowhere to put one: the Free module's buffer has room for numbers and
	 * nothing else.
	 */
volume_space:
	stmfd	sp!, {r4-r8, lr}

	mov	r4, #0			@ free, low
	mov	r5, #0			@ free, high
	mov	r6, #0			@ total, low
	mov	r7, #0			@ total, high

	bl	current_volume
	bvs	volume_space_out
	mov	r8, r0

	mov	r0, r8
	bl	cluster_bytes
	stmfd	sp!, {r0}		@ bytes per cluster, wanted twice

	ldr	r0, [r8, #VOL_CLUSTERS]
	ldr	r1, [sp]
	bl	mul64
	mov	r6, r0
	mov	r7, r1

	mov	r0, r8
	bl	volume_free_clusters
	movvs	r0, #0			@ a disc that cannot be read is not free
	ldr	r1, [sp]
	bl	mul64
	mov	r4, r0
	mov	r5, r1

	add	sp, sp, #4

volume_space_out:
	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	mov	r3, r7
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r4-r8, pc}


	/* The routine the Free module calls, registered at init.
	 *
	 * Entry: R0 = reason code, R2 = buffer where one is wanted, R12 = the
	 * private word this was registered with, and the return address is on the
	 * stack rather than in LR. It runs in user mode.
	 *
	 * Without registering, *ShowFree and the filer's Free menu entry answer
	 * "Unknown filing system": the Free module keeps its own list and does not
	 * ask FileSwitch who exists.
	 *
	 * The numbers are worked out here rather than asked for through
	 * OS_FSControl 49 and 55. Going round by FileSwitch would be the tidier
	 * shape, but FileSwitch does not pass those on to this filing system:
	 * watching FSEntry_Func at full speed through a whole *ShowFree shows
	 * thirty calls and not one of them reason 30 or 35. Our own HostFS gets
	 * them because it sets bit 23 of its information word, and bit 23 means
	 * image filing system extensions - setting it here left the disc with no
	 * name on the icon bar, which is a poor trade for a number.
	 */
free_routine:
	cmp	r0, #5
	addlo	pc, pc, r0, lsl #2
	ldmfd	sp!, {pc}		@ reason 5 and above: not ours
	ldmfd	sp!, {pc}		@ 0 - no operation
	b	free_device_name	@ 1
	b	free_space32		@ 2
	b	free_compare		@ 3
	b	free_space64		@ 4

free_device_name:
	stmfd	sp!, {r1-r3}
	mov	r1, r2
	adrl	r3, fs_name
0:	ldrb	r0, [r3], #1
	strb	r0, [r1], #1
	teq	r0, #0
	bne	0b
	sub	r0, r1, r2
	ldmfd	sp!, {r1-r3}
	ldmfd	sp!, {pc}

	@ Reason 2 clamps at 2GB-1 rather than wrapping, the way our own HostFS
	@ does: any stick worth having overflows a 32-bit byte count, and RISC OS
	@ asks reason 4 when it can.
free_space32:
	stmfd	sp!, {r0-r8, r12}
	ldr	wp, [wp]
	mov	r8, r2			@ where the answer goes

	bl	volume_space

	ldr	r4, =0x7fffffff
	cmp	r1, #0
	movne	r0, r4
	cmp	r0, r4
	movhi	r0, r4
	cmp	r3, #0
	movne	r2, r4
	cmp	r2, r4
	movhi	r2, r4

	str	r2, [r8, #0]		@ total
	str	r0, [r8, #4]		@ free
	sub	r2, r2, r0
	str	r2, [r8, #8]		@ used

	ldmfd	sp!, {r0-r8, r12}
	ldmfd	sp!, {pc}

free_compare:
	teq	r0, r0			@ set Z: there is only one of us
	ldmfd	sp!, {pc}

free_space64:
	stmfd	sp!, {r1-r8, r12}
	ldr	wp, [wp]
	mov	r8, r2

	bl	volume_space

	str	r2, [r8, #0]		@ total, low
	str	r3, [r8, #4]		@ total, high
	str	r0, [r8, #8]		@ free, low
	str	r1, [r8, #12]		@ free, high
	subs	r2, r2, r0
	sbc	r3, r3, r1
	str	r2, [r8, #16]		@ used, low
	str	r3, [r8, #20]		@ used, high

	ldmfd	sp!, {r1-r8, r12}
	mov	r0, #0			@ success
	ldmfd	sp!, {pc}


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
	str	r2, [wp, #WS_IT_DIRCONTIG]	@ exFAT: is this directory one run?
	mov	r4, r0

	@ NTFS has no cluster chain and no slots. What arrives in R1 is the
	@ directory's MFT record number rather than a cluster - that is what the
	@ synthesised entry carries in FAT's first-cluster field - and zero means
	@ the root, which always lives in record five.
	ldr	r2, [r4, #VOL_TYPE]
	cmp	r2, #128
	bne	dir_open_fat
	cmp	r1, #0
	moveq	r1, #NTFS_ROOT_REC
	mov	r0, r4
	bl	ntfs_dir_open
	ldmfd	sp!, {r0-r4, pc}

dir_open_fat:

	cmp	r1, #0
	bne	dir_open_chain

	ldr	r2, [r4, #VOL_TYPE]
	cmp	r2, #32
	bhs	dir_open_root32		@ FAT32 and exFAT: the root is a chain

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

	mov	r0, #0
	str	r0, [wp, #WS_IT_INDEX]

	@ Any half-gathered long name belonged to the directory being left.
	bl	lfn_reset

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

	@ exFAT keeps nothing in a directory that FAT would recognise: no 8.3
	@ names, no &E5, no long-name fragments. Its slots are read by the same
	@ machinery and understood by an entirely different one.
	ldr	r0, [wp, #WS_IT_REC]
	ldr	r0, [r0, #VOL_TYPE]
	cmp	r0, #64
	beq	dir_next_exfat
	cmp	r0, #128
	beq	ntfs_dir_next

dir_next_again:
	bl	dir_raw_next
	bvs	dir_next_out
	cmp	r0, #0
	beq	dir_next_end
	mov	r1, r0

	ldrb	r2, [r1]
	cmp	r2, #0
	beq	dir_next_end		@ a zero name byte ends the directory
	cmp	r2, #0xe5
	beq	dir_next_again		@ deleted
	cmp	r2, #'.'
	beq	dir_next_again		@ FAT's own "." and "..", which RISC OS has
					@ no use for: it goes up by path, not by
					@ entry, and showing them puts a directory
					@ called "." in every Filer window

	@ Gather up a long name as it goes past. The fragments arrive before the
	@ 8.3 entry they belong to, so by the time the caller is handed that entry
	@ the name is already assembled and waiting for it.
	ldrb	r2, [r1, #11]
	and	r2, r2, #0x0f
	cmp	r2, #0x0f
	movne	r0, r1
	bne	dir_next_ok
	mov	r0, r1
	bl	lfn_take
	b	dir_next_again

dir_next_ok:
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

dir_next_end:
	mov	r0, #0
	cmp	pc, #0

dir_next_out:
	ldmfd	sp!, {r1-r7, pc}


	/* The next raw 32-byte slot of the directory being walked.
	 *
	 * Exit: R0 = a pointer to it, or 0 when the directory runs out.
	 *       VS on a read error.
	 *
	 * Knows about sectors and clusters and nothing about what is in them,
	 * which is what lets FAT and exFAT share it.
	 */
dir_raw_next:
	stmfd	sp!, {r1-r7, lr}
	ldr	r0, [wp, #WS_IT_OFF]
	cmp	r0, #512
	blo	dir_raw_have_sector

	@ Another sector, from this cluster or the next one.
	ldr	r0, [wp, #WS_IT_SECLEFT]
	cmp	r0, #0
	bne	dir_raw_read

	ldr	r0, [wp, #WS_IT_FIXED]
	cmp	r0, #0
	bne	dir_raw_end		@ a fixed root simply runs out

	ldr	r0, [wp, #WS_IT_REC]
	ldr	r1, [wp, #WS_IT_CLUSTER]
	ldr	r2, [wp, #WS_IT_DIRCONTIG]
	bl	chain_next
	bvs	dir_raw_out
	cmp	r0, #0
	beq	dir_raw_end

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

dir_raw_read:
	ldr	r0, [wp, #WS_IT_REC]
	ldr	r0, [r0, #VOL_DRIVE]
	ldr	r1, [wp, #WS_IT_SECTOR]
	add	r2, wp, #WS_DIRSEC
	bl	read_sector
	bvs	dir_raw_out

	@ Remember which sector this is. WS_IT_SECTOR moves on to the next one
	@ below, so it cannot answer "where did that entry come from" afterwards,
	@ and writing an entry back needs exactly that.
	ldr	r0, [wp, #WS_IT_SECTOR]
	str	r0, [wp, #WS_DIRSEC_LBA]
	add	r0, r0, #1
	str	r0, [wp, #WS_IT_SECTOR]
	ldr	r0, [wp, #WS_IT_SECLEFT]
	sub	r0, r0, #1
	str	r0, [wp, #WS_IT_SECLEFT]
	mov	r0, #0
	str	r0, [wp, #WS_IT_OFF]

dir_raw_have_sector:
	ldr	r0, [wp, #WS_IT_OFF]
	add	r1, wp, #WS_DIRSEC
	add	r1, r1, r0
	add	r0, r0, #32
	str	r0, [wp, #WS_IT_OFF]

	@ Count every slot stepped over, skipped ones included, so the total
	@ matches dir_entry_at's numbering. The entry being returned is at
	@ WS_IT_INDEX minus one by the time anybody reads it.
	ldr	r0, [wp, #WS_IT_INDEX]
	add	r0, r0, #1
	str	r0, [wp, #WS_IT_INDEX]

	mov	r0, r1
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

dir_raw_end:
	mov	r0, #0
	cmp	pc, #0

dir_raw_out:
	ldmfd	sp!, {r1-r7, pc}


@ ---------------------------------------------------------------------------
@ Finding an object by name
@ ---------------------------------------------------------------------------

	/* Fold a character for comparison.
	 *
	 * FAT short names are upper case on the medium whatever the user typed,
	 * and RISC OS does not care about case in a filename either, so both
	 * sides are folded before comparing.
	 *
	 * Nothing but R0 is touched, and that matters: this used to borrow R1 as
	 * scratch for the second half of the range test, and name_compare below
	 * keeps the other string's folded character there. Any name with a lower
	 * case letter in it was therefore compared against 'z' minus itself and
	 * could never match. It went unnoticed for as long as every name came off
	 * the medium in 8.3 upper case - long names are the first ones that do not.
	 */
upper:
	cmp	r0, #'a'
	movlt	pc, lr
	cmp	r0, #'z'
	subls	r0, r0, #('a' - 'A')
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


	/* Does a name from the disc match a path component?
	 *
	 * Entry: R0 = the name as the disc has it, R1 = the component asked for,
	 *        which may carry RISC OS's wildcards: * for any run of characters
	 *        and # for exactly one.
	 * Exit:  Z set if they match.
	 *
	 * Lookup used to be an exact comparison, so *Copy MultiFS::0.$.Report*
	 * answered "not found" however many files began with Report - and there
	 * was no hint that a wildcard was the problem rather than the name. This
	 * is the ordinary backtracking match: remember where the last star was,
	 * and when the rest stops matching, let it swallow one more character and
	 * try again. Case is folded both ways, as everywhere else here.
	 */
name_matches:
	stmfd	sp!, {r0-r8, lr}

	mov	r3, r0			@ the name
	mov	r4, r1			@ the pattern
	mov	r5, #0			@ where we are in the name
	mov	r6, #0			@ where we are in the pattern
	mvn	r7, #0			@ the last star seen, -1 for none
	mov	r8, #0			@ what the name had reached when we met it

name_matches_loop:
	ldrb	r2, [r3, r5]
	cmp	r2, #0
	beq	name_matches_tail

	ldrb	r1, [r4, r6]
	cmp	r1, #'*'
	beq	name_matches_star
	cmp	r1, #0
	beq	name_matches_back	@ pattern spent, name is not
	cmp	r1, #'#'
	beq	name_matches_step	@ takes any one character

	mov	r0, r2
	bl	upper
	mov	r2, r0
	ldrb	r0, [r4, r6]
	bl	upper
	cmp	r0, r2
	bne	name_matches_back

name_matches_step:
	add	r5, r5, #1
	add	r6, r6, #1
	b	name_matches_loop

name_matches_star:
	mov	r7, r6
	mov	r8, r5
	add	r6, r6, #1
	b	name_matches_loop

name_matches_back:
	cmn	r7, #1
	beq	name_matches_no		@ no star to fall back on
	ldrb	r1, [r3, r8]
	cmp	r1, #0
	beq	name_matches_no		@ nothing left for it to swallow
	add	r8, r8, #1
	mov	r5, r8
	add	r6, r7, #1
	b	name_matches_loop

name_matches_tail:
	@ The name is used up, so whatever is left of the pattern must be stars.
	ldrb	r1, [r4, r6]
	cmp	r1, #'*'
	addeq	r6, r6, #1
	beq	name_matches_tail
	cmp	r1, #0
	bne	name_matches_back

	mov	r0, #0
	cmp	r0, #0			@ set Z: it matches
	ldmfd	sp!, {r0-r8, pc}

name_matches_no:
	mov	r0, #1
	cmp	r0, #0			@ clear Z
	ldmfd	sp!, {r0-r8, pc}

	.ltorg


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

	@ Empty WS_FOUND before starting. The root has no directory entry of its
	@ own, so a lookup of the root writes nothing here - and the caller reads
	@ this block to decide whether there was an entry at all. Left holding the
	@ last lookup's, a Filer window on the root listed whichever subdirectory
	@ had been opened before it.
	ldr	r0, =WS_FOUND
	add	r0, wp, r0
	mov	r1, #0
	mov	r2, #0
path_clear_found:
	str	r1, [r0, r2]
	add	r2, r2, #4
	cmp	r2, #32
	blo	path_clear_found

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
	cmp	r2, #255		@ as long as a RISC OS name may be
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
	ldr	r2, =WS_FOUND_CONTIG
	ldr	r2, [wp, r2]
	cmp	r9, #0
	moveq	r2, #0			@ the root is always a chain
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
	bl	build_entry_name
	ldr	r1, =WS_COMP
	add	r1, wp, r1
	bl	name_matches
	bne	path_search

	@ Found this component. Keep the entry: the directory buffer it lives in
	@ will be reused by the next dir_open. Where it came from is kept too -
	@ writing a file back means changing this entry in place, and by then the
	@ iterator has moved on and cannot say where it was.
	ldr	r0, [wp, #WS_DIRSEC_LBA]
	ldr	r1, =WS_FOUND_SEC
	str	r0, [wp, r1]
	add	r0, wp, #WS_DIRSEC
	sub	r0, r7, r0
	ldr	r1, =WS_FOUND_OFF
	str	r0, [wp, r1]

	@ Which slot it is, and which directory it is in. Deleting and renaming
	@ need both, and by the time either is asked for, the iterator has moved
	@ on and R9 is long gone.
	ldr	r0, [wp, #WS_IT_INDEX]
	sub	r0, r0, #1
	ldr	r1, =WS_FOUND_IDX
	str	r0, [wp, r1]
	ldr	r1, =WS_PARENT
	str	r9, [wp, r1]

	@ exFAT: whether this object is a contiguous run travels with it, because
	@ by the time anything opens it the iterator has moved on. So does where
	@ its entry set is.
	ldr	r0, [wp, #WS_IT_CONTIG]
	ldr	r1, =WS_FOUND_CONTIG
	str	r0, [wp, r1]
	ldr	r0, [wp, #WS_IT_SETIDX]
	ldr	r1, =WS_FOUND_SETIDX
	str	r0, [wp, r1]
	ldr	r0, [wp, #WS_IT_SETCNT]
	ldr	r1, =WS_FOUND_SETCNT
	str	r0, [wp, r1]

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
	@ Say where we got to as well as that we failed. Creating a file means
	@ knowing which directory it belongs in and what it is to be called, and
	@ this is the only place that still knows both: the component is in
	@ WS_COMP and the directory it was looked for in is in R9. It only counts
	@ if this was the last part of the path - "$.nosuchdir.file" must not
	@ quietly create "file" somewhere.
	ldr	r1, =WS_PARENT
	str	r9, [wp, r1]
	ldrb	r0, [r11]
	cmp	r0, #0
	moveq	r0, #1
	movne	r0, #0
	ldr	r1, =WS_LEAF_OK
	str	r0, [wp, r1]

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
	/* The volume to work on, and the place media change is noticed.
	 *
	 * The table used to be built once and believed for ever, so a stick that
	 * had been pulled out went on having an icon, a name and a root directory
	 * until the module was reloaded. Now the medium is re-read before the
	 * table is handed out: if the boot record has stopped answering, the stick
	 * has gone and the table goes with it.
	 *
	 * Not on every call, though. A directory listing asks for the volume once
	 * per entry, and a sector read apiece would turn a Filer window into a
	 * conversation with the device. Half a second is short enough that the
	 * icon goes almost at once and long enough that a listing costs one read
	 * rather than hundreds.
	 */
current_volume:
	stmfd	sp!, {r1-r3, lr}

	@ Dismounted means dismounted. Without this the table is emptied and the
	@ very next question fills it in again, so the disc came straight back and
	@ the menu entry appeared to do nothing at all.
	ldr	r1, =WS_DISMOUNTED
	ldr	r1, [wp, r1]
	cmp	r1, #0
	bne	current_volume_none

	ldr	r0, [wp, #WS_NVOLUMES]
	cmp	r0, #0
	beq	current_volume_scan

	swi	XOS_ReadMonotonicTime
	ldr	r1, [wp, #WS_CHECKED]
	sub	r2, r0, r1
	cmp	r2, #MEDIA_CHECK_CS
	blo	current_volume_have	@ looked recently enough
	str	r0, [wp, #WS_CHECKED]

	bl	media_present
	bvc	current_volume_have

	@ It has gone. Emptying the table is what takes the icon off the bar and
	@ stops paths resolving out of memory for a disc that is not there.
	mov	r0, #0
	str	r0, [wp, #WS_NVOLUMES]

current_volume_scan:
	@ Nothing in the table. That is the ordinary state when the module was
	@ loaded before the drive was ready, or when a stick has just been put
	@ in, so look again rather than declaring there is no disc.
	bl	scan_all
	swi	XOS_ReadMonotonicTime
	str	r0, [wp, #WS_CHECKED]
	ldr	r0, [wp, #WS_NVOLUMES]
	cmp	r0, #0
	beq	current_volume_none

current_volume_have:
	add	r0, wp, #WS_VOLUMES

	@ NTFS is read only here, and this is where that is decided. Writing it
	@ safely means maintaining $LogFile, $Bitmap and the $MFT's allocation;
	@ a half-correct writer does not make a slightly wrong disc, it makes one
	@ chkdsk cannot repair.
	ldr	r1, [r0, #VOL_TYPE]
	cmp	r1, #128
	moveq	r1, #1
	movne	r1, #0
	ldr	r2, =WS_READONLY
	str	r1, [wp, r2]

	cmp	pc, #0
	ldmfd	sp!, {r1-r3, pc}
current_volume_none:
	adr	r0, err_no_disc
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r3, pc}


	/* Is the medium still there?
	 *
	 * Entry: nothing.  Exit: V clear if the first volume's boot record still
	 * reads and still looks like one, V set if it does not.
	 *
	 * Re-reading the boot record is the cheapest question that a removed
	 * device cannot answer by accident: an absent drive fails the transfer,
	 * and a different stick put in its place answers with its own record
	 * rather than this one's.
	 */
media_present:
	stmfd	sp!, {r0-r3, lr}

	add	r3, wp, #WS_VOLUMES
	ldr	r0, [r3, #VOL_DRIVE]
	ldr	r1, [r3, #VOL_START]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	media_present_gone

	bl	boot_signature
	bne	media_present_gone

	cmp	pc, #0
	ldmfd	sp!, {r0-r3, pc}

media_present_gone:
	adr	r0, err_no_disc
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r3, pc}

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
	 * Reason 5, read catalogue info, is what everything else asks first and
	 * what tells FileSwitch an object exists at all. Reason 7 is how a file
	 * comes into existence - see fs_file_create, which is not the entry point
	 * the obvious reading of the documentation leads you to.
	 *
	 *   Out  R0 = object type, 0 none, 1 file, 2 directory
	 *        R2 = load address, R3 = exec address, R4 = length,
	 *        R5 = attributes
	 */
fs_file:
	stmfd	sp!, {r1, r6-r12, lr}
	ldr	wp, [wp]		@ FileSwitch hands us the private word

	cmp	r0, #1
	beq	fs_file_settype
	cmp	r0, #6
	beq	fs_file_delete
	cmp	r0, #7
	beq	fs_file_create
	cmp	r0, #8
	beq	fs_file_mkdir

	@ Reason 0 is "save this whole file". With FS_SAVE_BY_STREAM set in the
	@ information word FileSwitch does that itself, by creating the file with
	@ reason 7 and then opening, writing and closing it, so this should no
	@ longer arrive. Saying so plainly is still worth it if it ever does:
	@ returning "nothing here" instead let FileSwitch decide the DIRECTORY was
	@ missing, so copying a file to the disc failed with "MultiFS::0.$ not
	@ found", which sends you looking in entirely the wrong place.
	cmp	r0, #0
	beq	fs_file_cannot_save

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

	@ A file.
	ldr	r6, =WS_FOUND
	add	r6, wp, r6
	mov	r0, r6
	mov	r1, #28
	bl	ld32
	mov	r4, r0			@ length
	mov	r0, r6
	bl	entry_load_exec
	mov	r2, r0			@ load, with the type and the date in it
	mov	r3, r1			@ exec
	ldrb	r0, [r6, #11]
	mov	r5, #3			@ read and write
	tst	r0, #0x01		@ read-only on the medium
	bicne	r5, r5, #2
	mov	r0, #1
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_dir:
	ldr	r6, =WS_FOUND
	add	r6, wp, r6
	mov	r0, r6
	bl	entry_load_exec
	mov	r2, r0
	mov	r3, r1
	mov	r4, #0
	mov	r5, #3
	mov	r0, #2
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_none:
	mov	r0, #0
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

	/* FSEntry_File 7 - create a file that is not there yet.
	 *
	 *   R1 = name, R2 = load address, R3 = exec address,
	 *   R4 = start address, R5 = end address, so the length wanted is R5-R4.
	 *
	 * This is how a file is really made on RISC OS 3 and later, and finding
	 * that out was the whole difficulty. FSEntry_Open reason 1 is documented
	 * as "create and open for update", reads like the obvious route, and is a
	 * dead letter: FileSwitch has not used it since RISC OS 2 - our own
	 * src/hostfs.c says exactly that against OPEN_MODE_CREATE_OPEN_UPDATE.
	 * Everything above calls File 7 and then opens the result for update, so
	 * a filing system that answers Open reason 1 and nothing else can be
	 * written to but can never be written to FIRST. The visible symptom was
	 * "Can't open" from *Save and "0 files copied" from *Copy, with no entry
	 * of ours ever reached.
	 *
	 * The entry is made empty, whatever length is asked for. Growing a file
	 * needs no clusters until something is actually put in it - the rule
	 * fs_args_setextent already works to - so FileSwitch's own open, PutBytes,
	 * set-extent and close finish the job through paths that were working
	 * before this was written.
	 *
	 * The load and exec addresses in R2 and R3 carry the file type, and FAT
	 * has nowhere to put it: a file saved as Text comes back as Data. That is
	 * a MimeMap question and is left alone here rather than guessed at.
	 */
fs_file_create:
	stmfd	sp!, {r2-r5}

	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_file_create_out
	mov	r7, r0


	@ NTFS is read only. Refuse here so the message says why, rather than
	@ letting the FAT code run and be stopped by write_sector half way
	@ through, having already changed something.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_file_ro4
	mov	r0, r7			@ path_lookup wants the volume back in R0

	bl	path_lookup
	bvs	fs_file_create_out
	cmp	r0, #2
	beq	fs_file_create_isdir
	cmp	r0, #1
	beq	fs_file_create_replace

	@ Not there, which is the ordinary case. path_lookup is the only thing
	@ that still knows both halves of the answer - which directory the search
	@ ended in and what the missing leaf was called.
	ldr	r0, =WS_LEAF_OK
	ldr	r0, [wp, r0]
	cmp	r0, #0
	beq	fs_file_create_nopath

	mov	r0, r7
	ldr	r1, =WS_PARENT
	ldr	r1, [wp, r1]
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	mov	r3, #0x20		@ a new file, marked as written to
	mov	r0, #0
	ldr	r2, =WS_X_ALLOC
	str	r0, [wp, r2]
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	mov	r0, r7
	beq	fs_file_create_exfat
	bl	dir_create
	b	fs_file_create_made

fs_file_create_exfat:
	bl	exfat_create
	mov	r0, #0			@ the type goes in the set, not in a sector
	mov	r1, #0

fs_file_create_made:
	bvs	fs_file_create_out
	cmp	r0, #0
	beq	fs_file_create_done	@ exFAT: nothing more to patch

	@ The type the caller asked for, which is in the load address it passed.
	mov	r2, r1
	mov	r1, r0
	ldr	r3, [sp]		@ the R2 saved on entry
	mov	r0, r7
	bl	entry_set_type
	bvs	fs_file_create_out

	b	fs_file_create_done

	/* The name is already taken. Reason 7 means REPLACE what is there, not
	 * add to it, so the old contents go before the new ones arrive. Leaving
	 * them left a 512-byte file that had just been saved over with 128 bytes
	 * still 512 bytes long, with the tail of the previous contents sitting
	 * after the new ones - and nothing above reported anything wrong, because
	 * as far as FileSwitch was concerned the write it asked for had happened.
	 */
fs_file_create_replace:
	ldr	r6, =WS_FOUND
	add	r6, wp, r6
	mov	r0, r6
	mov	r1, #20
	bl	ld16
	mov	r3, r0, lsl #16
	mov	r0, r6
	mov	r1, #26
	bl	ld16
	orr	r3, r3, r0		@ the chain it had

	mov	r0, r7
	ldr	r1, =WS_FOUND_SEC
	ldr	r1, [wp, r1]
	ldr	r2, =WS_FOUND_OFF
	ldr	r2, [wp, r2]
	bl	entry_empty
	bvs	fs_file_create_out

	@ It is being written afresh, so it takes the new type too.
	mov	r0, r7
	ldr	r1, =WS_FOUND_SEC
	ldr	r1, [wp, r1]
	ldr	r2, =WS_FOUND_OFF
	ldr	r2, [wp, r2]
	ldr	r3, [sp]		@ the R2 saved on entry
	bl	entry_set_type
	bvs	fs_file_create_out

fs_file_create_done:
	cmp	pc, #0

fs_file_create_out:
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_create_isdir:
	adrl	r0, err_is_dir
	b	fs_file_create_err

fs_file_create_nopath:
	adrl	r0, err_no_path

fs_file_create_err:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}

err_is_dir:
	.int	0xba
	.string	"A directory of that name is already on the USB disc"
	.align
err_no_path:
	.int	0xd6
	.string	"That directory on the USB disc does not exist"
	.align

	/* FSEntry_File 1 - write catalogue information.
	 *
	 *   R1 = name, R2 = load address, R3 = exec address, R5 = attributes
	 *
	 * This is what *SetType comes down to. Only the type is kept: FAT has no
	 * room for a load and exec pair, and the attributes it does have are a
	 * read-only bit and little else, which RISC OS's own idea of attributes
	 * does not map onto cleanly enough to be worth pretending about.
	 */
fs_file_settype:
	stmfd	sp!, {r2-r5}

	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_file_settype_out
	mov	r7, r0


	@ NTFS is read only. Refuse here so the message says why, rather than
	@ letting the FAT code run and be stopped by write_sector half way
	@ through, having already changed something.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_file_ro4
	mov	r0, r7			@ path_lookup wants the volume back in R0

	bl	path_lookup
	bvs	fs_file_settype_out
	cmp	r0, #1
	bne	fs_file_settype_done	@ not a file, so nothing to type

	@ exFAT keeps the type in its entry set, not in a FAT entry's spare
	@ bytes, so entry_set_type below must not be let loose on one.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	fs_file_settype_done

	mov	r0, r7
	ldr	r1, =WS_FOUND_SEC
	ldr	r1, [wp, r1]
	ldr	r2, =WS_FOUND_OFF
	ldr	r2, [wp, r2]
	ldr	r3, [sp]		@ the R2 saved on entry
	bl	entry_set_type
	bvs	fs_file_settype_out

fs_file_settype_done:
	cmp	pc, #0

fs_file_settype_out:
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}


	/* FSEntry_File 6 - delete an object.
	 *
	 *   R1 = name
	 *   Out  R0 = what it WAS, 0 none, 1 file, 2 directory
	 *        R2 = load address, R3 = exec address, R4 = length,
	 *        R5 = attributes
	 *
	 * Deleting something that is not there is not an error: R0 = 0 and
	 * FileSwitch decides what to make of it. A directory has to be empty
	 * first, and the root is never deletable however it is asked for.
	 */
fs_file_delete:
	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_file_delete_out
	mov	r7, r0


	@ NTFS is read only. Refuse here so the message says why, rather than
	@ letting the FAT code run and be stopped by write_sector half way
	@ through, having already changed something.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_file_ro
	mov	r0, r7			@ path_lookup wants the volume back in R0

	bl	path_lookup
	bvs	fs_file_delete_out
	cmp	r0, #0
	beq	fs_file_delete_none
	mov	r8, r0			@ 1 file, 2 directory

	@ The root answers reason 2 with no entry behind it, and an entry whose
	@ first name byte is zero is exactly that. Nothing may delete it.
	ldr	r6, =WS_FOUND
	add	r6, wp, r6
	ldrb	r0, [r6]
	cmp	r0, #0
	beq	fs_file_delete_isroot

	@ Where it lives, before anything else walks a directory and loses it.
	ldr	r0, =WS_FOUND_IDX
	ldr	r10, [wp, r0]
	ldr	r0, =WS_PARENT
	ldr	r11, [wp, r0]

	@ Its first cluster, which is what has to go back to the free pool.
	mov	r0, r6
	mov	r1, #20
	bl	ld16
	mov	r9, r0, lsl #16
	mov	r0, r6
	mov	r1, #26
	bl	ld16
	orr	r9, r9, r0

	@ Everything the caller is owed, read out now: the entry is about to stop
	@ being readable.
	mov	r0, r6
	mov	r1, #28
	bl	ld32
	mov	r4, r0			@ length
	ldr	r2, =0xfffffd00		@ load
	mov	r3, #0			@ exec
	ldrb	r0, [r6, #11]
	mov	r5, #3
	tst	r0, #0x01
	bicne	r5, r5, #2		@ read-only on the medium
	cmp	r8, #2
	moveq	r4, #0

	cmp	r8, #2
	bne	fs_file_delete_go

	@ A directory, so it has to be empty. Its own cluster is R9.
	stmfd	sp!, {r2-r5}
	mov	r0, r7
	mov	r1, r9
	bl	dir_is_empty
	mov	r6, r0
	ldmfd	sp!, {r2-r5}
	bvs	fs_file_delete_out
	cmp	r6, #0
	beq	fs_file_delete_notempty

fs_file_delete_go:
	stmfd	sp!, {r2-r5}

	@ The entry goes first. If freeing the chain fails half way the worst
	@ left behind is some unreachable clusters, which *MultiFSFree will
	@ notice; strike it out afterwards and a failure leaves an entry
	@ pointing at clusters that are now free for somebody else to take.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	fs_file_delete_exfat

	mov	r0, r7
	mov	r1, r11
	mov	r2, r10
	bl	dir_delete_at
	bvs	fs_file_delete_undo

	cmp	r9, #0
	beq	fs_file_delete_done
	mov	r0, r7
	mov	r1, r9
	bl	free_chain
	bvs	fs_file_delete_undo
	b	fs_file_delete_done

fs_file_delete_exfat:
	mov	r0, r7
	mov	r1, r11
	ldr	r2, =WS_FOUND_SETIDX
	ldr	r2, [wp, r2]
	ldr	r3, =WS_FOUND_SETCNT
	ldr	r3, [wp, r3]
	bl	exfat_set_delete
	bvs	fs_file_delete_undo

	cmp	r9, #0
	beq	fs_file_delete_done

	@ A contiguous run has no chain to walk, so its length is the only thing
	@ that says how much to give back.
	ldr	r0, =WS_FOUND_CONTIG
	ldr	r0, [wp, r0]
	cmp	r0, #0
	beq	fs_file_delete_exfat_chain

	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9
	ldr	r2, =WS_FOUND
	add	r2, wp, r2
	stmfd	sp!, {r0}
	mov	r1, #28
	mov	r0, r2
	bl	ld32
	ldmfd	sp!, {r1}
	mov	r2, #1
	mov	r2, r2, lsl r1
	sub	r2, r2, #1
	add	r0, r0, r2
	mov	r2, r0, lsr r1		@ clusters the extent covers
	mov	r0, r7
	mov	r1, r9
	bl	free_run
	bvs	fs_file_delete_undo
	b	fs_file_delete_done

fs_file_delete_exfat_chain:
	mov	r0, r7
	mov	r1, r9
	bl	free_chain
	bvs	fs_file_delete_undo

fs_file_delete_done:
	ldmfd	sp!, {r2-r5}
	mov	r0, r8
	cmp	pc, #0
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_delete_undo:
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_delete_none:
	mov	r0, #0
	mov	r2, #0
	mov	r3, #0
	mov	r4, #0
	mov	r5, #0
	cmp	pc, #0

fs_file_delete_out:
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_delete_isroot:
	adrl	r0, err_del_root
	b	fs_file_delete_err

fs_file_delete_notempty:
	adrl	r0, err_not_empty

fs_file_delete_err:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r6-r12, pc}

err_del_root:
	.int	0xbd
	.string	"The root directory of a USB disc cannot be deleted"
	.align
err_not_empty:
	.int	0xb4
	.string	"That directory on the USB disc is not empty"
	.align


	/* FSEntry_File 8 - create a directory.
	 *
	 *   R1 = name, R4 = entries wanted, which FAT has no use for
	 *
	 * The order here is not free choice. The entry is made FIRST and the
	 * cluster allocated afterwards, because making room for the entry can
	 * itself extend the directory and so take a cluster - ask for both at
	 * once and the same cluster is handed out twice. The reference makes the
	 * same point in as many words above fs_getNextFreeCluster.
	 */
fs_file_mkdir:
	stmfd	sp!, {r2-r5}

	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_file_mkdir_out
	mov	r7, r0


	@ NTFS is read only. Refuse here so the message says why, rather than
	@ letting the FAT code run and be stopped by write_sector half way
	@ through, having already changed something.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_file_ro4
	mov	r0, r7			@ path_lookup wants the volume back in R0

	bl	path_lookup
	bvs	fs_file_mkdir_out
	cmp	r0, #2
	beq	fs_file_mkdir_done	@ already a directory, which is success
	cmp	r0, #1
	beq	fs_file_mkdir_isfile

	ldr	r0, =WS_LEAF_OK
	ldr	r0, [wp, r0]
	cmp	r0, #0
	beq	fs_file_mkdir_nopath

	ldr	r0, =WS_PARENT
	ldr	r11, [wp, r0]		@ the directory it goes in

	mov	r0, r7
	mov	r1, r11
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	mov	r3, #0x10		@ a directory
	mov	r0, #1
	ldr	r2, =WS_X_ALLOC
	str	r0, [wp, r2]
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	mov	r0, r7
	beq	fs_file_mkdir_exfat
	bl	dir_create
	bvs	fs_file_mkdir_out
	mov	r9, r0			@ the entry's sector
	mov	r10, r1			@ and its offset
	b	fs_file_mkdir_cluster

fs_file_mkdir_exfat:
	bl	exfat_create
	bvs	fs_file_mkdir_out
	mov	r9, r0			@ the set's index
	mov	r10, r1			@ and how many entries

fs_file_mkdir_cluster:
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	fs_file_mkdir_exfat_link

	@ Now, and only now, a cluster for its contents.
	mov	r0, r7
	bl	alloc_cluster
	bvs	fs_file_mkdir_out
	mov	r8, r0

	@ End the chain at once: a cluster that is allocated but still reads as
	@ free is one a second allocation will hand out again.
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	mvneq	r2, #0
	beq	fs_file_mkdir_eoc
	cmp	r2, #32
	ldreq	r2, =0x0fffffff
	ldrne	r2, =0xffff
fs_file_mkdir_eoc:
	mov	r0, r7
	mov	r1, r8
	bl	fat_set
	bvs	fs_file_mkdir_out

	mov	r0, r7
	mov	r1, r8
	bl	dir_clear_cluster
	bvs	fs_file_mkdir_out

	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	fs_file_mkdir_exfat_link

	@ "." and "..", which every FAT directory but the root carries. ".."
	@ points at zero when the parent IS the root, whatever cluster the root
	@ has. exFAT has neither entry - see below.
	mov	r0, r7
	mov	r1, r8
	mov	r2, r11
	bl	dir_write_dots
	bvs	fs_file_mkdir_out

	mov	r0, r7
	mov	r1, r9
	mov	r2, r10
	mov	r3, r8
	bl	entry_set_cluster
	bvs	fs_file_mkdir_out
	b	fs_file_mkdir_done

	/* exFAT directories have no "." and ".." at all: walking up is done by
	 * path and nothing on the medium records a parent. All that is left is to
	 * tell the entry set where its contents are and how long they are - and a
	 * directory's length is what is allocated to it, one cluster here.
	 */
fs_file_mkdir_exfat_link:
	@ Nothing to do: exfat_create allocated the cluster, cleared it and put
	@ it in the set itself. exFAT directories have no "." and ".." either -
	@ walking up is done by path and nothing on the medium records a parent.

fs_file_mkdir_done:
	cmp	pc, #0

fs_file_mkdir_out:
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_mkdir_isfile:
	adrl	r0, err_is_file
	b	fs_file_mkdir_err

fs_file_mkdir_nopath:
	adrl	r0, err_no_path

fs_file_mkdir_err:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r2-r5}
	ldmfd	sp!, {r1, r6-r12, pc}

err_is_file:
	.int	0xba
	.string	"A file of that name is already on the USB disc"
	.align

	/* Read-only refusals. There are two doors because three of the callers
	 * push {r2-r5} of their own on top of fs_file's frame and one does not,
	 * and leaving that on the stack unwound the wrong words on the way out -
	 * which showed up as an undefined instruction a long way from here.
	 */
fs_file_ro4:
	ldmfd	sp!, {r2-r5}

fs_file_ro:
	adrl	r0, err_readonly_fs
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r6-r12, pc}

fs_file_cannot_save:
	adrl	r0, err_no_create
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r6-r12, pc}

err_no_create:
	.int	0
	.string	"MultiFS was asked to save a whole file directly, which it does not do"
	.align

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
	cmp	r0, #8
	beq	fs_func_rename
	cmp	r0, #11
	beq	fs_func_discname
	cmp	r0, #30
	beq	fs_func_free32
	cmp	r0, #35
	beq	fs_func_free64

	@ Everything else is accepted and does nothing, which is what most of the
	@ reasons want: refusing them makes FileSwitch give up on the whole
	@ filing system rather than on the one operation.
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

	/* Reason 8: rename an object.
	 *
	 *   R1 = the name it has, R2 = the name it is to have
	 *   Out  R1 = 0 renamed, non-zero "I cannot, do it the long way"
	 *
	 * Renaming is NOT reason 25, which is what the obvious guess says; 25 is
	 * read defect list. Reason 8 it is, which the reference's own FSEntry_Func
	 * switch settles - its case 11 is the disc name, the one already answered
	 * above, so the numbering is not in doubt.
	 *
	 * Answering non-zero is always safe: FileSwitch then copies the object and
	 * deletes the original, which works and is merely slower. So every case
	 * that is not plainly a rename within one directory tree - the name taken,
	 * the path missing - refuses rather than guesses.
	 *
	 * The entry is created at the destination BEFORE the original is struck
	 * out, which is the opposite way round from the reference. It matters
	 * only if something fails between the two, and it decides which way the
	 * damage falls: this way leaves the object under both names, which can be
	 * seen and put right, where the other way round can leave it under
	 * neither. A duplicate is recoverable; a lost file is not.
	 */
fs_func_rename:
	stmfd	sp!, {r3, r4}
	sub	sp, sp, #32

	mov	r5, r1			@ the name it has
	mov	r6, r2			@ the name it is to have

	bl	current_volume
	bvs	fs_func_rename_fail
	mov	r7, r0

	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_func_rename_no	@ NTFS: read only, so decline it

	mov	r0, r7
	mov	r1, r5
	bl	path_lookup
	bvs	fs_func_rename_fail
	cmp	r0, #0
	beq	fs_func_rename_no	@ nothing of that name to rename

	@ Keep the whole entry: the second lookup below overwrites WS_FOUND, and
	@ everything except the name is about to be carried across unchanged.
	ldr	r1, =WS_FOUND
	add	r1, wp, r1
	mov	r2, #0
fs_func_rename_save:
	ldrb	r0, [r1, r2]
	strb	r0, [sp, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	fs_func_rename_save

	ldrb	r0, [sp]
	cmp	r0, #0
	beq	fs_func_rename_no	@ the root has no entry to move

	ldr	r0, =WS_FOUND_IDX
	ldr	r10, [wp, r0]
	ldr	r0, =WS_PARENT
	ldr	r11, [wp, r0]

	mov	r0, r7
	mov	r1, r6
	bl	path_lookup
	bvs	fs_func_rename_fail
	cmp	r0, #0
	bne	fs_func_rename_no	@ the new name is already taken

	ldr	r0, =WS_LEAF_OK
	ldr	r0, [wp, r0]
	cmp	r0, #0
	beq	fs_func_rename_no
	ldr	r0, =WS_PARENT
	ldr	r9, [wp, r0]		@ the directory it is moving into

	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	fs_func_rename_exfat

	ldrb	r3, [sp, #11]		@ carry the attribute over
	mov	r0, r7
	mov	r1, r9
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	bl	dir_create
	bvs	fs_func_rename_fail
	mov	r8, r0			@ the sector it landed in
	mov	r4, r1			@ and where in it

	@ Everything from the attribute onwards - cluster, length and dates - is
	@ the old entry's. Only the name is new.
	mov	r1, r8
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	fs_func_rename_fail

	add	r1, wp, #WS_FILESEC
	add	r1, r1, r4
	mov	r2, #11
fs_func_rename_copy:
	ldrb	r0, [sp, r2]
	strb	r0, [r1, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	fs_func_rename_copy

	mov	r1, r8
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	fs_func_rename_fail

	@ Now the original goes, entry and long-name run, but NOT its clusters:
	@ the data has not moved and the new entry points at it.
	mov	r0, r7
	mov	r1, r11
	mov	r2, r10
	bl	dir_delete_at
	bvs	fs_func_rename_fail

	mov	r1, #0
	b	fs_func_rename_leave

	/* The same on exFAT, where a name is a whole entry SET.
	 *
	 * A new set is built under the new name and then given the old one's
	 * cluster, length and dates; the old set is struck out afterwards. The
	 * data never moves. exfat_create must NOT be allowed to claim a cluster
	 * here even for a directory - it already has one, and a second would
	 * orphan its contents.
	 */
fs_func_rename_exfat:
	mov	r0, #0
	ldr	r1, =WS_X_ALLOC
	str	r0, [wp, r1]

	ldr	r0, =WS_FOUND_SETIDX	@ where the OLD set is, before it is lost
	ldr	r10, [wp, r0]
	ldr	r0, =WS_FOUND_SETCNT
	ldr	r4, [wp, r0]

	ldrb	r3, [sp, #11]		@ the attribute it had
	mov	r0, r7
	mov	r1, r9
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	bl	exfat_create
	bvs	fs_func_rename_fail
	mov	r8, r0			@ the new set's index
	mov	r6, r1			@ and how many entries

	@ Give the new set what the old one held.
	mov	r0, r7
	mov	r1, r9
	mov	r2, r8
	mov	r3, r6
	bl	exfat_set_read
	bvs	fs_func_rename_fail

	ldr	r0, =WS_XSET
	add	r0, wp, r0
	add	r0, r0, #32		@ its stream extension

	@ First cluster and length, taken from the entry saved on the stack -
	@ which dir_next_exfat wrote in FAT's shape, so they come from there.
	ldrb	r1, [sp, #26]
	ldrb	r2, [sp, #27]
	orr	r1, r1, r2, lsl #8
	ldrb	r2, [sp, #20]
	orr	r1, r1, r2, lsl #16
	ldrb	r2, [sp, #21]
	orr	r1, r1, r2, lsl #24
	strb	r1, [r0, #20]
	mov	r2, r1, lsr #8
	strb	r2, [r0, #21]
	mov	r2, r1, lsr #16
	strb	r2, [r0, #22]
	mov	r2, r1, lsr #24
	strb	r2, [r0, #23]

	ldrb	r1, [sp, #28]
	ldrb	r2, [sp, #29]
	orr	r1, r1, r2, lsl #8
	ldrb	r2, [sp, #30]
	orr	r1, r1, r2, lsl #16
	ldrb	r2, [sp, #31]
	orr	r1, r1, r2, lsl #24
	strb	r1, [r0, #24]
	mov	r2, r1, lsr #8
	strb	r2, [r0, #25]
	mov	r2, r1, lsr #16
	strb	r2, [r0, #26]
	mov	r2, r1, lsr #24
	strb	r2, [r0, #27]
	strb	r1, [r0, #8]
	mov	r2, r1, lsr #8
	strb	r2, [r0, #9]
	mov	r2, r1, lsr #16
	strb	r2, [r0, #10]
	mov	r2, r1, lsr #24
	strb	r2, [r0, #11]

	mov	r0, r7
	mov	r1, r9
	mov	r2, r8
	mov	r3, r6
	bl	exfat_set_write
	bvs	fs_func_rename_fail

	@ And the old set goes.
	mov	r0, r7
	mov	r1, r11
	mov	r2, r10
	mov	r3, r4
	bl	exfat_set_delete
	bvs	fs_func_rename_fail

	mov	r1, #0
	b	fs_func_rename_leave

fs_func_rename_no:
	mov	r1, #1

fs_func_rename_leave:
	add	sp, sp, #32
	ldmfd	sp!, {r3, r4}
	cmp	pc, #0
	b	fs_func_out

fs_func_rename_fail:
	add	sp, sp, #32
	ldmfd	sp!, {r3, r4}
	b	fs_func_out


	/* Reason 11: read disc name and boot option, R2 = buffer.
	 *
	 * The buffer is not a plain string. Byte 0 is the LENGTH of the name,
	 * the name follows at byte 1, and the boot option comes after it. Writing
	 * the name from byte 0 instead makes FileSwitch read the name's first
	 * character as a length - a disc called "USB STICK" then announces itself
	 * as 85 characters of rubbish beginning "SB STICK", and every path built
	 * from it fails to resolve.
	 */
fs_func_discname:
	bl	current_volume
	bvs	fs_func_out
	mov	r7, r0
	ldmfd	sp, {r0-r2}		@ recover the caller's R2 without popping
	add	r8, r7, #VOL_LABEL
	mov	r3, #0
fs_func_dn_copy:
	ldrb	r0, [r8, r3]
	cmp	r0, #0
	beq	fs_func_dn_done
	add	r1, r3, #1
	strb	r0, [r2, r1]
	add	r3, r3, #1
	cmp	r3, #11
	blo	fs_func_dn_copy
fs_func_dn_done:
	strb	r3, [r2]		@ the length, at the front
	add	r1, r3, #1
	mov	r0, #0			@ boot option: none
	strb	r0, [r2, r1]
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}


	/* Reason 30: free space, 32 bit.
	 *
	 *   Out R0 = free bytes, R1 = the largest file that could be created,
	 *   R2 = the size of the disc
	 *
	 * Reason 35 is the same question in 64 bits: R0/R1 free, R2 the largest
	 * creatable file, R3/R4 the size.
	 *
	 * Both are answered in clusters multiplied up, which is the only honest
	 * arithmetic available: FAT records occupancy per cluster and nothing
	 * finer. A stick larger than 4GB overflows the 32-bit answer, so reason 30
	 * pins its figures at 2GB-1 the way our own HostFS does rather than
	 * wrapping them - RISC OS asks the 64-bit question when it can.
	 */
fs_func_free32:
	bl	current_volume
	bvs	fs_func_out
	mov	r5, r0

	bl	fs_func_free_common	@ R0/R1 free, R2/R3 size, all 64 bit

	@ Clamp both to 2GB-1 rather than let them wrap.
	ldr	r6, =0x7fffffff
	cmp	r1, #0
	movne	r0, r6
	cmp	r0, r6
	movhi	r0, r6

	cmp	r3, #0
	movne	r2, r6
	cmp	r2, r6
	movhi	r2, r6

	mov	r1, r6			@ largest creatable file
	b	fs_func_free_return

fs_func_free64:
	bl	current_volume
	bvs	fs_func_out
	mov	r5, r0

	bl	fs_func_free_common
	mov	r4, r3			@ size high
	mov	r3, r2			@ size low
	ldr	r2, =0x7fffffff		@ largest creatable file

fs_func_free_return:
	cmp	pc, #0			@ clear V
	b	fs_func_out

	/* Free and total, in bytes, as 64-bit pairs.
	 *
	 * Entry: R5 = volume record.
	 * Exit:  R0/R1 = free bytes lo/hi, R2/R3 = total bytes lo/hi.
	 *
	 * A cluster count times a cluster size overflows 32 bits on any stick
	 * worth the name, so both products are done long-hand into a pair.
	 */
fs_func_free_common:
	stmfd	sp!, {r5-r8, lr}

	mov	r0, r5
	bl	cluster_bytes
	mov	r8, r0			@ bytes per cluster

	mov	r0, r5
	bl	volume_free_clusters
	movvs	r0, #0			@ a disc that cannot be read is not free
	mov	r6, r0

	ldr	r7, [r5, #VOL_CLUSTERS]

	mov	r0, r7
	mov	r1, r8
	bl	mul64
	mov	r2, r0			@ total bytes, low
	mov	r3, r1			@ total bytes, high

	mov	r0, r6
	mov	r1, r8
	bl	mul64			@ free bytes in R0/R1

	ldmfd	sp!, {r5-r8, pc}


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
	ldr	r2, =WS_FOUND_CONTIG
	ldr	r2, [wp, r2]
	cmp	r6, #0
	moveq	r2, #0
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
	@ Where the caller's buffer ends. A long name runs to 260 characters
	@ where an 8.3 name was at most thirteen, so "it will fit" stopped being
	@ a safe assumption and the room left is now measured before each entry.
	ldr	r0, [sp, #8]		@ the caller's R2, the buffer
	ldr	r5, [sp, #12]		@ the caller's R5, its length
	add	r5, r0, r5

	mov	r4, #0			@ entries put in the buffer
fs_func_collect_loop:
	cmp	r4, r10
	bhs	fs_func_done
	bl	fs_func_next_real
	bvs	fs_func_out
	cmp	r0, #0
	beq	fs_func_end

	mov	r6, r0

	@ The name comes first whichever reason this is, because how much room
	@ the entry needs depends on how long it turned out to be.
	mov	r0, r6
	bl	build_entry_name
	mov	r1, r0

	mov	r2, #0
fs_func_measure:
	ldrb	r3, [r1, r2]
	add	r2, r2, #1
	cmp	r3, #0
	bne	fs_func_measure
	add	r2, r2, #3
	bic	r2, r2, #3		@ the name, padded up to a word
	cmp	r9, #15
	addeq	r2, r2, #20		@ and the five words in front of it

	add	r3, r8, r2
	cmp	r3, r5
	bls	fs_func_room

	@ Out of room. Stop here and let FileSwitch ask again from the index
	@ being returned. If nothing has gone in at all the buffer cannot hold
	@ even one name, and saying "no more" is the only answer that does not
	@ leave it asking the same question for ever.
	cmp	r4, #0
	bne	fs_func_done
	b	fs_func_end

fs_func_room:
	cmp	r9, #15
	beq	fs_func_with_info

	@ Reason 14: just the name
	bl	copy_name_to_buffer
	b	fs_func_counted

fs_func_with_info:
	@ Reason 15: five words, then the name
	stmfd	sp!, {r1}		@ the name, which the words below scribble on
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
	ldmfd	sp!, {r1}
	bl	copy_name_to_buffer

fs_func_counted:
	add	r4, r4, #1
	add	r11, r11, #1
	b	fs_func_collect_loop

	@ R3 = how many, R4 = where to carry on from. Both are live registers that
	@ the exit below does not restore, so setting them is all that is needed -
	@ they used also to be written to [sp, #12] and [sp, #16] as "the caller's
	@ R3 and R4", which those slots are not: they hold the saved R5 and R6,
	@ and the stores handed FileSwitch a count where its own registers should
	@ have come back.
fs_func_done:
	mov	r3, r4
	mov	r4, r11
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

fs_func_end:
	mov	r3, r4
	mvn	r4, #0			@ -1: nothing left
	cmp	pc, #0
	ldmfd	sp!, {r0-r2, r5-r12, pc}

fs_func_notdir:
	adr	r0, err_not_dir
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

	/* Every branch here is an error path, and the error pointer is in R0.
	 * Reloading the entry's R0 over the top of it would hand FileSwitch a
	 * reason code where it expects an error block, so the saved R0-R2 are
	 * dropped instead. The free-space reasons leave by the same door, for the
	 * same reason: their answer is in R0-R4.
	 */
fs_func_out:
	add	sp, sp, #12
	ldmfd	sp!, {r5-r12, pc}

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


	/* FSEntry_Open - open a file.
	 *
	 *   In   R0 = reason: 0 read, 1 create and update, 2 update
	 *        R1 = name, R3 = FileSwitch's handle, R6 = special field
	 *   Out  R0 = information word, R1 = our handle, R2 = buffer size,
	 *        R3 = extent, R4 = space allocated
	 *
	 * Read only for now, and it says so rather than opening something it
	 * cannot honour: a file opened for update that silently discards writes
	 * is how data gets lost.
	 */
fs_open:
	stmfd	sp!, {r5-r12, lr}
	ldr	wp, [wp]

	cmp	r0, #2
	bhi	fs_open_readonly	@ reasons above 2 are not ours
	mov	r10, r0			@ 0 read, 1 create and update, 2 update


	stmfd	sp!, {r1}
	bl	current_volume
	ldmfd	sp!, {r1}
	bvs	fs_open_out
	mov	r7, r0

	@ NTFS opens for reading and nothing else. This uses the volume already
	@ fetched rather than asking again: calling current_volume a second time
	@ from here clobbered R10 - the open reason - so every write-open quietly
	@ became a read-open and the save above it failed with "Can't open".
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	bne	fs_open_notro
	cmp	r10, #0
	bne	fs_open_ro

fs_open_notro:
	mov	r0, r7			@ path_lookup wants the volume in R0

	bl	path_lookup
	bvs	fs_open_out
	cmp	r0, #1
	beq	fs_open_found

	@ Not there. Reason 1 means create it; anything else takes the answer.
	cmp	r10, #1
	bne	fs_open_none
	cmp	r0, #0
	bne	fs_open_none		@ it is a directory, not a missing file

	ldr	r1, =WS_LEAF_OK
	ldr	r1, [wp, r1]
	cmp	r1, #0
	beq	fs_open_none		@ something earlier in the path was missing

	mov	r0, r7
	ldr	r1, =WS_PARENT
	ldr	r1, [wp, r1]
	ldr	r2, =WS_COMP
	add	r2, wp, r2
	mov	r3, #0x20		@ a new file, marked as written to
	bl	dir_create
	bvs	fs_open_out

	@ dir_create says where the entry went; WS_FOUND has to agree, because
	@ everything below reads the file's details out of it.
	ldr	r2, =WS_FOUND_SEC
	str	r0, [wp, r2]
	ldr	r2, =WS_FOUND_OFF
	str	r1, [wp, r2]

	@ A brand new file: no cluster and no length, which is what the entry
	@ dir_create just wrote says too.
	ldr	r2, =WS_FOUND
	add	r2, wp, r2
	mov	r0, #0
	mov	r3, #0
fs_open_blank:
	strb	r0, [r2, r3]
	add	r3, r3, #1
	cmp	r3, #32
	blo	fs_open_blank

fs_open_found:

	@ A free slot
	add	r8, wp, #WS_FILES
	mov	r5, #0
fs_open_slot:
	ldr	r0, [r8, #FH_USED]
	cmp	r0, #0
	beq	fs_open_got_slot
	add	r8, r8, #FH_ENTRY
	add	r5, r5, #1
	cmp	r5, #MAX_OPEN
	blo	fs_open_slot
	b	fs_open_toomany

fs_open_got_slot:
	ldr	r6, =WS_FOUND
	add	r6, wp, r6

	mov	r0, #1
	str	r0, [r8, #FH_USED]
	str	r7, [r8, #FH_VOL]

	mov	r0, r6
	mov	r1, #20
	bl	ld16
	mov	r9, r0, lsl #16
	mov	r0, r6
	mov	r1, #26
	bl	ld16
	orr	r9, r9, r0
	str	r9, [r8, #FH_CLUSTER]

	mov	r0, r6
	mov	r1, #28
	bl	ld32
	str	r0, [r8, #FH_SIZE]

	@ Where the entry lives, so Close can put the size and first cluster back.
	ldr	r1, =WS_FOUND_SEC
	ldr	r1, [wp, r1]
	str	r1, [r8, #FH_DIRSEC]
	ldr	r1, =WS_FOUND_OFF
	ldr	r1, [wp, r1]
	str	r1, [r8, #FH_DIROFF]

	mov	r1, #0
	cmp	r10, #0
	movne	r1, #FHF_WRITE

	@ exFAT: carry the contiguity across. Without it every read past the
	@ first cluster follows FAT entries that were never maintained.
	ldr	r2, =WS_FOUND_CONTIG
	ldr	r2, [wp, r2]
	cmp	r2, #0
	orrne	r1, r1, #FHF_CONTIG
	str	r1, [r8, #FH_FLAGS]

	@ And where its entry set is, which is what Close has to rewrite.
	ldr	r1, =WS_FOUND_SETIDX
	ldr	r1, [wp, r1]
	str	r1, [r8, #FH_SETIDX]
	ldr	r1, =WS_FOUND_SETCNT
	ldr	r1, [wp, r1]
	str	r1, [r8, #FH_SETCNT]
	ldr	r1, =WS_PARENT
	ldr	r1, [wp, r1]
	str	r1, [r8, #FH_DIRCLUS]

	mov	r3, r0			@ extent
	mov	r4, r0			@ and all of it is allocated
	add	r1, r5, #1		@ our handle
	mov	r2, #512		@ buffer size FileSwitch should use
	@ Bit 30 says the file may be READ and bit 31 that it may be written -
	@ that way round, which is the opposite of the obvious guess. Claiming
	@ bit 31 alone gets "Not open for reading" on the first *Type.
	mov	r0, #(1 << 30)
	cmp	r10, #0
	orrne	r0, r0, #(1 << 31)
	cmp	pc, #0
	ldmfd	sp!, {r5-r12, pc}

fs_open_none:
	mov	r0, #0
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	mov	r4, #0
	cmp	pc, #0
	ldmfd	sp!, {r5-r12, pc}

fs_open_ro:
	adrl	r0, err_readonly_fs
	b	fs_open_setv

fs_open_readonly:
	adr	r0, err_readonly
	b	fs_open_setv

fs_open_toomany:
	adr	r0, err_toomany

fs_open_setv:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

fs_open_out:
	ldmfd	sp!, {r5-r12, pc}

err_readonly:
	.int	0xc9
	.string	"MultiFS is read only so far"
	.align
err_toomany:
	.int	0xc2
	.string	"Too many open files on MultiFS"
	.align
err_badhandle:
	.int	0xde
	.string	"Bad MultiFS file handle"
	.align

	.ltorg


	/* Turn a handle into its slot. Exit R0 = slot, or VS. */
handle_slot:
	stmfd	sp!, {r1, lr}
	subs	r0, r1, #1
	bmi	handle_slot_bad
	cmp	r0, #MAX_OPEN
	bhs	handle_slot_bad
	mov	r1, #FH_ENTRY
	mul	r0, r1, r0
	add	r0, wp, r0
	add	r0, r0, #WS_FILES
	ldr	r1, [r0, #FH_USED]
	cmp	r1, #0
	beq	handle_slot_bad
	cmp	pc, #0
	ldmfd	sp!, {r1, pc}
handle_slot_bad:
	adr	r0, err_badhandle
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, pc}


	/* FSEntry_GetBytes - read from an open file.
	 *
	 *   R1 = our handle, R2 = buffer, R3 = bytes wanted, R4 = file offset
	 *
	 * The chain is walked from the beginning on every call. That is honest
	 * rather than clever: a cache of the last position belongs here once
	 * something large is being read, and would be a bug farm before then.
	 */
fs_getbytes:
	stmfd	sp!, {r0-r12, lr}
	ldr	wp, [wp]
	mov	r5, #0			@ bytes copied so far

	bl	handle_slot
	bvs	fs_getbytes_out
	mov	r8, r0			@ slot

	cmp	r3, #0
	beq	fs_getbytes_done

	ldr	r7, [r8, #FH_VOL]
	ldr	r9, [r8, #FH_SIZE]

	@ NTFS keeps no clusters in the handle - a file there is an MFT record,
	@ and its contents may not have clusters at all if it is small enough to
	@ live inside that record.
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #128
	beq	fs_getbytes_ntfs

	@ Nothing past the end
	cmp	r4, r9
	bhs	fs_getbytes_done
	sub	r0, r9, r4
	cmp	r3, r0
	movhi	r3, r0

	@ Which cluster the offset falls in, and where inside it
	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9		@ bytes per cluster, as a shift
	mov	r10, r4, lsr r0		@ clusters to skip
	mov	r1, #1
	mov	r1, r1, lsl r0
	sub	r1, r1, #1
	and	r11, r4, r1		@ offset within the cluster

	ldr	r6, [r8, #FH_CLUSTER]
fs_getbytes_skip:
	cmp	r10, #0
	beq	fs_getbytes_ready
	mov	r0, r7
	mov	r1, r6
	ldr	r2, [r8, #FH_FLAGS]
	and	r2, r2, #FHF_CONTIG
	bl	chain_next
	bvs	fs_getbytes_out
	cmp	r0, #0
	beq	fs_getbytes_done	@ chain shorter than the size claims
	mov	r6, r0
	sub	r10, r10, #1
	b	fs_getbytes_skip

fs_getbytes_ready:
	mov	r5, #0			@ bytes copied

fs_getbytes_loop:
	cmp	r5, r3
	bhs	fs_getbytes_done

	@ Sector holding the current position, and the offset within it
	mov	r0, r7
	mov	r1, r6
	bl	cluster_sector
	add	r0, r0, r11, lsr #9
	mov	r1, r0
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	fs_getbytes_out

	@ Nothing in here may touch R12: that is wp, and the workspace address is
	@ needed on every turn of this loop. Using it as a scratch register for
	@ one byte of the copy destroyed the workspace on the first iteration,
	@ which read as an empty file and then as a hung machine.
	ldr	r0, =511
	and	r10, r11, r0		@ offset within the sector
	rsb	r0, r10, #512		@ how much of it is left
	sub	r1, r3, r5		@ how much is still wanted
	cmp	r0, r1
	movhi	r0, r1			@ copy the smaller

	@ Copy it out
	ldr	r1, [sp, #8]		@ the caller's R2, the destination
	add	r1, r1, r5
	add	r2, wp, #WS_FILESEC
	add	r2, r2, r10
	mov	r10, #0
fs_getbytes_copy:
	ldrb	r4, [r2, r10]
	strb	r4, [r1, r10]
	add	r10, r10, #1
	cmp	r10, r0
	blo	fs_getbytes_copy

	add	r5, r5, r0
	add	r11, r11, r0

	@ Off the end of this cluster?
	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9
	mov	r1, #1
	mov	r1, r1, lsl r0
	cmp	r11, r1
	blo	fs_getbytes_loop

	sub	r11, r11, r1
	mov	r0, r7
	mov	r1, r6
	ldr	r2, [r8, #FH_FLAGS]
	and	r2, r2, #FHF_CONTIG
	bl	chain_next
	bvs	fs_getbytes_out
	cmp	r0, #0
	beq	fs_getbytes_done
	mov	r6, r0
	b	fs_getbytes_loop

fs_getbytes_done:
	@ FileSwitch wants to be told how many bytes did NOT arrive, and it asks
	@ for more than the file holds as a matter of course - 1088 bytes of a
	@ 77 byte file, in the first case this was tried on. Returning R3
	@ untouched says "none of them arrived", and *Type prints nothing at all
	@ while every layer below reports success.
	ldr	r0, [sp, #12]		@ the caller's R3, what was asked for
	subs	r0, r0, r5
	movmi	r0, #0
	str	r0, [sp, #12]
	cmp	pc, #0

fs_getbytes_out:
	ldmfd	sp!, {r0-r12, pc}

	/* NTFS: hand the whole thing to the reader, which knows about resident
	 * data and run lists. R5 is the count delivered, which is what
	 * fs_getbytes_done turns into "how many did not arrive".
	 */
fs_getbytes_ntfs:
	cmp	r4, r9
	bhs	fs_getbytes_done
	sub	r0, r9, r4
	cmp	r3, r0
	movhi	r3, r0			@ never past the extent

	mov	r6, r2			@ the buffer
	mov	r10, r3			@ how many
	mov	r0, r7
	ldr	r1, [r8, #FH_CLUSTER]	@ the MFT record, where FAT keeps a cluster
	mov	r2, r4			@ the offset in the file
	mov	r3, r6
	mov	r4, r10
	bl	ntfs_read_bytes
	bvs	fs_getbytes_out
	mov	r5, r0
	b	fs_getbytes_done


	.ltorg


	/* FSEntry_Close - R1 = our handle. */
fs_close:
	stmfd	sp!, {r0-r2, r12, lr}
	ldr	wp, [wp]
	bl	handle_slot
	bvs	fs_close_out

	@ The size and first cluster live in the directory entry, and nothing has
	@ put them there yet: until this runs, a file that was just written is
	@ bytes on a disc that nothing can find.
	bl	dir_writeback
	bvs	fs_close_out

	mov	r1, #0
	str	r1, [r0, #FH_USED]
	cmp	pc, #0
fs_close_out:
	ldmfd	sp!, {r0-r2, r12, pc}




	/* Walk a directory's sectors, one at a time.
	 *
	 * Entry: R0 = volume, R1 = directory cluster (0 = the root), R2 = which
	 *        sector of the directory, counting from zero.
	 * Exit:  R0 = its absolute LBA, or 0 when the directory has no such
	 *        sector; V set on a read error.
	 *
	 * The fixed root of a FAT16 volume is a run of sectors and cannot grow;
	 * everything else is a cluster chain. Both are asked the same question
	 * here so that the callers below need not care which they have.
	 */
dir_sector_at:
	stmfd	sp!, {r1-r7, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ directory
	mov	r7, r2			@ which sector

	cmp	r6, #0
	bne	dir_sector_chain

	ldr	r0, [r5, #VOL_TYPE]
	cmp	r0, #32
	bhs	dir_sector_root32	@ FAT32 and exFAT: the root is a chain

	@ FAT16's fixed root: a flat run, and there is no more of it than there is
	ldr	r0, [r5, #VOL_ROOTSECS]
	cmp	r7, r0
	bhs	dir_sector_none
	ldr	r0, [r5, #VOL_ROOTSEC0]
	add	r0, r0, r7
	b	dir_sector_ok

dir_sector_root32:
	ldr	r6, [r5, #VOL_ROOTCLUS]

dir_sector_chain:
	@ Which cluster of the chain, and which sector inside it.
	ldr	r0, [r5, #VOL_SPCLOG]
	mov	r1, r7, lsr r0		@ clusters to step over
	mov	r2, #1
	mov	r2, r2, lsl r0
	sub	r2, r2, #1
	and	r4, r7, r2		@ sector within the cluster

dir_sector_step:
	cmp	r1, #0
	beq	dir_sector_have
	mov	r0, r5
	stmfd	sp!, {r1}
	mov	r1, r6
	bl	fat_next
	ldmfd	sp!, {r1}
	bvs	dir_sector_out
	cmp	r0, #0
	beq	dir_sector_none		@ the directory stops before here
	mov	r6, r0
	sub	r1, r1, #1
	b	dir_sector_step

dir_sector_have:
	mov	r0, r5
	mov	r1, r6
	bl	cluster_sector
	add	r0, r0, r4

dir_sector_ok:
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

dir_sector_none:
	mov	r0, #0
	cmp	pc, #0

dir_sector_out:
	ldmfd	sp!, {r1-r7, pc}


	/* Add one cluster to a directory and clear it.
	 *
	 * Entry: R0 = volume, R1 = directory cluster (not the root of a FAT16).
	 * Exit:  V set on failure.
	 *
	 * A new directory cluster has to be zeroed before it is linked, not
	 * after: an entry read out of whatever the cluster held before is a file
	 * that never existed, and something would go looking for it.
	 */
dir_extend:
	stmfd	sp!, {r0-r8, lr}

	mov	r5, r0
	mov	r6, r1

	@ Find the end of the chain.
dir_extend_end:
	mov	r0, r5
	mov	r1, r6
	bl	fat_next
	bvs	dir_extend_out
	cmp	r0, #0
	beq	dir_extend_add
	mov	r6, r0
	b	dir_extend_end

dir_extend_add:
	mov	r0, r5
	bl	alloc_cluster
	bvs	dir_extend_out
	mov	r7, r0

	@ Clear every sector of it before anything can see it.
	add	r0, wp, #WS_FILESEC
	mov	r1, #0
	mov	r2, #512
dir_extend_zero:
	subs	r2, r2, #4
	str	r1, [r0, r2]
	bne	dir_extend_zero

	ldr	r8, [r5, #VOL_SPCLOG]
	mov	r0, #1
	mov	r8, r0, lsl r8		@ sectors in a cluster
	mov	r0, r5
	mov	r1, r7
	bl	cluster_sector
	mov	r4, r0
dir_extend_clear:
	mov	r1, r4
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	dir_extend_out
	add	r4, r4, #1
	subs	r8, r8, #1
	bne	dir_extend_clear

	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	bl	fat_set
	bvs	dir_extend_out
	cmp	pc, #0

dir_extend_out:
	ldmfd	sp!, {r0-r8, pc}



	/* Where the Nth entry of a directory lives.
	 *
	 * Entry: R0 = volume, R1 = directory cluster (0 = root), R2 = index.
	 * Exit:  R0 = sector, 0 if the directory does not reach that far;
	 *        R1 = byte offset within it. V set on a read error.
	 */
dir_entry_at:
	stmfd	sp!, {r2-r5, lr}
	and	r5, r2, #15		@ sixteen 32-byte entries to a sector
	mov	r2, r2, lsr #4
	bl	dir_sector_at
	mov	r1, r5, lsl #5
	ldmfd	sp!, {r2-r5, pc}


	/* Read one entry out of a directory.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = index, R3 = 32-byte buffer.
	 * Exit:  R0 = 1 if there was one, 0 if the directory ends first.
	 */
dir_read_entry:
	stmfd	sp!, {r1-r8, lr}

	mov	r6, r0			@ volume
	mov	r8, r3			@ destination
	bl	dir_entry_at
	bvs	dir_read_entry_out
	cmp	r0, #0
	beq	dir_read_entry_none
	mov	r7, r1			@ offset

	mov	r1, r0
	ldr	r0, [r6, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	dir_read_entry_out

	add	r1, wp, #WS_FILESEC
	add	r1, r1, r7
	mov	r2, #0
dir_read_entry_copy:
	ldrb	r0, [r1, r2]
	strb	r0, [r8, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	dir_read_entry_copy

	mov	r0, #1
	cmp	pc, #0
	ldmfd	sp!, {r1-r8, pc}

dir_read_entry_none:
	mov	r0, #0
	cmp	pc, #0

dir_read_entry_out:
	ldmfd	sp!, {r1-r8, pc}


	/* Write one entry into a directory, leaving the rest of the sector as it
	 * was.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = index, R3 = the 32 bytes.
	 * Exit:  R0 = the sector it went in, R1 = the offset. V set on failure.
	 */
dir_write_entry:
	stmfd	sp!, {r2-r8, lr}

	mov	r6, r0
	mov	r8, r3
	bl	dir_entry_at
	bvs	dir_write_entry_out
	cmp	r0, #0
	beq	dir_write_entry_off_end
	mov	r7, r1
	mov	r5, r0			@ the sector

	mov	r1, r5
	ldr	r0, [r6, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	dir_write_entry_out

	add	r1, wp, #WS_FILESEC
	add	r1, r1, r7
	mov	r2, #0
dir_write_entry_copy:
	ldrb	r0, [r8, r2]
	strb	r0, [r1, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	dir_write_entry_copy

	mov	r1, r5
	ldr	r0, [r6, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	dir_write_entry_out

	mov	r0, r5
	mov	r1, r7
	cmp	pc, #0
	ldmfd	sp!, {r2-r8, pc}

dir_write_entry_off_end:
	adrl	r0, err_dir_full
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

dir_write_entry_out:
	ldmfd	sp!, {r2-r8, pc}

err_dir_full:
	.int	0xb3
	.string	"That directory on the USB disc is full"
	.align


	/* Find a run of free entries, making the directory longer if it has none.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = how many in a row.
	 * Exit:  R0 = the index the run starts at. V set on failure.
	 *
	 * A first byte of zero means this entry and every one after it has never
	 * been used, so a run can be counted from there without reading on; &E5
	 * means deleted and is equally free. Anything else is in use and the count
	 * starts again.
	 */
dir_find_run:
	stmfd	sp!, {r1-r10, lr}

	mov	r6, r0			@ volume
	mov	r7, r1			@ directory
	mov	r8, r2			@ how many wanted
	mov	r9, #0			@ index
	mov	r10, #0			@ how many free in a row so far
	sub	sp, sp, #32		@ somewhere to read an entry

dir_find_run_loop:
	mov	r0, r6
	mov	r1, r7
	mov	r2, r9
	mov	r3, sp
	bl	dir_read_entry
	bvs	dir_find_run_out
	cmp	r0, #0
	beq	dir_find_run_grow	@ ran off the end of the directory

	@ What counts as a free slot differs: FAT marks a deleted entry &E5,
	@ exFAT clears the top bit of the type byte. Zero ends the directory in
	@ both, and everything from there on is free.
	ldrb	r0, [sp]
	cmp	r0, #0
	beq	dir_find_run_free
	ldr	r1, [r6, #VOL_TYPE]
	cmp	r1, #64
	beq	dir_find_run_exfat
	cmp	r0, #0xe5
	beq	dir_find_run_free
	b	dir_find_run_used

dir_find_run_exfat:
	tst	r0, #0x80
	beq	dir_find_run_free

dir_find_run_used:
	mov	r10, #0			@ in use: start counting again
	b	dir_find_run_next

dir_find_run_free:
	add	r10, r10, #1
	cmp	r10, r8
	bhs	dir_find_run_found

dir_find_run_next:
	add	r9, r9, #1
	b	dir_find_run_loop

dir_find_run_found:
	sub	r0, r9, r8
	add	r0, r0, #1		@ the index the run starts at
	add	sp, sp, #32
	cmp	pc, #0
	ldmfd	sp!, {r1-r10, pc}

dir_find_run_grow:
	@ The directory has run out. A FAT16 root cannot grow; anything else can.
	cmp	r7, #0
	bne	dir_find_run_extend
	ldr	r0, [r6, #VOL_TYPE]
	cmp	r0, #32
	bne	dir_find_run_full

dir_find_run_extend:
	mov	r0, r6
	mov	r1, r7
	cmp	r1, #0
	ldreq	r1, [r6, #VOL_ROOTCLUS]
	bl	dir_extend
	bvs	dir_find_run_out
	b	dir_find_run_loop

dir_find_run_full:
	adrl	r0, err_dir_full
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

dir_find_run_out:
	add	sp, sp, #32
	ldmfd	sp!, {r1-r10, pc}


	/* Zero every sector of a cluster.
	 *
	 * Entry: R0 = volume, R1 = the cluster.
	 * Exit:  V set on failure.
	 *
	 * A new directory has to be blank before anything can look at it: an
	 * entry read out of whatever the cluster held before is a file that never
	 * existed, and something would go looking for it.
	 */
dir_clear_cluster:
	stmfd	sp!, {r1-r8, lr}

	mov	r5, r0
	mov	r6, r1

	add	r0, wp, #WS_FILESEC
	mov	r1, #0
	mov	r2, #512
dir_clear_cluster_zero:
	subs	r2, r2, #4
	str	r1, [r0, r2]
	bne	dir_clear_cluster_zero

	ldr	r8, [r5, #VOL_SPCLOG]
	mov	r0, #1
	mov	r8, r0, lsl r8		@ sectors in a cluster
	mov	r0, r5
	mov	r1, r6
	bl	cluster_sector
	mov	r4, r0

dir_clear_cluster_loop:
	mov	r1, r4
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	dir_clear_cluster_out
	add	r4, r4, #1
	subs	r8, r8, #1
	bne	dir_clear_cluster_loop
	cmp	pc, #0

dir_clear_cluster_out:
	ldmfd	sp!, {r1-r8, pc}


	/* Has this directory anything in it?
	 *
	 * Entry: R0 = volume, R1 = its cluster.
	 * Exit:  R0 = 1 if it is empty, 0 if it is not. V set on a read error.
	 *
	 * dir_next already passes over "." and ".." and the deleted slots, so
	 * anything it hands back at all counts - except the volume label, which
	 * is not a child of anything. An orphaned fragment will read as "not
	 * empty", which refuses a delete that might have been allowed; that is
	 * the safe way round.
	 */
dir_is_empty:
	stmfd	sp!, {r1-r4, lr}

	ldr	r2, =WS_FOUND_CONTIG
	ldr	r2, [wp, r2]
	cmp	r1, #0
	moveq	r2, #0
	bl	dir_open
	bvs	dir_is_empty_out

dir_is_empty_loop:
	bl	dir_next
	bvs	dir_is_empty_out
	cmp	r0, #0
	beq	dir_is_empty_yes

	ldrb	r1, [r0, #11]
	tst	r1, #0x08
	bne	dir_is_empty_loop	@ the volume label

	mov	r0, #0
	cmp	pc, #0
	ldmfd	sp!, {r1-r4, pc}

dir_is_empty_yes:
	mov	r0, #1
	cmp	pc, #0

dir_is_empty_out:
	ldmfd	sp!, {r1-r4, pc}


	/* The time now, packed the way a directory entry wants it.
	 *
	 * Exit: R0 = time, hours<<11 | minutes<<5 | seconds/2
	 *       R1 = date, (year-1980)<<9 | month<<5 | day
	 *
	 * Never fails. A clock that will not answer gives zero, which is what FAT
	 * itself means by "no date" and is a great deal better than refusing to
	 * create a file because the territory module was in a mood.
	 *
	 * FAT records LOCAL time - it has no field to say otherwise - so the zone
	 * offset goes on before the value is broken up. Territory gives that
	 * offset in centiseconds, which is the unit the five-byte value counts in,
	 * so it is one 40-bit addition rather than a conversion. Stamping UTC here
	 * instead would be the same ordinals call, only the value handed to it
	 * differing.
	 *
	 * Local is what the FAT specification says and what Windows both writes
	 * and expects, so it is what we write. Be ready for Linux to disagree:
	 * with no "tz=" mount option the kernel uses sys_tz, which on a machine
	 * whose hardware clock is UTC is zero, so it writes and reads FAT stamps
	 * as UTC and shows ours an hour ahead every British summer. Checked on
	 * this machine 16/08/2026: Linux touched a file at 09:37:32 BST and put
	 * 08:37:32 on the medium, where we put 09:36:34 for 09:36:34 BST. That is
	 * a well-known Linux/Windows disagreement about FAT and not a fault here -
	 * before "correcting" it, mount with tz=UTC and see which one moves.
	 */
fat_now:
	stmfd	sp!, {r2-r9, lr}
	sub	sp, sp, #48

	mov	r0, #0
	str	r0, [sp]
	str	r0, [sp, #4]

	mov	r0, #OSWord_ReadClock
	mov	r1, sp
	mov	r2, #ReadClock_UTC
	strb	r2, [r1]
	swi	XOS_Word
	bvs	fat_now_none

	@ The offset comes back in R1, NOT R0: R0 is a pointer to the zone's name.
	@ Reading it out of R0 stamped every file with a date seven days adrift,
	@ because the pointer was being added to the clock as a centisecond count.
	swi	XTerritory_ReadCurrentTimeZone
	movvs	r1, #0			@ no territory: UTC will have to do
	mov	r0, r1

	@ R0 is signed centiseconds, so the top word of it is its sign bit.
	ldr	r2, [sp]
	ldrb	r3, [sp, #4]
	adds	r2, r2, r0
	adc	r3, r3, r0, asr #31
	str	r2, [sp]
	strb	r3, [sp, #4]

	mvn	r0, #0			@ the current territory
	mov	r1, sp
	add	r2, sp, #8
	swi	XTerritory_ConvertTimeToUTCOrdinals
	bvs	fat_now_none

	@ The ordinals arrive as words: centiseconds, seconds, minutes, hours,
	@ day, month, year.
	ldr	r4, [sp, #(8 + 4)]
	ldr	r5, [sp, #(8 + 8)]
	ldr	r6, [sp, #(8 + 12)]
	ldr	r7, [sp, #(8 + 16)]
	ldr	r8, [sp, #(8 + 20)]
	ldr	r9, [sp, #(8 + 24)]

	mov	r0, r4, lsr #1		@ seconds go in twos, which is FAT all over
	and	r0, r0, #0x1f
	and	r1, r5, #0x3f
	orr	r0, r0, r1, lsl #5
	and	r1, r6, #0x1f
	orr	r0, r0, r1, lsl #11

	and	r1, r7, #0x1f
	and	r2, r8, #0x0f
	orr	r1, r1, r2, lsl #5
	@ The FAT epoch is 1980, which no single ARM immediate can express.
	sub	r2, r9, #1920
	subs	r2, r2, #60
	movmi	r2, #0
	cmp	r2, #127
	movhi	r2, #0
	orr	r1, r1, r2, lsl #9

	add	sp, sp, #48
	ldmfd	sp!, {r2-r9, pc}

fat_now_none:
	mov	r0, #0
	mov	r1, #0
	add	sp, sp, #48
	ldmfd	sp!, {r2-r9, pc}


	/* A RISC OS load and exec pair for an entry.
	 *
	 * Entry: R0 = the 32-byte entry.
	 * Exit:  R0 = load address, R1 = exec address.
	 *
	 * The last-written stamp is the one reported, which is what RISC OS means
	 * by a file's date. FAT keeps it as local time and RISC OS keeps a UTC
	 * centisecond count, and Territory_ConvertOrdinalsToTime is the call that
	 * bridges them: it reads its ordinals as LOCAL and takes the zone off on
	 * the way out, which is exactly the direction wanted here. SyncClock
	 * relies on the same behaviour and says so.
	 *
	 * A date that will not convert is not worth an error - the file is still
	 * perfectly readable - so it comes back as a typed file with a zero
	 * stamp, which is what an undated file looks like anyway.
	 */
entry_load_exec:
	stmfd	sp!, {r2-r9, lr}
	sub	sp, sp, #48

	mov	r9, r0

	mov	r0, r9
	bl	ftype_of
	mov	r8, r0			@ the type

	mov	r0, r9
	mov	r1, #22			@ last written: time
	bl	ld16
	mov	r6, r0
	mov	r0, r9
	mov	r1, #24			@ and date
	bl	ld16
	mov	r7, r0

	cmp	r7, #0
	beq	entry_load_exec_none	@ never stamped

	mov	r0, #0
	str	r0, [sp, #ORD_CS]
	and	r0, r6, #0x1f
	mov	r0, r0, lsl #1		@ seconds are counted in twos
	str	r0, [sp, #ORD_SECOND]
	mov	r0, r6, lsr #5
	and	r0, r0, #0x3f
	str	r0, [sp, #ORD_MINUTE]
	mov	r0, r6, lsr #11
	and	r0, r0, #0x1f
	str	r0, [sp, #ORD_HOUR]
	and	r0, r7, #0x1f
	str	r0, [sp, #ORD_DAY]
	mov	r0, r7, lsr #5
	and	r0, r0, #0x0f
	str	r0, [sp, #ORD_MONTH]
	mov	r0, r7, lsr #9
	and	r0, r0, #0x7f
	add	r0, r0, #1920
	add	r0, r0, #60		@ 1980, which no immediate can carry
	str	r0, [sp, #ORD_YEAR]

	mvn	r0, #0			@ the current territory
	add	r1, sp, #40		@ five bytes out
	add	r2, sp, #0		@ the ordinals in
	swi	XTerritory_ConvertOrdinalsToTime
	bvs	entry_load_exec_none

	ldr	r1, [sp, #40]		@ the low four bytes go in the exec word
	ldrb	r0, [sp, #44]		@ and the fifth in the load word

	orr	r0, r0, r8, lsl #8
	ldr	r2, =0xfff00000
	orr	r0, r0, r2

	add	sp, sp, #48
	ldmfd	sp!, {r2-r9, pc}

entry_load_exec_none:
	mov	r0, r8, lsl #8
	ldr	r2, =0xfff00000
	orr	r0, r0, r2
	mov	r1, #0
	add	sp, sp, #48
	ldmfd	sp!, {r2-r9, pc}


	/* Reach a given cluster of a file, bringing it into existence.
	 *
	 * Entry: R0 = volume, R1 = first cluster (0 if the file has none yet),
	 *        R2 = which cluster of the file is wanted, counting from zero.
	 * Exit:  R0 = that cluster, R1 = the first cluster, which may have just
	 *        been allocated; V set if the disc is full or a read failed.
	 */
chain_reach:
	stmfd	sp!, {r2-r8, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ first cluster
	mov	r7, r2			@ how far along

	cmp	r6, #0
	bne	chain_reach_first
	mov	r0, r5
	bl	alloc_cluster
	bvs	chain_reach_out
	mov	r6, r0

chain_reach_first:
	mov	r4, r6			@ where we are
	mov	r3, #0			@ and how far along that is

chain_reach_loop:
	cmp	r3, r7
	beq	chain_reach_done

	mov	r0, r5
	mov	r1, r4
	bl	fat_next
	bvs	chain_reach_out
	cmp	r0, #0
	bne	chain_reach_step

	@ The chain stops here and the file needs to go further, so lengthen it.
	mov	r0, r5
	bl	alloc_cluster
	bvs	chain_reach_out
	mov	r8, r0
	mov	r0, r5
	mov	r1, r4
	mov	r2, r8
	bl	fat_set
	bvs	chain_reach_out
	mov	r0, r8

chain_reach_step:
	mov	r4, r0
	add	r3, r3, #1
	b	chain_reach_loop

chain_reach_done:
	mov	r0, r4
	mov	r1, r6
	cmp	pc, #0

chain_reach_out:
	ldmfd	sp!, {r2-r8, pc}


	/* Put a handle's size and first cluster back into its directory entry.
	 *
	 * Entry: R0 = the handle slot.
	 *
	 * Nothing on the medium says how long a file is except this entry, so
	 * until this runs a written file is bytes on a disc that nothing can
	 * find. It is called from Close and from Args 3, and does nothing unless
	 * the handle is marked dirty.
	 */
dir_writeback:
	stmfd	sp!, {r0-r7, lr}

	mov	r6, r0
	ldr	r0, [r6, #FH_FLAGS]
	tst	r0, #FHF_DIRTY
	beq	dir_writeback_done

	ldr	r7, [r6, #FH_VOL]
	ldr	r0, [r7, #VOL_TYPE]
	cmp	r0, #64
	beq	dir_writeback_exfat

	ldr	r1, [r6, #FH_DIRSEC]
	cmp	r1, #0
	beq	dir_writeback_done	@ no entry to speak of

	ldr	r7, [r6, #FH_VOL]
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	dir_writeback_out

	add	r5, wp, #WS_FILESEC
	ldr	r0, [r6, #FH_DIROFF]
	add	r5, r5, r0

	@ The first cluster is split across the entry: the high half at 20 and
	@ the low half at 26, which is FAT32 bolted onto a FAT16 layout.
	ldr	r0, [r6, #FH_CLUSTER]
	mov	r1, r0, lsr #16
	strb	r1, [r5, #20]
	mov	r1, r0, lsr #24
	strb	r1, [r5, #21]
	strb	r0, [r5, #26]
	mov	r1, r0, lsr #8
	strb	r1, [r5, #27]

	ldr	r0, [r6, #FH_SIZE]
	strb	r0, [r5, #28]
	mov	r1, r0, lsr #8
	strb	r1, [r5, #29]
	mov	r1, r0, lsr #16
	strb	r1, [r5, #30]
	mov	r1, r0, lsr #24
	strb	r1, [r5, #31]

	@ Say it has been written to, which is what every other system expects
	@ to find after a change, and say when.
	ldrb	r0, [r5, #11]
	orr	r0, r0, #0x20
	strb	r0, [r5, #11]

	bl	fat_now
	strb	r0, [r5, #22]
	mov	r2, r0, lsr #8
	strb	r2, [r5, #23]
	strb	r1, [r5, #24]
	mov	r2, r1, lsr #8
	strb	r2, [r5, #25]
	strb	r1, [r5, #18]		@ and that it was touched today
	strb	r2, [r5, #19]

	ldr	r1, [r6, #FH_DIRSEC]
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	dir_writeback_out

	ldr	r0, [r6, #FH_FLAGS]
	bic	r0, r0, #FHF_DIRTY
	str	r0, [r6, #FH_FLAGS]

dir_writeback_done:
	cmp	pc, #0

dir_writeback_out:
	ldmfd	sp!, {r0-r7, pc}

	/* The same for exFAT, where the length and the first cluster live in the
	 * Stream Extension rather than the entry that carries the name, and where
	 * changing any byte means the set's checksum has to be worked out again.
	 */
dir_writeback_exfat:
	ldr	r0, [r6, #FH_SETCNT]
	cmp	r0, #0
	beq	dir_writeback_done	@ nothing recorded: leave the medium alone

	mov	r0, r7
	ldr	r1, [r6, #FH_DIRCLUS]
	ldr	r2, [r6, #FH_SETIDX]
	ldr	r3, [r6, #FH_SETCNT]
	bl	exfat_set_read
	bvs	dir_writeback_out

	ldr	r0, =WS_XSET
	add	r5, wp, r0
	add	r4, r5, #32		@ the stream extension

	ldr	r0, [r6, #FH_CLUSTER]
	strb	r0, [r4, #20]
	mov	r1, r0, lsr #8
	strb	r1, [r4, #21]
	mov	r1, r0, lsr #16
	strb	r1, [r4, #22]
	mov	r1, r0, lsr #24
	strb	r1, [r4, #23]

	@ DataLength and ValidDataLength both, and both 64-bit: the high half has
	@ to be cleared or a file that was once long stays long.
	ldr	r0, [r6, #FH_SIZE]
	strb	r0, [r4, #24]
	mov	r1, r0, lsr #8
	strb	r1, [r4, #25]
	mov	r1, r0, lsr #16
	strb	r1, [r4, #26]
	mov	r1, r0, lsr #24
	strb	r1, [r4, #27]
	strb	r0, [r4, #8]
	mov	r1, r0, lsr #8
	strb	r1, [r4, #9]
	mov	r1, r0, lsr #16
	strb	r1, [r4, #10]
	mov	r1, r0, lsr #24
	strb	r1, [r4, #11]
	mov	r1, #0
	strb	r1, [r4, #28]
	strb	r1, [r4, #29]
	strb	r1, [r4, #30]
	strb	r1, [r4, #31]
	strb	r1, [r4, #12]
	strb	r1, [r4, #13]
	strb	r1, [r4, #14]
	strb	r1, [r4, #15]

	@ Say it has been written to, and when.
	ldrb	r0, [r5, #4]
	orr	r0, r0, #0x20
	strb	r0, [r5, #4]

	bl	fat_now
	orr	r2, r0, r1, lsl #16
	strb	r2, [r5, #12]
	mov	r3, r2, lsr #8
	strb	r3, [r5, #13]
	mov	r3, r2, lsr #16
	strb	r3, [r5, #14]
	mov	r3, r2, lsr #24
	strb	r3, [r5, #15]

	mov	r0, r7
	ldr	r1, [r6, #FH_DIRCLUS]
	ldr	r2, [r6, #FH_SETIDX]
	ldr	r3, [r6, #FH_SETCNT]
	bl	exfat_set_write
	bvs	dir_writeback_out

	ldr	r0, [r6, #FH_FLAGS]
	bic	r0, r0, #FHF_DIRTY
	str	r0, [r6, #FH_FLAGS]
	cmp	pc, #0
	ldmfd	sp!, {r0-r7, pc}


	/* FSEntry_PutBytes - write to an open file.
	 *
	 *   R1 = our handle, R2 = buffer, R3 = bytes, R4 = file offset
	 *
	 * Whole sectors are still read before they are written. Writing a part
	 * of a sector obviously has to, and doing it for every sector costs one
	 * read where it need not - but a file written at an offset that happens
	 * to be sector aligned is the exception rather than the rule, and one
	 * code path that is always right is worth more here than two where the
	 * fast one is wrong at the edges.
	 */
fs_putbytes:
	stmfd	sp!, {r0-r12, lr}
	ldr	wp, [wp]

	bl	handle_slot
	bvs	fs_putbytes_out
	mov	r8, r0

	ldr	r0, [r8, #FH_FLAGS]
	tst	r0, #FHF_WRITE
	beq	fs_putbytes_readonly

	cmp	r3, #0
	beq	fs_putbytes_done

	ldr	r7, [r8, #FH_VOL]

	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9		@ bytes per cluster, as a shift
	mov	r10, r4, lsr r0		@ which cluster of the file
	mov	r1, #1
	mov	r1, r1, lsl r0
	sub	r1, r1, #1
	and	r11, r4, r1		@ and where inside it

	mov	r0, r7
	ldr	r1, [r8, #FH_CLUSTER]
	mov	r2, r10
	bl	chain_reach
	bvs	fs_putbytes_out
	str	r1, [r8, #FH_CLUSTER]
	mov	r6, r0

	mov	r5, #0			@ bytes written so far

fs_putbytes_loop:
	cmp	r5, r3
	bhs	fs_putbytes_grow

	mov	r0, r7
	mov	r1, r6
	bl	cluster_sector
	add	r0, r0, r11, lsr #9
	mov	r9, r0			@ the sector being changed

	mov	r1, r9
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	fs_putbytes_out

	ldr	r0, =511
	and	r10, r11, r0		@ offset within the sector
	rsb	r0, r10, #512		@ room to the end of it
	sub	r1, r3, r5		@ still to write
	cmp	r0, r1
	movhi	r0, r1

	@ R12 is wp and must not be touched here, for the reason GetBytes gives.
	ldr	r1, [sp, #8]		@ the caller's R2, the source
	add	r1, r1, r5
	add	r2, wp, #WS_FILESEC
	add	r2, r2, r10
	mov	r10, #0
fs_putbytes_copy:
	ldrb	r4, [r1, r10]
	strb	r4, [r2, r10]
	add	r10, r10, #1
	cmp	r10, r0
	blo	fs_putbytes_copy

	stmfd	sp!, {r0}
	mov	r1, r9
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	ldmfd	sp!, {r0}
	bvs	fs_putbytes_out

	add	r5, r5, r0
	add	r11, r11, r0

	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9
	mov	r1, #1
	mov	r1, r1, lsl r0
	cmp	r11, r1
	blo	fs_putbytes_loop

	@ Over the end of this cluster and into the next, making one if the file
	@ has never been this long before.
	sub	r11, r11, r1
	mov	r0, r7
	mov	r1, r6
	bl	fat_next
	bvs	fs_putbytes_out
	cmp	r0, #0
	bne	fs_putbytes_next

	mov	r0, r7
	bl	alloc_cluster
	bvs	fs_putbytes_out
	mov	r9, r0
	mov	r0, r7
	mov	r1, r6
	mov	r2, r9
	bl	fat_set
	bvs	fs_putbytes_out
	mov	r0, r9

fs_putbytes_next:
	mov	r6, r0
	b	fs_putbytes_loop

fs_putbytes_grow:
	ldr	r0, [sp, #16]		@ the caller's R4, the file offset
	add	r0, r0, r3
	ldr	r1, [r8, #FH_SIZE]
	cmp	r0, r1
	strhi	r0, [r8, #FH_SIZE]

	ldr	r0, [r8, #FH_FLAGS]
	orr	r0, r0, #FHF_DIRTY
	str	r0, [r8, #FH_FLAGS]

fs_putbytes_done:
	cmp	pc, #0
	ldmfd	sp!, {r0-r12, pc}

fs_putbytes_readonly:
	adrl	r0, err_readonly
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT

fs_putbytes_out:
	ldmfd	sp!, {r0-r12, pc}

	.ltorg

	/* FSEntry_Args - R0 = reason, R1 = our handle.
	 *
	 * 2 and 4 are the extent and the allocated size, which is all a reader
	 * asks for; the rest are accepted and left alone.
	 */
fs_args:
	stmfd	sp!, {r1, r3-r12, lr}
	ldr	wp, [wp]

	cmp	r0, #3
	beq	fs_args_setextent

	cmp	r0, #2
	cmpne	r0, #4
	bne	fs_args_ignore

	bl	handle_slot
	bvs	fs_args_out
	ldr	r2, [r0, #FH_SIZE]
	cmp	pc, #0
	ldmfd	sp!, {r1, r3-r12, pc}

	/* Reason 3: set the extent, with R2 the new length.
	 *
	 * FileSwitch writes through a buffer and then says how long the file
	 * really is, so without this a file came out the size of the buffer
	 * rather than the size of its contents - 512 bytes of which only the
	 * first nineteen were the ones asked for, and the rest whatever the
	 * cluster held before.
	 *
	 * Shrinking gives the clusters past the new end back, rather than
	 * leaving them attached to a chain nothing reads: they are lost space
	 * otherwise, and lost space on a removable disc turns up as a stick that
	 * mysteriously fills.
	 */
fs_args_setextent:
	mov	r4, r2			@ the new extent
	bl	handle_slot
	bvs	fs_args_out
	mov	r5, r0

	ldr	r0, [r5, #FH_FLAGS]
	tst	r0, #FHF_WRITE
	beq	fs_args_readonly
	orr	r0, r0, #FHF_DIRTY
	str	r0, [r5, #FH_FLAGS]

	ldr	r6, [r5, #FH_SIZE]
	str	r4, [r5, #FH_SIZE]
	cmp	r4, r6
	bhs	fs_args_extent_done	@ growing needs no clusters until it is written

	ldr	r7, [r5, #FH_VOL]
	ldr	r8, [r5, #FH_CLUSTER]
	cmp	r8, #0
	beq	fs_args_extent_done

	cmp	r4, #0
	bne	fs_args_extent_keep

	@ Nothing left at all: the whole chain goes.
	mov	r0, r7
	mov	r1, r8
	bl	free_chain
	bvs	fs_args_out
	mov	r0, #0
	str	r0, [r5, #FH_CLUSTER]
	b	fs_args_extent_done

fs_args_extent_keep:
	@ How many clusters the new length needs, less one: the last one kept.
	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9
	sub	r1, r4, #1
	mov	r9, r1, lsr r0		@ index of the last cluster still wanted

	mov	r0, r7
	mov	r1, r8
	mov	r2, r9
	bl	chain_reach
	bvs	fs_args_out
	mov	r10, r0			@ the last one to keep

	@ What follows it is surplus.
	mov	r0, r7
	mov	r1, r10
	bl	fat_next
	bvs	fs_args_out
	mov	r11, r0

	@ Mark the kept one as the end before freeing the rest, so a failure
	@ half way leaves a short chain rather than one running into free space.
	ldr	r2, [r7, #VOL_TYPE]
	cmp	r2, #64
	mvneq	r2, #0
	beq	fs_args_extent_eoc
	cmp	r2, #32
	ldreq	r2, =0x0fffffff
	ldrne	r2, =0xffff
fs_args_extent_eoc:
	mov	r0, r7
	mov	r1, r10
	bl	fat_set
	bvs	fs_args_out

	cmp	r11, #0
	beq	fs_args_extent_done
	mov	r0, r7
	mov	r1, r11
	bl	free_chain
	bvs	fs_args_out

fs_args_extent_done:
	mov	r0, r5
	bl	dir_writeback
	bvs	fs_args_out
	cmp	pc, #0
	ldmfd	sp!, {r1, r3-r12, pc}

fs_args_readonly:
	adrl	r0, err_readonly
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1, r3-r12, pc}

fs_args_ignore:
	cmp	pc, #0

fs_args_out:
	ldmfd	sp!, {r1, r3-r12, pc}


	/* FSEntry_GBPB is not implemented: FileSwitch will fall back on the
	   stream entries above, which do the same job. */
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


@ ---------------------------------------------------------------------------
@ The filing systems themselves
@ ---------------------------------------------------------------------------
@
@ Everything above is shared: sectors, clusters, the FAT walk, the directory
@ iterator, path lookup and the FileSwitch entry points. Everything below knows
@ about one format only. They are .included rather than assembled separately so
@ that labels stay visible in both directions without a line of declarations.

	.include "multifs-fat.s"
	.include "multifs-exfat.s"
	.include "multifs-ntfs.s"

	.end
