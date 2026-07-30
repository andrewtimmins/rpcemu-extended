@ RPCEmuPCIEmulator - the PCI SWIs a driver needs on a machine with no PCI bus
@
@ Copyright (C) 2026 Andy Timmins
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ RISC OS's USB stack is written for machines that have a PCI bus, and it reaches
@ for PCI for one thing that has nothing to do with PCI: memory a host controller
@ can bus-master. OHCIDriver's usb_allocmem is malloc_contig, and malloc_contig
@ is PCI_RAMAlloc (see its c.bsd_fns). On IOMD there is no PCI module, so every
@ allocation fails, ohci_init gives up before it writes a single controller
@ register - silently, because module_init ignores what ohci_init returns - and
@ the controller is never started.
@
@ This module supplies that memory. It claims the PCI SWI chunk and implements
@ PCI_RAMAlloc and PCI_RAMFree over a dynamic area of DMA-capable memory; every
@ other SWI in the chunk returns "no such SWI", exactly as it would if this
@ module were not here, so a driver that probes for a PCI bus still finds none
@ and moves on.
@
@ Why the memory has to be physically contiguous
@ ----------------------------------------------
@ A caller asks for one block and then carves fixed-stride descriptors out of it.
@ OHCIDriver's transfer descriptors are 16 bytes on an 80-byte stride, so one of
@ them lands across a page boundary sooner or later, and the controller reads it
@ as sixteen linear bytes of physical memory. Two pages that are next to each
@ other logically but not physically would give it half of one descriptor and
@ half of something else. Hence the run-finding in heap_init: RISC OS will not
@ promise a dynamic area contiguous physical pages, so the area is asked for what
@ each of its pages really is and the longest contiguous run is used. If that run
@ is too small to be useful the module says so rather than handing out memory
@ that would fail in a way nobody could diagnose.
@
@ *PCIRAMInfo reports what was found.
@
@ 32-bit compatible, assembled to the ARMv3 floor like the other guest modules.

	.arch	armv3

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_Write0			= 0x20002
	XOS_NewLine			= 0x20003
	XOS_Module			= 0x2001e
	XOS_DynamicArea			= 0x20066
	XOS_Memory			= 0x20068
	XOS_ConvertHex8			= 0x200d4

	Module_Claim			= 6
	Module_Free			= 7

	DynamicArea_Create		= 0
	DynamicArea_Remove		= 1

	@ Dynamic area flags. Uncached and unbuffered because something other
	@ than the CPU reads this memory.
	@
	@ DynAreaFlags_NeedsDMA, which is what a driver would ask for on a
	@ machine where only some memory can be reached by a bus master, is
	@ deliberately not here: RISC OS on IOMD refuses a dynamic area that
	@ asks for it ("Memory cannot be moved"), there being no such pool to
	@ allocate from. Every page is equally reachable here, so the flag buys
	@ nothing anyway - but the area still has to be physically contiguous,
	@ and heap_init is what makes sure of that.
	DA_NotBufferable		= 1 << 4
	DA_NotCacheable			= 1 << 5
	DA_FLAGS			= DA_NotBufferable | DA_NotCacheable

	@ OS_Memory reason 0, "here is a logical address, tell me the physical
	@ one". The block is three words: page number, logical, physical.
	Memory_LogicalGiven		= 1 << 9
	Memory_PhysicalWanted		= 1 << 13
	Memory_L2P			= Memory_LogicalGiven | Memory_PhysicalWanted

	MEMBLOCK_LOGICAL		= 4
	MEMBLOCK_PHYSICAL		= 8
	MEMBLOCK_SIZE			= 12

	@ The PCI SWI chunk, and the two SWIs in it that we answer.
	PCI_SWI_CHUNK			= 0x50380
	PCI_RAMAlloc			= 16
	PCI_RAMFree			= 17
	PCI_SWI_COUNT			= 18

	@ Error numbers. The "no such SWI" one is what RISC OS itself returns
	@ for an unclaimed SWI in a claimed chunk, so a caller probing for PCI
	@ cannot tell this module from the absence of one.
	Error_NoSuchSWI			= 0x1e6
	Error_Base			= 0x820b00
	Error_NoMemory			= Error_Base + 0
	Error_BadFree			= Error_Base + 1

	V_bit				= 1 << 28

	@ How much DMA-capable memory to keep. Enough for OHCIDriver's endpoint
	@ and transfer descriptor chunks (about 24K between them), its 256-byte
	@ communications area, and the buffers of whatever transfers are in
	@ flight, with room to spare. Claimed on first use, not at boot, so a
	@ machine with nothing that wants it pays nothing.
	HEAP_SIZE			= 512 * 1024
	PAGE_SIZE			= 4096
	HEAP_PAGES			= HEAP_SIZE / PAGE_SIZE

	@ Allocation granularity. Every block starts on a 256-byte boundary,
	@ which satisfies any alignment a caller has ever asked for (OHCI's
	@ largest is the 256 its communications area needs) without the
	@ allocator having to think about alignment at all.
	UNIT_SHIFT			= 8
	UNIT_SIZE			= 1 << UNIT_SHIFT
	UNITS_PER_PAGE			= PAGE_SIZE / UNIT_SIZE
	MAX_UNITS			= HEAP_SIZE / UNIT_SIZE
	BITMAP_BYTES			= MAX_UNITS / 8

	@ Below this there is no point pretending we can help.
	MIN_HEAP_PAGES			= 16

	@ Workspace
	WS_DA				= 0	@ dynamic area number, -1 if none
	WS_DABASE			= 4	@ where the area was put
	WS_HEAP				= 8	@ contiguous run inside it, 0 if none
	WS_HEAPPHYS			= 12	@ its physical address
	WS_UNITS			= 16	@ its length in UNIT_SIZE units
	WS_TRIED			= 20	@ have we tried to make the heap yet
	WS_USED				= 24	@ bitmap: unit is allocated
	WS_START			= WS_USED + BITMAP_BYTES
	WORKSPACE_SIZE			= WS_START + BITMAP_BYTES


	.global	_start
