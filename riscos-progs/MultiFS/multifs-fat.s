@ MultiFS - FAT12, FAT16 and FAT32
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
@ What is FAT's and nobody else's: 8.3 names and the long-name fragments that
@ hang in front of them, the alias generator, entry creation and deletion, the
@ file type convention borrowed from the creation timestamp, and the FSInfo
@ sector. exFAT shares the cluster layer below this and none of this.

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


@ ---------------------------------------------------------------------------
@ Long file names
@ ---------------------------------------------------------------------------

	/* Forget any long name gathered so far. */
lfn_reset:
	stmfd	sp!, {r0-r1, lr}
	ldr	r1, =WS_LFN
	add	r1, wp, r1
	mov	r0, #0
	str	r0, [r1, #LN_OK]
	str	r0, [r1, #LN_EXPECT]
	str	r0, [r1, #LN_COUNT]
	str	r0, [r1, #LN_CKSUM]
	strb	r0, [r1, #LN_TEXT]
	ldmfd	sp!, {r0-r1, pc}


	/* The checksum VFAT stamps on every fragment of a long name.
	 *
	 * Entry: R0 = the 8.3 directory entry. Exit: R0 = the checksum.
	 *
	 * It is computed over the eleven raw name bytes, rotating right one place
	 * and adding, all in eight bits. It is the only thing tying a run of
	 * fragments to the entry they claim to name: a disc written by something
	 * that did not understand long names leaves the fragments behind as
	 * orphans, and this is what notices.
	 */
lfn_checksum:
	stmfd	sp!, {r1-r4, lr}
	mov	r4, r0
	mov	r1, #0			@ the running sum
	mov	r2, #0
lfn_checksum_loop:
	mov	r3, r1, lsl #7
	and	r3, r3, #0x80		@ (sum & 1) << 7
	orr	r3, r3, r1, lsr #1	@ + (sum >> 1)
	ldrb	r0, [r4, r2]
	add	r1, r3, r0
	and	r1, r1, #0xff
	add	r2, r2, #1
	cmp	r2, #11
	blo	lfn_checksum_loop
	mov	r0, r1
	ldmfd	sp!, {r1-r4, pc}


	/* Take in one fragment of a long name.
	 *
	 * Entry: R0 = a directory entry whose attribute byte is &0F.
	 *
	 * VFAT writes the fragments backwards. The one carrying the LAST thirteen
	 * characters comes first and is flagged with bit 6 of its ordinal; the
	 * ordinals then count down to one, which sits immediately before the 8.3
	 * entry they all belong to. So each fragment is dropped into its own slot
	 * rather than appended, and the run is checked as it goes: a fragment out
	 * of order, or carrying a different checksum, marks the whole run bad and
	 * the 8.3 name is used instead. Being wrong about a file's name is worse
	 * than being dull about it.
	 */
lfn_take:
	stmfd	sp!, {r0-r8, lr}

	mov	r4, r0
	ldr	r8, =WS_LFN
	add	r8, wp, r8

	ldrb	r5, [r4]		@ ordinal, with its flags
	ldrb	r6, [r4, #13]		@ the checksum this fragment carries

	tst	r5, #0x40
	beq	lfn_take_continue

	@ The first fragment of a run, which is the last of the name.
	and	r5, r5, #0x3f
	cmp	r5, #0
	beq	lfn_take_bad
	cmp	r5, #LFN_MAX_ORD
	bhi	lfn_take_bad
	str	r5, [r8, #LN_COUNT]
	str	r6, [r8, #LN_CKSUM]
	mov	r0, #1
	str	r0, [r8, #LN_OK]
	b	lfn_take_place

lfn_take_continue:
	ldr	r0, [r8, #LN_OK]
	cmp	r0, #0
	beq	lfn_take_out		@ already bad, so nothing to spoil
	and	r5, r5, #0x3f
	ldr	r0, [r8, #LN_CKSUM]
	cmp	r0, r6
	bne	lfn_take_bad

lfn_take_place:
	@ Ordinals count down, so this one has to be the one expected - except
	@ for the flagged fragment, which starts the count.
	ldr	r0, [r8, #LN_EXPECT]
	cmp	r0, #0
	beq	lfn_take_expect_ok
	cmp	r0, r5
	bne	lfn_take_bad
lfn_take_expect_ok:
	sub	r0, r5, #1
	str	r0, [r8, #LN_EXPECT]

	@ Thirteen characters, at 1, 14 and 28 in the entry.
	sub	r7, r5, #1
	mov	r0, #LFN_CHARS
	mul	r7, r0, r7		@ where in the name they go
	add	r7, r7, #LN_TEXT
	add	r7, r8, r7

	mov	r0, r4
	mov	r1, #1
	mov	r2, #5
	bl	lfn_chars
	mov	r0, r4
	mov	r1, #14
	mov	r2, #6
	bl	lfn_chars
	mov	r0, r4
	mov	r1, #28
	mov	r2, #2
	bl	lfn_chars
	b	lfn_take_out

lfn_take_bad:
	mov	r0, #0
	str	r0, [r8, #LN_OK]

lfn_take_out:
	ldmfd	sp!, {r0-r8, pc}


	/* Copy some of a fragment's characters out.
	 *
	 * Entry: R0 = entry, R1 = offset in it, R2 = how many, R7 = destination,
	 *        advanced as it goes.
	 *
	 * The characters are UCS-2 and RISC OS filenames are bytes, so anything
	 * above &FF becomes a question mark rather than being truncated into a
	 * different letter. A NUL or &FFFF is padding and ends the fragment.
	 *
	 * Dot and slash change places. RISC OS separates directories with a dot
	 * and separates a name from its extension with a slash, which is exactly
	 * the other way round from the world the disc came from - the same swap
	 * the 8.3 name gets a few lines above.
	 */
lfn_chars:
	stmfd	sp!, {r0-r6, lr}
	mov	r4, r0
	mov	r5, #0
lfn_chars_loop:
	cmp	r5, r2
	bhs	lfn_chars_out

	add	r6, r1, r5, lsl #1
	ldrb	r0, [r4, r6]
	add	r6, r6, #1
	ldrb	r3, [r4, r6]

	cmp	r3, #0
	bne	lfn_chars_high
	cmp	r0, #0
	beq	lfn_chars_end		@ the name ended inside this fragment

	@ Two separate tests, not one chain: writing the swap as CMP/MOVEQ then
	@ CMPNE/MOVEQ looks symmetrical and is not, because the second CMP does
	@ not execute when the first matched and the stale Z then puts the
	@ character straight back the way it was.
	cmp	r0, #'.'
	moveq	r0, #'/'
	beq	lfn_chars_store
	cmp	r0, #'/'
	moveq	r0, #'.'
	b	lfn_chars_store

lfn_chars_high:
	cmp	r3, #0xff
	cmpeq	r0, #0xff
	beq	lfn_chars_out		@ &FFFF is padding past the end
	mov	r0, #'?'
	b	lfn_chars_store

	@ Keep the terminator rather than just stopping at it. The buffer is not
	@ cleared between names, so without this a short name shows the tail of
	@ the last long one: "ReadMe/md" came out as "ReadMe/mdhars" after
	@ "ThirteenChars".
lfn_chars_end:
	mov	r0, #0
	strb	r0, [r7]
	b	lfn_chars_out

lfn_chars_store:
	strb	r0, [r7], #1
	add	r5, r5, #1
	b	lfn_chars_loop

lfn_chars_out:
	ldmfd	sp!, {r0-r6, pc}

	.ltorg


	/* The long name gathered for this entry, if there is a good one.
	 *
	 * Entry: R0 = the 8.3 directory entry.
	 * Exit:  R0 = a pointer to the name, or 0 if the 8.3 name should be used.
	 */
lfn_name:
	stmfd	sp!, {r1-r6, lr}

	ldr	r6, =WS_LFN
	add	r6, wp, r6

	ldr	r1, [r6, #LN_OK]
	cmp	r1, #0
	beq	lfn_name_none

	@ Every fragment down to ordinal one has to have turned up.
	ldr	r1, [r6, #LN_EXPECT]
	cmp	r1, #0
	bne	lfn_name_none

	bl	lfn_checksum
	ldr	r1, [r6, #LN_CKSUM]
	cmp	r0, r1
	bne	lfn_name_none

	@ The name fills its fragments exactly when there is no padding to
	@ terminate it, so the end is capped at what the run said it holds.
	ldr	r1, [r6, #LN_COUNT]
	mov	r2, #LFN_CHARS
	mul	r3, r2, r1
	add	r4, r6, #LN_TEXT
	mov	r2, #0
lfn_name_end:
	cmp	r2, r3
	bhs	lfn_name_terminate
	ldrb	r0, [r4, r2]
	cmp	r0, #0
	beq	lfn_name_terminate
	add	r2, r2, #1
	b	lfn_name_end
lfn_name_terminate:
	cmp	r2, #0
	beq	lfn_name_none		@ an empty name is no name
	mov	r0, #0
	strb	r0, [r4, r2]
	mov	r0, r4
	ldmfd	sp!, {r1-r6, pc}

lfn_name_none:
	mov	r0, #0
	ldmfd	sp!, {r1-r6, pc}


	/* Put the free count back into the FAT32 FSInfo sector.
	 *
	 * Entry: R0 = volume record, R1 = free clusters.
	 * Exit:  V set on failure, which is not worth passing on to a caller: a
	 *        hint that could not be written is a hint, not a lost file.
	 *
	 * FSInfo is only a cache, and every system is entitled to ignore it and
	 * count the FAT. Windows does not: it believes this number, so leaving a
	 * stale one behind is worse than leaving none. The two signatures are
	 * checked before anything is changed, so a sector that is not an FSInfo
	 * is never written over.
	 */
fsinfo_write:
	stmfd	sp!, {r0-r8, lr}

	mov	r5, r0
	mov	r6, r1

	ldr	r7, [r5, #VOL_FSINFO]
	cmp	r7, #0
	beq	fsinfo_write_out

	mov	r1, r7
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	read_sector
	bvs	fsinfo_write_out

	add	r0, wp, #WS_SECTOR
	mov	r1, #0
	bl	ld32
	ldr	r1, =0x41615252		@ "RRaA"
	cmp	r0, r1
	bne	fsinfo_write_out

	add	r0, wp, #WS_SECTOR
	ldr	r1, =484
	bl	ld32
	ldr	r1, =0x61417272		@ "rrAa"
	cmp	r0, r1
	bne	fsinfo_write_out

	add	r4, wp, #WS_SECTOR
	ldr	r0, =488
	add	r4, r4, r0
	strb	r6, [r4]
	mov	r0, r6, lsr #8
	strb	r0, [r4, #1]
	mov	r0, r6, lsr #16
	strb	r0, [r4, #2]
	mov	r0, r6, lsr #24
	strb	r0, [r4, #3]

	@ And where to start looking next time, which is only a hint too.
	ldr	r0, [r5, #VOL_NEXTFREE]
	strb	r0, [r4, #4]
	mov	r1, r0, lsr #8
	strb	r1, [r4, #5]
	mov	r1, r0, lsr #16
	strb	r1, [r4, #6]
	mov	r1, r0, lsr #24
	strb	r1, [r4, #7]

	mov	r1, r7
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_SECTOR
	bl	write_sector
	bvs	fsinfo_write_out

	mov	r0, #0
	str	r0, [r5, #VOL_FSDIRTY]

fsinfo_write_out:
	cmp	pc, #0			@ never report this upwards
	ldmfd	sp!, {r0-r8, pc}


	/* Build a unique 8.3 alias for a long name, at WS_NEWENT.
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = the RISC OS name.
	 * Exit:  the eleven name bytes of WS_NEWENT filled in. V set on failure.
	 *
	 * Every long name also gets a short one, because that is what a system
	 * which knows nothing of long names will see, and it has to be unique in
	 * the directory. The shape is the usual one: up to six characters of the
	 * name, then a tilde and a number that counts up until nothing else in
	 * the directory has it.
	 */
dir_alias:
	stmfd	sp!, {r1-r11, lr}

	mov	r7, r0			@ volume
	mov	r8, r1			@ directory
	mov	r9, r2			@ the name
	ldr	r0, =WS_NEWENT
	add	r10, wp, r0

	@ Space-fill the eleven name bytes.
	mov	r0, #' '
	mov	r1, #0
dir_alias_pad:
	strb	r0, [r10, r1]
	add	r1, r1, #1
	cmp	r1, #11
	blo	dir_alias_pad

	@ Up to six usable characters from the front of the name.
	mov	r1, #0			@ where we are in the name
	mov	r2, #0			@ how many we have taken
dir_alias_take:
	cmp	r2, #6
	bhs	dir_alias_ext
	ldrb	r0, [r9, r1]
	cmp	r0, #0
	beq	dir_alias_ext
	add	r1, r1, #1
	bl	alias_ok
	cmp	r0, #0
	beq	dir_alias_take
	strb	r0, [r10, r2]
	add	r2, r2, #1
	b	dir_alias_take

dir_alias_ext:
	cmp	r2, #0
	moveq	r0, #'X'		@ a name of nothing usable still needs one
	streqb	r0, [r10]
	moveq	r2, #1

	@ The extension is whatever follows the last slash, which is the dot the
	@ disc will see.
	mov	r3, #0			@ where the last slash was, 0 for none
	mov	r1, #0
dir_alias_findext:
	ldrb	r0, [r9, r1]
	cmp	r0, #0
	beq	dir_alias_gotext
	add	r1, r1, #1
	cmp	r0, #'/'
	moveq	r3, r1
	b	dir_alias_findext

dir_alias_gotext:
	cmp	r3, #0
	beq	dir_alias_number
	mov	r4, #0
dir_alias_extcopy:
	cmp	r4, #3
	bhs	dir_alias_number
	ldrb	r0, [r9, r3]
	cmp	r0, #0
	beq	dir_alias_number
	add	r3, r3, #1
	bl	alias_ok
	cmp	r0, #0
	beq	dir_alias_extcopy
	add	r5, r4, #8
	strb	r0, [r10, r5]
	add	r4, r4, #1
	b	dir_alias_extcopy

	/* Now make it unique: NAME~1, NAME~2, and so on. */
dir_alias_number:
	mov	r11, #1

dir_alias_try:
	@ Put "~n" in, just after the characters kept, room permitting.
	cmp	r11, #9
	movhi	r6, #2			@ two digits
	movls	r6, #1
	add	r0, r6, #1		@ the tilde as well
	rsb	r0, r0, #8
	cmp	r2, r0
	movhi	r2, r0			@ shuffle back to make room

	mov	r0, #'~'
	strb	r0, [r10, r2]
	add	r0, r2, #1
	cmp	r11, #9
	bls	dir_alias_one_digit

	mov	r1, r11
	mov	r3, #0
dir_alias_tens:
	cmp	r1, #10
	blo	dir_alias_tens_done
	sub	r1, r1, #10
	add	r3, r3, #1
	b	dir_alias_tens
dir_alias_tens_done:
	add	r3, r3, #'0'
	strb	r3, [r10, r0]
	add	r0, r0, #1
	add	r1, r1, #'0'
	strb	r1, [r10, r0]
	b	dir_alias_check

dir_alias_one_digit:
	add	r1, r11, #'0'
	strb	r1, [r10, r0]

dir_alias_check:
	mov	r0, r7
	mov	r1, r8
	mov	r2, r10
	bl	dir_alias_used
	bvs	dir_alias_out
	cmp	r0, #0
	beq	dir_alias_done

	add	r11, r11, #1
	cmp	r11, #99
	bls	dir_alias_try
	adrl	r0, err_dir_full
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r11, pc}

dir_alias_done:
	cmp	pc, #0

dir_alias_out:
	ldmfd	sp!, {r1-r11, pc}


	/* Is this 8.3 name already in the directory?
	 *
	 * Entry: R0 = volume, R1 = directory, R2 = eleven name bytes.
	 * Exit:  R0 = 1 if something has it. V set on a read error.
	 */
dir_alias_used:
	stmfd	sp!, {r1-r9, lr}

	mov	r6, r0
	mov	r7, r1
	mov	r8, r2
	mov	r9, #0
	sub	sp, sp, #32

dir_alias_used_loop:
	mov	r0, r6
	mov	r1, r7
	mov	r2, r9
	mov	r3, sp
	bl	dir_read_entry
	bvs	dir_alias_used_out
	cmp	r0, #0
	beq	dir_alias_used_no	@ end of the directory

	ldrb	r0, [sp]
	cmp	r0, #0
	beq	dir_alias_used_no	@ nothing beyond here has ever been used
	cmp	r0, #0xe5
	beq	dir_alias_used_next
	ldrb	r0, [sp, #11]
	and	r0, r0, #0x0f
	cmp	r0, #0x0f
	beq	dir_alias_used_next	@ a long-name fragment, not a name

	mov	r0, #0
dir_alias_used_cmp:
	ldrb	r1, [sp, r0]
	ldrb	r2, [r8, r0]
	cmp	r1, r2
	bne	dir_alias_used_next
	add	r0, r0, #1
	cmp	r0, #11
	blo	dir_alias_used_cmp

	mov	r0, #1
	add	sp, sp, #32
	cmp	pc, #0
	ldmfd	sp!, {r1-r9, pc}

dir_alias_used_next:
	add	r9, r9, #1
	b	dir_alias_used_loop

dir_alias_used_no:
	mov	r0, #0
	add	sp, sp, #32
	cmp	pc, #0
	ldmfd	sp!, {r1-r9, pc}

dir_alias_used_out:
	add	sp, sp, #32
	ldmfd	sp!, {r1-r9, pc}


	/* Fold a character for an 8.3 name, or say it cannot be used.
	 *
	 * Entry: R0 = the character. Exit: R0 = what to store, or 0 to skip it.
	 */
alias_ok:
	stmfd	sp!, {r1-r2, lr}
	cmp	r0, #'/'
	beq	alias_ok_drop		@ the separator is not part of either half
	cmp	r0, #' '
	bls	alias_ok_drop		@ spaces and control characters cannot be used
	bl	upper
	adrl	r1, alias_bad
alias_ok_scan:
	ldrb	r2, [r1], #1
	cmp	r2, #0
	beq	alias_ok_out
	cmp	r2, r0
	bne	alias_ok_scan
alias_ok_drop:
	mov	r0, #0
alias_ok_out:
	ldmfd	sp!, {r1-r2, pc}

alias_bad:
	.string	"+,;=[]"
	.align


	/* Write "." and ".." into the first sector of a new directory.
	 *
	 * Entry: R0 = volume, R1 = the new directory's cluster, R2 = its
	 *        parent's cluster.
	 * Exit:  V set on failure.
	 *
	 * ".." must hold ZERO when the parent is the root, whatever cluster the
	 * root actually starts at - that is what the specification says and what
	 * everything else checks for when it decides it has walked up as far as
	 * it can go. The cluster is already cleared, so only these two slots are
	 * written and the rest of the sector stays as it is.
	 */
dir_write_dots:
	stmfd	sp!, {r1-r10, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ this directory
	mov	r7, r2			@ its parent

	@ The root is not a parent anything may name.
	ldr	r0, [r5, #VOL_TYPE]
	cmp	r0, #32
	ldreq	r0, [r5, #VOL_ROOTCLUS]
	cmpeq	r7, r0
	moveq	r7, #0
	cmp	r7, #1
	movls	r7, #0

	ldr	r0, =WS_NEWENT
	add	r10, wp, r0

	mov	r0, r5
	mov	r1, r6
	mov	r2, #0
	mov	r3, r6
	bl	dir_build_dot		@ "."
	bvs	dir_write_dots_out

	mov	r0, r5
	mov	r1, r6
	mov	r2, #1
	mov	r3, r7
	bl	dir_build_dot		@ ".."
	bvs	dir_write_dots_out
	cmp	pc, #0

dir_write_dots_out:
	ldmfd	sp!, {r1-r10, pc}


	/* One of the two dot entries.
	 *
	 * Entry: R0 = volume, R1 = the directory, R2 = which slot (0 is ".",
	 *        1 is ".."), R3 = the cluster it should point at.
	 * Exit:  V set on failure.
	 */
dir_build_dot:
	stmfd	sp!, {r1-r10, lr}

	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3

	ldr	r0, =WS_NEWENT
	add	r10, wp, r0

	@ Eleven bytes of name: dots at the front, spaces after.
	mov	r0, #0
	mov	r1, #' '
dir_build_dot_pad:
	strb	r1, [r10, r0]
	add	r0, r0, #1
	cmp	r0, #11
	blo	dir_build_dot_pad

	mov	r0, #'.'
	strb	r0, [r10]
	cmp	r7, #0
	strneb	r0, [r10, #1]

	mov	r0, #11
	mov	r1, #0
dir_build_dot_clear:
	strb	r1, [r10, r0]
	add	r0, r0, #1
	cmp	r0, #32
	blo	dir_build_dot_clear

	mov	r0, #0x10		@ a directory
	strb	r0, [r10, #11]

	mov	r0, r8, lsr #16
	strb	r0, [r10, #20]
	mov	r0, r8, lsr #24
	strb	r0, [r10, #21]
	strb	r8, [r10, #26]
	mov	r0, r8, lsr #8
	strb	r0, [r10, #27]

	bl	fat_now
	strb	r0, [r10, #14]
	mov	r2, r0, lsr #8
	strb	r2, [r10, #15]
	strb	r1, [r10, #16]
	mov	r2, r1, lsr #8
	strb	r2, [r10, #17]
	strb	r1, [r10, #18]
	strb	r2, [r10, #19]
	strb	r0, [r10, #22]
	mov	r2, r0, lsr #8
	strb	r2, [r10, #23]
	strb	r1, [r10, #24]
	mov	r2, r1, lsr #8
	strb	r2, [r10, #25]

	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	mov	r3, r10
	bl	dir_write_entry

	ldmfd	sp!, {r1-r10, pc}


	/* Strike out an entry and the long-name run in front of it.
	 *
	 * Entry: R0 = volume, R1 = directory cluster, R2 = index of the 8.3 entry.
	 * Exit:  V set on failure.
	 *
	 * FAT deletes by writing &E5 over the first byte of the name and changing
	 * nothing else, so the fragments carrying the long name have to be struck
	 * out too, one at a time, walking backwards. Leave them and they become
	 * orphans: the next thing to write here finds a long name with no entry
	 * to belong to, and the name it shows is somebody else's. The walk stops
	 * at the first slot that is not a fragment, which is what ui_delete_entry
	 * does in the reference.
	 *
	 * The chain is NOT freed here - rename needs the entry struck out while
	 * the data stays exactly where it is.
	 */
dir_delete_at:
	stmfd	sp!, {r1-r8, lr}
	sub	sp, sp, #32

	mov	r5, r0			@ volume
	mov	r6, r1			@ directory
	mov	r7, r2			@ index

dir_delete_loop:
	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	mov	r3, sp
	bl	dir_read_entry
	bvs	dir_delete_out
	cmp	r0, #0
	beq	dir_delete_done

	mov	r0, #0xe5
	strb	r0, [sp]
	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	mov	r3, sp
	bl	dir_write_entry
	bvs	dir_delete_out

	@ Is the slot in front of it a fragment of the name just struck out?
	cmp	r7, #0
	beq	dir_delete_done
	sub	r7, r7, #1
	mov	r0, r5
	mov	r1, r6
	mov	r2, r7
	mov	r3, sp
	bl	dir_read_entry
	bvs	dir_delete_out
	cmp	r0, #0
	beq	dir_delete_done
	ldrb	r0, [sp, #11]
	cmp	r0, #0x0f
	beq	dir_delete_loop

dir_delete_done:
	add	sp, sp, #32
	cmp	pc, #0
	ldmfd	sp!, {r1-r8, pc}

dir_delete_out:
	add	sp, sp, #32
	ldmfd	sp!, {r1-r8, pc}


	/* Put a first cluster into an entry that is already on the medium.
	 *
	 * Entry: R0 = volume, R1 = the entry's sector, R2 = its offset in it,
	 *        R3 = the cluster.
	 * Exit:  V set on failure.
	 *
	 * A new directory cannot be given its cluster when its entry is written,
	 * because the cluster must not be allocated until after the entry exists:
	 * making room for the entry can itself want a cluster, and asking for two
	 * at once gets the same one twice.
	 */
entry_set_cluster:
	stmfd	sp!, {r1-r8, lr}

	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3

	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	entry_set_cluster_out

	add	r4, wp, #WS_FILESEC
	add	r4, r4, r7

	mov	r0, r8, lsr #16
	strb	r0, [r4, #20]
	mov	r0, r8, lsr #24
	strb	r0, [r4, #21]
	strb	r8, [r4, #26]
	mov	r0, r8, lsr #8
	strb	r0, [r4, #27]

	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	entry_set_cluster_out
	cmp	pc, #0

entry_set_cluster_out:
	ldmfd	sp!, {r1-r8, pc}


	/* Put a file type onto an entry that is already on the medium.
	 *
	 * Entry: R0 = volume, R1 = the entry's sector, R2 = its offset in it,
	 *        R3 = a RISC OS load address.
	 * Exit:  V set on failure.
	 *
	 * A load address that is not of the form &FFFxxxxx is a real address
	 * rather than a type, and there is nowhere on FAT to keep one, so the
	 * entry is left alone and the file reads back as Data. Nothing is lost
	 * that FAT could have held.
	 */
entry_set_type:
	stmfd	sp!, {r1-r8, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ sector
	mov	r7, r2			@ offset

	ldr	r0, =0xfff00000
	and	r4, r3, r0
	cmp	r4, r0
	bne	entry_set_type_done

	mov	r8, r3, lsl #12
	mov	r8, r8, lsr #20		@ the twelve type bits

	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	entry_set_type_out

	add	r4, wp, #WS_FILESEC
	add	r4, r4, r7

	mov	r0, r8
	bl	ftype_pack
	strb	r0, [r4, #14]		@ the creation time carries the type
	mov	r1, r0, lsr #8
	strb	r1, [r4, #15]

	ldr	r0, =CTIME_FTYPE_MAGIC
	strb	r0, [r4, #16]		@ and the creation date marks it
	mov	r1, r0, lsr #8
	strb	r1, [r4, #17]

	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	entry_set_type_out

entry_set_type_done:
	cmp	pc, #0

entry_set_type_out:
	ldmfd	sp!, {r1-r8, pc}


	/* Empty a directory entry that is about to be written over.
	 *
	 * Entry: R0 = volume, R1 = the entry's sector, R2 = its offset in that
	 *        sector, R3 = the first cluster it had, 0 if none.
	 * Exit:  V set on failure.
	 *
	 * The chain goes back to the free pool and the entry is left at length
	 * zero with no cluster, which is exactly the state dir_create leaves a
	 * brand new one in - so whatever writes next cannot tell the difference.
	 */
entry_empty:
	stmfd	sp!, {r1-r8, lr}

	mov	r5, r0			@ volume
	mov	r6, r1			@ the sector the entry is in
	mov	r7, r2			@ and where in it

	cmp	r3, #0
	beq	entry_empty_entry
	mov	r0, r5
	mov	r1, r3
	bl	free_chain
	bvs	entry_empty_out

entry_empty_entry:
	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	read_sector
	bvs	entry_empty_out

	add	r4, wp, #WS_FILESEC
	add	r4, r4, r7

	mov	r0, #0
	strb	r0, [r4, #20]		@ first cluster, high half
	strb	r0, [r4, #21]
	strb	r0, [r4, #26]		@ and low
	strb	r0, [r4, #27]
	strb	r0, [r4, #28]		@ length
	strb	r0, [r4, #29]
	strb	r0, [r4, #30]
	strb	r0, [r4, #31]

	bl	fat_now
	strb	r0, [r4, #22]
	mov	r2, r0, lsr #8
	strb	r2, [r4, #23]
	strb	r1, [r4, #24]
	mov	r2, r1, lsr #8
	strb	r2, [r4, #25]

	ldrb	r0, [r4, #11]
	orr	r0, r0, #0x20		@ it has been written to
	strb	r0, [r4, #11]

	mov	r1, r6
	ldr	r0, [r5, #VOL_DRIVE]
	add	r2, wp, #WS_FILESEC
	bl	write_sector
	bvs	entry_empty_out
	cmp	pc, #0

entry_empty_out:
	ldmfd	sp!, {r1-r8, pc}


	/* Spread a file type across a packed FAT time, and gather it back.
	 *
	 * The twelve bits go in three nibbles at bits 11-14, 5-8 and 0-3, which
	 * keeps them clear of the bits a packed time would carry into. The layout
	 * is fixed by convention and MUST NOT be changed: a stick typed here has to
	 * read correctly on other machines. See the README's credits for whose
	 * convention it is and why it is followed.
	 *
	 * Entry: R0 = the value. Exit: R0 = the other form.
	 */
ftype_pack:
	stmfd	sp!, {r1-r2, lr}
	mov	r1, r0, lsl #3
	ldr	r2, =0x7800
	and	r1, r1, r2
	mov	r2, r0, lsl #1
	and	r2, r2, #0x01e0
	orr	r1, r1, r2
	and	r2, r0, #0x000f
	orr	r0, r1, r2
	ldmfd	sp!, {r1-r2, pc}

ftype_unpack:
	stmfd	sp!, {r1-r2, lr}
	mov	r1, r0, lsr #3
	and	r1, r1, #0xf00
	mov	r2, r0, lsr #1
	and	r2, r2, #0x0f0
	orr	r1, r1, r2
	and	r2, r0, #0x00f
	orr	r0, r1, r2
	ldmfd	sp!, {r1-r2, pc}


	/* The file type an entry carries.
	 *
	 * Entry: R0 = the 32-byte entry. Exit: R0 = the type.
	 *
	 * Anything that does not say otherwise is Data. Guessing from the name's
	 * extension would need MimeMap; being dull about it beats being wrong
	 * about it.
	 */
ftype_of:
	stmfd	sp!, {r1-r4, lr}
	mov	r4, r0

	mov	r0, r4
	mov	r1, #16			@ the creation date
	bl	ld16
	ldr	r2, =CTIME_FTYPE_MAGIC
	cmp	r0, r2
	bne	ftype_of_legacy

	mov	r0, r4
	mov	r1, #14			@ the creation time carries it
	bl	ld16
	ldr	r2, =0xdead
	cmp	r0, r2
	beq	ftype_of_data		@ the "being written" marker
	bl	ftype_unpack
	b	ftype_of_out

ftype_of_legacy:
	@ Media written before the creation-date marker put the type in the two
	@ bytes at 12, which is only believable when it looks like a type. ZERO
	@ does not: those bytes are zero on everything the rest of the world
	@ writes, and believing it reported every ordinary file as type &000.
	@ The reference throws out both 0 and anything &1000 or over.
	mov	r0, r4
	mov	r1, #12
	bl	ld16
	cmp	r0, #0
	beq	ftype_of_data
	cmp	r0, #0x1000
	blo	ftype_of_out

ftype_of_data:
	ldr	r0, =FILETYPE_DATA

ftype_of_out:
	ldmfd	sp!, {r1-r4, pc}


	/* Create a directory entry for a name that is not there yet.
	 *
	 * Entry: R0 = volume, R1 = directory cluster (0 = root), R2 = RISC OS
	 *        name, R3 = attribute byte.
	 * Exit:  R0 = the sector the 8.3 entry landed in, R1 = its offset in that
	 *        sector. V set on failure.
	 *
	 * The long name goes in front of the 8.3 entry as a run of fragments,
	 * written highest ordinal first, which is the order VFAT reads them back
	 * in. A dot and a slash change places on the way in, the same swap that
	 * reading does the other way.
	 */
dir_create:
	stmfd	sp!, {r2-r12, lr}

	mov	r4, r0			@ volume
	mov	r5, r1			@ directory
	mov	r6, r2			@ the name
	mov	r11, r3			@ attributes
	ldr	r0, =WS_NEWENT
	add	r10, wp, r0

	mov	r7, #0
dir_create_len:
	ldrb	r0, [r6, r7]
	cmp	r0, #0
	beq	dir_create_gotlen
	add	r7, r7, #1
	cmp	r7, #LFN_MAX
	blo	dir_create_len
dir_create_gotlen:
	cmp	r7, #0
	beq	dir_create_bad

	@ Fragments needed, rounding up, and one more for the 8.3 entry itself.
	add	r8, r7, #(LFN_CHARS - 1)
	mov	r0, #0
dir_create_div:
	cmp	r8, #LFN_CHARS
	blo	dir_create_divdone
	sub	r8, r8, #LFN_CHARS
	add	r0, r0, #1
	b	dir_create_div
dir_create_divdone:
	mov	r8, r0			@ fragments
	add	r9, r8, #1		@ entries in all

	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	bl	dir_alias
	bvs	dir_create_out

	@ The checksum every fragment carries. It goes in R7, whose earlier use -
	@ the length of the name - is finished with once the fragment count is
	@ worked out. It emphatically does NOT go in R12: R12 is wp, and putting
	@ the checksum there left every routine called below reading its workspace
	@ through an eight-bit number. That was the crash, not the entry writer.
	mov	r0, r10
	bl	lfn_checksum
	mov	r7, r0

	mov	r0, r4
	mov	r1, r5
	mov	r2, r9
	bl	dir_find_run
	bvs	dir_create_out
	mov	r2, r0			@ where the run starts

	@ The fragments, ordinal n first.
	mov	r3, r8			@ which ordinal
dir_create_frag:
	stmfd	sp!, {r2, r3}
	mov	r0, r3
	mov	r1, r6
	mov	r2, r7
	cmp	r3, r8			@ Z for the highest ordinal, which is last
	bl	lfn_build		@ builds one at WS_FRAG
	ldmfd	sp!, {r2, r3}

	stmfd	sp!, {r2, r3}
	mov	r0, r4
	mov	r1, r5
	add	r2, r2, r8
	sub	r2, r2, r3		@ start + (n - ordinal)
	ldr	r3, =WS_FRAG
	add	r3, wp, r3
	bl	dir_write_entry
	ldmfd	sp!, {r2, r3}
	bvs	dir_create_out

	subs	r3, r3, #1
	bne	dir_create_frag

	@ Then the 8.3 entry itself. Everything past the eleven name bytes goes to
	@ zero and only what matters is put back - the attribute and the times -
	@ which is what dir_createDefaultEntry does in the reference. R2 is still
	@ holding where the run starts, so the scratch here is R3, which the
	@ fragment loop has finished with.
	mov	r0, #11
dir_create_clear:
	mov	r1, #0
	strb	r1, [r10, r0]
	add	r0, r0, #1
	cmp	r0, #32
	blo	dir_create_clear
	strb	r11, [r10, #11]

	bl	fat_now
	strb	r0, [r10, #14]		@ created, time then date
	mov	r3, r0, lsr #8
	strb	r3, [r10, #15]
	strb	r1, [r10, #16]
	mov	r3, r1, lsr #8
	strb	r3, [r10, #17]
	strb	r1, [r10, #18]		@ last accessed, which is a date alone
	strb	r3, [r10, #19]
	strb	r0, [r10, #22]		@ last written
	mov	r3, r0, lsr #8
	strb	r3, [r10, #23]
	strb	r1, [r10, #24]
	mov	r3, r1, lsr #8
	strb	r3, [r10, #25]

	mov	r0, r4
	mov	r1, r5
	add	r2, r2, r8		@ the entry after the fragments
	mov	r3, r10
	bl	dir_write_entry
	bvs	dir_create_out
	cmp	pc, #0

dir_create_out:
	ldmfd	sp!, {r2-r12, pc}

dir_create_bad:
	adrl	r0, err_bad_name
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r2-r12, pc}

err_bad_name:
	.int	0
	.string	"MultiFS cannot make a file with that name"
	.align


	/* Build one long-name fragment at WS_FRAG.
	 *
	 * Entry: R0 = ordinal (1 is the one next to the 8.3 entry), R1 = the
	 *        name, R2 = the checksum, Z set if this is the last fragment of
	 *        the name and so carries bit 6.
	 */
lfn_build:
	stmfd	sp!, {r0-r9, lr}

	moveq	r9, #0x40		@ the flag, if this is the last one
	movne	r9, #0

	mov	r5, r0			@ ordinal
	mov	r6, r1			@ name
	ldr	r0, =WS_FRAG
	add	r7, wp, r0

	@ Everything not a character is either zero or &FF, so start from &FF and
	@ put the fixed fields in afterwards.
	mov	r0, #0xff
	mov	r1, #0
lfn_build_fill:
	strb	r0, [r7, r1]
	add	r1, r1, #1
	cmp	r1, #32
	blo	lfn_build_fill

	orr	r0, r5, r9
	strb	r0, [r7]		@ ordinal, with its flag
	mov	r0, #0x0f
	strb	r0, [r7, #11]		@ the attribute that marks a fragment
	mov	r0, #0
	strb	r0, [r7, #12]
	strb	r2, [r7, #13]		@ the checksum
	strb	r0, [r7, #26]
	strb	r0, [r7, #27]

	@ Thirteen characters, at 1, 14 and 28.
	sub	r8, r5, #1
	mov	r0, #LFN_CHARS
	mul	r8, r0, r8		@ where in the name this fragment starts

	mov	r4, #0			@ which of the thirteen
lfn_build_chars:
	cmp	r4, #LFN_CHARS
	bhs	lfn_build_done

	cmp	r4, #5
	movlo	r0, #1
	addlo	r0, r0, r4, lsl #1
	blo	lfn_build_place
	cmp	r4, #11
	sublo	r0, r4, #5
	addlo	r0, r0, r0
	addlo	r0, r0, #14
	blo	lfn_build_place
	sub	r0, r4, #11
	add	r0, r0, r0
	add	r0, r0, #28

lfn_build_place:
	add	r3, r8, r4
	ldrb	r1, [r6, r3]
	cmp	r1, #0
	bhi	lfn_build_char

	@ At or past the end: one NUL, then the &FFFF that is already there.
	add	r3, r7, r0
	strb	r1, [r3]
	strb	r1, [r3, #1]
	b	lfn_build_done

lfn_build_char:
	cmp	r1, #'/'
	moveq	r1, #'.'		@ RISC OS's separator is the disc's dot
	add	r3, r7, r0
	strb	r1, [r3]
	mov	r1, #0
	strb	r1, [r3, #1]
	add	r4, r4, #1
	b	lfn_build_chars

lfn_build_done:
	ldmfd	sp!, {r0-r9, pc}

	.ltorg
