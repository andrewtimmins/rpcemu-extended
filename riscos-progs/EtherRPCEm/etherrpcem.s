@ EtherRPCEm - the RISC OS half of RPCEmu's emulated network card
@
@ Copyright (C) 2003 J Ballance / Castle Technology (as EtherY)
@ Copyright (C) 2007 Alex Waugh (adaptation for RPCEmu)
@ Copyright (C) 2026 Andy Timmins (this assembler port)
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of version 2 of the GNU General Public License as published by the
@ Free Software Foundation. It is distributed in the hope that it will be
@ useful, but WITHOUT ANY WARRANTY; see the GNU General Public License (the
@ LICENSE file in this directory) for more details.
@
@ A DCI4 driver that presents itself to the Internet stack as an Ethernet
@ interface. There is no NIC chip to drive: frames move through the private SWI
@ &56AC4, which the emulator intercepts in src/arm_common.c before it ever
@ becomes a real SWI, so the hardware-specific half of a real driver is simply
@ absent. See docs/network.md and src/network.c for the host side.
@
@ ---------------------------------------------------------------------------
@ Why this file exists
@ ---------------------------------------------------------------------------
@ This is a transcription of the original c.Module, s.intveneer and
@ cmhg.ModHdr into GNU as, so the module builds on the host with the ARM
@ binutils the other guest modules already use (build.sh --podules) and in CI,
@ instead of needing the Acorn DDE inside a running emulator. Nothing else here
@ built that way, so a change to the driver could not be assembled by anyone
@ who did not first set up a RISC OS C toolchain in a guest.
@
@ Deliberate differences from the C, all commented where they appear:
@   - No shared C library. The C build RMEnsured SharedCLibrary, FPEmulator and
@     CallASWI at initialisation, from a module that loads out of a podule ROM
@     before System: exists.
@   - Workspace is claimed in the RMA and reached through the private word,
@     as in the other guest modules, rather than living in the module's own
@     static data.
@   - The expansion card's base address is saved at initialisation and used to
@     release the device vector, rather than trusting R11 to hold the same
@     value at finalisation as it did at initialisation.
@   - The callback registered at initialisation is removed at finalisation.
@   - Failed initialisation undoes what it had already done. RISC OS does not
@     call the finalisation entry of a module whose initialisation returned an
@     error, so the C left the Mbuf session open and the device vector claimed.
@
@ Everything else is the same code doing the same things in the same order,
@ including behaviour worth calling inherited rather than intended: see the
@ notes on InitChip and on the statistics table.
@
@ 32-bit compatible, and written in the 26/32-neutral idiom used by the other
@ guest modules (no MRS/MSR outside the one place a mode change needs them, and
@ flag-preserving returns).

	@ Target the lowest RiscPC CPU: ARM610/ARM710 are ARMv3 (StrongARM is v4).
	@ Assembling to the ARMv3 floor stops anything v4-only getting in, and
	@ makes the assembler reject rather than silently promote a non-encodable
	@ immediate to movw/movt, which is undefined on every RiscPC CPU.
	.arch	armv3

	wp	.req	r12

	@ ARM constants
	NBIT = 1 << 31

	@ RISC OS SWIs (X-form: bit 17 set, so errors return via V and R0)
	XOS_WriteC			= 0x20000
	XOS_Write0			= 0x20002
	XOS_NewLine			= 0x20003
	XOS_File			= 0x20008
	XOS_IntOn			= 0x20013
	XOS_IntOff			= 0x20014
	XOS_Module			= 0x2001e
	XOS_ReadVarVal			= 0x20023
	XOS_SetVarVal			= 0x20024
	XOS_ServiceCall			= 0x20030
	XOS_ClaimDeviceVector		= 0x2004b
	XOS_ReleaseDeviceVector		= 0x2004c
	XOS_AddCallBack			= 0x20054
	XOS_RemoveCallBack		= 0x20055
	XOS_ConvertHex2			= 0x200d1
	XOS_ConvertHex4			= 0x200d2
	XOS_ConvertCardinal4		= 0x200d8
	XMessageTrans_ErrorLookup	= 0x61506
	XPodule_ReadInfo		= 0x6028d
	XMbuf_OpenSession		= 0x6a580
	XMbuf_CloseSession		= 0x6a581

	@ The emulator's private network SWI. Not an X-form SWI and there is no
	@ X-form of it: it is intercepted in the emulator before SWI dispatch, and
	@ reports failure by returning a non-zero R0 rather than through V.
	Net_SWI				= 0x56ac4

	@ ...and its R0 sub-operations (src/network.c, network_swi())
	Net_Transmit			= 0
	Net_Receive			= 1
	Net_SetIRQStatus		= 2
	Net_SetIRQ			= 3
	Net_ReadHWAddr			= 4

	@ OS_Module reason codes
	Module_Claim			= 6
	Module_Free			= 7

	@ OS_File reason codes
	File_ReadInfo			= 17
	File_SaveBlock			= 10

	@ OS_SetVarVal R4 type: a literal string, not GSTrans'd or evaluated
	VarType_LiteralString		= 4

	@ Service calls we handle
	Service_StartWimp		= 0x49
	Service_EnumerateNetworkDrivers	= 0x9b
	Service_DCIProtocolStatus	= 0x9f
	Service_MbufManagerStatus	= 0xa2

	@ Service calls we issue
	Service_DCIDriverStatus		= 0x9d
	Service_DCIFrameTypeFree	= 0x9e

	@ ...and their reasons
	DCIDriver_Starting		= 0
	DCIDriver_Dying			= 1

	@ The DCI specification version implemented, 4.03
	DCI_Version			= 403

	@ Our SWI chunk, and the offsets within it. The chunk, the driver name
	@ ("rpcem"), the module name and the error chunk are all allocated to
	@ RPCEmu - see the Notes file.
	SWI_CHUNK			= 0x58cc0

	DCI4Version			= 0
	DCI4Inquire			= 1
	DCI4GetNetworkMTU		= 2
	DCI4SetNetworkMTU		= 3
	DCI4Transmit			= 4
	DCI4Filter			= 5
	DCI4Stats			= 6
	DCI4MulticastRequest		= 7

	@ Command numbers in the table below
	CMD_ERPCEmInfo			= 0

	@ Inquire flags. Multicast and promiscuous are advertised and NOT
	@ implemented; that is inherited, and recorded in the module's README
	@ rather than quietly changed here, because the Internet stack has been
	@ seeing these flags for twenty years.
	INQ_MULTICAST			= 1 << 0
	INQ_PROMISCUOUS			= 1 << 1
	INQ_RXERRORS			= 1 << 4
	INQ_HWADDRVALID			= 1 << 5
	INQ_SOFTHWADDR			= 1 << 6
	INQ_HASSTATS			= 1 << 8
	INQUIRE_FLAGS = INQ_MULTICAST | INQ_PROMISCUOUS | INQ_RXERRORS | INQ_HWADDRVALID | INQ_SOFTHWADDR | INQ_HASSTATS

	@ Frame levels for the Filter SWI (top 16 bits of R2)
	FRMLVL_E2SPECIFIC		= 1
	FRMLVL_E2SINK			= 2
	FRMLVL_E2MONITOR		= 3
	FRMLVL_IEEE			= 4

	@ Address filtering levels
	ADDRLVL_MULTICAST		= 2
	ADDRLVL_PROMISCUOUS		= 3

	@ ...and the interface flags they set in WS_FLAGS. Computed and then never
	@ used or passed to the host, exactly as in the C.
	IFF_ALLMULTI			= 1
	IFF_PROMISC			= 2

	@ Ethernet
	EY_MTU				= 1500
	IEEE_MAX_LENGTH			= 1500	@ R2 <= this in Filter means IEEE

	@ Mbuf types
	MT_HEADER			= 2

	@ The version of the Mbuf manager specification we speak
	MBUF_MANAGER_VERSION		= 100

	@ Expansion card interrupt device vector. Hardcoded as 13 in the C rather
	@ than read from Podule_ReadInfo, and left that way: it is the shared
	@ expansion card IRQ on every machine RPCEmu emulates.
	PODULE_DEVICE_VECTOR		= 13

	@ How many expansion cards to look through when asking by card number
	PODULE_COUNT			= 8

	@ Podule_ReadInfo reason bits. Bit 1 alone returns just the sync base;
	@ bits 15 and 16 return the interrupt mask register address and the bit
	@ within it that is ours.
	ReadInfo_SyncBase		= 1 << 1
	ReadInfo_IntMask		= 1 << 15
	ReadInfo_IntValue		= 1 << 16
	READINFO_INT_ITEMS		= ReadInfo_IntMask | ReadInfo_IntValue

	@ Error numbers. The DCI4 specification suggests deriving them from errno
	@ values in a driver's own error chunk; these are the ones the C used.
	DCI4_ERROR_BLOCK		= 0x20e00
	ERR_ENXIO			= DCI4_ERROR_BLOCK | 0x06
	ERR_EINVAL			= DCI4_ERROR_BLOCK | 0x16
	ERR_ENOTTY			= DCI4_ERROR_BLOCK | 0x19
	ERR_ENOBUFS			= DCI4_ERROR_BLOCK | 0x37
	ERR_AlreadyClaimed		= DCI4_ERROR_BLOCK | 0x87

	@ The error number the emulator's messages are reported under. Inherited
	@ from s.intveneer, where the number sat in front of the message buffer
	@ the host writes into.
	ERR_HOST			= 0x804d38

	@ "SWI value not known", looked up in the global messages file so it comes
	@ out in the user's language, with the module title as %0. This is what
	@ cmhg generated for an unrecognised SWI in our chunk, and returning the
	@ same thing keeps a driver's "does this SWI exist?" probe behaving as it
	@ always has.
	ERR_BAD_SWI			= 0x1e6

	@ ---------------------------------------------------------------------
	@ Structure layouts.
	@
	@ Every offset here is taken from Acorn's own headers (h/dcistructs,
	@ h/mbuf_c) and from h/Structs, and is checked against them by
	@ tests/test_etherrpcem_layout.c, which parses this file. A wrong offset
	@ in a driver like this does not fail to assemble: it shows up as a data
	@ abort during a boot, or as frames going to the wrong protocol module, so
	@ it is worth a test rather than a careful read.
	@ ---------------------------------------------------------------------

	@ struct mbuf (h/mbuf_c). The host reads and writes the same fields
	@ through struct ro_mbuf_part in src/network.h.
	MBUF_NEXT			= 0
	MBUF_LIST			= 4
	MBUF_OFF			= 8
	MBUF_LEN			= 12
	MBUF_INIOFF			= 16
	MBUF_INILEN			= 20
	MBUF_TYPE			= 24
	MBUF_SIZE			= 28

	@ Dib, the device information block protocol modules are handed
	DIB_SWIBASE			= 0
	DIB_NAME			= 4
	DIB_UNIT			= 8
	DIB_ADDRESS			= 12
	DIB_MODULE			= 16
	DIB_LOCATION			= 20
	DIB_SLOT			= 24
	DIB_INQUIRE			= 28
	DIB_SIZE			= 32

	@ ChDib, one link of the chain built for Service_EnumerateNetworkDrivers
	CHDIB_NEXT			= 0
	CHDIB_DIB			= 4
	CHDIB_SIZE			= 8

	@ RxHdr, the header passed to a protocol module's receive handler. The
	@ host fills this in - struct rx_hdr in src/network.h.
	RXHDR_PTR			= 0
	RXHDR_TAG			= 4
	RXHDR_SRC_ADDR			= 8	@ 6 bytes + 2 of padding
	RXHDR_DST_ADDR			= 16	@ 6 bytes + 2 of padding
	RXHDR_FRAME_TYPE		= 24
	RXHDR_ERROR_LEVEL		= 28
	RXHDR_CKSUM			= 32
	RXHDR_SIZE			= 36

	@ ClaimBuf, one protocol module's claim on a frame type
	CLAIM_FLAGS			= 0
	CLAIM_UNIT			= 4
	CLAIM_FRAME_TYPE		= 8
	CLAIM_FRAME_LEVEL		= 12
	CLAIM_ADDRESS_LEVEL		= 16
	CLAIM_ERROR_LEVEL		= 20
	CLAIM_HANDLER			= 24
	CLAIM_PWP			= 28
	CLAIM_NEXT			= 32
	CLAIM_PREV			= 36
	CLAIM_SIZE			= 40

	@ dci4_mbctl (h/mbuf_c): the fields we set before opening a session, and
	@ the two routines in its table that we call.
	MBCTL_OPAQUE			= 0
	MBCTL_MBCSIZE			= 4
	MBCTL_MBCVERS			= 8
	MBCTL_FLAGS			= 12
	MBCTL_ALLOC_S			= 60
	MBCTL_FREEM			= 80
	MBCTL_SIZE			= 136

	@ struct stats, as returned by the Stats SWI
	ST_INTERFACE_TYPE		= 0
	ST_LINK_STATUS			= 1
	ST_LINK_POLARITY		= 2
	ST_TX_FRAMES			= 28
	ST_UNWANTED_FRAMES		= 76
	ST_RX_FRAMES			= 80
	ST_SIZE				= 100

	@ Interface type and link status bits we report
	ST_TYPE_10BASET			= 3
	ST_STATUS_OK			= 1 << 0
	ST_STATUS_ACTIVE		= 1 << 1
	ST_STATUS_PROMISCUOUS		= 3 << 2
	ST_STATUS_FULL_DUPLEX		= 1 << 4
	ST_LINK_STATUS_REPORTED = ST_STATUS_OK | ST_STATUS_ACTIVE | ST_STATUS_PROMISCUOUS | ST_STATUS_FULL_DUPLEX
	ST_LINK_POLARITY_CORRECT	= 1

	@ ---------------------------------------------------------------------
	@ Workspace, claimed in the RMA at initialisation and reached through the
	@ private word. The first nine fields are the C's workspace structure,
	@ field for field, because the Dib inside it is handed out to protocol
	@ modules and its address has to stay put for the life of the session.
	@ ---------------------------------------------------------------------
	WS_PWP		= 0	@ our private word pointer
	WS_MBCTL	= 4	@ -> the live mbctl block, or 0 with no session
	WS_DIB		= 8	@ DIB_SIZE bytes
	WS_DEV_ADDR	= 40	@ 6-byte hardware address (+2 bytes of padding)
	WS_FLAGS	= 48
	WS_CLAIMS	= 52	@ head of the ClaimBuf list
	WS_TX_FRAMES	= 56
	WS_UNWANTED	= 60
	WS_RX_FRAMES	= 64
	@ --- fields the C kept in module statics or a separate allocation ---
	WS_PODULE_BASE	= 68	@ saved so finalisation can match the claim
	WS_SEMA		= 72	@ receive loop re-entry guard
	WS_RXHDR_MBUF	= 76	@ mbuf held ready for the next frame's header
	WS_DATA_MBUF	= 80	@ ...and for its payload
	WS_ERR		= 84	@ error block: the number...
	WS_ERRMESS	= 88	@ ...then the text, which the host writes
	ERRMESS_SIZE	= 252	@ 4 + 252 is the largest a RISC OS error block may be
	WS_SCRATCH	= WS_ERRMESS + ERRMESS_SIZE	@ 16 bytes: SWI results, numbers
	SCRATCH_SIZE	= 16
	WS_MBCTL_BLK	= WS_SCRATCH + SCRATCH_SIZE	@ MBCTL_SIZE bytes
	WORKSPACE_SIZE	= WS_MBCTL_BLK + MBCTL_SIZE


	.global	_start
