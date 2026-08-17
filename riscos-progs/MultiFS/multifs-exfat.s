@ MultiFS - exFAT
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ This file is .included by multifs.s and is NOT assembled on its own: it shares
@ that file's constants, its workspace layout and its register conventions, and
@ every label here is visible to it and to the other formats. Splitting the
@ formats into files of their own is about reading the thing, not about linkage
@ - one module image comes out either way.
@
@ exFAT is a different filing system that happens to keep a 32-bit FAT. Below
@ the directory layer the cluster arithmetic is FAT32's and lives in multifs.s;
@ above it nothing is shared. There are no 8.3 names, no &E5 deleted marker, no
@ long-name fragments and no fixed root. An object is a SET of consecutive
@ entries tied together by a checksum and a name hash, and free space is an
@ allocation bitmap rather than a walk of the FAT.

	/* Walk an exFAT root directory for the things the volume needs.
	 *
	 * Entry: R0 = volume record. Exit: V set on a read error.
	 *
	 * exFAT keeps in the root what FAT keeps in the boot record: the volume
	 * label, and - the one that matters - where the allocation bitmap is.
	 * There is no free-cluster count anywhere on an exFAT volume; the bitmap
	 * IS the count, one bit per cluster, and without finding it there is no
	 * way to answer how much room is left or to allocate any.
	 *
	 * Entry types: &81 bitmap, &82 up-case table, &83 label, &85 a file. The
	 * top bit is "in use", so &03 is a deleted label and &05 a deleted file,
	 * and a first byte of zero ends the directory.
	 */
exfat_read_root:
	stmfd	sp!, {r1-r10, lr}
	sub	sp, sp, #32

	mov	r6, r0			@ volume
	mov	r9, #0			@ index

exfat_root_loop:
	mov	r0, r6
	mov	r1, #0			@ the root
	mov	r2, r9
	mov	r3, sp
	bl	dir_read_entry
	bvs	exfat_root_out
	cmp	r0, #0
	beq	exfat_root_done		@ ran off the end

	ldrb	r0, [sp]
	cmp	r0, #0
	beq	exfat_root_done		@ nothing beyond here has been used

	cmp	r0, #0x81
	beq	exfat_root_bitmap
	cmp	r0, #0x83
	beq	exfat_root_label

exfat_root_next:
	add	r9, r9, #1
	cmp	r9, #256		@ these live at the very front; do not walk it all
	blo	exfat_root_loop
	b	exfat_root_done

