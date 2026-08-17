@ MultiFS - NTFS, read only
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ This file is .included by multifs.s and is NOT assembled on its own.
@
@ READ ONLY, and deliberately so. Writing NTFS safely means maintaining the
@ $LogFile journal, $Bitmap and the $MFT's own allocation, and a half-correct
@ writer does not produce a slightly wrong disc - it produces one chkdsk cannot
@ put back together. Every entry point below refuses to change anything.
@
@ NTFS shares almost nothing with FAT. There is no FAT, no directory of fixed
@ slots and no cluster chain. Everything on the volume is a FILE, including the
@ list of files, and every file is a record in the Master File Table made of
@ typed ATTRIBUTES. A small file's contents live inside its own record; a large
@ one's are described by a RUN LIST of (length, cluster) pairs. A directory is
@ a B+ tree of index entries keyed on the filename.
@
@ Two traps are worth knowing before reading any of it:
@
@ - **Fixups.** The last two bytes of every 512-byte sector of an MFT record and
@   of an index block are NOT data. They hold a copy of the record's update
@   sequence number, and the real bytes are kept in an array at the front. They
@   have to be put back before the record is read, or every structure that
@   straddles a sector boundary reads rubbish - and the first 510 bytes are
@   perfectly fine, so it looks like it works.
@
@ - **A run list is relative.** Each run's cluster number is a SIGNED offset
@   from the previous run's, not an absolute address, and the sizes of both
@   fields are packed into one nibble each. Reading them as absolute works for
@   the first run of every file and then wanders off the volume.


	@ Attribute types, of which these are the ones that matter here.
	NTFS_AT_STANDARD	= 0x10
	NTFS_AT_FILENAME	= 0x30
	NTFS_AT_DATA		= 0x80
	NTFS_AT_INDEX_ROOT	= 0x90
	NTFS_AT_INDEX_ALLOC	= 0xa0
	NTFS_AT_END		= 0xffffffff

	NTFS_AT_VOLUME_NAME	= 0x60

	@ The records every volume keeps in the same places.
	NTFS_VOLUME_REC		= 3
	NTFS_ROOT_REC		= 5
	NTFS_BITMAP_REC		= 6

	@ $FILE_NAME namespaces. 2 is the DOS 8.3 companion of a long name and is
	@ skipped: taking it as well lists every file twice.
	NTFS_NS_DOS		= 2


	/* An NTFS boot record.
	 *
	 * Entry: R10 = drive, R11 = start LBA, the sector already in WS_SECTOR.
	 *
	 * The two "clusters per" fields are signed: a positive value is a count
	 * of clusters, and a NEGATIVE one is the log2 of a byte count, so &F6 is
	 * -10 and means 1024 bytes. Reading it as a count gives a 246-cluster
	 * record and nothing works.
	 */