_start:

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Module header
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

module_start:
	.int	0		@ Start (not runnable)
	.int	init		@ Initialisation
	.int	final		@ Finalisation
	.int	service		@ Service Call
	.int	title		@ Title String
	.int	help		@ Help String
	.int	table		@ Help and Command keyword table
	.int	SWI_CHUNK	@ SWI Chunk base
	.int	swi_handler	@ SWI handler code
	.int	swi_names	@ SWI decoding table
	.int	0		@ SWI decoding code
	.int	0		@ Message File
	.int	modflags	@ Module Flags

modflags:
	.int	1		@ 32-bit compatible

title:
	.string	"EtherRPCEm"
	.align

	@ Defines "help", the *Help / *Modules string, generated from VersionNum
	@ by the Makefile so the version, the date and the original authors' credit
	@ live in one place.
	.include "version.inc"

	@ SWI decoding table: the chunk's name, then one name per SWI in order, so
	@ *Help and error messages can talk about them.
swi_names:
	.string	"EtherRPCEm"
	.string	"DCIVersion"
	.string	"Inquire"
	.string	"GetNetworkMTU"
	.string	"SetNetworkMTU"
	.string	"Transmit"
	.string	"Filter"
	.string	"Stats"
	.string	"Multicastreq"
	.byte	0		@ table terminator
	.align

	@ Help and Command keyword table