exfat_root_bitmap:
	@ Bit 0 of the flags byte says which of the two possible bitmaps this is;
	@ only the first is ours, and a volume that is not TexFAT has only one.
	ldrb	r0, [sp, #1]
	tst	r0, #1
	bne	exfat_root_next
	and	r0, r0, #2		@ NoFatChain, as for any other object
	str	r0, [r6, #VOL_BMP_CONTIG]

	mov	r0, sp
	mov	r1, #20
	bl	ld32
	str	r0, [r6, #VOL_BMP_CLUS]
	mov	r0, sp
	mov	r1, #24
	bl	ld32
	str	r0, [r6, #VOL_BMP_LEN]
	b	exfat_root_next

exfat_root_label:
	@ The label is UTF-16, up to eleven characters, with the count in byte 1.
	ldrb	r7, [sp, #1]
	cmp	r7, #11
	movhi	r7, #11
	add	r8, r6, #VOL_LABEL
	mov	r0, #0
exfat_root_label_copy:
	cmp	r0, r7
	bhs	exfat_root_label_end
	add	r1, sp, #2
	add	r1, r1, r0, lsl #1
	ldrb	r2, [r1]
	ldrb	r3, [r1, #1]
	cmp	r3, #0
	movne	r2, #'_'		@ nothing outside Latin-1 survives a path
	strb	r2, [r8, r0]
	add	r0, r0, #1
	b	exfat_root_label_copy
exfat_root_label_end:
	mov	r1, #0
	strb	r1, [r8, r0]
	mov	r0, r8
	bl	label_legalise
	b	exfat_root_next

exfat_root_done:
	@ A volume with no label still needs a name to appear under.
	ldrb	r0, [r6, #VOL_LABEL]
	cmp	r0, #0
	adreq	r1, exfat_no_label
	addeq	r0, r6, #VOL_LABEL
	bleq	copy_string_z

	cmp	pc, #0

exfat_root_out:
	add	sp, sp, #32
	ldmfd	sp!, {r1-r10, pc}

exfat_no_label:
	.string	"EXFAT"
	.align


	/* Copy a terminated string. Entry: R0 = to, R1 = from. */
copy_string_z:
	stmfd	sp!, {r0-r3, lr}
	mov	r2, #0
copy_string_z_loop:
	ldrb	r3, [r1, r2]
	strb	r3, [r0, r2]
	cmp	r3, #0
	addne	r2, r2, #1
	bne	copy_string_z_loop
	ldmfd	sp!, {r0-r3, pc}


	/* An exFAT boot record.
	 *
	 * Entry: R10 = drive, R11 = start LBA, the sector already in WS_SECTOR.
	 *
	 * exFAT is a different filing system that happens to keep a 32-bit FAT.
	 * Everything above the cluster layer differs - the directory has no 8.3
	 * names, no long-name fragments and no fixed root - but the cluster
	 * arithmetic is FAT32's, so the volume record is filled in with the same
	 * fields and the code below the directory layer does not care.
	 *
	 * The two shifts are logs, not counts: BytesPerSectorShift and
	 * SectorsPerClusterShift. Reading them as counts gives a 9-byte sector
	 * and a 6-sector cluster, which looks plausible enough to get a long way
	 * before anything goes wrong.
	 */
add_volume_exfat:
	@ R9 holds the record throughout. It must NOT be LR: every ld32 below is
	@ a BL, and a BL overwrites LR, so a record pointer kept there survives
	@ exactly one field. The FAT32 path above reloads it from WS_REC after
	@ each call for the same reason; a spare register is simpler.
	ldr	r2, [wp, #WS_NVOLUMES]
	mov	r3, r2, lsl #VOL_SHIFT
	add	r9, wp, #WS_VOLUMES
	add	r9, r9, r3

	str	r10, [r9, #VOL_DRIVE]
	str	r11, [r9, #VOL_START]

	mov	r0, #64			@ exFAT, in the same field as 12, 16 and 32
	str	r0, [r9, #VOL_TYPE]

	@ Bytes per sector and sectors per cluster are LOGS, not counts. Read as
	@ counts they give a nine-byte sector and a six-sector cluster, which is
	@ plausible enough to get a long way before anything looks wrong.
	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, #108]
	cmp	r1, #9			@ only 512-byte sectors: the buffers are 512
	bne	add_volume_bad
	mov	r2, #1
	mov	r2, r2, lsl r1
	str	r2, [r9, #VOL_BPS]

	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, #109]
	cmp	r1, #25
	bhs	add_volume_bad
	str	r1, [r9, #VOL_SPCLOG]
	mov	r2, #1
	mov	r2, r2, lsl r1
	str	r2, [r9, #VOL_SPC]

	add	r0, wp, #WS_SECTOR
	mov	r1, #80
	bl	ld32
	add	r0, r0, r11
	str	r0, [r9, #VOL_FATSEC0]

	add	r0, wp, #WS_SECTOR
	mov	r1, #84
	bl	ld32
	str	r0, [r9, #VOL_FATSZ]

	add	r0, wp, #WS_SECTOR
	ldrb	r0, [r0, #110]
	str	r0, [r9, #VOL_NFATS]

	@ The cluster heap: where cluster 2 begins.
	add	r0, wp, #WS_SECTOR
	mov	r1, #88
	bl	ld32
	str	r0, [r9, #VOL_FIRSTDATA]
	add	r0, r0, r11
	str	r0, [r9, #VOL_DATASEC0]

	add	r0, wp, #WS_SECTOR
	mov	r1, #92
	bl	ld32
	str	r0, [r9, #VOL_CLUSTERS]

	add	r0, wp, #WS_SECTOR
	mov	r1, #96
	bl	ld32
	str	r0, [r9, #VOL_ROOTCLUS]

	add	r0, wp, #WS_SECTOR
	mov	r1, #72
	bl	ld32
	str	r0, [r9, #VOL_TOTSEC]

	@ Things exFAT simply has not got.
	mov	r0, #0
	str	r0, [r9, #VOL_ROOTENTS]
	str	r0, [r9, #VOL_ROOTSEC0]
	str	r0, [r9, #VOL_ROOTSECS]
	str	r0, [r9, #VOL_RSVD]
	str	r0, [r9, #VOL_FSINFO]	@ no FSInfo: the allocation bitmap is the truth
	str	r0, [r9, #VOL_FSDIRTY]
	str	r0, [r9, #VOL_BMP_CLUS]
	str	r0, [r9, #VOL_BMP_LEN]
	str	r0, [r9, #VOL_BMP_CONTIG]
	strb	r0, [r9, #VOL_LABEL]
	mvn	r0, #0
	str	r0, [r9, #VOL_FREE]
	mov	r0, #2
	str	r0, [r9, #VOL_NEXTFREE]

	str	r9, [wp, #WS_REC]

	@ Count it in BEFORE reading the root: finding the bitmap and the label
	@ means walking the root through the ordinary machinery, and that expects
	@ the volume to be in the table.
	ldr	r2, [wp, #WS_NVOLUMES]
	add	r2, r2, #1
	str	r2, [wp, #WS_NVOLUMES]

	mov	r0, r9
	bl	exfat_read_root
	bvs	add_volume_exfat_undo

	cmp	pc, #0
	ldmfd	sp!, {r1-r11, pc}

add_volume_exfat_undo:
	ldr	r2, [wp, #WS_NVOLUMES]
	sub	r2, r2, #1
	str	r2, [wp, #WS_NVOLUMES]
	b	add_volume_out


	/* Read an entry set off the medium into WS_XSET.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = the set's first index,
	 *        R3 = how many entries.
	 * Exit:  V set on failure.
	 */
exfat_set_read:
	stmfd	sp!, {r0-r8, lr}
	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3
	ldr	r0, =WS_XSET
	add	r4, wp, r0
	mov	r3, #0
exfat_set_read_loop:
	cmp	r3, r8
	bhs	exfat_set_read_done
	cmp	r3, #WS_XSET_MAX
	bhs	exfat_set_read_done
	stmfd	sp!, {r3}
	mov	r0, r5
	mov	r1, r6
	add	r2, r7, r3
	add	r3, r4, r3, lsl #5
	bl	dir_read_entry
	ldmfd	sp!, {r3}
	bvs	exfat_set_read_out
	add	r3, r3, #1
	b	exfat_set_read_loop
exfat_set_read_done:
	cmp	pc, #0
exfat_set_read_out:
	ldmfd	sp!, {r0-r8, pc}


	/* Put WS_XSET back, checksum first.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = the set's first index,
	 *        R3 = how many entries.
	 */
exfat_set_write:
	stmfd	sp!, {r0-r8, lr}
	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3
	ldr	r0, =WS_XSET
	add	r4, wp, r0

	@ Anything that changed a byte of the set has invalidated the checksum,
	@ so it is always recomputed here rather than by the caller.
	mov	r0, r4
	mov	r1, r8
	bl	exfat_set_checksum
	strb	r0, [r4, #2]
	mov	r1, r0, lsr #8
	strb	r1, [r4, #3]

	mov	r3, #0
exfat_set_write_loop:
	cmp	r3, r8
	bhs	exfat_set_write_done
	cmp	r3, #WS_XSET_MAX
	bhs	exfat_set_write_done
	stmfd	sp!, {r3}
	mov	r0, r5
	mov	r1, r6
	add	r2, r7, r3
	add	r3, r4, r3, lsl #5
	bl	dir_write_entry
	ldmfd	sp!, {r3}
	bvs	exfat_set_write_out
	add	r3, r3, #1
	b	exfat_set_write_loop
exfat_set_write_done:
	cmp	pc, #0
exfat_set_write_out:
	ldmfd	sp!, {r0-r8, pc}


	/* Strike out an exFAT entry set.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = first index, R3 = count.
	 *
	 * exFAT deletes by clearing the top bit of each type byte - &85 becomes
	 * &05 - and every entry of the set has to be done, not just the first.
	 * A half-deleted set leaves name entries that belong to nothing.
	 */
exfat_set_delete:
	stmfd	sp!, {r0-r8, lr}
	sub	sp, sp, #32
	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3
	mov	r4, #0
exfat_set_delete_loop:
	cmp	r4, r8
	bhs	exfat_set_delete_done
	mov	r0, r5
	mov	r1, r6
	add	r2, r7, r4
	mov	r3, sp
	bl	dir_read_entry
	bvs	exfat_set_delete_out
	cmp	r0, #0
	beq	exfat_set_delete_done
	ldrb	r0, [sp]
	bic	r0, r0, #0x80
	strb	r0, [sp]
	mov	r0, r5
	mov	r1, r6
	add	r2, r7, r4
	mov	r3, sp
	bl	dir_write_entry
	bvs	exfat_set_delete_out
	add	r4, r4, #1
	b	exfat_set_delete_loop
exfat_set_delete_done:
	cmp	pc, #0
exfat_set_delete_out:
	add	sp, sp, #32
	ldmfd	sp!, {r0-r8, pc}


	/* Create an exFAT entry set for a name that is not there yet.
	 *
	 * Entry: R0 = volume, R1 = directory cluster (0 = root), R2 = the RISC OS
	 *        name, R3 = attributes.
	 * Exit:  R0 = the index the set starts at, R1 = entries in it.
	 *        V set on failure.
	 *
	 * An exFAT object is a run of consecutive entries and the whole run is
	 * one unit: a File entry saying how many follow, a Stream Extension with
	 * the length and first cluster, and File Name entries of fifteen UTF-16
	 * characters each. Two numbers tie it together and both must be right or
	 * other systems quietly disbelieve the lot - the SetChecksum over every
	 * byte except its own two, and the NameHash over the up-cased name.
	 */
exfat_create:
	stmfd	sp!, {r2-r12, lr}

	mov	r4, r0			@ volume
	mov	r5, r1			@ directory
	mov	r6, r2			@ the name
	mov	r11, r3			@ attributes

	ldr	r0, =WS_XSET
	add	r10, wp, r0

	@ How long the name is, and how many entries it needs.
	mov	r7, #0
exfat_create_len:
	ldrb	r0, [r6, r7]
	cmp	r0, #0
	beq	exfat_create_gotlen
	add	r7, r7, #1
	cmp	r7, #255
	blo	exfat_create_len
exfat_create_gotlen:
	cmp	r7, #0
	beq	exfat_create_bad

	add	r0, r7, #14
	mov	r8, #0
exfat_create_div:
	cmp	r0, #15
	blo	exfat_create_divdone
	sub	r0, r0, #15
	add	r8, r8, #1
	b	exfat_create_div
exfat_create_divdone:
	add	r9, r8, #2		@ entries in the whole set

	@ Clear the lot, then fill it in.
	mov	r0, #0
	mov	r1, #0
	mov	r2, r9, lsl #5
exfat_create_clear:
	strb	r0, [r10, r1]
	add	r1, r1, #1
	cmp	r1, r2
	blo	exfat_create_clear

	@ The File entry.
	mov	r0, #0x85
	strb	r0, [r10]
	add	r0, r8, #1		@ the stream entry plus the name entries
	strb	r0, [r10, #1]
	strb	r11, [r10, #4]		@ attributes, in FAT's own bits

	bl	fat_now			@ R0 = time, R1 = date
	orr	r2, r0, r1, lsl #16
	strb	r2, [r10, #8]		@ created
	mov	r3, r2, lsr #8
	strb	r3, [r10, #9]
	mov	r3, r2, lsr #16
	strb	r3, [r10, #10]
	mov	r3, r2, lsr #24
	strb	r3, [r10, #11]
	strb	r2, [r10, #12]		@ last modified
	mov	r3, r2, lsr #8
	strb	r3, [r10, #13]
	mov	r3, r2, lsr #16
	strb	r3, [r10, #14]
	mov	r3, r2, lsr #24
	strb	r3, [r10, #15]
	strb	r2, [r10, #16]		@ last accessed
	mov	r3, r2, lsr #8
	strb	r3, [r10, #17]
	mov	r3, r2, lsr #16
	strb	r3, [r10, #18]
	mov	r3, r2, lsr #24
	strb	r3, [r10, #19]

	@ The Stream Extension. Bit 0 of the flags says an allocation is possible
	@ at all; bit 1 would say the run is contiguous, and nothing written here
	@ is - everything gets a chain, which is always legal.
	add	r1, r10, #32
	mov	r0, #0xc0
	strb	r0, [r1]
	mov	r0, #1
	strb	r0, [r1, #1]
	strb	r7, [r1, #3]		@ characters in the name

	mov	r0, r6
	bl	exfat_name_hash
	strb	r0, [r1, #4]
	mov	r2, r0, lsr #8
	strb	r2, [r1, #5]

	@ The name entries.
	@
	@ R4 and R5 are borrowed as scratch and put back afterwards. They hold
	@ the volume and the DIRECTORY, and using R5 without saving it left the
	@ write below asking for a cluster the size of a workspace address - which
	@ came back as "cannot reach that far into the disc", a long way from the
	@ actual mistake.
	stmfd	sp!, {r4, r5}
	mov	r2, #0			@ characters placed
	mov	r3, #0			@ which name entry
exfat_create_name:
	cmp	r3, r8
	bhs	exfat_create_sum
	add	r1, r10, #64
	add	r1, r1, r3, lsl #5
	mov	r0, #0xc1
	strb	r0, [r1]
	mov	r0, #0
	strb	r0, [r1, #1]

	mov	r0, #0
exfat_create_chars:
	cmp	r0, #15
	bhs	exfat_create_name_done
	cmp	r2, r7
	bhs	exfat_create_name_done
	@ R4 and R5, not R12: R12 is wp. Using it as scratch here left every
	@ routine called afterwards reading its workspace through the last
	@ character of the name.
	ldrb	r4, [r6, r2]
	cmp	r4, #'/'
	moveq	r4, #'.'		@ RISC OS's separator becomes the disc's
	add	r5, r1, #2
	add	r5, r5, r0, lsl #1
	strb	r4, [r5]
	mov	r4, #0
	strb	r4, [r5, #1]
	add	r0, r0, #1
	add	r2, r2, #1
	b	exfat_create_chars

exfat_create_name_done:
	add	r3, r3, #1
	b	exfat_create_name

exfat_create_sum:
	ldmfd	sp!, {r4, r5}
	mov	r0, r10
	mov	r1, r9
	bl	exfat_set_checksum
	strb	r0, [r10, #2]
	mov	r1, r0, lsr #8
	strb	r1, [r10, #3]

	@ Somewhere to put it. This may extend the directory, and extending a
	@ directory takes a cluster - which is why any cluster for the object
	@ itself is claimed AFTER this and not before.
	mov	r0, r4
	mov	r1, r5
	mov	r2, r9
	bl	dir_find_run
	bvs	exfat_create_out
	mov	r11, r0			@ where the set starts

	@ A directory needs a cluster of its own, and gets it here rather than
	@ from the caller. Handing the caller an index to read back, patch and
	@ rewrite is one more chance to patch the wrong set - which is exactly
	@ what happened: the directory ended up recording the cluster belonging
	@ to the file created before it.
	ldr	r0, =WS_X_ALLOC
	ldr	r0, [wp, r0]
	cmp	r0, #0
	beq	exfat_create_write_set
	ldrb	r0, [r10, #4]
	tst	r0, #0x10
	beq	exfat_create_write_set

	stmfd	sp!, {r3, r4, r5, r9, r10, r11}
	mov	r0, r4
	bl	alloc_cluster
	ldmfd	sp!, {r3, r4, r5, r9, r10, r11}
	bvs	exfat_create_out
	mov	r3, r0			@ the cluster

	stmfd	sp!, {r3, r4, r5, r9, r10, r11}
	mov	r0, r4
	mov	r1, r3
	bl	dir_clear_cluster
	ldmfd	sp!, {r3, r4, r5, r9, r10, r11}
	bvs	exfat_create_out

	add	r1, r10, #32		@ the stream extension
	strb	r3, [r1, #20]
	mov	r0, r3, lsr #8
	strb	r0, [r1, #21]
	mov	r0, r3, lsr #16
	strb	r0, [r1, #22]
	mov	r0, r3, lsr #24
	strb	r0, [r1, #23]

	@ A directory's length is what is allocated to it.
	ldr	r0, [r4, #VOL_SPCLOG]
	add	r0, r0, #9
	mov	r2, #1
	mov	r2, r2, lsl r0
	strb	r2, [r1, #24]
	mov	r0, r2, lsr #8
	strb	r0, [r1, #25]
	mov	r0, r2, lsr #16
	strb	r0, [r1, #26]
	mov	r0, r2, lsr #24
	strb	r0, [r1, #27]
	strb	r2, [r1, #8]
	mov	r0, r2, lsr #8
	strb	r0, [r1, #9]
	mov	r0, r2, lsr #16
	strb	r0, [r1, #10]
	mov	r0, r2, lsr #24
	strb	r0, [r1, #11]

	@ The set changed, so its checksum has to be worked out again.
	mov	r0, r10
	mov	r1, r9
	bl	exfat_set_checksum
	strb	r0, [r10, #2]
	mov	r1, r0, lsr #8
	strb	r1, [r10, #3]

exfat_create_write_set:

	@ Everything this loop needs is saved across the call. Nothing below
	@ dir_write_entry preserves R11 - the deepest of them saves only up to
	@ R10 - so the run's start came back as rubbish and the caller was told
	@ the set had been written somewhere absurd.
	mov	r3, #0
exfat_create_write:
	cmp	r3, r9
	bhs	exfat_create_done
	stmfd	sp!, {r3, r4, r5, r9, r10, r11}
	mov	r0, r4
	mov	r1, r5
	add	r2, r11, r3
	add	r3, r10, r3, lsl #5
	bl	dir_write_entry
	ldmfd	sp!, {r3, r4, r5, r9, r10, r11}
	bvs	exfat_create_out
	add	r3, r3, #1
	b	exfat_create_write

exfat_create_done:
	mov	r0, r11
	mov	r1, r9
	cmp	pc, #0

exfat_create_out:
	ldmfd	sp!, {r2-r12, pc}

exfat_create_bad:
	adrl	r0, err_bad_name
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r2-r12, pc}


	/* Up-case one character, exFAT's way as far as ASCII goes.
	 *
	 * Entry: R0 = a character. Exit: R0 = its upper case.
	 *
	 * exFAT keeps an up-case TABLE on the volume and expects names to be
	 * folded through it before they are hashed or compared. For ASCII that
	 * table is the obvious mapping, which is what this does. Above &7F it is
	 * not necessarily, so a name with accented characters written here may
	 * get a hash Windows disagrees with, and Windows uses the hash to decide
	 * which entries are worth looking at - the file would still be listed but
	 * might not be found by name. Reading the table properly is the fix; it
	 * can be run-length compressed, which is why it has not been done yet.
	 */
exfat_upcase:
	@ R1 is saved because the hash below keeps its accumulator there, and an
	@ RSBGES scratching R1 to test the range destroyed it on every character.
	stmfd	sp!, {r1, lr}
	cmp	r0, #'a'
	blt	exfat_upcase_out
	cmp	r0, #'z'
	subls	r0, r0, #32
exfat_upcase_out:
	ldmfd	sp!, {r1, pc}


	/* The hash exFAT keeps of an up-cased name.
	 *
	 * Entry: R0 = the name, 8-bit and terminated. Exit: R0 = the hash.
	 *
	 * Sixteen bits, rotated right one place and added, over every BYTE of the
	 * UTF-16 name - so each character contributes twice, low half then high.
	 * The high half is zero for everything this writes, but it still has to
	 * go through the sum or the hash is not the one anybody else computes.
	 */
exfat_name_hash:
	stmfd	sp!, {r1-r5, lr}
	mov	r4, r0
	mov	r1, #0			@ the running hash
	mov	r2, #0
exfat_hash_loop:
	ldrb	r0, [r4, r2]
	cmp	r0, #0
	beq	exfat_hash_done
	cmp	r0, #'/'
	moveq	r0, #'.'		@ the name as the disc will hold it
	bl	exfat_upcase

	@ Low byte, then the high byte which is always zero.
	mov	r3, r1, lsl #15
	mov	r5, r1, lsr #1
	orr	r3, r5, r3
	add	r1, r3, r0
	mov	r1, r1, lsl #16
	mov	r1, r1, lsr #16

	mov	r3, r1, lsl #15
	mov	r5, r1, lsr #1
	orr	r3, r5, r3
	add	r1, r3, #0
	mov	r1, r1, lsl #16
	mov	r1, r1, lsr #16

	add	r2, r2, #1
	b	exfat_hash_loop

exfat_hash_done:
	mov	r0, r1
	ldmfd	sp!, {r1-r5, pc}


	/* The checksum that ties an entry set together.
	 *
	 * Entry: R0 = the set, laid out end to end, R1 = how many entries.
	 * Exit:  R0 = the checksum.
	 *
	 * Sixteen bits, rotated right and added over every byte of every entry -
	 * except bytes 2 and 3 of the FIRST entry, which are where the answer
	 * goes. Miss that exclusion and the set never validates.
	 */
exfat_set_checksum:
	stmfd	sp!, {r1-r6, lr}
	mov	r4, r0
	mov	r5, r1, lsl #5		@ bytes in the set
	mov	r1, #0
	mov	r2, #0
exfat_sum_loop:
	cmp	r2, r5
	bhs	exfat_sum_done
	cmp	r2, #2
	beq	exfat_sum_skip
	cmp	r2, #3
	beq	exfat_sum_skip
	ldrb	r0, [r4, r2]
	mov	r3, r1, lsl #15
	mov	r6, r1, lsr #1
	orr	r3, r6, r3
	add	r1, r3, r0
	mov	r1, r1, lsl #16
	mov	r1, r1, lsr #16
exfat_sum_skip:
	add	r2, r2, #1
	b	exfat_sum_loop

exfat_sum_done:
	mov	r0, r1
	ldmfd	sp!, {r1-r6, pc}


	/* Where a cluster's bit lives in the exFAT allocation bitmap.
	 *
	 * Entry: R0 = volume, R1 = cluster.
	 * Exit:  R0 = the absolute sector holding it, R1 = the byte within that
	 *        sector, R2 = the bit's mask. V set on failure, R0 = 0 if the
	 *        cluster is outside the volume.
	 *
	 * The bitmap describes cluster 2 onwards with bit 0 of byte 0, and is
	 * itself an ordinary exFAT object - a chain, or a contiguous run - so
	 * getting at a far-off bit means walking to the right cluster of it.
	 */
bmp_locate:
	stmfd	sp!, {r3-r10, lr}

	mov	r7, r0			@ volume
	sub	r8, r1, #2		@ bit number
	ldr	r0, [r7, #VOL_CLUSTERS]
	cmp	r8, r0
	bhs	bmp_locate_none
	cmp	r1, #2
	blo	bmp_locate_none

	and	r2, r8, #7
	mov	r9, #1
	mov	r9, r9, lsl r2		@ the mask
	mov	r8, r8, lsr #3		@ byte offset into the bitmap

	@ Which cluster of the bitmap that byte is in, and where inside it.
	ldr	r0, [r7, #VOL_SPCLOG]
	add	r0, r0, #9		@ bytes per cluster, as a shift
	mov	r5, r8, lsr r0		@ clusters to step over
	mov	r6, #1
	mov	r6, r6, lsl r0
	sub	r6, r6, #1
	and	r6, r8, r6		@ offset within that cluster

	ldr	r4, [r7, #VOL_BMP_CLUS]
	cmp	r4, #0
	beq	bmp_locate_none

bmp_locate_step:
	cmp	r5, #0
	beq	bmp_locate_have
	mov	r0, r7
	mov	r1, r4
	ldr	r2, [r7, #VOL_BMP_CONTIG]
	bl	chain_next
	bvs	bmp_locate_out
	cmp	r0, #0
	beq	bmp_locate_none
	mov	r4, r0
	sub	r5, r5, #1
	b	bmp_locate_step

bmp_locate_have:
	mov	r0, r7
	mov	r1, r4
	bl	cluster_sector
	add	r0, r0, r6, lsr #9
	ldr	r1, =511
	and	r1, r6, r1
	mov	r2, r9
	cmp	pc, #0
	ldmfd	sp!, {r3-r10, pc}

bmp_locate_none:
	mov	r0, #0
	cmp	pc, #0

bmp_locate_out:
	ldmfd	sp!, {r3-r10, pc}


	/* Is this cluster in use?
	 *
	 * Entry: R0 = volume, R1 = cluster.
	 * Exit:  R0 = non-zero if it is allocated. V set on a read error.
	 */
bmp_get:
	stmfd	sp!, {r1-r7, lr}

	mov	r7, r0
	bl	bmp_locate
	bvs	bmp_get_out
	cmp	r0, #0
	beq	bmp_get_used		@ outside the volume: never hand it out

	mov	r4, r1			@ byte within the sector
	mov	r5, r2			@ the bit
	mov	r1, r0
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	bmp_get_out

	add	r0, wp, #WS_SECTOR
	ldrb	r0, [r0, r4]
	and	r0, r0, r5
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

bmp_get_used:
	mov	r0, #1
	cmp	pc, #0

bmp_get_out:
	ldmfd	sp!, {r1-r7, pc}


	/* Claim or release a cluster in the bitmap.
	 *
	 * Entry: R0 = volume, R1 = cluster, R2 = non-zero to claim it.
	 * Exit:  V set on failure.
	 */
bmp_set:
	stmfd	sp!, {r0-r8, lr}

	mov	r7, r0
	mov	r8, r2
	bl	bmp_locate
	bvs	bmp_set_out
	cmp	r0, #0
	beq	bmp_set_done		@ outside the volume: nothing to record

	mov	r4, r1
	mov	r5, r2
	mov	r6, r0			@ the sector

	mov	r1, r6
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	bmp_set_out

	add	r0, wp, #WS_SECTOR
	ldrb	r1, [r0, r4]
	cmp	r8, #0
	orrne	r1, r1, r5
	biceq	r1, r1, r5
	strb	r1, [r0, r4]

	mov	r1, r6
	ldr	r0, [r7, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	write_sector
	bvs	bmp_set_out

bmp_set_done:
	cmp	pc, #0

bmp_set_out:
	ldmfd	sp!, {r0-r8, pc}


	/* Give a whole cluster chain back.
	 *
	 * Entry: R0 = volume record, R1 = first cluster (0 means nothing to do).
	 * Exit:  V set on failure.
	 */
	/* Give a contiguous run back to the bitmap.
	 *
	 * Entry: R0 = volume, R1 = first cluster, R2 = how many.
	 * Exit:  V set on failure.
	 *
	 * exFAT's NoFatChain files have no chain to walk - the FAT entries were
	 * never written - so the count from the extent is the only thing that
	 * says where the run ends.
	 */
free_run:
	stmfd	sp!, {r0-r6, lr}

	mov	r5, r0
	mov	r6, r1
	mov	r4, r2

free_run_loop:
	cmp	r4, #0
	beq	free_run_done
	mov	r0, r5
	mov	r1, r6
	mov	r2, #0
	bl	bmp_set
	bvs	free_run_out

	ldr	r0, [r5, #VOL_FREE]
	cmn	r0, #1
	addne	r0, r0, #1
	strne	r0, [r5, #VOL_FREE]

	add	r6, r6, #1
	sub	r4, r4, #1
	b	free_run_loop

free_run_done:
	mov	r0, #1
	str	r0, [r5, #VOL_FSDIRTY]
	cmp	pc, #0

free_run_out:
	ldmfd	sp!, {r0-r6, pc}


free_chain:
	stmfd	sp!, {r0-r6, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ cluster

free_chain_loop:
	cmp	r6, #2
	blo	free_chain_done
	ldr	r0, [r5, #VOL_CLUSTERS]
	add	r0, r0, #2
	cmp	r6, r0
	bhs	free_chain_done

	@ Where it goes next has to be read before the link is broken.
	mov	r0, r5
	mov	r1, r6
	bl	fat_raw
	bvs	free_chain_out
	mov	r4, r0

	mov	r0, r5
	mov	r1, r6
	mov	r2, #0
	bl	fat_set
	bvs	free_chain_out

	@ exFAT: the bitmap is what actually says a cluster is free.
	ldr	r0, [r5, #VOL_TYPE]
	cmp	r0, #64
	bne	free_chain_counted
	mov	r0, r5
	mov	r1, r6
	mov	r2, #0
	bl	bmp_set
	bvs	free_chain_out

free_chain_counted:
	ldr	r0, [r5, #VOL_FREE]
	cmn	r0, #1
	addne	r0, r0, #1
	strne	r0, [r5, #VOL_FREE]

	mov	r0, #1
	str	r0, [r5, #VOL_FSDIRTY]

	@ Stop at an end marker rather than trying to free it.
	ldr	r0, [r5, #VOL_TYPE]
	cmp	r0, #64
	ldreq	r0, =0xfffffff8
	beq	free_chain_endtest
	cmp	r0, #32
	ldreq	r0, =0x0ffffff8
	ldrne	r0, =0xfff8
free_chain_endtest:
	cmp	r4, r0
	bhs	free_chain_done
	mov	r6, r4
	b	free_chain_loop

free_chain_done:
	cmp	pc, #0

free_chain_out:
	ldmfd	sp!, {r0-r6, pc}

	.ltorg


	/* The next object in an exFAT directory.
	 *
	 * Exit: R0 = a 32-byte entry in FAT's shape, or 0 at the end of the
	 *       directory. VS on a read error.
	 *
	 * An exFAT object is not one entry but a SET of them, consecutive and
	 * counted: a File entry (&85) saying how many follow, then a Stream
	 * Extension (&C0) carrying the length and the first cluster, then one or
	 * more File Name entries (&C1) carrying fifteen UTF-16 characters each.
	 * Nothing in the set resembles an 8.3 name and there is no checksum tying
	 * a name to an entry the way VFAT's does - the set is the unit.
	 *
	 * What comes back is a synthesised FAT directory entry, because
	 * everything above this - path_lookup, fs_file, fs_open, fs_getbytes -
	 * already knows how to read one. The name goes straight into WS_BUILT,
	 * which build_entry_name hands back untouched for an exFAT volume.
	 *
	 * The one thing that does not fit in FAT's shape is NoFatChain, which
	 * says the file is a contiguous run and its FAT entries are meaningless.
	 * That goes in WS_IT_CONTIG, and path_lookup copies it onward.
	 */
dir_next_exfat:
	bl	dir_raw_next
	bvs	dir_next_out
	cmp	r0, #0
	beq	dir_next_end
	mov	r1, r0

	ldrb	r2, [r1]
	cmp	r2, #0
	beq	dir_next_end		@ never used, so nothing beyond here either
	tst	r2, #0x80
	beq	dir_next_exfat		@ in-use bit clear: deleted
	cmp	r2, #0x85
	bne	dir_next_exfat		@ bitmap, up-case table or label

	@ Where this set begins, for anything that later wants to change it.
	ldr	r0, [wp, #WS_IT_INDEX]
	sub	r0, r0, #1
	str	r0, [wp, #WS_IT_SETIDX]
	ldrb	r0, [r1, #1]
	add	r0, r0, #1		@ the File entry itself counts
	str	r0, [wp, #WS_IT_SETCNT]

	@ Everything wanted from this slot has to be taken now: the next read
	@ may bring in a different sector and the pointer stops being valid.
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	mov	r0, #0
	mov	r2, #0
dir_next_exfat_clear:
	strb	r0, [r3, r2]
	add	r2, r2, #1
	cmp	r2, #32
	blo	dir_next_exfat_clear

	@ The eleven name bytes are never read on exFAT - the name comes from the
	@ set - but they must not be left as zeros: a zero first byte is how the
	@ code above recognises "this is the root, which has no entry", so an
	@ ordinary file looked like the root and refused to be deleted.
	mov	r0, #' '
	mov	r2, #0
dir_next_exfat_pad:
	strb	r0, [r3, r2]
	add	r2, r2, #1
	cmp	r2, #11
	blo	dir_next_exfat_pad

	ldrb	r7, [r1, #1]		@ how many entries follow this one

	mov	r0, r1
	mov	r1, #4
	bl	ld16			@ file attributes, in FAT's own bits
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #11]

	@ Timestamps. exFAT packs each as one word, time in the low half and date
	@ in the high, which is exactly how FAT keeps them in two. The slot
	@ pointer is worked out again from the iterator each time because ld32
	@ costs R0 and R1.
	ldr	r0, [wp, #WS_IT_OFF]
	sub	r0, r0, #32
	add	r1, wp, #WS_DIRSEC
	add	r1, r1, r0

	mov	r0, r1
	mov	r1, #8
	bl	ld32			@ created
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #14]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #15]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #16]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #17]

	ldr	r0, [wp, #WS_IT_OFF]
	sub	r0, r0, #32
	add	r1, wp, #WS_DIRSEC
	add	r1, r1, r0
	mov	r0, r1
	mov	r1, #12
	bl	ld32			@ last modified
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #22]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #23]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #24]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #25]
	strb	r2, [r3, #19]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #18]

	@ The stream extension, which must be the very next slot.
	bl	dir_raw_next
	bvs	dir_next_out
	cmp	r0, #0
	beq	dir_next_end
	mov	r1, r0
	ldrb	r2, [r1]
	cmp	r2, #0xc0
	bne	dir_next_exfat		@ a set that makes no sense: skip the lot

	ldrb	r2, [r1, #1]
	and	r2, r2, #2		@ NoFatChain
	str	r2, [wp, #WS_IT_CONTIG]
	ldrb	r6, [r1, #3]		@ characters in the name

	mov	r0, r1
	mov	r1, #20
	bl	ld32			@ first cluster
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #26]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #27]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #20]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #21]

	ldr	r0, [wp, #WS_IT_OFF]
	sub	r0, r0, #32
	add	r1, wp, #WS_DIRSEC
	add	r1, r1, r0
	mov	r0, r1
	mov	r1, #24
	bl	ld32			@ length, low half of a 64-bit value
	ldr	r3, =WS_XENT
	add	r3, wp, r3
	strb	r0, [r3, #28]
	mov	r2, r0, lsr #8
	strb	r2, [r3, #29]
	mov	r2, r0, lsr #16
	strb	r2, [r3, #30]
	mov	r2, r0, lsr #24
	strb	r2, [r3, #31]

	@ Then the name, fifteen characters to an entry.
	sub	r7, r7, #1		@ the stream entry is one of the count
	ldr	r4, =WS_BUILT
	add	r4, wp, r4
	mov	r5, #0			@ characters taken so far

dir_next_exfat_name:
	cmp	r7, #0
	beq	dir_next_exfat_done
	bl	dir_raw_next
	bvs	dir_next_out
	cmp	r0, #0
	beq	dir_next_exfat_done
	mov	r1, r0
	ldrb	r2, [r1]
	cmp	r2, #0xc1
	bne	dir_next_exfat_done	@ not a name entry: take what there is

	mov	r2, #0
dir_next_exfat_chars:
	cmp	r2, #15
	bhs	dir_next_exfat_chars_done
	cmp	r5, r6
	bhs	dir_next_exfat_chars_done
	add	r3, r1, #2
	add	r3, r3, r2, lsl #1
	ldrb	r0, [r3]
	ldrb	r3, [r3, #1]
	cmp	r3, #0
	movne	r0, #'_'		@ nothing outside Latin-1 survives a path
	@ RISC OS's separator and the disc's change places. This has to BRANCH
	@ out after the first match: a second CMP does not execute when the first
	@ matched, and the stale Z puts the character straight back.
	cmp	r0, #'.'
	moveq	r0, #'/'
	beq	dir_next_exfat_store
	cmp	r0, #'/'
	moveq	r0, #'.'

dir_next_exfat_store:
	cmp	r5, #LFN_MAX
	strlob	r0, [r4, r5]
	add	r5, r5, #1
	add	r2, r2, #1
	b	dir_next_exfat_chars

dir_next_exfat_chars_done:
	sub	r7, r7, #1
	b	dir_next_exfat_name

dir_next_exfat_done:
	cmp	r5, #LFN_MAX
	movhs	r5, #LFN_MAX
	mov	r0, #0
	strb	r0, [r4, r5]

	ldr	r0, =WS_XENT
	add	r0, wp, r0
	cmp	pc, #0
	ldmfd	sp!, {r1-r7, pc}

	.ltorg