add_volume_ntfs:
	@ R9 holds the record throughout, for the same reason as the exFAT path:
	@ every ld32 below is a BL, and a BL overwrites LR.
	ldr	r2, [wp, #WS_NVOLUMES]
	mov	r3, r2, lsl #VOL_SHIFT
	add	r9, wp, #WS_VOLUMES
	add	r9, r9, r3

	str	r10, [r9, #VOL_DRIVE]
	str	r11, [r9, #VOL_START]

	mov	r0, #128		@ NTFS, in the same field as 12, 16, 32 and 64
	str	r0, [r9, #VOL_TYPE]

	add	r0, wp, #WS_SECTOR
	mov	r1, #11
	bl	ld16
	cmp	r0, #512		@ only 512-byte sectors: the buffers are 512
	bne	add_volume_bad
	str	r0, [r9, #VOL_BPS]

	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, #13]
	mov	r0, r1
	bl	log2
	cmp	r0, #0
	blt	add_volume_bad
	str	r0, [r9, #VOL_SPCLOG]
	mov	r2, #1
	mov	r2, r2, lsl r0
	str	r2, [r9, #VOL_SPC]

	@ Published now, not at the end: ntfs_sizefield below needs the cluster
	@ size for a POSITIVE "clusters per" field, and reads it from here.
	str	r9, [wp, #WS_REC]

	@ Total sectors, and where the MFT starts. Both are 64-bit on the medium
	@ and both are taken as 32: a volume that needs more than that is one
	@ read_sector cannot reach anyway.
	add	r0, wp, #WS_SECTOR
	mov	r1, #40
	bl	ld32
	str	r0, [r9, #VOL_TOTSEC]

	add	r0, wp, #WS_SECTOR
	mov	r1, #48
	bl	ld32
	str	r0, [r9, #VOL_MFT_CLUS]

	@ Bytes in an MFT record, and in an index block.
	@ armv3 has no LDRSB, and this byte is signed - see above.
	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, #64]
	mov	r1, r1, lsl #24
	mov	r0, r1, asr #24
	bl	ntfs_sizefield
	cmp	r0, #NTFS_MAX_REC
	bhi	add_volume_bad
	str	r0, [r9, #VOL_MFT_RECSZ]

	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, #68]
	mov	r1, r1, lsl #24
	mov	r0, r1, asr #24
	bl	ntfs_sizefield
	cmp	r0, #NTFS_MAX_IDX
	bhi	add_volume_bad
	str	r0, [r9, #VOL_IDX_SZ]

	@ The cluster heap is the whole volume: NTFS cluster 0 is sector 0 of the
	@ partition, not the start of some data area after a FAT. cluster_sector
	@ works out DATASEC0 + (cluster - 2) * SPC, so the base has to be pushed
	@ UP by two clusters for that subtraction to cancel - down, and every
	@ cluster reads four sectors before where it lives.
	ldr	r0, [r9, #VOL_SPC]
	mov	r1, r0, lsl #1		@ two clusters' worth of sectors
	add	r0, r11, r1
	str	r0, [r9, #VOL_DATASEC0]

	@ Things NTFS has not got.
	mov	r0, #0
	str	r0, [r9, #VOL_ROOTENTS]
	str	r0, [r9, #VOL_ROOTSEC0]
	str	r0, [r9, #VOL_ROOTSECS]
	str	r0, [r9, #VOL_RSVD]
	str	r0, [r9, #VOL_FSINFO]
	str	r0, [r9, #VOL_FSDIRTY]
	str	r0, [r9, #VOL_FATSEC0]
	str	r0, [r9, #VOL_FATSZ]
	str	r0, [r9, #VOL_NFATS]
	str	r0, [r9, #VOL_ROOTCLUS]
	mvn	r0, #0
	str	r0, [r9, #VOL_FREE]

	ldr	r0, [r9, #VOL_TOTSEC]
	ldr	r1, [r9, #VOL_SPCLOG]
	mov	r0, r0, lsr r1
	str	r0, [r9, #VOL_CLUSTERS]

	adrl	r1, ntfs_label
	add	r0, r9, #VOL_LABEL
	bl	copy_string_z

	ldr	r2, [wp, #WS_NVOLUMES]
	add	r2, r2, #1
	str	r2, [wp, #WS_NVOLUMES]

	@ The Master File Table is itself a file, and its record is the first
	@ one, so it can be read before anything is known about where the rest
	@ are. Its data runs are what make every other record reachable.
	mov	r0, r9
	bl	ntfs_read_mft_runs
	bvs	add_volume_ntfs_undo

	@ Now that any record can be reached, the volume's real name can be.
	mov	r0, r9
	bl	ntfs_read_label

	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

add_volume_ntfs_undo:
	ldr	r2, [wp, #WS_NVOLUMES]
	sub	r2, r2, #1
	str	r2, [wp, #WS_NVOLUMES]
	b	add_volume_out

ntfs_name:
	.ascii	"NTFS    "
	.align
ntfs_label:
	.string	"NTFS"
	.align


	/* One of NTFS's signed "clusters per" fields.
	 *
	 * Entry: R0 = the byte, sign extended. Exit: R0 = a byte count.
	 */
ntfs_sizefield:
	stmfd	sp!, {r1-r2, lr}
	cmp	r0, #0
	bgt	ntfs_sizefield_clusters

	rsb	r1, r0, #0		@ negative: it is a log2 of bytes
	cmp	r1, #31
	movhi	r1, #31
	mov	r0, #1
	mov	r0, r0, lsl r1
	ldmfd	sp!, {r1-r2, pc}

ntfs_sizefield_clusters:
	@ Positive: a count of clusters, and the volume record is part-filled,
	@ so the cluster size comes from what has been stored in it so far.
	ldr	r1, [wp, #WS_REC]
	ldr	r2, [r1, #VOL_SPCLOG]
	add	r2, r2, #9
	mov	r0, r0, lsl r2
	ldmfd	sp!, {r1-r2, pc}


	/* Put an MFT record or index block back together.
	 *
	 * Entry: R0 = the block, R1 = its length in bytes.
	 * Exit:  V set if it does not check out.
	 *
	 * The last two bytes of every 512-byte sector hold a copy of the update
	 * sequence number rather than data; the real bytes live in an array
	 * whose offset and size are in the header. If the copy does not match
	 * the number, the block was written half way through and is not to be
	 * believed.
	 *
	 * Skipping this is the classic way to get an NTFS reader that works
	 * perfectly until a structure crosses offset 510.
	 */
ntfs_fixup:
	stmfd	sp!, {r0-r8, lr}

	mov	r4, r0
	mov	r5, r1

	mov	r0, r4
	mov	r1, #4
	bl	ld16
	mov	r6, r0			@ offset of the update sequence array
	mov	r0, r4
	mov	r1, #6
	bl	ld16
	mov	r7, r0			@ its size in words, the first being the number

	cmp	r7, #2
	blo	ntfs_fixup_bad
	sub	r7, r7, #1		@ words that are actual replacements

	@ It must describe exactly this block: one word per sector.
	mov	r0, r5, lsr #9
	cmp	r7, r0
	bne	ntfs_fixup_bad

	add	r8, r4, r6
	mov	r0, r8
	mov	r1, #0
	bl	ld16
	mov	r3, r0			@ the update sequence number

	mov	r2, #0			@ which sector
ntfs_fixup_loop:
	cmp	r2, r7
	bhs	ntfs_fixup_done

	@ The last two bytes of sector R2 must currently hold the number.
	add	r0, r2, #1
	mov	r0, r0, lsl #9
	sub	r0, r0, #2
	add	r0, r4, r0
	stmfd	sp!, {r0}
	mov	r1, #0
	bl	ld16
	cmp	r0, r3
	ldmfd	sp!, {r0}
	bne	ntfs_fixup_bad

	@ Put the real bytes back from the array.
	add	r1, r8, #2
	add	r1, r1, r2, lsl #1
	ldrb	r6, [r1]
	strb	r6, [r0]
	ldrb	r6, [r1, #1]
	strb	r6, [r0, #1]

	add	r2, r2, #1
	b	ntfs_fixup_loop

ntfs_fixup_done:
	cmp	pc, #0
	ldmfd	sp!, {r0-r8, pc}

ntfs_fixup_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}

err_ntfs_bad:
	.int	0xb0
	.string	"That NTFS structure does not check out"
	.align


	/* Decode a run list.
	 *
	 * Entry: R0 = volume, R1 = the run list, R2 = where to put it,
	 *        R3 = where to put the count.
	 * Exit:  V set on failure.
	 *
	 * Each run is a header byte whose low nibble is the width of the length
	 * field and whose high nibble is the width of the cluster field, then
	 * those two fields, little endian. A header of zero ends the list. The
	 * cluster field is a SIGNED offset from the previous run's cluster, not
	 * an absolute number - and a run with no cluster field at all is a hole
	 * in a sparse file.
	 *
	 * Stored as three words a run: first VCN, first LCN, length in clusters.
	 */
ntfs_runs:
	stmfd	sp!, {r0-r10, lr}

	mov	r4, r1			@ where we are in the list
	mov	r5, r2			@ the table
	mov	r6, r3			@ where the count goes
	mov	r7, #0			@ runs so far
	mov	r8, #0			@ running LCN
	mov	r9, #0			@ running VCN

ntfs_runs_loop:
	ldrb	r0, [r4]
	cmp	r0, #0
	beq	ntfs_runs_done
	cmp	r7, #NTFS_MAX_RUNS
	bhs	ntfs_runs_toomany

	and	r10, r0, #0x0f		@ bytes of length
	mov	r1, r0, lsr #4		@ bytes of cluster offset
	cmp	r10, #0
	beq	ntfs_runs_bad
	cmp	r10, #4
	bhi	ntfs_runs_bad
	cmp	r1, #4
	bhi	ntfs_runs_bad

	add	r4, r4, #1

	@ The length, unsigned, gathered a byte at a time: ARM cannot shift by a
	@ variable amount inside an ORR, so there is no neater way.
	add	r4, r4, r10
	mov	r2, #0
	mov	r3, r10
ntfs_runs_len:
	subs	r3, r3, #1
	blt	ntfs_runs_gotlen
	sub	r0, r4, r10
	ldrb	r0, [r0, r3]
	mov	r2, r2, lsl #8
	orr	r2, r2, r0
	b	ntfs_runs_len
ntfs_runs_gotlen:

	@ The cluster offset, SIGNED, and absent for a sparse hole.
	cmp	r1, #0
	beq	ntfs_runs_hole

	mov	r3, r1
	mov	r0, #0
ntfs_runs_lcn:
	subs	r3, r3, #1
	blt	ntfs_runs_gotlcn
	ldrb	lr, [r4, r3]
	mov	r0, r0, lsl #8
	orr	r0, r0, lr
	b	ntfs_runs_lcn
ntfs_runs_gotlcn:
	@ Sign extend from the width it was written in.
	rsb	lr, r1, #4
	mov	lr, lr, lsl #3
	mov	r0, r0, lsl lr
	mov	r0, r0, asr lr

	add	r8, r8, r0		@ relative to the run before it
	add	r4, r4, r1
	b	ntfs_runs_store

ntfs_runs_hole:
	mov	r8, r8			@ a hole keeps the LCN where it was

ntfs_runs_store:
	mov	r0, r7
	mov	r0, r0, lsl #1
	add	r0, r0, r7		@ three words a run
	mov	r0, r0, lsl #2
	add	r0, r5, r0
	str	r9, [r0]
	str	r8, [r0, #4]
	str	r2, [r0, #8]

	add	r9, r9, r2
	add	r7, r7, #1
	b	ntfs_runs_loop

ntfs_runs_done:
	str	r7, [r6]
	cmp	pc, #0
	ldmfd	sp!, {r0-r10, pc}

ntfs_runs_toomany:
	str	r7, [r6]
	cmp	pc, #0			@ what there is will have to do
	ldmfd	sp!, {r0-r10, pc}

ntfs_runs_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r10, pc}


	/* Turn a virtual cluster of a file into a real one.
	 *
	 * Entry: R0 = the run table, R1 = how many runs, R2 = the VCN.
	 * Exit:  R0 = the LCN, or 0 if the file does not reach that far.
	 */
ntfs_vcn_to_lcn:
	stmfd	sp!, {r1-r6, lr}
	mov	r4, r0
	mov	r5, #0
ntfs_vcn_loop:
	cmp	r5, r1
	bhs	ntfs_vcn_none
	mov	r0, r5
	mov	r0, r0, lsl #1
	add	r0, r0, r5
	mov	r0, r0, lsl #2
	add	r6, r4, r0
	ldr	r0, [r6]		@ first VCN
	ldr	r3, [r6, #8]		@ length
	add	r3, r0, r3
	cmp	r2, r0
	blo	ntfs_vcn_next
	cmp	r2, r3
	bhs	ntfs_vcn_next
	ldr	r3, [r6, #4]		@ first LCN
	sub	r0, r2, r0
	add	r0, r3, r0
	ldmfd	sp!, {r1-r6, pc}
ntfs_vcn_next:
	add	r5, r5, #1
	b	ntfs_vcn_loop
ntfs_vcn_none:
	mov	r0, #0
	ldmfd	sp!, {r1-r6, pc}


	/* Read the $MFT's own record and remember where the table lives.
	 *
	 * Entry: R0 = volume. Exit: V set on failure.
	 *
	 * Record zero is the Master File Table's own, and it is the one record
	 * whose position is known without consulting the table - it is at the
	 * start of it. Its $DATA run list is what makes every other record
	 * reachable, including the ones in later fragments.
	 */
ntfs_read_mft_runs:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0

	@ Record zero, read straight off the medium.
	ldr	r0, [r6, #VOL_MFT_CLUS]
	mov	r1, r0
	mov	r0, r6
	bl	cluster_sector
	mov	r7, r0

	mov	r0, r6
	mov	r1, r7
	bl	ntfs_read_raw_rec
	bvs	ntfs_read_mft_out

	@ Its unnamed $DATA attribute describes the whole table.
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_DATA
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_read_mft_bad

	ldrb	r1, [r0, #8]
	cmp	r1, #0
	beq	ntfs_read_mft_bad	@ a resident $MFT makes no sense

	mov	r1, r0
	mov	r0, r1
	mov	r1, #32
	bl	ld16			@ offset of the data runs
	add	r1, r0, #0
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	bl	ntfs_find_attr_again
	add	r1, r0, r1

	mov	r0, r6
	ldr	r2, =WS_MFT_RUNS
	add	r2, wp, r2
	ldr	r3, =WS_MFT_NRUNS
	add	r3, wp, r3
	bl	ntfs_runs
	bvs	ntfs_read_mft_out

	cmp	pc, #0

ntfs_read_mft_out:
	ldmfd	sp!, {r0-r8, pc}

ntfs_read_mft_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}


	/* Read one MFT record straight off the medium, by sector, and fix it up.
	 *
	 * Entry: R0 = volume, R1 = the first sector of it.
	 */
ntfs_read_raw_rec:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0
	mov	r7, r1
	ldr	r8, [r6, #VOL_MFT_RECSZ]

	ldr	r4, =WS_MFT
	add	r4, wp, r4
	mov	r5, #0

ntfs_read_raw_loop:
	cmp	r5, r8
	bhs	ntfs_read_raw_got
	ldr	r0, [r6, #VOL_DRIVE]
	add	r1, r7, r5, lsr #9
	add	r2, r4, r5
	bl	read_sector
	bvs	ntfs_read_raw_out
	add	r5, r5, #512
	b	ntfs_read_raw_loop

ntfs_read_raw_got:
	@ It must say FILE, or the table is not where the boot record claims.
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	adrl	r1, ntfs_file_sig
	mov	r2, #4
	bl	mem_equal
	bne	ntfs_read_raw_bad

	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, r8
	bl	ntfs_fixup
	bvs	ntfs_read_raw_out

	cmp	pc, #0

ntfs_read_raw_out:
	ldmfd	sp!, {r0-r8, pc}

ntfs_read_raw_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}

ntfs_file_sig:
	.ascii	"FILE"
	.align


	/* Read MFT record N, wherever in the table it is.
	 *
	 * Entry: R0 = volume, R1 = the record number.
	 * Exit:  V set on failure; the record is in WS_MFT, fixed up.
	 */
ntfs_read_rec:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0
	mov	r7, r1

	@ Which cluster of the table it falls in, and where inside it.
	ldr	r0, [r6, #VOL_MFT_RECSZ]
	ldr	r1, [r6, #VOL_SPCLOG]
	add	r1, r1, #9		@ bytes in a cluster, as a shift
	mov	r2, #1
	mov	r2, r2, lsl r1

	mul	r3, r7, r0		@ byte offset into the table
	mov	r4, r3, lsr r1		@ which VCN
	sub	r5, r2, #1
	and	r5, r3, r5		@ and where in it

	ldr	r0, =WS_MFT_RUNS
	add	r0, wp, r0
	ldr	r1, =WS_MFT_NRUNS
	ldr	r1, [wp, r1]
	mov	r2, r4
	bl	ntfs_vcn_to_lcn
	cmp	r0, #0
	beq	ntfs_read_rec_bad

	mov	r1, r0
	mov	r0, r6
	bl	cluster_sector
	add	r0, r0, r5, lsr #9

	mov	r1, r0
	mov	r0, r6
	bl	ntfs_read_raw_rec
	bvs	ntfs_read_rec_out

	cmp	pc, #0

ntfs_read_rec_out:
	ldmfd	sp!, {r0-r8, pc}

ntfs_read_rec_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}


	/* Find an attribute in the record now in WS_MFT.
	 *
	 * Entry: R0 = the record, R1 = the type wanted.
	 * Exit:  R0 = the attribute, or 0 if it has not got one.
	 *
	 * Attributes are laid end to end from the offset in the header, each
	 * carrying its own length, and a type of &FFFFFFFF ends the list. A
	 * length of zero would loop for ever, so it stops on that too.
	 */
ntfs_find_attr:
	stmfd	sp!, {r1-r8, lr}

	mov	r6, r0
	mov	r7, r1

	mov	r0, r6
	mov	r1, #20
	bl	ld16
	add	r5, r6, r0

ntfs_find_attr_loop:
	mov	r0, r5
	mov	r1, #0
	bl	ld32
	ldr	r1, =NTFS_AT_END
	cmp	r0, r1
	beq	ntfs_find_attr_none
	cmp	r0, r7
	beq	ntfs_find_attr_got

	mov	r0, r5
	mov	r1, #4
	bl	ld32
	cmp	r0, #0
	beq	ntfs_find_attr_none
	add	r5, r5, r0

	@ Do not run off the end of the buffer whatever the record claims.
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	sub	r0, r5, r0
	cmp	r0, #NTFS_MAX_REC
	bhs	ntfs_find_attr_none
	b	ntfs_find_attr_loop

ntfs_find_attr_got:
	mov	r0, r5
	ldr	lr, =WS_N_OFF
	str	r0, [wp, lr]	@ remembered for ntfs_find_attr_again
	ldmfd	sp!, {r1-r8, pc}

ntfs_find_attr_none:
	mov	r0, #0
	ldmfd	sp!, {r1-r8, pc}


	/* The attribute ntfs_find_attr last found, without looking again.
	 *
	 * R12 is wp and R14 is the way back, so neither can be borrowed to hold
	 * the offset - the first version of this loaded the offset into LR and
	 * then returned to it.
	 */
ntfs_find_attr_again:
	stmfd	sp!, {r1, lr}
	ldr	r1, =WS_N_OFF
	ldr	r0, [wp, r1]
	ldmfd	sp!, {r1, pc}


	/* Start walking an NTFS directory.
	 *
	 * Entry: R0 = volume, R1 = the directory's MFT record number.
	 * Exit:  V set on failure.
	 *
	 * A directory is a B+ tree keyed on the filename. $INDEX_ROOT holds the
	 * root node inside the record itself; if it does not all fit, the rest
	 * is in $INDEX_ALLOCATION, a non-resident run of index blocks.
	 *
	 * The tree is NOT walked in order here. Every index entry lives in
	 * exactly one node, so reading the root node and then every allocation
	 * block in turn hands back every entry exactly once - which is all a
	 * listing or a lookup by name needs, and a great deal less code than a
	 * proper ordered descent.
	 */
ntfs_dir_open:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0
	mov	r7, r1

	ldr	r0, =WS_N_DIRREC
	str	r7, [wp, r0]
	mov	r0, #0
	ldr	r1, =WS_N_PHASE
	str	r0, [wp, r1]
	ldr	r1, =WS_N_VCN
	str	r0, [wp, r1]
	ldr	r1, =WS_N_NVCN
	str	r0, [wp, r1]
	ldr	r1, =WS_N_NIRUNS
	str	r0, [wp, r1]

	mov	r0, r6
	mov	r1, r7
	bl	ntfs_read_rec
	bvs	ntfs_dir_open_out

	@ $INDEX_ROOT is always resident, and always there for a directory.
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_INDEX_ROOT
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_dir_open_notdir
	mov	r5, r0

	mov	r0, r5
	mov	r1, #20
	bl	ld16			@ where the resident value starts
	add	r5, r5, r0

	@ The value is an INDEX_ROOT header then an INDEX_HEADER at +16, whose
	@ own first-entry offset is relative to ITSELF, not to the attribute.
	add	r4, r5, #16
	mov	r0, r4
	mov	r1, #0
	bl	ld32
	add	r0, r4, r0
	ldr	r1, =WS_N_OFF
	str	r0, [wp, r1]

	mov	r0, r4
	mov	r1, #4
	bl	ld32			@ total size of the entries
	add	r0, r4, r0
	ldr	r1, =WS_N_END
	str	r0, [wp, r1]

	@ Bit 0 of the index header's flags says there is more of it outside.
	mov	r0, r4
	mov	r1, #12
	bl	ld32
	tst	r0, #1
	beq	ntfs_dir_open_done

	@ $INDEX_ALLOCATION, whose runs say where the blocks are.
	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_INDEX_ALLOC
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_dir_open_done
	mov	r5, r0

	ldrb	r0, [r5, #8]
	cmp	r0, #0
	beq	ntfs_dir_open_done	@ resident: nothing to walk

	@ How many index blocks there are, from the attribute's real size.
	mov	r0, r5
	mov	r1, #48
	bl	ld32
	ldr	r1, [r6, #VOL_IDX_SZ]
	mov	r2, #0
ntfs_dir_open_count:
	cmp	r0, r1
	blo	ntfs_dir_open_counted
	sub	r0, r0, r1
	add	r2, r2, #1
	b	ntfs_dir_open_count
ntfs_dir_open_counted:
	cmp	r0, #0
	addne	r2, r2, #1
	ldr	r0, =WS_N_NVCN
	str	r2, [wp, r0]

	mov	r0, r5
	mov	r1, #32
	bl	ld16			@ offset of the data runs
	add	r1, r5, r0
	mov	r0, r6
	ldr	r2, =WS_N_IRUNS
	add	r2, wp, r2
	ldr	r3, =WS_N_NIRUNS
	add	r3, wp, r3
	bl	ntfs_runs
	bvs	ntfs_dir_open_out

ntfs_dir_open_done:
	cmp	pc, #0

ntfs_dir_open_out:
	ldmfd	sp!, {r0-r8, pc}

ntfs_dir_open_notdir:
	adrl	r0, err_not_dir
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}


	/* Bring index block N of the directory in and fix it up.
	 *
	 * Entry: R0 = volume, R1 = which block.
	 * Exit:  V set on failure; WS_IDX holds it and WS_N_OFF/END bracket its
	 *        entries.
	 */
ntfs_read_index:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0
	mov	r7, r1

	@ Which cluster that block starts at.
	ldr	r0, [r6, #VOL_IDX_SZ]
	ldr	r1, [r6, #VOL_SPCLOG]
	add	r1, r1, #9
	mov	r2, #1
	mov	r2, r2, lsl r1		@ bytes in a cluster

	mul	r3, r7, r0		@ byte offset into the attribute
	mov	r4, r3, lsr r1		@ which VCN

	ldr	r0, =WS_N_IRUNS
	add	r0, wp, r0
	ldr	r1, =WS_N_NIRUNS
	ldr	r1, [wp, r1]
	mov	r2, r4
	bl	ntfs_vcn_to_lcn
	cmp	r0, #0
	beq	ntfs_read_index_bad

	mov	r1, r0
	mov	r0, r6
	bl	cluster_sector
	mov	r7, r0

	ldr	r8, [r6, #VOL_IDX_SZ]
	ldr	r4, =WS_IDX
	add	r4, wp, r4
	mov	r5, #0
ntfs_read_index_loop:
	cmp	r5, r8
	bhs	ntfs_read_index_got
	ldr	r0, [r6, #VOL_DRIVE]
	add	r1, r7, r5, lsr #9
	add	r2, r4, r5
	bl	read_sector
	bvs	ntfs_read_index_out
	add	r5, r5, #512
	b	ntfs_read_index_loop

ntfs_read_index_got:
	ldr	r0, =WS_IDX
	add	r0, wp, r0
	adrl	r1, ntfs_indx_sig
	mov	r2, #4
	bl	mem_equal
	bne	ntfs_read_index_bad

	ldr	r0, =WS_IDX
	add	r0, wp, r0
	mov	r1, r8
	bl	ntfs_fixup
	bvs	ntfs_read_index_out

	@ The INDEX_HEADER sits at +24, and its offsets are relative to itself.
	ldr	r4, =WS_IDX
	add	r4, wp, r4
	add	r4, r4, #24
	mov	r0, r4
	mov	r1, #0
	bl	ld32
	add	r0, r4, r0
	ldr	r1, =WS_N_OFF
	str	r0, [wp, r1]
	mov	r0, r4
	mov	r1, #4
	bl	ld32
	add	r0, r4, r0
	ldr	r1, =WS_N_END
	str	r0, [wp, r1]

	cmp	pc, #0

ntfs_read_index_out:
	ldmfd	sp!, {r0-r8, pc}

ntfs_read_index_bad:
	adrl	r0, err_ntfs_bad
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r0-r8, pc}

ntfs_indx_sig:
	.ascii	"INDX"
	.align


	/* The next object in an NTFS directory.
	 *
	 * Exit: R0 = a 32-byte entry in FAT's shape, or 0 at the end.
	 *
	 * As on exFAT, what comes back is synthesised so that path_lookup,
	 * fs_file and fs_open work unchanged, and the name goes into WS_BUILT.
	 * An index entry is a header, then the $FILE_NAME of the object it
	 * refers to; the DOS namespace duplicates are skipped or every file is
	 * listed twice.
	 */
	@ NOTE: dir_next BRANCHES here, it does not call, so this runs inside
	@ dir_next's frame and returns through it. Pushing a frame of its own
	@ leaked thirty-two bytes of stack per entry listed, and the stack ran
	@ out somewhere after the last file with "branch through zero".
	@ Registers are therefore limited to the r1-r7 dir_next saved.
ntfs_dir_next:
ntfs_dir_next_again:
	ldr	r0, =WS_N_OFF
	ldr	r5, [wp, r0]
	ldr	r0, =WS_N_END
	ldr	r6, [wp, r0]
	cmp	r5, r6
	bhs	ntfs_dir_next_node

	@ Entry length, which also says where the next one starts.
	mov	r0, r5
	mov	r1, #8
	bl	ld16
	cmp	r0, #0
	beq	ntfs_dir_next_node	@ a zero length would never advance
	add	r1, r5, r0
	ldr	r0, =WS_N_OFF
	str	r1, [wp, r0]

	@ Bit 1 of the flags marks the end entry, which names nothing.
	mov	r0, r5
	mov	r1, #12
	bl	ld16
	tst	r0, #2
	bne	ntfs_dir_next_node

	@ The $FILE_NAME follows the sixteen-byte header.
	add	r7, r5, #16

	ldrb	r0, [r7, #65]
	cmp	r0, #NTFS_NS_DOS
	beq	ntfs_dir_next_again	@ the 8.3 companion of a long name

	@ Records below the first user one are the volume's own metadata files -
	@ $MFT, $Bitmap, $LogFile and the rest - and have no business in a
	@ listing.
	mov	r0, r5
	mov	r1, #0
	bl	ld32
	cmp	r0, #16
	blo	ntfs_dir_next_again

	bl	ntfs_build_entry
	ldr	r0, =WS_XENT
	add	r0, wp, r0
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

ntfs_dir_next_node:
	@ This node is finished; move to the next index block if there is one.
	ldr	r0, =WS_N_PHASE
	ldr	r1, [wp, r0]
	cmp	r1, #0
	moveq	r1, #1
	streq	r1, [wp, r0]
	beq	ntfs_dir_next_block

	ldr	r0, =WS_N_VCN
	ldr	r1, [wp, r0]
	add	r1, r1, #1
	str	r1, [wp, r0]

ntfs_dir_next_block:
	ldr	r0, =WS_N_VCN
	ldr	r1, [wp, r0]
	ldr	r0, =WS_N_NVCN
	ldr	r2, [wp, r0]
	cmp	r1, r2
	bhs	ntfs_dir_next_end

	ldr	r0, [wp, #WS_IT_REC]
	bl	ntfs_read_index
	bvs	ntfs_dir_next_out
	b	ntfs_dir_next_again

ntfs_dir_next_end:
	mov	r0, #0
	cmp	pc, #0

ntfs_dir_next_out:
	ldmfd	sp!, {r1-r7, pc}


	/* Turn the $FILE_NAME at R7 into an entry the rest of MultiFS knows.
	 *
	 * The name goes to WS_BUILT and a FAT-shaped entry to WS_XENT, exactly
	 * as the exFAT reader does, so nothing above this has to learn a third
	 * directory format.
	 */
ntfs_build_entry:
	stmfd	sp!, {r0-r8, lr}

	ldr	r3, =WS_XENT
	add	r3, wp, r3
	mov	r0, #0
	mov	r2, #0
ntfs_build_clear:
	strb	r0, [r3, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	ntfs_build_clear
	mov	r0, #' '
	mov	r2, #0
ntfs_build_pad:
	strb	r0, [r3, r2]
	add	r2, r2, #1
	cmp	r2, #11
	blo	ntfs_build_pad

	@ Flags at +56: bit 28 marks a directory, in FAT's own bit 4 sense.
	mov	r0, r7
	mov	r1, #56
	bl	ld32
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	mov	r1, #0
	tst	r0, #0x10000000
	orrne	r1, r1, #0x10
	tst	r0, #1
	orrne	r1, r1, #0x01		@ read only
	orr	r1, r1, #0x20		@ archive, which everything here has
	strb	r1, [r3, #11]

	@ The real size, at +48. FAT's field is 32 bits and so is ours.
	mov	r0, r7
	mov	r1, #48
	bl	ld32
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #28]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #29]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #30]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #31]

	@ The MFT record number goes where FAT keeps a first cluster: nothing
	@ else needs a cluster here, and everything needs the record.
	sub	r0, r7, #16
	mov	r1, #0
	bl	ld32
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #26]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #27]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #20]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #21]

	@ And the name, UTF-16, up to 255 characters.
	ldrb	r6, [r7, #64]
	ldr	r4, =WS_BUILT
	add	r4, wp, r4
	mov	r5, #0
ntfs_build_name:
	cmp	r5, r6
	bhs	ntfs_build_name_done
	cmp	r5, #LFN_MAX
	bhs	ntfs_build_name_done
	add	r0, r7, #66
	add	r0, r0, r5, lsl #1
	ldrb	r1, [r0]
	ldrb	r2, [r0, #1]
	cmp	r2, #0
	movne	r1, #'_'		@ nothing outside Latin-1 survives a path
	cmp	r1, #'.'
	moveq	r1, #'/'
	beq	ntfs_build_store
	cmp	r1, #'/'
	moveq	r1, #'.'
ntfs_build_store:
	strb	r1, [r4, r5]
	add	r5, r5, #1
	b	ntfs_build_name

ntfs_build_name_done:
	mov	r0, #0
	strb	r0, [r4, r5]
	ldmfd	sp!, {r0-r8, pc}


	/* Read from a file.
	 *
	 * Entry: R0 = volume, R1 = its MFT record, R2 = offset in the file,
	 *        R3 = where to put it, R4 = how many bytes.
	 * Exit:  R0 = how many arrived. V set on failure.
	 *
	 * A small file has no clusters at all: its contents sit inside its own
	 * MFT record as a RESIDENT $DATA attribute. A large one is a run list.
	 * Both are ordinary here and the resident case is not a special case to
	 * be skipped - "ReadMe.md" is six bytes and lives entirely in its record.
	 */
ntfs_read_bytes:
	stmfd	sp!, {r1-r11, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ record
	mov	r7, r2			@ offset in the file
	mov	r8, r3			@ destination
	mov	r9, r4			@ wanted
	mov	r10, #0			@ delivered

	mov	r0, r5
	mov	r1, r6
	bl	ntfs_read_rec
	bvs	ntfs_read_bytes_out

	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_DATA
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_read_bytes_done	@ no data at all: an empty file
	mov	r4, r0

	ldrb	r0, [r4, #8]
	cmp	r0, #0
	bne	ntfs_read_bytes_runs

	@ Resident: the whole file is in the record.
	mov	r0, r4
	mov	r1, #16
	bl	ld32
	mov	r11, r0			@ its length
	mov	r0, r4
	mov	r1, #20
	bl	ld16
	add	r4, r4, r0		@ where the value starts

	cmp	r7, r11
	bhs	ntfs_read_bytes_done	@ past the end
	sub	r0, r11, r7
	cmp	r9, r0
	movhi	r9, r0
	add	r4, r4, r7

	mov	r2, #0
ntfs_read_res_copy:
	cmp	r2, r9
	bhs	ntfs_read_bytes_counted
	ldrb	r0, [r4, r2]
	strb	r0, [r8, r2]
	add	r2, r2, #1
	b	ntfs_read_res_copy

ntfs_read_bytes_counted:
	mov	r10, r9
	b	ntfs_read_bytes_done

ntfs_read_bytes_runs:
	@ Non-resident: decode the runs once, then read cluster by cluster.
	mov	r0, r4
	mov	r1, #48
	bl	ld32
	mov	r11, r0			@ the real size

	mov	r0, r4
	mov	r1, #32
	bl	ld16
	add	r1, r4, r0
	mov	r0, r5
	ldr	r2, =WS_N_DATA
	add	r2, wp, r2
	ldr	r3, =WS_N_NDATA
	add	r3, wp, r3
	bl	ntfs_runs
	bvs	ntfs_read_bytes_out

	cmp	r7, r11
	bhs	ntfs_read_bytes_done
	sub	r0, r11, r7
	cmp	r9, r0
	movhi	r9, r0			@ nothing past the end

ntfs_read_run_loop:
	cmp	r10, r9
	bhs	ntfs_read_bytes_done

	@ Which cluster the next byte is in, and where inside it.
	ldr	r0, [r5, #VOL_SPCLOG]
	add	r0, r0, #9		@ bytes per cluster, as a shift
	add	r1, r7, r10
	mov	r2, r1, lsr r0		@ the VCN
	mov	r3, #1
	mov	r3, r3, lsl r0
	sub	r3, r3, #1
	and	r4, r1, r3		@ offset within the cluster

	stmfd	sp!, {r0, r4}
	ldr	r0, =WS_N_DATA
	add	r0, wp, r0
	ldr	r1, =WS_N_NDATA
	ldr	r1, [wp, r1]
	bl	ntfs_vcn_to_lcn
	ldmfd	sp!, {r3, r4}
	cmp	r0, #0
	beq	ntfs_read_bytes_done	@ a hole, or the file stops here

	mov	r1, r0
	mov	r0, r5
	bl	cluster_sector
	add	r0, r0, r4, lsr #9	@ the sector inside that cluster
	mov	r1, r0
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	ntfs_read_bytes_out

	@ Copy what is wanted out of this sector.
	ldr	r0, =511
	and	r0, r4, r0		@ offset within the sector
	rsb	r1, r0, #512		@ how much of it is left
	sub	r2, r9, r10
	cmp	r1, r2
	movhi	r1, r2			@ but no more than is wanted

	add	r3, wp, #WS_FILESEC
	add	r3, r3, r0
	mov	r2, #0
ntfs_read_run_copy:
	cmp	r2, r1
	bhs	ntfs_read_run_next
	ldrb	r0, [r3, r2]
	add	r4, r8, r10
	strb	r0, [r4, r2]
	add	r2, r2, #1
	b	ntfs_read_run_copy

ntfs_read_run_next:
	add	r10, r10, r1
	b	ntfs_read_run_loop

ntfs_read_bytes_done:
	mov	r0, r10
	cmp	pc, #0

ntfs_read_bytes_out:
	ldmfd	sp!, {r1-r11, pc}


	/* The volume's real name, out of $Volume.
	 *
	 * Entry: R0 = volume. Never fails loudly: a volume with no name, or one
	 * whose record will not read, keeps the "NTFS" it was given already.
	 *
	 * Record 3 is $Volume and its $VOLUME_NAME attribute holds the label as
	 * UTF-16. It is always resident - a volume name that needed a cluster of
	 * its own would be a strange thing.
	 */
ntfs_read_label:
	stmfd	sp!, {r0-r8, lr}

	mov	r6, r0
	mov	r1, #NTFS_VOLUME_REC
	bl	ntfs_read_rec
	bvs	ntfs_read_label_out

	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_VOLUME_NAME
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_read_label_out
	mov	r5, r0

	ldrb	r0, [r5, #8]
	cmp	r0, #0
	bne	ntfs_read_label_out	@ non-resident: not worth chasing

	mov	r0, r5
	mov	r1, #16
	bl	ld32
	mov	r7, r0, lsr #1		@ characters, not bytes
	cmp	r7, #0
	beq	ntfs_read_label_out
	cmp	r7, #11
	movhi	r7, #11			@ VOL_LABEL holds eleven and a terminator

	mov	r0, r5
	mov	r1, #20
	bl	ld16
	add	r5, r5, r0

	add	r8, r6, #VOL_LABEL
	mov	r4, #0
ntfs_label_copy:
	cmp	r4, r7
	bhs	ntfs_label_done
	add	r0, r5, r4, lsl #1
	ldrb	r1, [r0]
	ldrb	r2, [r0, #1]
	cmp	r2, #0
	movne	r1, #'_'		@ nothing outside Latin-1 survives a path
	strb	r1, [r8, r4]
	add	r4, r4, #1
	b	ntfs_label_copy

ntfs_label_done:
	mov	r0, #0
	strb	r0, [r8, r4]
	mov	r0, r8
	bl	label_legalise

ntfs_read_label_out:
	cmp	pc, #0			@ a nameless volume is not an error
	ldmfd	sp!, {r0-r8, pc}


	/* Free clusters, counted out of $Bitmap.
	 *
	 * Entry: R0 = volume. Exit: R0 = free clusters. V set on failure.
	 *
	 * Record 6 is $Bitmap and its $DATA is one bit per cluster of the whole
	 * volume, set when the cluster is in use. There is no running total kept
	 * anywhere, so counting it is the only answer - the same arrangement
	 * exFAT has, and about as cheap.
	 */
ntfs_free_clusters:
	stmfd	sp!, {r1-r11, lr}

	mov	r6, r0
	mov	r1, #NTFS_BITMAP_REC
	bl	ntfs_read_rec
	bvs	ntfs_free_out

	ldr	r0, =WS_MFT
	add	r0, wp, r0
	mov	r1, #NTFS_AT_DATA
	bl	ntfs_find_attr
	cmp	r0, #0
	beq	ntfs_free_none
	mov	r5, r0

	ldrb	r0, [r5, #8]
	cmp	r0, #0
	beq	ntfs_free_none		@ resident: a volume too small to care about

	mov	r0, r5
	mov	r1, #32
	bl	ld16
	add	r1, r5, r0
	mov	r0, r6
	ldr	r2, =WS_N_DATA
	add	r2, wp, r2
	ldr	r3, =WS_N_NDATA
	add	r3, wp, r3
	bl	ntfs_runs
	bvs	ntfs_free_out

	ldr	r9, [r6, #VOL_CLUSTERS]
	mov	r10, #0			@ free so far
	mov	r7, #0			@ clusters described so far
	mov	r8, #0			@ which VCN of the bitmap

ntfs_free_vcn:
	cmp	r7, r9
	bhs	ntfs_free_done

	ldr	r0, =WS_N_DATA
	add	r0, wp, r0
	ldr	r1, =WS_N_NDATA
	ldr	r1, [wp, r1]
	mov	r2, r8
	bl	ntfs_vcn_to_lcn
	cmp	r0, #0
	beq	ntfs_free_done		@ the bitmap stops here

	mov	r1, r0
	mov	r0, r6
	bl	cluster_sector
	mov	r4, r0

	ldr	r11, [r6, #VOL_SPCLOG]
	mov	r0, #1
	mov	r11, r0, lsl r11	@ sectors in a cluster

ntfs_free_sector:
	cmp	r7, r9
	bhs	ntfs_free_done
	cmp	r11, #0
	beq	ntfs_free_next_vcn

	mov	r1, r4
	ldr	r0, [r6, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	ntfs_free_out

	mov	r2, #0
ntfs_free_byte:
	cmp	r2, #512
	bhs	ntfs_free_sector_done
	cmp	r7, r9
	bhs	ntfs_free_done
	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, r2]
	mov	r0, #0
ntfs_free_bit:
	cmp	r0, #8
	bhs	ntfs_free_byte_done
	cmp	r7, r9
	bhs	ntfs_free_done
	tst	r1, #1
	addeq	r10, r10, #1		@ a clear bit is a free cluster
	mov	r1, r1, lsr #1
	add	r0, r0, #1
	add	r7, r7, #1
	b	ntfs_free_bit
ntfs_free_byte_done:
	add	r2, r2, #1
	b	ntfs_free_byte

ntfs_free_sector_done:
	add	r4, r4, #1
	sub	r11, r11, #1
	b	ntfs_free_sector

ntfs_free_next_vcn:
	add	r8, r8, #1
	b	ntfs_free_vcn

ntfs_free_done:
	mov	r0, r10
	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

ntfs_free_none:
	mov	r0, #0
	cmp	pc, #0

ntfs_free_out:
	ldmfd	sp!, {r1-r11, pc}