_start:

module_start:
	.int	0		@ Start (not runnable)
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	0		@ Service Call
	.int	title		@ Title String
	.int	help		@ Help String
	.int	table		@ Help and Command keyword table
	.int	PCI_SWI_CHUNK	@ SWI Chunk base
	.int	swi_handler	@ SWI handler code
	.int	swi_names	@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32-bit compatible

title:
	.string	"RPCEmuPCIEmulator"
	.align

	@ Defines "help", generated from VersionNum by the Makefile.
	.include "version.inc"

	@ The names of the SWIs in the chunk, so *Help and error messages can
	@ talk about them. Naming one is not a promise to implement it; the
	@ handler answers for the two it knows and refuses the rest.
swi_names:
	.string	"PCI"
	.string	"ReadID"
	.string	"ReadHeader"
	.string	"ReturnNumber"
	.string	"EnumerateFunctions"
	.string	"IORead"
	.string	"IOWrite"
	.string	"MemoryRead"
	.string	"MemoryWrite"
	.string	"ConfigurationRead"
	.string	"ConfigurationWrite"
	.string	"HardwareAddress"
	.string	"ReadInfo"
	.string	"SpecialCycle"
	.string	"FindByLocation"
	.string	"FindByID"
	.string	"FindByClass"
	.string	"RAMAlloc"
	.string	"RAMFree"
	.byte	0
	.align

table:
	.string	"PCIRAMInfo"
	.align
	.int	command_pciraminfo
	.int	0x00000000		@ no parameters
	.int	command_pciraminfo_syntax
	.int	command_pciraminfo_help

	.byte	0	@ Table terminator
	.align

command_pciraminfo_help:
	.string	"*PCIRAMInfo reports the DMA-capable memory this module hands out\n\rthrough PCI_RAMAlloc: where it is, how big it is, and how much is in use.\n\r"
	.align

command_pciraminfo_syntax:
	.string	"Syntax: *PCIRAMInfo\n\r"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