table:
	.string	"ERPCEmInfo"
	.align
	.int	command_info
	.int	0x00000000	@ no argument checking: the command takes none
	.int	0		@ no invalid-syntax message
	.int	command_info_help

	.byte	0	@ table terminator
	.align

command_info_help:
	.string	"*ERPCEmInfo displays EtherRPCEm internal statistics.\rSyntax: *ERPCEmInfo"
	.align

	@ Strings the DIB points at. They live in the module image, which is where
	@ they lived in the C, so nothing has to copy them into workspace.
dib_name_str:
	.string	"rpcem"
dib_name_end:
	.align
	DIB_NAME_LEN = dib_name_end - dib_name_str - 1

dib_location_str:
	.string	"Emulated"
	.align

inet_ethertype_var:
	.string	"Inet$EtherType"
	.align

boot_dir_var:
	.string	"Boot$Dir"
	.align

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Initialisation and finalisation
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Initialisation.
	 * Entry: R11 = this expansion card's base address, R12 -> private word.
	 * Exit:  R7-R11 and R13 preserved; V clear, or V set with R0 -> error.
	 */
init:
	stmfd	sp!, {r4-r11, lr}
	mov	r9, r11			@ card base, before anything can corrupt it
	mov	r8, r12			@ private word pointer

	@ Claim workspace and record it in the private word.
	ldr	r0, [r8]
	teq	r0, #0
	bne	init_have_workspace
	mov	r0, #Module_Claim
	ldr	r3, =WORKSPACE_SIZE
	swi	XOS_Module
	bvs	init_return		@ no memory: R0 already points at the error
	str	r2, [r8]
init_have_workspace:
	ldr	wp, [r8]

	@ OS_Module 6 does not clear what it hands back, and the claims list head
	@ and the two held mbufs are all read before anything writes them.
	mov	r0, #0
	mov	r1, wp
	ldr	r2, =WORKSPACE_SIZE
1:	str	r0, [r1], #4
	subs	r2, r2, #4
	bne	1b

	str	r8, [wp, #WS_PWP]
	str	r9, [wp, #WS_PODULE_BASE]

	@ The error block the emulator writes its messages into: the number is
	@ ours and fixed, the text follows it and the host fills that in.
	ldr	r0, =ERR_HOST
	str	r0, [wp, #WS_ERR]

	@ Ask the emulator for this machine's hardware address. It has to be
	@ asked for rather than assumed: since src/net_slot.c every instance gets
	@ its own, so that two machines on the virtual switch do not collide.
	mov	r0, #Net_ReadHWAddr
	add	r1, wp, #WS_ERRMESS
	add	r2, wp, #WS_DEV_ADDR
	swi	Net_SWI

	bl	mbuf_open
	bvs	init_return

	bl	init_chip

	@ Claim the expansion card interrupt. R2 is what the handler is entered
	@ with in R12, so passing the workspace saves an indirection in the
	@ receive path; the C passed the private word because cmhg's veneer
	@ needed it.
	mov	r0, #PODULE_DEVICE_VECTOR
	adrl	r1, device_irq
	mov	r2, wp
	mov	r3, r9
	mov	r4, #1
	swi	XOS_ClaimDeviceVector
	bvs	init_close_mbuf

	bl	podule_int_info
	bvs	init_release_vector

	@ Enable this card's interrupts in the mask register the Podule manager
	@ named. A read-modify-write on a register shared with other drivers, so
	@ with interrupts off around it.
	ldr	r0, [wp, #WS_SCRATCH]		@ mask register address
	ldr	r1, [wp, #WS_SCRATCH + 4]	@ our bit within it
	swi	XOS_IntOff
	ldrb	r2, [r0]
	orr	r2, r2, r1
	strb	r2, [r0]
	swi	XOS_IntOn

	@ Tell the emulator to start reporting interrupts. What it is given no
	@ longer matters provided it is not zero.
	mov	r0, #Net_SetIRQStatus
	add	r1, wp, #WS_ERRMESS
	mov	r2, #1
	swi	Net_SWI

	cmp	pc, #0			@ clear V: initialised
	ldmfd	sp!, {r4-r11, pc}

	/* Failed initialisation undoes what it had already done. RISC OS does
	 * NOT call the finalisation entry of a module whose initialisation
	 * returned an error, so anything left claimed here stays claimed for the
	 * life of the machine: the C left both the Mbuf session and the device
	 * vector behind. R0 has to be carried past SWIs that would overwrite it.
	 */
init_release_vector:
	mov	r6, r0
	mov	r0, #PODULE_DEVICE_VECTOR
	adrl	r1, device_irq
	mov	r2, wp
	ldr	r3, [wp, #WS_PODULE_BASE]
	mov	r4, #1
	swi	XOS_ReleaseDeviceVector
	mov	r0, r6
init_close_mbuf:
	mov	r6, r0
	bl	mbuf_close
	mov	r0, r6
init_return:
	cmp	r0, #NBIT		@ set V, leaving R0 -> error
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r4-r11, pc}

	.ltorg

	/* Fill in the device information block, publish Inet$EtherType and ask
	 * for a callback, so the driver announces itself once the machine is in a
	 * safe state to be told.
	 *
	 * Also clears the claims list, which is what the C did. On the
	 * Mbuf-manager-restarted path that drops any existing claims without
	 * freeing them; inherited behaviour, left alone rather than quietly
	 * changed, and recorded in this module's README.
	 */
init_chip:
	stmfd	sp!, {r0-r4, lr}
	ldr	r0, =SWI_CHUNK
	str	r0, [wp, #WS_DIB + DIB_SWIBASE]
	adrl	r0, dib_name_str
	str	r0, [wp, #WS_DIB + DIB_NAME]
	mov	r0, #0
	str	r0, [wp, #WS_DIB + DIB_UNIT]
	add	r0, wp, #WS_DEV_ADDR
	str	r0, [wp, #WS_DIB + DIB_ADDRESS]
	adrl	r0, title
	str	r0, [wp, #WS_DIB + DIB_MODULE]
	adrl	r0, dib_location_str
	str	r0, [wp, #WS_DIB + DIB_LOCATION]
	mov	r0, #0
	str	r0, [wp, #WS_DIB + DIB_SLOT]
	ldr	r0, =INQUIRE_FLAGS
	str	r0, [wp, #WS_DIB + DIB_INQUIRE]
	mov	r0, #0
	str	r0, [wp, #WS_CLAIMS]

	adrl	r0, inet_ethertype_var
	adrl	r1, dib_name_str
	mov	r2, #DIB_NAME_LEN
	mov	r3, #0
	mov	r4, #VarType_LiteralString
	swi	XOS_SetVarVal

	adrl	r0, driver_starting_callback
	mov	r1, wp
	swi	XOS_AddCallBack

	ldmfd	sp!, {r0-r4, pc}

	/* Announce the driver to the protocol modules, at callback time. Entered
	 * in SVC mode with R12 = what we passed in R1 to OS_AddCallBack, so the
	 * workspace pointer is already in place.
	 */
driver_starting_callback:
	stmfd	sp!, {r0-r9, lr}
	mov	r2, #DCIDriver_Starting
	bl	send_driver_status
	ldmfd	sp!, {r0-r9, pc}

	/* Service_DCIDriverStatus. Entry: R2 = starting or dying. */
send_driver_status:
	stmfd	sp!, {r0-r4, lr}
	add	r0, wp, #WS_DIB
	mov	r1, #Service_DCIDriverStatus
	ldr	r3, =DCI_Version
	swi	XOS_ServiceCall
	ldmfd	sp!, {r0-r4, pc}

	.ltorg

	/* Finalisation.
	 * Entry: R12 -> private word. Exit: R7-R11, R13 preserved, V clear.
	 */
final:
	stmfd	sp!, {r4-r11, lr}
	ldr	wp, [r12]
	teq	wp, #0
	beq	final_done		@ never initialised: nothing to undo

	swi	XOS_IntOff

	@ Stop the emulator raising our interrupt, then give the vector back.
	mov	r0, #Net_SetIRQStatus
	add	r1, wp, #WS_ERRMESS
	mov	r2, #0
	swi	Net_SWI

	@ Released with the base address saved at initialisation, not with R11:
	@ OS_ReleaseDeviceVector matches on every one of R0-R4, and the value
	@ R11 holds at finalisation is not the one it held at initialisation.
	@ Getting this wrong leaves the vector pointing into freed memory.
	mov	r0, #PODULE_DEVICE_VECTOR
	adrl	r1, device_irq
	mov	r2, wp
	ldr	r3, [wp, #WS_PODULE_BASE]
	mov	r4, #1
	swi	XOS_ReleaseDeviceVector

	swi	XOS_IntOn

	@ Drop the callback if it has not fired. The C left it registered, which
	@ pointed RISC OS at code that was about to be freed.
	adrl	r0, driver_starting_callback
	mov	r1, wp
	swi	XOS_RemoveCallBack

	mov	r2, #DCIDriver_Dying
	bl	send_driver_status

	@ Free the claims. No Service_DCIFrameTypeFree for each one: the whole
	@ driver is going, and it has just said so.
	ldr	r4, [wp, #WS_CLAIMS]
1:	teq	r4, #0
	beq	2f
	ldr	r5, [r4, #CLAIM_NEXT]
	mov	r0, #Module_Free
	mov	r2, r4
	swi	XOS_Module
	mov	r4, r5
	b	1b
2:	mov	r0, #0
	str	r0, [wp, #WS_CLAIMS]

	@ Give back the two mbufs held ready for the next frame, then close the
	@ session.
	ldr	r0, [wp, #WS_MBCTL]
	teq	r0, #0
	beq	final_done
	ldr	r0, [wp, #WS_RXHDR_MBUF]
	teq	r0, #0
	blne	mbuf_freem
	ldr	r0, [wp, #WS_DATA_MBUF]
	teq	r0, #0
	blne	mbuf_freem
	mov	r0, #0
	str	r0, [wp, #WS_RXHDR_MBUF]
	str	r0, [wp, #WS_DATA_MBUF]
	bl	mbuf_close

final_done:
	cmp	pc, #0			@ clear V: we do not refuse the kill
	ldmfd	sp!, {r4-r11, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The Mbuf memory manager
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Open a session. The control block lives in our own workspace rather
	 * than in a separate allocation as it did in the C: the manager keeps the
	 * pointer for the life of the session, and the workspace outlives that
	 * either way, so the allocation bought nothing but another failure path.
	 * WS_MBCTL is what says whether a session is open, exactly as the C's
	 * work.mbctl did.
	 */
mbuf_open:
	stmfd	sp!, {r1-r3, lr}
	add	r3, wp, #WS_MBCTL_BLK

	@ Everything the client does not set must be zero on entry; the manager
	@ fills in the rest.
	mov	r0, #0
	mov	r1, r3
	mov	r2, #MBCTL_SIZE
1:	str	r0, [r1], #4
	subs	r2, r2, #4
	bne	1b

	mov	r0, #MBCTL_SIZE
	str	r0, [r3, #MBCTL_MBCSIZE]
	mov	r0, #MBUF_MANAGER_VERSION
	str	r0, [r3, #MBCTL_MBCVERS]

	mov	r0, r3
	swi	XMbuf_OpenSession
	strvc	r3, [wp, #WS_MBCTL]
	ldmfd	sp!, {r1-r3, pc}	@ R0 is left as the SWI returned it

	/* Close it again, if one is open. */
mbuf_close:
	stmfd	sp!, {r0-r1, lr}
	ldr	r0, [wp, #WS_MBCTL]
	teq	r0, #0
	beq	1f
	mov	r1, #0
	str	r1, [wp, #WS_MBCTL]
	swi	XMbuf_CloseSession
1:	ldmfd	sp!, {r0-r1, pc}

	/* mbctl->freem(mbctl, mbuf). Entry: R0 = mbuf.
	 *
	 * The manager's routines follow the C calling standard, so they may
	 * corrupt R0-R3 and R12 - and R12 is the workspace pointer, hence the
	 * push. R4-R11 come back untouched, which is what lets the receive loop
	 * keep its state in them.
	 */
mbuf_freem:
	stmfd	sp!, {r1-r3, wp, lr}
	mov	r1, r0
	ldr	r0, [wp, #WS_MBCTL]
	ldr	r2, [r0, #MBCTL_FREEM]
	mov	lr, pc
	mov	pc, r2
	ldmfd	sp!, {r1-r3, wp, pc}

	/* mbctl->alloc_s(mbctl, bytes, NULL). Entry: R0 = bytes.
	 * Exit: R0 = mbuf, or 0 if the manager has none to give.
	 *
	 * alloc_s is the single-mbuf, always-safe allocator, which is why the
	 * receive path does not have to check for unsafe mbufs before handing
	 * them on.
	 */
mbuf_alloc_s:
	stmfd	sp!, {r1-r3, wp, lr}
	mov	r1, r0
	ldr	r0, [wp, #WS_MBCTL]
	mov	r2, #0
	ldr	r3, [r0, #MBCTL_ALLOC_S]
	mov	lr, pc
	mov	pc, r3
	ldmfd	sp!, {r1-r3, wp, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Where our interrupt comes from
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Ask where this card's interrupt mask register is, and which bit in it
	 * is ours. The two words land in WS_SCRATCH and WS_SCRATCH+4.
	 *
	 * Asking by base address, which is the obvious thing since that is what
	 * initialisation hands us in R11, does not work at every RAM size. The
	 * Podule manager resolves an address by masking it and then ORing in
	 * &180000, promoting whatever access speed was asked for to "sync",
	 * before comparing against the card's recorded sync base
	 * (ConvertR3ToPoduleNode in HWSupport.Podule). Bits 19 and 20 are that
	 * speed field, so the comparison can only succeed when the card's
	 * recorded base already has both set - and the logical address the kernel
	 * maps expansion card space at moves with the amount of RAM fitted. On
	 * RISC OS 5.30 a 256MB machine gets &f99cc000, where both bits are set
	 * and the promotion is a no-op, while a 128MB machine gets &f9acc000,
	 * where bit 20 is clear and the promoted value can never match. The call
	 * then fails with &500, "Bad expansion card identifier".
	 *
	 * The driver used to treat that as fatal, so no machine with less than
	 * 256MB fitted had any networking at all - issue #101, and what issue #2
	 * was really about. So fall back to asking by expansion card number,
	 * which takes the manager's list-scan path and never goes near the speed
	 * bits. Acorn's own Econet driver finds its card the same way.
	 */
podule_int_info:
	stmfd	sp!, {r1-r6, lr}

	ldr	r0, =READINFO_INT_ITEMS
	add	r1, wp, #WS_SCRATCH
	mov	r2, #8
	ldr	r3, [wp, #WS_PODULE_BASE]
	swi	XPodule_ReadInfo
	ldmvcfd	sp!, {r1-r6, pc}	@ the direct way worked

	ldr	r5, [wp, #WS_PODULE_BASE]
	mov	r4, #0
1:	mov	r0, #ReadInfo_SyncBase
	add	r1, wp, #WS_SCRATCH + 8
	mov	r2, #4
	mov	r3, r4
	swi	XPodule_ReadInfo
	bvs	2f
	ldr	r0, [wp, #WS_SCRATCH + 8]
	teq	r0, r5
	bne	2f

	@ This card is us: ask the same question by number.
	ldr	r0, =READINFO_INT_ITEMS
	add	r1, wp, #WS_SCRATCH
	mov	r2, #8
	mov	r3, r4
	swi	XPodule_ReadInfo
	ldmfd	sp!, {r1-r6, pc}	@ V as the SWI left it

2:	add	r4, r4, #1
	cmp	r4, #PODULE_COUNT
	blo	1b

	@ Our own card is not in the list at all, which should not be possible:
	@ we are running out of its ROM.
	adrl	r0, err_enxio
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r1-r6, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Service calls
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* The RISC OS 3.5+ service code-table form: the word before the entry
	 * point names a table of the service numbers we want, so the kernel can
	 * skip us entirely for everything else. The first instruction must be the
	 * magic MOV R0, R0 for the kernel to believe the table is there.
	 */
service_codetable:
	.int	0			@ flags
	.int	service_main		@ the real handler
	.int	Service_StartWimp
	.int	Service_EnumerateNetworkDrivers
	.int	Service_DCIProtocolStatus
	.int	Service_MbufManagerStatus
	.int	0			@ table terminator

	.int	service_codetable
service:
	mov	r0, r0			@ magic: there is a table at [service-4]
	teq	r1, #Service_StartWimp
	teqne	r1, #Service_EnumerateNetworkDrivers
	teqne	r1, #Service_DCIProtocolStatus
	teqne	r1, #Service_MbufManagerStatus
	movne	pc, lr

service_main:
	stmfd	sp!, {lr}
	ldr	wp, [r12]
	teq	wp, #0
	ldmeqfd	sp!, {pc}		@ not initialised: nothing to say
	teq	r1, #Service_StartWimp
	beq	service_startwimp
	teq	r1, #Service_EnumerateNetworkDrivers
	beq	service_enumerate
	teq	r1, #Service_MbufManagerStatus
	beq	service_mbufstatus
	@ Service_DCIProtocolStatus: nothing to do, as in the C. It is claimed
	@ here only because the C's cmhg header claimed it.
	ldmfd	sp!, {pc}

service_startwimp:
	bl	autosense_install
	ldmfd	sp!, {pc}

	/* Service_EnumerateNetworkDrivers: R0 -> chain of DIBs. Add ours to the
	 * front of it. If there is no memory for the link, leave the chain as it
	 * was rather than breaking it.
	 */
service_enumerate:
	stmfd	sp!, {r1-r4}
	mov	r4, r0			@ the chain as it stands
	mov	r0, #Module_Claim
	mov	r3, #CHDIB_SIZE
	swi	XOS_Module
	movvs	r0, r4			@ no memory: hand the chain straight back
	bvs	1f
	mov	r0, r2
	str	r4, [r0, #CHDIB_NEXT]
	add	r1, wp, #WS_DIB
	str	r1, [r0, #CHDIB_DIB]
1:	cmp	pc, #0			@ a service handler must not return V set
	ldmfd	sp!, {r1-r4}
	ldmfd	sp!, {pc}

	/* Service_MbufManagerStatus: R0 = 0 started, 1 stopping, anything else a
	 * scavenge request.
	 */
service_mbufstatus:
	stmfd	sp!, {r2}
	teq	r0, #0
	beq	3f
	teq	r0, #1
	bne	2f
	@ Stopping. It cannot while our session is open, so claim the service.
	ldr	r2, [wp, #WS_MBCTL]
	teq	r2, #0
	movne	r1, #0
2:	ldmfd	sp!, {r2}
	ldmfd	sp!, {pc}
3:	@ Started. Try to get a session back if we have none.
	ldr	r2, [wp, #WS_MBCTL]
	teq	r2, #0
	bne	2b
	bl	mbuf_open
	blvc	init_chip
	cmp	pc, #0			@ a failed open must not leak V out
	b	2b

	.ltorg

	/* Service_StartWimp: put the AutoSense file where !Boot's network
	 * configuration will look for it, if there is not one there already.
	 * Best effort - every error is ignored, as in the C.
	 */
autosense_install:
	stmfd	sp!, {r0-r9, lr}

	@ Is Boot$Dir set? R2 = -1 asks without copying the value.
	adrl	r0, boot_dir_var
	mov	r1, #0
	mvn	r2, #0
	mov	r3, #0
	swi	XOS_ReadVarVal
	cmp	r2, #0
	bge	autosense_done		@ not set: there is nowhere to put it

	@ The locations are a run of strings ending in an empty one, walked with a
	@ pointer, rather than a table of addresses: a module is relocated when it
	@ is loaded, so an address assembled into the image would be wrong by
	@ whatever the module base turned out to be.
	adrl	r7, autosense_locations
1:	ldrb	r0, [r7]
	teq	r0, #0
	beq	autosense_done
	mov	r8, r7			@ this location

	@ Does something already exist there?
	mov	r0, #File_ReadInfo
	mov	r1, r8
	swi	XOS_File
	bvs	2f			@ cannot tell: leave it alone
	teq	r0, #0
	bne	2f			@ already there

	@ Write it out. R2 is the load address, and passing the file type alone
	@ as the C did makes an untyped file rather than a BASIC one; kept as it
	@ was, because what reads this back looks it up by name. R3 was left
	@ holding whatever OS_File 17 returned, which is now set deliberately.
	mov	r0, #File_SaveBlock
	mov	r1, r8
	ldr	r2, =0xffb
	mov	r3, #0
	adrl	r4, autosense_data
	adrl	r5, autosense_end
	swi	XOS_File

2:	@ Step over this string and its terminator.
	ldrb	r0, [r7], #1
	teq	r0, #0
	bne	2b
	b	1b

autosense_done:
	ldmfd	sp!, {r0-r9, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The DCI4 SWIs
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* SWI handler.
	 * Entry: R11 = SWI offset within our chunk, R12 -> private word.
	 * Exit:  R0-R9 are returned to the caller; DCI4 says every register is
	 *        preserved except where a SWI is documented to return something.
	 */
swi_handler:
	stmfd	sp!, {r10, lr}
	ldr	wp, [r12]
	teq	wp, #0
	beq	swi_bad_swi		@ not initialised: we can answer nothing

	teq	r11, #DCI4Version
	beq	swi_version
	teq	r11, #DCI4Inquire
	beq	swi_inquire
	teq	r11, #DCI4GetNetworkMTU
	beq	swi_get_mtu
	teq	r11, #DCI4SetNetworkMTU
	beq	swi_set_mtu
	teq	r11, #DCI4Transmit
	beq	swi_transmit
	teq	r11, #DCI4Filter
	beq	swi_filter
	teq	r11, #DCI4Stats
	beq	swi_stats
	teq	r11, #DCI4MulticastRequest
	bne	swi_bad_swi

	/* Multicast filtering is not implemented; requests are accepted and
	 * ignored, as they always have been. What used to be here was a printf
	 * under "if (r->r[0] && ~3)" - a logical AND against a constant that is
	 * never zero, so it fired for any non-zero flags rather than the
	 * undefined ones it meant to catch, and it wrote to the shared C
	 * library's stdout from a SWI handler, which is not a safe place to do
	 * it. Both are gone; the behaviour they wrapped is unchanged.
	 */
swi_return_ok:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r10, pc}

	/* R0 -> error block on entry. */
swi_return_error:
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r10, pc}

swi_einval:
	adrl	r0, err_einval
	b	swi_return_error

	/* The error cmhg produced for an unrecognised SWI in our chunk: number
	 * &1E6 with the module title as %0, looked up in the global messages
	 * file so it comes out in the user's language. Returning the same thing
	 * keeps a "does this SWI exist?" probe behaving as it always has.
	 */
swi_bad_swi:
	adrl	r0, err_bad_swi
	mov	r1, #0
	mov	r2, #0
	adrl	r4, title
	swi	XMessageTrans_ErrorLookup
	b	swi_return_error

	/* DCIVersion: R0 = flags, all zero. Returns R1 = version. */
swi_version:
	teq	r0, #0
	bne	swi_einval
	ldr	r1, =DCI_Version
	b	swi_return_ok

	/* Inquire: R0 = flags, R1 = unit. Returns R2 = the feature bits. */
swi_inquire:
	teq	r0, #0
	bne	swi_einval
	ldr	r2, =INQUIRE_FLAGS
	b	swi_return_ok

	/* GetNetworkMTU: returns R2 = the physical MTU. */
swi_get_mtu:
	teq	r0, #0
	bne	swi_einval
	ldr	r2, =EY_MTU
	b	swi_return_ok

	/* SetNetworkMTU: the size cannot be changed, so this is the illegal
	 * operation the specification asks for.
	 */
swi_set_mtu:
	teq	r0, #0
	bne	swi_einval
	adrl	r0, err_enotty
	b	swi_return_error

	.ltorg

	/* Transmit.
	 * R0 = flags, R1 = unit, R2 = frame type, R3 -> mbuf chain,
	 * R4 -> destination hardware address, R5 -> source hardware address.
	 * Flags: bit 0 use R5 as the source, bit 1 the caller keeps the chain.
	 */
swi_transmit:
	stmfd	sp!, {r0-r9}
	mov	r10, #0			@ the first error, if any
	mov	r6, r3			@ the mbuf being sent
	mov	r8, r0			@ flags

1:	teq	r6, #0
	beq	4f
	ldr	r9, [r6, #MBUF_LIST]	@ the next chain, before this one is freed
	teq	r10, #0
	bne	3f

	@ R1 is the buffer the emulator writes a message into on failure; the
	@ error block it belongs to is the word in front of it.
	ldr	r3, [sp, #4 * 4]	@ caller's R4: destination
	tst	r8, #1
	ldrne	r4, [sp, #5 * 4]	@ caller's R5: source...
	moveq	r4, #0			@ ...or zero for our own address
	ldr	r5, [sp, #2 * 4]	@ caller's R2: frame type
	mov	r2, r6
	add	r1, wp, #WS_ERRMESS
	mov	r0, #Net_Transmit
	swi	Net_SWI
	teq	r0, #0
	addne	r10, wp, #WS_ERR

	@ Counted even when the transmit failed, as in the C.
	ldr	r0, [wp, #WS_TX_FRAMES]
	add	r0, r0, #1
	str	r0, [wp, #WS_TX_FRAMES]

3:	tst	r8, #2			@ does the caller keep the chain?
	movne	r6, r9
	bne	1b
	mov	r0, r6
	bl	mbuf_freem
	mov	r6, r9
	b	1b

4:	ldmfd	sp!, {r0-r9}
	teq	r10, #0
	beq	swi_return_ok
	mov	r0, r10
	b	swi_return_error

	.ltorg

	/* Filter.
	 * R0 = flags (bit 0: 0 claim, 1 release), R1 = unit,
	 * R2 = frame type in the low 16 bits, frame level in the high 16,
	 * R3 = address level, R4 = error level, R5 = handler's private word,
	 * R6 -> the routine that is to receive these frames.
	 */
swi_filter:
	stmfd	sp!, {r0-r9}
	teq	r1, #0
	bne	swi_filter_enxio	@ only unit 0 exists

	mov	r7, r2, lsl #16
	mov	r7, r7, lsr #16		@ frame type
	mov	r8, r2, lsr #16		@ frame level

	tst	r0, #1
	bne	swi_filter_release

	@ --- claim ---
	@ The frame type must be zero unless the level is Specific.
	teq	r8, #FRMLVL_E2SPECIFIC
	beq	1f
	teq	r7, #0
	bne	swi_filter_einval
1:
	ldr	r9, [wp, #WS_CLAIMS]
	teq	r8, #FRMLVL_E2SPECIFIC
	beq	filter_check_specific
	teq	r8, #FRMLVL_E2SINK
	beq	filter_check_sink
	teq	r8, #FRMLVL_E2MONITOR
	beq	filter_check_monitor
	teq	r8, #FRMLVL_IEEE
	beq	filter_check_ieee
	b	swi_filter_einval

	@ Is this exact frame type spoken for?
filter_check_specific:
1:	teq	r9, #0
	beq	filter_allocate
	ldr	r0, [r9, #CLAIM_FRAME_TYPE]
	teq	r0, r7
	beq	swi_filter_claimed
	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b

	@ Is there a sink already?
filter_check_sink:
1:	teq	r9, #0
	beq	filter_allocate
	ldr	r0, [r9, #CLAIM_FRAME_LEVEL]
	teq	r0, #FRMLVL_E2SINK
	beq	swi_filter_claimed
	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b

	@ A monitor wants every Ethernet 2.0 frame, so it cannot coexist with any
	@ claim that is not IEEE.
filter_check_monitor:
1:	teq	r9, #0
	beq	filter_allocate
	ldr	r0, [r9, #CLAIM_FRAME_LEVEL]
	teq	r0, #FRMLVL_IEEE
	bne	swi_filter_claimed
	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b

	@ Is IEEE already claimed?
filter_check_ieee:
1:	teq	r9, #0
	beq	filter_allocate
	ldr	r0, [r9, #CLAIM_FRAME_LEVEL]
	teq	r0, #FRMLVL_IEEE
	beq	swi_filter_claimed
	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b

filter_allocate:
	mov	r0, #Module_Claim
	mov	r3, #CLAIM_SIZE
	swi	XOS_Module
	bvs	swi_filter_enobufs
	mov	r9, r2

	mov	r0, #0
	mov	r1, r9
	mov	r3, #CLAIM_SIZE
1:	str	r0, [r1], #4
	subs	r3, r3, #4
	bne	1b

	ldr	r0, [sp, #0 * 4]	@ caller's R0: flags
	and	r0, r0, #1		@ bit 0 alone: ensure_safe wanted
	str	r0, [r9, #CLAIM_FLAGS]
	ldr	r0, [sp, #1 * 4]	@ R1: unit
	str	r0, [r9, #CLAIM_UNIT]
	str	r7, [r9, #CLAIM_FRAME_TYPE]
	str	r8, [r9, #CLAIM_FRAME_LEVEL]
	ldr	r0, [sp, #3 * 4]	@ R3: address level
	str	r0, [r9, #CLAIM_ADDRESS_LEVEL]
	ldr	r0, [sp, #4 * 4]	@ R4: error level
	str	r0, [r9, #CLAIM_ERROR_LEVEL]
	ldr	r0, [sp, #5 * 4]	@ R5: the handler's private word
	str	r0, [r9, #CLAIM_PWP]
	ldr	r0, [sp, #6 * 4]	@ R6: the handler
	str	r0, [r9, #CLAIM_HANDLER]

	@ Link it at the head of the list.
	ldr	r0, [wp, #WS_CLAIMS]
	str	r0, [r9, #CLAIM_NEXT]
	mov	r1, #0
	str	r1, [r9, #CLAIM_PREV]
	teq	r0, #0
	strne	r9, [r0, #CLAIM_PREV]
	str	r9, [wp, #WS_CLAIMS]
	b	swi_filter_recompute

	/* Release: every field has to match, because a protocol module may hold
	 * more than one claim.
	 */
swi_filter_release:
	ldr	r9, [wp, #WS_CLAIMS]
1:	teq	r9, #0
	beq	swi_filter_recompute	@ not found; the C said nothing either
	ldr	r0, [r9, #CLAIM_UNIT]
	ldr	r1, [sp, #1 * 4]
	teq	r0, r1
	bne	2f
	ldr	r0, [r9, #CLAIM_FRAME_TYPE]
	teq	r0, r7
	bne	2f
	ldr	r0, [r9, #CLAIM_FRAME_LEVEL]
	teq	r0, r8
	bne	2f
	ldr	r0, [r9, #CLAIM_ADDRESS_LEVEL]
	ldr	r1, [sp, #3 * 4]
	teq	r0, r1
	bne	2f
	ldr	r0, [r9, #CLAIM_ERROR_LEVEL]
	ldr	r1, [sp, #4 * 4]
	teq	r0, r1
	bne	2f
	ldr	r0, [r9, #CLAIM_PWP]
	ldr	r1, [sp, #5 * 4]
	teq	r0, r1
	bne	2f
	ldr	r0, [r9, #CLAIM_HANDLER]
	ldr	r1, [sp, #6 * 4]
	teq	r0, r1
	bne	2f
	mov	r0, r9
	bl	bounceclaim
	b	swi_filter_recompute
2:	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b

	/* Recompute the interface flags from the surviving claims. Computed here
	 * and then never used or passed to the host, which is why multicast and
	 * promiscuous are advertised by Inquire and do nothing. Inherited.
	 */
swi_filter_recompute:
	mov	r0, #0
	ldr	r9, [wp, #WS_CLAIMS]
1:	teq	r9, #0
	beq	2f
	ldr	r1, [r9, #CLAIM_ADDRESS_LEVEL]
	teq	r1, #ADDRLVL_MULTICAST
	orreq	r0, r0, #IFF_ALLMULTI
	teq	r1, #ADDRLVL_PROMISCUOUS
	orreq	r0, r0, #IFF_PROMISC
	ldr	r9, [r9, #CLAIM_NEXT]
	b	1b
2:	str	r0, [wp, #WS_FLAGS]
	ldmfd	sp!, {r0-r9}
	b	swi_return_ok

swi_filter_enxio:
	ldmfd	sp!, {r0-r9}
	adrl	r0, err_enxio
	b	swi_return_error

swi_filter_einval:
	ldmfd	sp!, {r0-r9}
	adrl	r0, err_einval
	b	swi_return_error

swi_filter_claimed:
	ldmfd	sp!, {r0-r9}
	adrl	r0, err_already_claimed
	b	swi_return_error

swi_filter_enobufs:
	ldmfd	sp!, {r0-r9}
	adrl	r0, err_enobufs
	b	swi_return_error

	.ltorg

	/* Report a frame type as free, unlink the claim and give the memory
	 * back. Entry: R0 -> ClaimBuf.
	 */
bounceclaim:
	stmfd	sp!, {r0-r6, lr}
	mov	r6, r0

	add	r0, wp, #WS_DIB
	mov	r1, #Service_DCIFrameTypeFree
	ldr	r2, [r6, #CLAIM_FRAME_TYPE]
	ldr	r3, [r6, #CLAIM_FRAME_LEVEL]
	orr	r2, r2, r3, lsl #16
	ldr	r3, [r6, #CLAIM_ADDRESS_LEVEL]
	ldr	r4, [r6, #CLAIM_ERROR_LEVEL]
	swi	XOS_ServiceCall

	ldr	r1, [r6, #CLAIM_PREV]
	ldr	r2, [r6, #CLAIM_NEXT]
	teq	r1, #0
	strne	r2, [r1, #CLAIM_NEXT]
	streq	r2, [wp, #WS_CLAIMS]
	teq	r2, #0
	strne	r1, [r2, #CLAIM_PREV]

	mov	r0, #Module_Free
	mov	r2, r6
	swi	XOS_Module
	ldmfd	sp!, {r0-r6, pc}

	/* Stats. R0 bit 0: 0 = say which figures are gathered, 1 = the figures.
	 * R2 -> a struct stats to fill in.
	 *
	 * Copied a word at a time, so the buffer has to be word aligned. Every
	 * field in the structure past the first four bytes is a 32-bit count, so
	 * a caller that did not align it would be broken anyway.
	 */
swi_stats:
	stmfd	sp!, {r0-r4}
	tst	r0, #1
	bne	1f

	adrl	r0, stats_supported
	mov	r1, r2
	mov	r3, #ST_SIZE
2:	ldr	r4, [r0], #4
	str	r4, [r1], #4
	subs	r3, r3, #4
	bne	2b
	ldmfd	sp!, {r0-r4}
	b	swi_return_ok

1:	@ Everything not set below stays zero, including every error counter -
	@ which the table above claims is gathered. Inherited, and listed in this
	@ module's README as a gap rather than papered over.
	mov	r0, #0
	mov	r1, r2
	mov	r3, #ST_SIZE
2:	str	r0, [r1], #4
	subs	r3, r3, #4
	bne	2b

	mov	r0, #ST_TYPE_10BASET
	strb	r0, [r2, #ST_INTERFACE_TYPE]
	mov	r0, #ST_LINK_STATUS_REPORTED
	strb	r0, [r2, #ST_LINK_STATUS]
	mov	r0, #ST_LINK_POLARITY_CORRECT
	strb	r0, [r2, #ST_LINK_POLARITY]
	ldr	r0, [wp, #WS_TX_FRAMES]
	str	r0, [r2, #ST_TX_FRAMES]
	ldr	r0, [wp, #WS_UNWANTED]
	str	r0, [r2, #ST_UNWANTED_FRAMES]
	ldr	r0, [wp, #WS_RX_FRAMES]
	str	r0, [r2, #ST_RX_FRAMES]
	ldmfd	sp!, {r0-r4}
	b	swi_return_ok

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Receiving
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* The expansion card's interrupt. Entered in IRQ mode with interrupts
	 * disabled and R12 = the workspace, which is what we passed as R2 to
	 * OS_ClaimDeviceVector.
	 *
	 * The frame is handed to the protocol module in SVC mode with interrupts
	 * enabled, which is what the C did: cmhg's generic veneer switched to SVC
	 * before calling the C function, and the function turned interrupts back
	 * on around the call. A protocol module is entitled to issue SWIs, so
	 * calling it in IRQ mode would not be safe.
	 */
device_irq:
	stmfd	sp!, {r0-r11, lr}

	/* Switch to SVC. There is no way to change mode without touching the
	 * PSR, so this is the one place in the module that cannot use the
	 * flag-preserving idiom the rest of it does. Both a 26-bit and a 32-bit
	 * path are written out and chosen at run time: TEQ PC, PC sets Z only
	 * when the PSR is not part of R15, that is, on a 32-bit configuration.
	 * This is cmhg's own sequence, instruction for instruction, and it
	 * assembles to the same words.
	 */
	teq	pc, pc
	movne	r10, pc			@ 26-bit: PSR and PC together
	orrne	r0, r10, #3		@ mode bits -> SVC26
	teqnep	r0, #0			@ TEQP: write it to the PSR
	mrseq	r10, cpsr		@ 32-bit
	biceq	r0, r10, #0xf
	orreq	r0, r0, #3		@ mode -> SVC32
	msreq	cpsr_c, r0

	@ R14_svc belongs to whatever we interrupted, and every SWI below would
	@ destroy it.
	mov	r11, lr

	bl	receive_frames

	mov	lr, r11			@ give it back
	teq	pc, pc
	teqnep	r10, #0			@ 26-bit: return to the caller's mode
	msreq	cpsr_c, r10		@ 32-bit
	ldmfd	sp!, {r0-r11, pc}

	/* Take every frame the emulator has waiting and hand each one to
	 * whichever protocol module claimed its frame type. Called in SVC mode
	 * with interrupts disabled; corrupts nothing above R9, so the mode
	 * switch above can keep its state in R10 and R11.
	 */
receive_frames:
	stmfd	sp!, {r0-r9, lr}

	@ Acknowledge the card.
	mov	r0, #Net_SetIRQ
	add	r1, wp, #WS_ERRMESS
	mov	r2, #0
	swi	Net_SWI

	@ One pass at a time: the protocol module below is called with interrupts
	@ on, so another frame can arrive while we are still inside this loop.
	ldr	r0, [wp, #WS_SEMA]
	teq	r0, #0
	bne	rx_done
	mov	r0, #1
	str	r0, [wp, #WS_SEMA]

rx_loop:
	@ Keep an mbuf ready for the header and another for the payload. Holding
	@ them between frames is what saves an allocation per frame.
	ldr	r0, [wp, #WS_RXHDR_MBUF]
	teq	r0, #0
	bne	1f
	mov	r0, #RXHDR_SIZE
	bl	mbuf_alloc_s
	str	r0, [wp, #WS_RXHDR_MBUF]
1:	ldr	r0, [wp, #WS_DATA_MBUF]
	teq	r0, #0
	bne	2f
	ldr	r0, =EY_MTU
	bl	mbuf_alloc_s
	str	r0, [wp, #WS_DATA_MBUF]
2:	ldr	r4, [wp, #WS_RXHDR_MBUF]
	ldr	r5, [wp, #WS_DATA_MBUF]
	teq	r4, #0
	teqne	r5, #0
	beq	rx_exhausted		@ no mbufs: try again on the next interrupt

	@ The header goes at the mbuf's own data offset.
	ldr	r0, [r4, #MBUF_OFF]
	add	r6, r4, r0
	mov	r0, #RXHDR_SIZE
	str	r0, [r4, #MBUF_LEN]

	mov	r0, #Net_Receive
	add	r1, wp, #WS_ERRMESS
	mov	r2, r5			@ where the payload goes
	mov	r3, r6			@ ...and the header
	swi	Net_SWI
	teq	r0, #0
	bne	rx_exhausted		@ the host reported a problem
	teq	r1, #0
	beq	rx_exhausted		@ nothing waiting

	ldr	r0, [wp, #WS_RX_FRAMES]
	add	r0, r0, #1
	str	r0, [wp, #WS_RX_FRAMES]

	ldr	r0, [r6, #RXHDR_FRAME_TYPE]
	bl	find_protocol
	teq	r0, #0
	bne	3f

	@ Nobody wants this frame type. The mbufs stay held for the next one.
	ldr	r0, [wp, #WS_UNWANTED]
	add	r0, r0, #1
	str	r0, [wp, #WS_UNWANTED]
	b	rx_loop

3:	mov	r7, r0			@ the claim

	@ Chain the payload behind the header and mark the header as one.
	str	r5, [r4, #MBUF_NEXT]
	mov	r0, #0
	str	r0, [r4, #MBUF_LIST]
	str	r0, [r5, #MBUF_LIST]
	mov	r0, #MT_HEADER
	strb	r0, [r4, #MBUF_TYPE]

	@ The protocol module owns both mbufs from here, so let go of them before
	@ the call rather than after it. The C did it afterwards, which was safe
	@ only because of the guard above.
	mov	r0, #0
	str	r0, [wp, #WS_RXHDR_MBUF]
	str	r0, [wp, #WS_DATA_MBUF]

	swi	XOS_IntOn
	add	r0, wp, #WS_DIB
	mov	r1, r4
	ldr	r2, [r7, #CLAIM_HANDLER]
	stmfd	sp!, {wp}
	ldr	wp, [r7, #CLAIM_PWP]	@ the handler wants its own R12
	mov	lr, pc
	mov	pc, r2
	ldmfd	sp!, {wp}
	swi	XOS_IntOff
	b	rx_loop

rx_exhausted:
	mov	r0, #0
	str	r0, [wp, #WS_SEMA]
rx_done:
	ldmfd	sp!, {r0-r9, pc}

	/* Which protocol module wants this frame? Entry: R0 = frame type, or the
	 * length field if it is an IEEE frame. Exit: R0 -> ClaimBuf, or 0.
	 */
find_protocol:
	stmfd	sp!, {r1-r3, lr}
	ldr	r1, [wp, #WS_CLAIMS]
	ldr	r3, =IEEE_MAX_LENGTH
	cmp	r0, r3
	bhi	fp_ethernet

	@ Small enough to be an IEEE length field: has anyone claimed IEEE?
1:	teq	r1, #0
	beq	fp_none
	ldr	r2, [r1, #CLAIM_FRAME_LEVEL]
	teq	r2, #FRMLVL_IEEE
	beq	fp_found
	ldr	r1, [r1, #CLAIM_NEXT]
	b	1b

fp_ethernet:
	@ Anyone who asked for this type, or for all of them
1:	teq	r1, #0
	beq	fp_sink
	ldr	r2, [r1, #CLAIM_FRAME_TYPE]
	teq	r2, r0
	beq	fp_found
	ldr	r2, [r1, #CLAIM_FRAME_LEVEL]
	teq	r2, #FRMLVL_E2MONITOR
	beq	fp_found
	ldr	r1, [r1, #CLAIM_NEXT]
	b	1b

fp_sink:
	@ Nobody. Is there a sink for unclaimed types?
	ldr	r1, [wp, #WS_CLAIMS]
1:	teq	r1, #0
	beq	fp_none
	ldr	r2, [r1, #CLAIM_FRAME_LEVEL]
	teq	r2, #FRMLVL_E2SINK
	beq	fp_found
	ldr	r1, [r1, #CLAIM_NEXT]
	b	1b

fp_found:
	mov	r0, r1
	ldmfd	sp!, {r1-r3, pc}

fp_none:
	mov	r0, #0
	ldmfd	sp!, {r1-r3, pc}

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ *ERPCEmInfo
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Entry: R0 -> command tail, R1 = parameter count, R12 -> private word.
	 * The same figures the C printed, without needing a C library to print
	 * them.
	 */
command_info:
	stmfd	sp!, {r0-r9, lr}
	ldr	wp, [r12]
	teq	wp, #0
	beq	cmd_done

	adrl	r0, msg_address
	swi	XOS_Write0
	mov	r4, #0
1:	add	r0, wp, #WS_DEV_ADDR
	ldrb	r0, [r0, r4]
	add	r1, wp, #WS_SCRATCH
	mov	r2, #SCRATCH_SIZE
	swi	XOS_ConvertHex2
	swi	XOS_Write0
	add	r4, r4, #1
	cmp	r4, #6
	bhs	2f
	mov	r0, #':'
	swi	XOS_WriteC
	b	1b
2:	swi	XOS_NewLine

	adrl	r0, msg_sent
	swi	XOS_Write0
	ldr	r0, [wp, #WS_TX_FRAMES]
	bl	print_cardinal
	swi	XOS_NewLine

	adrl	r0, msg_received
	swi	XOS_Write0
	ldr	r0, [wp, #WS_RX_FRAMES]
	bl	print_cardinal
	swi	XOS_NewLine

	adrl	r0, msg_undelivered
	swi	XOS_Write0
	ldr	r0, [wp, #WS_UNWANTED]
	bl	print_cardinal
	swi	XOS_NewLine

	adrl	r0, msg_clients
	swi	XOS_Write0
	swi	XOS_NewLine

	ldr	r4, [wp, #WS_CLAIMS]
1:	teq	r4, #0
	beq	cmd_done
	adrl	r0, msg_frame_type
	swi	XOS_Write0
	ldr	r0, [r4, #CLAIM_FRAME_TYPE]
	add	r1, wp, #WS_SCRATCH
	mov	r2, #SCRATCH_SIZE
	swi	XOS_ConvertHex4
	swi	XOS_Write0
	adrl	r0, msg_frame_level
	swi	XOS_Write0
	ldr	r0, [r4, #CLAIM_FRAME_LEVEL]
	bl	print_cardinal
	adrl	r0, msg_address_level
	swi	XOS_Write0
	ldr	r0, [r4, #CLAIM_ADDRESS_LEVEL]
	bl	print_cardinal
	adrl	r0, msg_error_level
	swi	XOS_Write0
	ldr	r0, [r4, #CLAIM_ERROR_LEVEL]
	bl	print_cardinal
	swi	XOS_NewLine
	ldr	r4, [r4, #CLAIM_NEXT]
	b	1b

cmd_done:
	cmp	pc, #0			@ clear V
	ldmfd	sp!, {r0-r9, pc}

	/* Print R0 as a decimal number. No newline: some of the lines above put
	 * more than one number on a line.
	 */
print_cardinal:
	stmfd	sp!, {r0-r2, lr}
	add	r1, wp, #WS_SCRATCH
	mov	r2, #SCRATCH_SIZE
	swi	XOS_ConvertCardinal4
	swi	XOS_Write0
	ldmfd	sp!, {r0-r2, pc}

	.ltorg

msg_address:
	.string	"Interface address   : "
	.align
msg_sent:
	.string	"Packets sent        : "
	.align
msg_received:
	.string	"Packets received    : "
	.align
msg_undelivered:
	.string	"Undelivered packets : "
	.align
msg_clients:
	.string	"Filter clients:"
	.align
msg_frame_type:
	.string	"    frame_type = "
	.align
msg_frame_level:
	.string	", frame_level = "
	.align
msg_address_level:
	.string	", address_level = "
	.align
msg_error_level:
	.string	", error_level = "
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Errors
@
@ The numbers the DCI4 specification suggests deriving from errno values, in
@ our own error chunk. Same numbers and same wording as the C's s.errors.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

err_enxio:
	.int	ERR_ENXIO
	.string	"Device not configured"
	.align
err_einval:
	.int	ERR_EINVAL
	.string	"Invalid argument"
	.align
err_enotty:
	.int	ERR_ENOTTY
	.string	"Inappropriate ioctl for device"
	.align
err_enobufs:
	.int	ERR_ENOBUFS
	.string	"No buffer space available"
	.align
err_already_claimed:
	.int	ERR_AlreadyClaimed
	.string	"Frame type already claimed"
	.align

	@ Looked up in the global messages file, so this is a token, not text.
err_bad_swi:
	.int	ERR_BAD_SWI
	.string	"BadSWI"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Which statistics are gathered
@
@ Returned by Stats with bit 0 of R0 clear: a copy of the structure with each
@ field set to all ones if it is gathered and zero if it is not. Byte fields
@ get 255 and word fields get &ffffffff, which is why this is written out
@ rather than computed.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

stats_supported:
	.byte	0xff		@ st_interface_type
	.byte	0xff		@ st_link_status
	.byte	0xff		@ st_link_polarity
	.byte	0		@ st_blank1
	.int	0		@ st_link_failures
	.int	0		@ st_network_collisions
	.int	0		@ st_collisions
	.int	0		@ st_excess_collisions
	.int	0		@ st_heartbeat_failures
	.int	0		@ st_not_listening
	.int	0xffffffff	@ st_tx_frames
	.int	0		@ st_tx_bytes
	.int	0xffffffff	@ st_tx_general_errors
	.space	8		@ st_last_dest_addr
	.int	0		@ st_crc_failures
	.int	0		@ st_frame_alignment_errors
	.int	0		@ st_dropped_frames
	.int	0		@ st_runt_frames
	.int	0		@ st_overlong_frames
	.int	0		@ st_jabbers
	.int	0		@ st_late_events
	.int	0xffffffff	@ st_unwanted_frames
	.int	0xffffffff	@ st_rx_frames
	.int	0		@ st_rx_bytes
	.int	0xffffffff	@ st_rx_general_errors
	.space	8		@ st_last_src_addr

	@ Assembling a table of the wrong length would hand a protocol module
	@ somebody else's memory, so say so here rather than find out later.
	.if	. - stats_supported != ST_SIZE
	.error	"stats_supported is not ST_SIZE bytes long"
	.endif


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ The AutoSense file
@
@ !Boot's network configuration reads this to learn how to drive us. The C
@ turned it into a C array with bin2c at build time; here it is simply included.
@ The locations are a run of strings ending in an empty one - see
@ autosense_install for why they are not a table of addresses.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

autosense_locations:
	.string	"<Boot$Dir>.Resources.Configure.!InetSetup.AutoSense.EtherRPCEm"
	.string	"<Boot$Dir>.RO420Hook.Res.Configure.!NetSetup.!IFSetup.AutoSense.EtherRPCEm"
	.string	"<Boot$Dir>.RO430Hook.Res.Configure.!NetSetup.!IFSetup.AutoSense.EtherRPCEm"
	.string	"<Boot$Dir>.RO440Hook.Res.Configure.!NetSetup.!IFSetup.AutoSense.EtherRPCEm"
	.byte	0		@ the empty string that ends the list
	.align

autosense_data:
	.incbin	"AutoSense/EtherRPCEm,ffb"
autosense_end:
	.align

	.end