init:
	stmfd	sp!, {lr}
	mov	r0, #Module_Claim
	mov	r3, #WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {pc}		@ no workspace: refuse to initialise
	str	r2, [r12]

	mvn	r0, #0
	str	r0, [r2, #WS_DA]	@ no dynamic area yet
	mov	r0, #0
	str	r0, [r2, #WS_DABASE]
	str	r0, [r2, #WS_HEAP]
	str	r0, [r2, #WS_HEAPPHYS]
	str	r0, [r2, #WS_UNITS]
	str	r0, [r2, #WS_TRIED]
	ldmfd	sp!, {pc}

final:
	stmfd	sp!, {r4, lr}
	ldr	r4, [r12]

	@ Give the dynamic area back. Anything still allocated out of it goes
	@ with it, which is the caller's problem for outliving us.
	ldr	r0, [r4, #WS_DA]
	cmp	r0, #0
	blt	final_no_da
	mov	r1, r0
	mov	r0, #DynamicArea_Remove
	swi	XOS_DynamicArea
final_no_da:

	mov	r0, #Module_Free
	mov	r2, r4
	swi	XOS_Module
	ldmfd	sp!, {r4, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The heap
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Make the heap, once. Called from the first allocation rather than from init.
@
@ On exit WS_HEAP is the base of a physically contiguous run of DMA-capable
@ memory and WS_UNITS its length, or WS_HEAP is zero and there is none. Either
@ way WS_TRIED is set, so this is not attempted twice.
heap_init:
	stmfd	sp!, {r0-r9, lr}
	ldr	r4, [r12]

	mov	r0, #1
	str	r0, [r4, #WS_TRIED]

	@@@ Claim the area.
	mov	r0, #DynamicArea_Create
	mvn	r1, #0			@ any area number
	ldr	r2, =HEAP_SIZE
	mvn	r3, #0			@ anywhere
	ldr	r4, =DA_FLAGS
	mov	r5, r2			@ fixed size: maximum is the size
	mov	r6, #0			@ no grow/shrink handler
	mov	r7, #0
	adrl	r8, str_da_name
	swi	XOS_DynamicArea
	ldmvsfd	sp!, {r0-r9, pc}	@ no area: no heap, and V is already set

	ldr	r4, [r12]
	str	r1, [r4, #WS_DA]
	str	r3, [r4, #WS_DABASE]

	@@@ Walk its pages and find the longest physically contiguous run.
	@@@ r5 = page being examined, r6 = physical address expected next,
	@@@ r7 = start page of the run in progress, r8 = best start,
	@@@ r9 = best length.
	sub	sp, sp, #MEMBLOCK_SIZE
	mov	r5, #0
	mov	r6, #0
	mov	r7, #0
	mov	r8, #0
	mov	r9, #0

heap_scan:
	ldr	r0, [r4, #WS_DABASE]
	add	r0, r0, r5, lsl #12
	str	r0, [sp, #MEMBLOCK_LOGICAL]
	ldr	r0, =Memory_L2P
	mov	r1, sp
	mov	r2, #1
	swi	XOS_Memory
	bvs	heap_scan_break		@ cannot say: treat as a break in the run

	ldr	r0, [sp, #MEMBLOCK_PHYSICAL]
	teq	r5, #0
	teqne	r0, r6
	beq	heap_scan_same
	mov	r7, r5			@ physically discontiguous: start again
heap_scan_same:
	add	r6, r0, #PAGE_SIZE	@ what the next page must be

	sub	r1, r5, r7
	add	r1, r1, #1		@ length of the run ending here
	cmp	r1, r9
	movhi	r9, r1
	movhi	r8, r7

	add	r5, r5, #1
	cmp	r5, #HEAP_PAGES
	blo	heap_scan
	b	heap_scan_done

heap_scan_break:
	@ Start a fresh run after the page we could not ask about.
	add	r5, r5, #1
	mov	r7, r5
	mov	r6, #0
	cmp	r5, #HEAP_PAGES
	blo	heap_scan

heap_scan_done:
	add	sp, sp, #MEMBLOCK_SIZE

	cmp	r9, #MIN_HEAP_PAGES
	blo	heap_too_small

	@@@ Record the run and start with nothing allocated in it.
	ldr	r0, [r4, #WS_DABASE]
	add	r0, r0, r8, lsl #12
	str	r0, [r4, #WS_HEAP]
	mov	r0, r9, lsl #(12 - UNIT_SHIFT)
	str	r0, [r4, #WS_UNITS]

	@ And its physical address, which *PCIRAMInfo shows and which is the
	@ quickest way to see that the run really is where it says.
	sub	sp, sp, #MEMBLOCK_SIZE
	ldr	r0, [r4, #WS_HEAP]
	str	r0, [sp, #MEMBLOCK_LOGICAL]
	ldr	r0, =Memory_L2P
	mov	r1, sp
	mov	r2, #1
	swi	XOS_Memory
	ldrvc	r0, [sp, #MEMBLOCK_PHYSICAL]
	movvs	r0, #0
	str	r0, [r4, #WS_HEAPPHYS]
	add	sp, sp, #MEMBLOCK_SIZE

	add	r0, r4, #WS_USED
	mov	r1, #(BITMAP_BYTES * 2)
	bl	zero_bytes

	ldmfd	sp!, {r0-r9, pc}

heap_too_small:
	@ Hand the area back rather than keep memory we cannot use.
	ldr	r0, [r4, #WS_DA]
	cmp	r0, #0
	blt	heap_too_small_done
	mov	r1, r0
	mov	r0, #DynamicArea_Remove
	swi	XOS_DynamicArea
	mvn	r0, #0
	str	r0, [r4, #WS_DA]
	mov	r0, #0
	str	r0, [r4, #WS_DABASE]
heap_too_small_done:
	ldmfd	sp!, {r0-r9, pc}

	.ltorg

@ Zero r1 bytes at r0.
zero_bytes:
	stmfd	sp!, {r0-r2, lr}
	mov	r2, #0
zero_bytes_loop:
	subs	r1, r1, #1
	strplb	r2, [r0], #1
	bpl	zero_bytes_loop
	ldmfd	sp!, {r0-r2, pc}

@ Is unit r1 of the bitmap at r0 set? Returns EQ if clear, NE if set.
bit_test:
	stmfd	sp!, {r2, r3, lr}
	mov	r2, r1, lsr #3
	ldrb	r3, [r0, r2]
	and	r2, r1, #7
	mov	r2, r3, lsr r2
	tst	r2, #1
	ldmfd	sp!, {r2, r3, pc}

@ Set unit r1 of the bitmap at r0.
bit_set:
	stmfd	sp!, {r1-r3, lr}
	mov	r2, r1, lsr #3
	ldrb	r3, [r0, r2]
	and	r1, r1, #7
	mov	lr, #1
	orr	r3, r3, lr, lsl r1
	strb	r3, [r0, r2]
	ldmfd	sp!, {r1-r3, pc}

@ Clear unit r1 of the bitmap at r0.
bit_clear:
	stmfd	sp!, {r1-r3, lr}
	mov	r2, r1, lsr #3
	ldrb	r3, [r0, r2]
	and	r1, r1, #7
	mov	lr, #1
	bic	r3, r3, lr, lsl r1
	strb	r3, [r0, r2]
	ldmfd	sp!, {r1-r3, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ SWI handler
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ r11 = SWI number within the chunk, r12 = private word pointer.
swi_handler:
	teq	r11, #PCI_RAMAlloc
	beq	swi_ramalloc
	teq	r11, #PCI_RAMFree
	beq	swi_ramfree

	@ Everything else in the chunk. Returning the same error RISC OS gives
	@ for an unclaimed SWI is what keeps a driver's "is there a PCI bus?"
	@ probe behaving as it would with no PCI module at all.
	adrl	r0, err_no_swi
	b	swi_error

@ PCI_RAMAlloc: r0 = size in bytes, r1 = alignment, r2 = flags (ignored).
@ Returns r0 = logical address of the block.
swi_ramalloc:
	stmfd	sp!, {r1-r9, lr}
	ldr	r4, [r12]

	ldr	r5, [r4, #WS_TRIED]
	teq	r5, #0
	bleq	heap_init		@ first time through: go and get some
	ldr	r4, [r12]		@ heap_init may have been the first user

	ldr	r5, [r4, #WS_HEAP]
	teq	r5, #0
	beq	swi_ramalloc_nomem

	@@@ How many units, and how far apart the candidate starts have to be.
	@@@ Every unit is UNIT_SIZE-aligned and the run starts on a page, so an
	@@@ alignment of UNIT_SIZE or less is satisfied by any unit at all.
	adds	r6, r0, #(UNIT_SIZE - 1)
	bcs	swi_ramalloc_nomem	@ absurd size
	mov	r6, r6, lsr #UNIT_SHIFT
	teq	r6, #0
	moveq	r6, #1			@ a zero-byte request still gets a unit

	mov	r7, #1			@ step, in units
	cmp	r1, #UNIT_SIZE
	movhi	r7, r1, lsr #UNIT_SHIFT

	ldr	r8, [r4, #WS_UNITS]
	mov	r9, #0			@ candidate first unit

swi_ramalloc_try:
	add	r0, r9, r6
	cmp	r0, r8
	bhi	swi_ramalloc_nomem	@ ran off the end

	@ Are all r6 units from r9 free? Walk backwards, so the first one that
	@ is taken tells us the earliest start worth trying next.
	add	r1, r9, r6
swi_ramalloc_check:
	sub	r1, r1, #1
	add	r0, r4, #WS_USED
	bl	bit_test
	bne	swi_ramalloc_taken
	cmp	r1, r9
	bhi	swi_ramalloc_check

	@@@ It fits. Mark the units and remember where the block began.
	mov	r1, r9
	add	r0, r4, #WS_START
	bl	bit_set
	add	r1, r9, r6
swi_ramalloc_mark:
	sub	r1, r1, #1
	add	r0, r4, #WS_USED
	bl	bit_set
	cmp	r1, r9
	bhi	swi_ramalloc_mark

	ldr	r0, [r4, #WS_HEAP]
	add	r0, r0, r9, lsl #UNIT_SHIFT
	@ Say plainly that this worked. A SWI further back - the OS_Memory in
	@ heap_init that could not name a page, say - may have left V set, and
	@ a caller reads that as failure however good the pointer is. r1 is
	@ scratch here because the very next instruction restores it.
	mrs	r1, cpsr
	bic	r1, r1, #V_bit
	msr	cpsr_f, r1
	ldmfd	sp!, {r1-r9, lr}
	mov	pc, lr

swi_ramalloc_taken:
	@ r1 is a taken unit inside the window, so nothing starting at or
	@ before it can work. Round the next candidate up to the step.
	add	r9, r1, #1
	add	r9, r9, r7
	sub	r9, r9, #1
	sub	r0, r7, #1
	bic	r9, r9, r0
	b	swi_ramalloc_try

swi_ramalloc_nomem:
	adrl	r0, err_no_memory
	ldmfd	sp!, {r1-r9, lr}
	b	swi_error

@ PCI_RAMFree: r0 = a block previously returned by PCI_RAMAlloc.
swi_ramfree:
	stmfd	sp!, {r0-r9, lr}
	ldr	r4, [r12]

	ldr	r5, [r4, #WS_HEAP]
	teq	r5, #0
	beq	swi_ramfree_bad

	@@@ Is this one of ours, and is it the start of a block?
	subs	r6, r0, r5
	bmi	swi_ramfree_bad
	tst	r6, #(UNIT_SIZE - 1)
	bne	swi_ramfree_bad
	mov	r6, r6, lsr #UNIT_SHIFT
	ldr	r8, [r4, #WS_UNITS]
	cmp	r6, r8
	bhs	swi_ramfree_bad

	mov	r1, r6
	add	r0, r4, #WS_START
	bl	bit_test
	beq	swi_ramfree_bad

	mov	r1, r6
	add	r0, r4, #WS_START
	bl	bit_clear

	@@@ Release units until the next block begins or the heap runs out.
	mov	r9, r6
swi_ramfree_loop:
	cmp	r9, r8
	bhs	swi_ramfree_done
	mov	r1, r9
	add	r0, r4, #WS_USED
	bl	bit_test
	beq	swi_ramfree_done	@ already free: end of this block
	teq	r9, r6
	beq	swi_ramfree_release
	mov	r1, r9
	add	r0, r4, #WS_START
	bl	bit_test
	bne	swi_ramfree_done	@ the next block starts here
swi_ramfree_release:
	mov	r1, r9
	add	r0, r4, #WS_USED
	bl	bit_clear
	add	r9, r9, #1
	b	swi_ramfree_loop

swi_ramfree_done:
	mrs	r1, cpsr
	bic	r1, r1, #V_bit
	msr	cpsr_f, r1
	ldmfd	sp!, {r0-r9, lr}
	mov	pc, lr

swi_ramfree_bad:
	adrl	r0, err_bad_free
	add	sp, sp, #4		@ drop the saved r0; r0 is the error now
	ldmfd	sp!, {r1-r9, lr}
	b	swi_error

@ Return the error block in r0 to the caller, with V set.
swi_error:
	mrs	r1, cpsr
	orr	r1, r1, #V_bit
	msr	cpsr_f, r1
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Printing
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

hexbuf:
	.space	16

@ Print the string at r0 and a newline.
print_line:
	stmfd	sp!, {lr}
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {pc}

@ Print "<label at r4> 0x<r5 as eight hex digits>" and a newline.
print_hex8:
	stmfd	sp!, {r0-r3, lr}
	mov	r0, r4
	swi	XOS_Write0
	adrl	r0, str_hex_prefix
	swi	XOS_Write0
	mov	r0, r5
	adrl	r1, hexbuf
	mov	r2, #16
	swi	XOS_ConvertHex8
	swi	XOS_Write0
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r3, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *PCIRAMInfo
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

command_pciraminfo:
	stmfd	sp!, {r4-r11, lr}
	ldr	r10, [r12]

	adrl	r0, str_banner
	bl	print_line

	ldr	r0, [r10, #WS_TRIED]
	teq	r0, #0
	bne	pciraminfo_tried
	adrl	r0, str_not_yet
	bl	print_line
	b	pciraminfo_exit

pciraminfo_tried:
	ldr	r0, [r10, #WS_HEAP]
	teq	r0, #0
	bne	pciraminfo_have
	adrl	r0, str_none
	bl	print_line
	b	pciraminfo_exit

pciraminfo_have:
	ldr	r5, [r10, #WS_HEAP]
	adrl	r4, str_base
	bl	print_hex8
	ldr	r5, [r10, #WS_HEAPPHYS]
	adrl	r4, str_phys
	bl	print_hex8
	ldr	r5, [r10, #WS_UNITS]
	mov	r5, r5, lsl #UNIT_SHIFT
	adrl	r4, str_size
	bl	print_hex8

	@@@ How much is out on loan, counted rather than tracked: the bitmap is
	@@@ the truth, and a separate running total is one more thing to get
	@@@ out of step with it.
	ldr	r6, [r10, #WS_UNITS]
	mov	r7, #0			@ units in use
	mov	r8, #0			@ blocks
	mov	r9, #0
pciraminfo_count:
	cmp	r9, r6
	bhs	pciraminfo_counted
	mov	r1, r9
	add	r0, r10, #WS_USED
	bl	bit_test
	addne	r7, r7, #1
	mov	r1, r9
	add	r0, r10, #WS_START
	bl	bit_test
	addne	r8, r8, #1
	add	r9, r9, #1
	b	pciraminfo_count

pciraminfo_counted:
	mov	r5, r7, lsl #UNIT_SHIFT
	adrl	r4, str_inuse
	bl	print_hex8
	mov	r5, r8
	adrl	r4, str_blocks
	bl	print_hex8

pciraminfo_exit:
	ldmfd	sp!, {r4-r11, lr}
	mov	r0, #0
	mrs	r1, cpsr		@ and no error
	bic	r1, r1, #V_bit
	msr	cpsr_f, r1
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Errors and strings
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

err_no_swi:
	.int	Error_NoSuchSWI
	.string	"SWI value not known"
	.align

err_no_memory:
	.int	Error_NoMemory
	.string	"No DMA-capable memory available"
	.align

err_bad_free:
	.int	Error_BadFree
	.string	"Not a block from PCI_RAMAlloc"
	.align

str_da_name:
	.string	"RPCEmu DMA memory"
	.align

str_banner:
	.string	"PCIRAMInfo: DMA-capable memory for PCI_RAMAlloc"
	.align
str_hex_prefix:
	.string	"0x"
	.align
str_base:
	.string	"Heap at: "
	.align
str_phys:
	.string	"Physically at: "
	.align
str_size:
	.string	"Size in bytes: "
	.align
str_inuse:
	.string	"Bytes in use: "
	.align
str_blocks:
	.string	"Blocks allocated: "
	.align

str_not_yet:
	.string	"Nothing has asked for any yet, so none has been claimed."
	.align
str_none:
	.string	"No usable memory: the dynamic area could not be claimed, or none\n\rof it was physically contiguous enough to be worth keeping."
	.align
