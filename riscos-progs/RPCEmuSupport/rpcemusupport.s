@ RPCEmuSupport - RPCEmu guest support module
@
@ Copyright (C) 2025-2026 Andy Timmins
@
@ Formerly called HostCmd. The name changed once the module was in line to carry
@ duties beyond the host command channel; the SWI interface, the * commands
@ (*HostCmdStatus, *Desktop_HostCmd) and the host-side protocol are unchanged,
@ so nothing that used HostCmd needs to change.
@
@ This program is free software; you can redistribute it and/or modify it under
@ the terms of the GNU General Public License as published by the Free Software
@ Foundation; either version 2 of the License, or (at your option) any later
@ version. It is distributed in the hope that it will be useful, but WITHOUT ANY
@ WARRANTY; see the GNU General Public License (COPYING) for more details.
@
@ Guest-side half of HostCmd. A background ticker polls the emulator (via the
@ ArcEm HostCmd SWI) for a command line submitted by the host; when one
@ arrives it is run through OS_CLI from a transient callback with WrchV claimed,
@ so all VDU output is captured into a ring buffer. The ticker streams that ring
@ back to the emulator (OUTPUT sub-op) while the command is still running, then
@ reports the return code (STATUS END) once the ring has drained.
@
@ The ArcEm HostCmd SWI is handled entirely inside the emulator and never
@ re-enters RISC OS, so it is safe to issue from the ticker (event/IRQ time) as
@ well as from the callback. OS_CLI, by contrast, is only ever issued from the
@ transient callback (a safe foreground context).
@
@ 32-bit compatible; written in the 26/32-neutral idiom used by hostfs.s
@ (no mrs/msr, flag-preserving returns).

	@ Target the lowest RiscPC CPU: ARM610/ARM710 are ARMv3 (StrongARM is v4).
	@ Assemble to the ARMv3 floor so nothing v4-only (e.g. ldrh) sneaks in, and
	@ so the assembler rejects rather than silently promotes non-encodable
	@ immediates to movw/movt (undefined on every RiscPC CPU).
	.arch	armv3

	wp	.req	r12

	@ ARM constants
	NBIT = 1 << 31

	@ RISC OS SWIs (X-form: bit 17 set so errors return via V + R0)
	XOS_WriteC		= 0x20000
	XOS_Write0		= 0x20002
	XOS_NewLine		= 0x20003
	XOS_CLI			= 0x20005
	XOS_File		= 0x20008
	XOS_Exit		= 0x20011
	XOS_Module		= 0x2001e
	XOS_GSTrans		= 0x20027
	XOS_Claim		= 0x2001f
	XOS_Release		= 0x20020
	XOS_ReadVarVal		= 0x20023
	XOS_ChangeEnvironment	= 0x20040
	XOS_CallEvery		= 0x2003c
	XOS_RemoveTickerEvent	= 0x2003d
	XOS_AddCallBack		= 0x20054
	XOS_RemoveCallBack	= 0x20055
	XOS_ConvertCardinal4	= 0x200d8

	@ Wimp SWIs (X-form) - used only by the desktop TaskWindow path
	XWimp_Initialise	= 0x600c0
	XWimp_Poll		= 0x600c7
	XWimp_CloseDown		= 0x600dd
	XWimp_StartTask		= 0x600de
	XWimp_SendMessage	= 0x600e7

	@ OS_Module reason codes
	Module_Enter	= 2
	Module_Claim	= 6
	Module_Free	= 7

	@ Service call numbers
	Service_Reset		= 0x27
	Service_StartWimp	= 0x49
	Service_StartedWimp	= 0x4a

	@ Wimp
	WIMP_VERSION	= 310
	TASK_MAGIC	= 0x4b534154		@ "TASK"
	@ Poll mask: enable pollword scanning (bit 22), suppress null events (bit 0).
	@ User-message events (17/18) cannot be masked and are always delivered.
	WIMP_POLL_MASK	= (1 << 22) | (1 << 0)

	@ Wimp event codes
	EVENT_USERMSG	= 17
	EVENT_USERMSG_R	= 18
	EVENT_POLLWORD	= 13

	@ Wimp message action codes
	Message_Quit		= 0
	TaskWindow_Input	= 0x808c0
	TaskWindow_Output	= 0x808c1
	TaskWindow_Ego		= 0x808c2
	TaskWindow_Morio	= 0x808c3
	TaskWindow_Morite	= 0x808c4

	@ Wimp poll/message block field offsets
	MSG_SIZE	= 0
	MSG_SENDER	= 4
	MSG_MYREF	= 8
	MSG_YOURREF	= 12
	MSG_ACTION	= 16
	MSG_DATA	= 20	@ for TaskWindow Input/Output: [MSG_DATA]=count, MSG_DATA+4=bytes

	@ WrchV vector number
	WrchV		= 3

	@ ArcEm SWI chunk
	ARCEM_SWI_CHUNK  = 0x56ac0
	ARCEM_SWI_CHUNKX = ARCEM_SWI_CHUNK | 0x20000
	ArcEm_HostCmd    = ARCEM_SWI_CHUNKX + 5

	HOSTCMD_PROTOCOL_VERSION = 1

	@ HostCmd SWI sub-operations (selected in r9)
	OP_REGISTER	= 0xffffffff
	OP_POLL		= 0
	OP_OUTPUT	= 1
	OP_STATUS	= 2

	@ STATUS markers (r0)
	STATUS_START	= 0
	STATUS_END	= 1

	@ Module state machine (WS_STATE)
	STATE_IDLE	= 0
	STATE_PENDING	= 1	@ WrchV path: command polled, callback scheduled
	STATE_RUNNING	= 2	@ WrchV path: OS_CLI running under the callback
	STATE_FLUSHING	= 3	@ either path: command done, drain ring then STATUS END
	STATE_TW_PENDING = 4	@ TaskWindow path: command polled, poll loop to spawn
	STATE_TW_RUNNING = 5	@ TaskWindow path: child task running, awaiting Morio

	@ Sizes (RING_SIZE and CMD_SIZE must keep the workspace layout aligned;
	@ RING_SIZE must be a power of two for the index masking below.)
	CMD_SIZE	= 256
	RCBUF_SIZE	= 16
	TWCMD_SIZE	= 512			@ TaskWindow spawn command string
	POLLBLK_SIZE	= 256			@ Wimp poll / message block
	STACK_SIZE	= 1024			@ private USR stack for the Wimp-task poll
					@ loop: a module task entered via OS_Module
					@ Enter is NOT given a usable USR stack, so we
					@ point sp here before any push (see start)
	OBEY_SIZE	= 512			@ Obey rc-capture wrapper content builder
	SCRAP_SIZE	= 256			@ expanded <Wimp$ScrapDir> wrapper path
	RING_SHIFT	= 16			@ 64 KiB output ring (holds a full command
	RING_SIZE	= 1 << RING_SHIFT	@ burst; OS_CLI can outrun the 1cs ticker)

	OBEY_FILETYPE	= 0xFEB			@ RISC OS Obey file type

	@ Workspace layout (offsets from the claimed workspace pointer)
	WS_STATE	= 0
	WS_RING_R	= 4	@ ring read index  (owned by ticker/consumer)
	WS_RING_W	= 8	@ ring write index (owned by producer: WrchV or poll loop)
	WS_OVERFLOW	= 12
	WS_RETCODE	= 16
	WS_WRCH_CLAIMED	= 20
	WS_CMD_LEN	= 24
	WS_MEMLIMIT	= 28			@ saved app memory limit during OS_CLI
	WS_TASK_HANDLE	= 32			@ our Wimp task handle (0 = not a task / no desktop)
	WS_CHILD_HANDLE	= 36			@ spawned TaskWindow child handle (0 = none)
	WS_POLL_WORD	= 40			@ Wimp poll word (ticker sets non-zero to wake loop)
	WS_TWCMD	= 44			@ TWCMD_SIZE bytes: TaskWindow command builder
	WS_POLLBLK	= WS_TWCMD + TWCMD_SIZE	@ POLLBLK_SIZE bytes: Wimp poll block
	WS_RCBUF	= WS_POLLBLK + POLLBLK_SIZE	@ RCBUF_SIZE bytes
	WS_CMD		= WS_RCBUF + RCBUF_SIZE	@ CMD_SIZE bytes
	WS_RING		= WS_CMD + CMD_SIZE	@ RING_SIZE bytes
	@ Private USR stack last, so the fields above keep small (add-immediate
	@ encodable) offsets; WS_STACK_TOP is only ever loaded via ldr =.
	WS_STACK	= WS_RING + RING_SIZE	@ STACK_SIZE bytes (grows down from top)
	WS_STACK_TOP	= WS_STACK + STACK_SIZE
	WS_OBEY		= WS_STACK_TOP		@ OBEY_SIZE bytes: Obey wrapper builder
	WS_SCRAP	= WS_OBEY + OBEY_SIZE	@ SCRAP_SIZE bytes: expanded wrapper path
	WORKSPACE_SIZE	= WS_SCRAP + SCRAP_SIZE


	.global	_start
_start:

module_start:
	.int	start		@ Start (entered as a Wimp task; see service)
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
	.string	"RPCEmuSupport"
	.align

	@ Defines "help", the *Help / *Modules string, generated from VersionNum by
	@ the Makefile so the version and date live in one place.
	.include "version.inc"

	@ Help and Command keyword table
table:
	.string	"HostCmdStatus"
	.align
	.int	command_status
	.int	0x00000000
	.int	0
	.int	command_status_help

	@ Internal command used to bootstrap ourselves into a Wimp task at desktop
	@ start (see the service handler). Hidden from *Help / *Commands and rejected
	@ if typed by hand.
desktop_hostcmd:
	.string	"Desktop_HostCmd"
	.align
	.int	command_desktop_hostcmd
	.int	0			@ flags (match ScrollWheel/HostFSFiler bootstrap cmd)
	.int	0			@ invalid-syntax message (none)
	.int	command_desktop_hostcmd_help

	.byte	0	@ Table terminator

command_status_help:
	.string	"*HostCmdStatus shows the HostCmd gateway state.\rSyntax: *HostCmdStatus"
	.align

command_desktop_hostcmd_help:
	.string	"HostCmd provides a host command channel.\rDo not use *Desktop_HostCmd; use *Desktop instead."
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Service handler + Wimp-task bootstrap (ScrollWheel idiom). On Service_StartWimp
@ we ask the Wimp to *run our hidden Desktop_HostCmd command, which OS_Module-
@ Enters us so the Start entry (below) becomes a genuine Wimp task with a poll
@ loop. Only then do we have a task handle to give TaskWindow children a slot.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Service call, using the RISC OS 3.5+ service code-table form. This is
	 * required in practice: both ScrollWheel and HostFSFiler use it to become
	 * Wimp tasks, and a plain (old-style) handler was NOT dispatched
	 * Service_StartWimp on this RISC OS, so we never started as a task.
	 *
	 * The word immediately before the entry points at the table; the first
	 * instruction MUST be the magic `mov r0, r0` so the kernel reads it. */
service_codetable:
	.int	0			@ flags (none)
	.int	service_main		@ real handler (offset from module base)
	.int	Service_Reset
	.int	Service_StartWimp
	.int	Service_StartedWimp
	.int	0			@ table terminator

	.int	service_codetable	@ (lives at service-4: pointer to the table)
service:
	mov	r0, r0			@ magic: "I have a service code table at [service-4]"
	teq	r1, #Service_Reset
	teqne	r1, #Service_StartWimp
	teqne	r1, #Service_StartedWimp
	movne	pc, lr

service_main:
	stmfd	sp!, {lr}
	ldr	wp, [r12]
	teq	r1, #Service_StartWimp
	beq	service_startwimp
	teq	r1, #Service_StartedWimp
	beq	service_startedwimp
	@ Service_Reset: drop any stale task/child handles.
	mov	r0, #0
	str	r0, [wp, #WS_TASK_HANDLE]
	str	r0, [wp, #WS_CHILD_HANDLE]
	ldmfd	sp!, {pc}

service_startwimp:
	ldr	r0, [wp, #WS_TASK_HANDLE]
	teq	r0, #0			@ already a task (or starting)?
	moveq	r0, #-1			@ no -> provisionally mark "starting"
	streq	r0, [wp, #WS_TASK_HANDLE]
	adreq	r0, desktop_hostcmd	@ command the Wimp will start as our task
	moveq	r1, #0			@ claim the service
	ldmfd	sp!, {pc}

service_startedwimp:
	@ If we never actually initialised (handle still the -1 marker), clear it
	@ so a later Service_StartWimp can try again.
	ldr	r0, [wp, #WS_TASK_HANDLE]
	cmn	r0, #1			@ handle == -1 ?
	moveq	r0, #0
	streq	r0, [wp, #WS_TASK_HANDLE]
	ldmfd	sp!, {pc}

	/* *Desktop_HostCmd - enter ourselves as the current application (a Wimp
	 * task). r0 = command tail on entry. Mirrors ScrollWheel/HostFSFiler. */
command_desktop_hostcmd:
	stmfd	sp!, {lr}
	mov	r2, r0			@ rest of command line
	adr	r1, title
	mov	r0, #Module_Enter
	swi	XOS_Module
	ldmfd	sp!, {pc}

tw_prefix:
	.string	"TaskWindow -wimpslot 640k -quit -name RPCEmuSupport -ctrl -txt 1 -task "
	.align

	@ Passed to Wimp_Initialise, so this is the name the Task Manager lists us
	@ under in Module tasks. Display only, nothing looks the task up by name, so
	@ it can read as two words even though the module name cannot (a module name
	@ has to stay a single token for *RMKill, *RMEnsure and friends).
	@
	@ Note the -name in tw_prefix above stays one word: it is an argument on a
	@ command line, so a space would end it early and TaskWindow would take
	@ "Support" as the start of the command to run.
task_name:
	.string	"RPCEmu Support"
	.align

	@ Messages we must receive from TaskWindow children. For a Wimp >= 3.00
	@ task (we pass version 310) Wimp_Initialise R3 is a message-list pointer;
	@ passing 0 does NOT deliver these, so the child's output/completion would
	@ never arrive. Terminated by 0 (Message_Quit, action 0, is always sent
	@ regardless of the list). Matches the TServer reference (reference/TServer).
tw_messages:
	.int	TaskWindow_Output	@ 0x808c1: child VDU output
	.int	TaskWindow_Ego		@ 0x808c2: child announces its task handle
	.int	TaskWindow_Morio	@ 0x808c3: child ended (completion signal)
	.int	0			@ list terminator


	/* Initialisation.
	 * Entry: r12 = pointer to private word; r13 = SVC stack.
	 * Exit:  r7-r11, r13 preserved.
	 */
init:
	stmfd	sp!, {r9, lr}

	@ Register with the emulator; a stock emulator has no such SWI, so the
	@ call returns with V set (Unknown SWI) and we decline to initialise.
	mov	r0, #HOSTCMD_PROTOCOL_VERSION
	mov	r9, #OP_REGISTER
	swi	ArcEm_HostCmd
	bvs	init_unsupported
	cmn	r0, #1			@ r0 == 0xffffffff (i.e. -1) ?
	bne	init_unsupported

	@ Claim zeroed workspace and record its pointer in the private word.
	ldr	r0, [r12]
	teq	r0, #0
	bne	1f
	mov	r0, #Module_Claim
	ldr	r3, =WORKSPACE_SIZE
	swi	XOS_Module
	ldmvsfd	sp!, {r9, pc}		@ no memory -> refuse to initialise
	str	r2, [r12]
1:
	ldr	wp, [r12]

	@ Initialise workspace
	mov	r0, #STATE_IDLE
	str	r0, [wp, #WS_STATE]
	mov	r0, #0
	str	r0, [wp, #WS_RING_R]
	str	r0, [wp, #WS_RING_W]
	str	r0, [wp, #WS_OVERFLOW]
	str	r0, [wp, #WS_RETCODE]
	str	r0, [wp, #WS_WRCH_CLAIMED]
	str	r0, [wp, #WS_TASK_HANDLE]
	str	r0, [wp, #WS_CHILD_HANDLE]
	str	r0, [wp, #WS_POLL_WORD]

	@ Register the ticker (every centisecond). r2 is passed to the routine
	@ in r12, so the ticker (and everything it schedules) gets wp directly.
	mov	r0, #1			@ interval, centiseconds
	adr	r1, ticker
	mov	r2, wp
	swi	XOS_CallEvery
	ldmvsfd	sp!, {r9, pc}		@ if the ticker couldn't be registered, bail

	cmp	pc, #0			@ clear V for a successful return
	ldmfd	sp!, {r9, pc}

init_unsupported:
	adr	r0, err_unsupported
	cmp	r0, #NBIT
	cmnvc	r0, #NBIT
	ldmfd	sp!, {r9, pc}		@ return with V set

err_unsupported:
	.int	0
	.string	"HostCmd is not supported by this emulator"
	.align


	/* Finalisation.
	 * Entry: r12 = pointer to private word.
	 */
final:
	stmfd	sp!, {lr}

	ldr	wp, [r12]

	@ Remove the ticker
	adr	r0, ticker
	mov	r1, wp
	swi	XOS_RemoveTickerEvent

	@ Cancel any pending transient callback
	adr	r0, run_command
	mov	r1, wp
	swi	XOS_RemoveCallBack

	@ Release WrchV if we happen to hold it (defensive)
	ldr	r0, [wp, #WS_WRCH_CLAIMED]
	teq	r0, #0
	beq	1f
	mov	r0, #WrchV
	adr	r1, wrchv_handler
	mov	r2, wp
	swi	XOS_Release
	mov	r0, #0
	str	r0, [wp, #WS_WRCH_CLAIMED]
1:
	@ If we are running as a Wimp task, close it down before we vanish.
	ldr	r0, [wp, #WS_TASK_HANDLE]
	teq	r0, #0
	beq	2f
	ldr	r1, =TASK_MAGIC
	swi	XWimp_CloseDown
	mov	r0, #0
	str	r0, [wp, #WS_TASK_HANDLE]
2:
	@ Free workspace
	mov	r0, #Module_Free
	mov	r2, wp
	swi	XOS_Module

	cmp	pc, #0			@ clear V so the module dies cleanly
	ldmfd	sp!, {pc}


	/* *HostCmdStatus - print the current state digit. Minimal diagnostics. */
command_status:
	stmfd	sp!, {r0-r1, lr}
	ldr	wp, [r12]
	adr	r0, status_text
	swi	XOS_Write0
	ldr	r0, [wp, #WS_STATE]
	add	r0, r0, #'0'
	swi	XOS_WriteC
	swi	XOS_NewLine
	ldmfd	sp!, {r0-r1, pc}

status_text:
	.string	"HostCmd state "
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ Ticker - runs ~every centisecond at event time. It is the SOLE consumer of
@ the output ring (so the ordering "all OUTPUT before STATUS END" holds), and
@ the only place that polls for and dispatches new commands.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

ticker:
	stmfd	sp!, {r0-r9, lr}	@ unknown caller context: preserve widely
	@ r12 = wp (from the CallEvery r2 value)

	bl	flush_ring

	ldr	r3, [wp, #WS_STATE]
	teq	r3, #STATE_IDLE
	beq	ticker_poll
	teq	r3, #STATE_FLUSHING
	beq	ticker_check_flush
	b	ticker_done		@ PENDING/RUNNING/TW_PENDING/TW_RUNNING: keep streaming

ticker_check_flush:
	@ The command has finished; once the ring has drained, send STATUS END.
	ldr	r0, [wp, #WS_RING_R]
	ldr	r1, [wp, #WS_RING_W]
	teq	r0, r1
	bne	ticker_done		@ still bytes to flush; try again next tick
	mov	r0, #STATUS_END
	ldr	r1, [wp, #WS_RETCODE]
	ldr	r2, [wp, #WS_OVERFLOW]
	mov	r9, #OP_STATUS
	swi	ArcEm_HostCmd
	mov	r0, #STATE_IDLE
	str	r0, [wp, #WS_STATE]
	b	ticker_done

ticker_poll:
	add	r0, wp, #WS_CMD
	mov	r1, #CMD_SIZE
	mov	r9, #OP_POLL
	swi	ArcEm_HostCmd
	teq	r0, #1			@ 1 = command delivered
	bne	ticker_done		@ 0 = none (2 = too big; cannot happen, capped)
	str	r1, [wp, #WS_CMD_LEN]

	@ Route the command. If we are a live Wimp task (the desktop is up), hand it
	@ to the poll loop, which runs it in a TaskWindow child with its own WimpSlot
	@ (memory for cc / BASIC, and a real stdin). Otherwise fall back to the
	@ headless path: OS_CLI under a transient callback with WrchV captured.
	ldr	r0, [wp, #WS_TASK_HANDLE]
	teq	r0, #0
	bne	ticker_poll_taskwindow

	mov	r0, #STATE_PENDING
	str	r0, [wp, #WS_STATE]
	@ Schedule the command to run in a safe foreground context.
	adr	r0, run_command
	mov	r1, wp
	swi	XOS_AddCallBack
	b	ticker_done

ticker_poll_taskwindow:
	mov	r0, #STATE_TW_PENDING
	str	r0, [wp, #WS_STATE]
	@ Wake the Wimp poll loop; it services the spawn in a foreground context.
	mov	r0, #1
	str	r0, [wp, #WS_POLL_WORD]

ticker_done:
	ldmfd	sp!, {r0-r9, pc}


@ flush_ring: drain as much of the output ring to the emulator (OUTPUT) as the
@ host will accept, honouring backpressure. Sole consumer of WS_RING_R.
@ Preserves all registers used by the caller.
flush_ring:
	stmfd	sp!, {r0-r4, r9, lr}
flush_loop:
	ldr	r1, [wp, #WS_RING_R]
	ldr	r2, [wp, #WS_RING_W]
	teq	r1, r2
	beq	flush_done		@ ring empty
	@ contiguous run length: to W if W>R, else to end of buffer (wrap)
	cmp	r2, r1
	subhi	r3, r2, r1		@ W > R: len = W - R
	movls	r3, #RING_SIZE		@ R > W: len = RING_SIZE - R
	subls	r3, r3, r1
	ldr	r0, =WS_RING		@ (offset too large for an add immediate)
	add	r0, wp, r0
	add	r0, r0, r1		@ r0 = &ring[R] (guest address)
	mov	r1, r3			@ length
	mov	r9, #OP_OUTPUT
	swi	ArcEm_HostCmd		@ r0 = bytes accepted
	ldr	r1, [wp, #WS_RING_R]
	add	r1, r1, r0
	mov	r1, r1, lsl #(32 - RING_SHIFT)	@ mask to low 12 bits (& (RING_SIZE-1))
	mov	r1, r1, lsr #(32 - RING_SHIFT)
	str	r1, [wp, #WS_RING_R]
	teq	r0, #0
	beq	flush_done		@ host accepted nothing: stop (backpressure)
	b	flush_loop
flush_done:
	ldmfd	sp!, {r0-r4, r9, pc}


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ run_command - transient callback. Entered in a safe foreground context (SVC,
@ IRQs enabled) where full OS_CLI use is permitted. r12 = wp (AddCallBack r1).
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

run_command:
	stmfd	sp!, {r0-r9, lr}

	mov	r0, #STATE_RUNNING
	str	r0, [wp, #WS_STATE]

	@ Tell the host a command's output is starting.
	mov	r0, #STATUS_START
	mov	r1, #0
	mov	r2, #0
	mov	r9, #OP_STATUS
	swi	ArcEm_HostCmd

	mov	r0, #0
	str	r0, [wp, #WS_OVERFLOW]
	str	r0, [wp, #WS_RETCODE]

	@ Claim WrchV so all VDU output is captured (and still displayed).
	mov	r0, #WrchV
	adr	r1, wrchv_handler
	mov	r2, wp
	swi	XOS_Claim
	mov	r0, #1
	str	r0, [wp, #WS_WRCH_CLAIMED]

	@ Raise the application memory limit around OS_CLI so memory-hungry
	@ commands (BASIC, compilers) don't fail the "not enough memory" test when
	@ run outside a TaskWindow slot. Technique from Pace's TServer telnet shell
	@ (reference/TServer). Restored immediately afterwards.
	mov	r0, #0			@ OS_ChangeEnvironment reason 0 = MemoryLimit
	ldr	r1, =0x00a00000		@ 10 MiB
	swi	XOS_ChangeEnvironment
	str	r1, [wp, #WS_MEMLIMIT]	@ previous limit is returned in r1

	@ Run the command (X-form so a command error doesn't abort the callback).
	add	r0, wp, #WS_CMD
	swi	XOS_CLI
	mov	r5, #0			@ r5 = error-block ptr (0 = no error)
	movvs	r5, r0			@ on error, OS_CLI returns the error ptr in r0

	@ Restore the previous memory limit.
	mov	r0, #0
	ldr	r1, [wp, #WS_MEMLIMIT]
	swi	XOS_ChangeEnvironment

	@ If it errored, echo the error text now, while WrchV is still claimed,
	@ so the host sees the failure (X-form OS_CLI does not print it itself).
	teq	r5, #0
	beq	1f
	add	r0, r5, #4		@ error block = number word, then the string
	swi	XOS_Write0
	swi	XOS_NewLine
1:
	@ Release WrchV.
	mov	r0, #WrchV
	adr	r1, wrchv_handler
	mov	r2, wp
	swi	XOS_Release
	mov	r0, #0
	str	r0, [wp, #WS_WRCH_CLAIMED]

	@ Return code: 1 on a command error, else read Sys$ReturnCode (best effort).
	teq	r5, #0
	movne	r0, #1
	strne	r0, [wp, #WS_RETCODE]
	adreq	r0, var_returncode
	bleq	read_returncode

	@ Hand back to the ticker to flush remaining output then send STATUS END.
	mov	r0, #STATE_FLUSHING
	str	r0, [wp, #WS_STATE]

	ldmfd	sp!, {r0-r9, pc}


@ read_returncode: parse the decimal system variable named by r0 (NUL-terminated)
@ into WS_RETCODE. The WrchV path reads Sys$ReturnCode; the TaskWindow path reads
@ HostCmd$Rc (Sys$ReturnCode is reset by TaskWindow -quit before we see Morio).
@ Best effort - leaves 0 on any failure.
read_returncode:
	stmfd	sp!, {r0-r6, lr}
	mov	r1, r0			@ preserve varname across the WS_RETCODE store
	mov	r0, #0
	str	r0, [wp, #WS_RETCODE]
	mov	r0, r1			@ r0 = varname (from caller)
	add	r1, wp, #WS_RCBUF
	mov	r2, #(RCBUF_SIZE - 1)
	mov	r3, #0
	mov	r4, #0
	swi	XOS_ReadVarVal
	bvs	rc_done
	@ r2 = length of value string; NUL-terminate and parse decimal.
	add	r1, wp, #WS_RCBUF
	mov	r0, #0
	strb	r0, [r1, r2]
	mov	r4, #0			@ accumulator
rc_parse:
	ldrb	r0, [r1], #1
	teq	r0, #0
	beq	rc_store
	cmp	r0, #'0'
	blt	rc_store
	cmp	r0, #'9'
	bgt	rc_store
	sub	r0, r0, #'0'
	add	r4, r4, r4, lsl #2	@ r4 = r4 * 5
	add	r4, r0, r4, lsl #1	@ r4 = r0 + r4 * 2  => r4*10 + digit
	b	rc_parse
rc_store:
	str	r4, [wp, #WS_RETCODE]
rc_done:
	ldmfd	sp!, {r0-r6, pc}

var_returncode:
	.string	"Sys$ReturnCode"
	.align
var_hostcmdrc:
	.string	"HostCmd$Rc"
	.align


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ wrchv_handler - WrchV tap. Entered with r0 = character, r12 = wp. Appends the
@ byte to the output ring (single-producer) and passes the character on so it
@ is still displayed locally. Must be minimal and register-preserving.
@
@ NOTE: no interrupt-off critical section yet (kept 26/32-neutral, no mrs/msr).
@ The ring is single-producer/single-consumer with a single-word index publish,
@ which is safe unless WrchV is re-entered at interrupt time mid-update; adding
@ an IRQ-off guard is a flagged hardening follow-up.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

wrchv_handler:
	stmfd	sp!, {r1, r2, r3}
	ldr	r1, [wp, #WS_RING_W]
	ldr	r2, [wp, #WS_RING_R]
	add	r3, r1, #1
	mov	r3, r3, lsl #(32 - RING_SHIFT)	@ mask to low 12 bits (& (RING_SIZE-1))
	mov	r3, r3, lsr #(32 - RING_SHIFT)
	teq	r3, r2			@ would this write fill the ring?
	beq	wrchv_overflow
	ldr	r2, =WS_RING		@ (offset too large for an add immediate)
	add	r2, wp, r2
	strb	r0, [r2, r1]		@ ring[W] = char
	str	r3, [wp, #WS_RING_W]	@ publish new write index
	ldmfd	sp!, {r1, r2, r3}
	mov	pc, lr			@ pass the character on (still printed)

wrchv_overflow:
	mov	r3, #1
	str	r3, [wp, #WS_OVERFLOW]
	ldmfd	sp!, {r1, r2, r3}
	mov	pc, lr

	.ltorg


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ TaskWindow (desktop) path. When the desktop is up we run as a Wimp task and
@ run each command in a TaskWindow child, which gives it its own WimpSlot (the
@ memory that cc / BASIC need) and a real stdin. The child's VDU output arrives
@ as TaskWindow_Output messages which we feed into the same output ring the
@ ticker drains; completion arrives as TaskWindow_Morio.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

	/* Start entry - entered via OS_Module Enter (from *Desktop_HostCmd) so we
	 * become the current application. r12 = private word ptr; USR mode. */
start:
	ldr	wp, [r12]
	teq	wp, #0
	swieq	XOS_Exit		@ no workspace (init declined) -> nothing to do

	@ A module entered as a Wimp task (OS_Module Enter, from Service_StartWimp)
	@ is NOT handed a usable USR stack - R13 is 0. ScrollWheel copes by never
	@ touching the stack in its poll loop; we make subroutine calls (spawn,
	@ ring_put_block, ...), so point sp at our own workspace stack first, before
	@ any push. Runs in USR mode; SWIs use their own (SVC/IRQ) stacks.
	ldr	r0, =WS_STACK_TOP	@ (offset > 4095: not an encodable add immediate)
	add	sp, wp, r0

	ldr	r0, =WIMP_VERSION	@ (310 is not an encodable mov immediate)
	ldr	r1, =TASK_MAGIC
	adrl	r2, task_name		@ far reference -> two-instruction adrl
	adrl	r3, tw_messages		@ message list: TaskWindow Output/Ego/Morio
	swi	XWimp_Initialise
	bvs	start_failed
	str	r1, [wp, #WS_TASK_HANDLE]	@ real task handle (replaces the -1 marker)

@ Main Wimp poll loop.
wimp_poll_loop:
	ldr	r0, =WIMP_POLL_MASK
	add	r1, wp, #WS_POLLBLK
	add	r3, wp, #WS_POLL_WORD
	swi	XWimp_Poll
	bvs	wimp_poll_loop		@ ignore poll errors, just retry

	teq	r0, #EVENT_POLLWORD
	beq	wpl_pollword
	teq	r0, #EVENT_USERMSG
	teqne	r0, #EVENT_USERMSG_R
	beq	wpl_message
	b	wimp_poll_loop

start_failed:
	mov	r0, #0			@ clear the provisional -1 so a retry is possible
	str	r0, [wp, #WS_TASK_HANDLE]
	swi	XOS_Exit

@ Poll word raised by the ticker: a command is waiting to be spawned.
wpl_pollword:
	mov	r0, #0
	str	r0, [wp, #WS_POLL_WORD]
	ldr	r0, [wp, #WS_STATE]
	teq	r0, #STATE_TW_PENDING
	bleq	spawn_taskwindow
	b	wimp_poll_loop

@ A Wimp message arrived; dispatch on its action code.
wpl_message:
	add	r4, wp, #WS_POLLBLK
	ldr	r0, [r4, #MSG_ACTION]
	teq	r0, #Message_Quit
	beq	wpl_quit
	ldr	r1, =TaskWindow_Output
	teq	r0, r1
	beq	wpl_tw_output
	ldr	r1, =TaskWindow_Ego
	teq	r0, r1
	beq	wpl_tw_ego
	ldr	r1, =TaskWindow_Morio
	teq	r0, r1
	beq	wpl_tw_morio
	b	wimp_poll_loop

@ Child VDU output: append the bytes to the output ring (ticker drains it).
wpl_tw_output:
	ldr	r0, [wp, #WS_STATE]
	teq	r0, #STATE_TW_RUNNING
	bne	wimp_poll_loop		@ stray output outside a run -> ignore
	add	r4, wp, #WS_POLLBLK
	ldr	r5, [r4, #MSG_DATA]	@ byte count
	cmp	r5, #0
	ble	wimp_poll_loop
	add	r4, r4, #(MSG_DATA + 4)	@ -> the bytes
	bl	ring_put_block		@ r4 = src, r5 = count
	b	wimp_poll_loop

@ Child announced itself: record its task handle (for future input / kill).
wpl_tw_ego:
	add	r4, wp, #WS_POLLBLK
	ldr	r0, [r4, #MSG_SENDER]
	str	r0, [wp, #WS_CHILD_HANDLE]
	b	wimp_poll_loop

@ Child finished: read the return code, hand back to the ticker to drain the
@ ring and send STATUS END (keeps the "all output before END" ordering).
wpl_tw_morio:
	ldr	r0, [wp, #WS_STATE]
	teq	r0, #STATE_TW_RUNNING
	bne	wimp_poll_loop
	adrl	r0, var_hostcmdrc	@ the wrapper captured the real rc here
	bl	read_returncode
	mov	r0, #0
	str	r0, [wp, #WS_CHILD_HANDLE]
	mov	r0, #STATE_FLUSHING
	str	r0, [wp, #WS_STATE]
	b	wimp_poll_loop

@ Desktop shutting down. Abort any in-flight command so the host is not left
@ waiting, close the task, and exit the application.
wpl_quit:
	ldr	r0, [wp, #WS_STATE]
	teq	r0, #STATE_TW_PENDING
	teqne	r0, #STATE_TW_RUNNING
	bne	wpl_quit_close
	mov	r0, #STATUS_END
	mov	r1, #255		@ aborted
	ldr	r2, [wp, #WS_OVERFLOW]
	mov	r9, #OP_STATUS
	swi	ArcEm_HostCmd
	mov	r0, #STATE_IDLE
	str	r0, [wp, #WS_STATE]
wpl_quit_close:
	ldr	r0, [wp, #WS_TASK_HANDLE]
	ldr	r1, =TASK_MAGIC
	swi	XWimp_CloseDown
	mov	r0, #0
	str	r0, [wp, #WS_TASK_HANDLE]
	str	r0, [wp, #WS_CHILD_HANDLE]
	swi	XOS_Exit


@ spawn_taskwindow - build and issue the TaskWindow command for WS_CMD. Runs in
@ the poll loop (foreground). On success the child streams output back to us.
spawn_taskwindow:
	stmfd	sp!, {r0-r9, lr}

	@ Announce the command's output is starting.
	mov	r0, #STATUS_START
	mov	r1, #0
	mov	r2, #0
	mov	r9, #OP_STATUS
	swi	ArcEm_HostCmd

	mov	r0, #0
	str	r0, [wp, #WS_OVERFLOW]
	str	r0, [wp, #WS_RETCODE]
	str	r0, [wp, #WS_CHILD_HANDLE]

	@ Build "<prefix>-task <handle> \"<inner>\"" into WS_TWCMD, where <inner> is
	@ either `Obey <scrap>` (the rc-capturing wrapper) or, if that could not be
	@ written, the raw command itself.
	add	r4, wp, #WS_TWCMD	@ r4 = write cursor
	adrl	r5, tw_prefix		@ far reference -> two-instruction adrl
	bl	copy_string		@ append prefix (up to "-task ")

	ldr	r0, [wp, #WS_TASK_HANDLE]
	mov	r1, r4
	mov	r2, #16
	swi	XOS_ConvertCardinal4	@ decimal handle at r1; r1 -> terminating NUL
	mov	r4, r1			@ overwrite the NUL onward

	mov	r0, #' '
	strb	r0, [r4], #1
	mov	r0, #'"'
	strb	r0, [r4], #1

	@ Prefer the rc-capturing Obey wrapper (carry clear => WS_SCRAP holds path).
	bl	write_obey_wrapper
	bcs	tw_inner_raw

	adrl	r5, obey_cmd_prefix	@ <inner> = "Obey <scrappath>"
	bl	copy_string
	ldr	r5, =WS_SCRAP
	add	r5, wp, r5		@ expanded, NUL-terminated wrapper path
	bl	copy_string
	b	tw_inner_done

tw_inner_raw:
	add	r5, wp, #WS_CMD		@ fallback <inner> = the raw command
	ldr	r6, [wp, #WS_CMD_LEN]
tw_copy_cmd:
	teq	r6, #0
	beq	tw_inner_done
	ldrb	r0, [r5], #1
	strb	r0, [r4], #1
	sub	r6, r6, #1
	b	tw_copy_cmd

tw_inner_done:
	mov	r0, #'"'
	strb	r0, [r4], #1
	mov	r0, #0
	strb	r0, [r4], #1		@ NUL-terminate for Wimp_StartTask

	mov	r0, #STATE_TW_RUNNING
	str	r0, [wp, #WS_STATE]
	add	r0, wp, #WS_TWCMD
	swi	XWimp_StartTask
	bvs	spawn_failed
	ldmfd	sp!, {r0-r9, pc}

spawn_failed:
	mov	r0, #STATUS_END
	mov	r1, #255		@ could not start the task
	mov	r2, #0
	mov	r9, #OP_STATUS
	swi	ArcEm_HostCmd
	mov	r0, #STATE_IDLE
	str	r0, [wp, #WS_STATE]
	ldmfd	sp!, {r0-r9, pc}


@ write_obey_wrapper - expand <Wimp$ScrapDir>.HostCmdRC into WS_SCRAP, build an
@ Obey rc-capture wrapper for WS_CMD into WS_OBEY, and save it there. Wrapper:
@	Set HostCmd$Rc 255
@	Set Sys$ReturnCode 0
@	<command>
@	Set HostCmd$Rc <Sys$ReturnCode>
@ The last line GSTrans-expands <Sys$ReturnCode> at *execution* time (after the
@ command ran) into HostCmd$Rc - a variable TaskWindow's -quit does NOT reset -
@ so the parent can read the real code on Morio. (Reading Sys$ReturnCode there
@ always yields 0 because -quit has already reset it.) HostCmd$Rc is pre-seeded
@ with 255: a command that hard-errors aborts the Obey before the capture line,
@ so we then report failure (255) rather than a stale previous code. Carry CLEAR on
@ success (WS_SCRAP = NUL-terminated path), carry SET on failure (Wimp$ScrapDir
@ unset, save failed, ...) so the caller falls back to running the raw command.
write_obey_wrapper:
	stmfd	sp!, {r0-r7, lr}

	@ Expand the wrapper path (OS_File takes a raw, un-GSTrans'd filename).
	adrl	r0, scrap_template
	ldr	r1, =WS_SCRAP
	add	r1, wp, r1
	ldr	r2, =SCRAP_SIZE
	swi	XOS_GSTrans
	bvs	wow_fail		@ e.g. Wimp$ScrapDir not set
	ldr	r0, =WS_SCRAP
	add	r0, wp, r0
	mov	r1, #0
	strb	r1, [r0, r2]		@ NUL-terminate the expanded path

	@ Build the wrapper text into WS_OBEY.
	ldr	r4, =WS_OBEY
	add	r4, wp, r4		@ r4 = write cursor
	adrl	r5, obey_pre		@ "Set Sys$ReturnCode 0\n"
	bl	copy_string
	add	r5, wp, #WS_CMD		@ append the command bytes (WS_CMD_LEN)
	ldr	r6, [wp, #WS_CMD_LEN]
wow_copy_cmd:
	teq	r6, #0
	beq	wow_copy_done
	ldrb	r0, [r5], #1
	strb	r0, [r4], #1
	sub	r6, r6, #1
	b	wow_copy_cmd
wow_copy_done:
	adrl	r5, obey_post		@ "\nSet HostCmd$Rc <Sys$ReturnCode>\n"
	bl	copy_string
	mov	r7, r4			@ r7 = end of content

	@ Save WS_OBEY..r7 as an Obey (&FEB) file at the expanded path.
	mov	r0, #10
	ldr	r1, =WS_SCRAP
	add	r1, wp, r1
	ldr	r2, =OBEY_FILETYPE
	mov	r3, #0
	ldr	r4, =WS_OBEY
	add	r4, wp, r4		@ R4 = start
	mov	r5, r7			@ R5 = end
	swi	XOS_File
	bvs	wow_fail

	adds	r0, r0, #0		@ carry CLEAR = success (add 0 never carries)
	ldmfd	sp!, {r0-r7, pc}
wow_fail:
	subs	r0, r0, #0		@ carry SET = failure (sub 0 never borrows)
	ldmfd	sp!, {r0-r7, pc}

scrap_template:
	.string	"<Wimp$ScrapDir>.HostCmdRC"
	.align
obey_pre:
	.string	"Set HostCmd$Rc 255\nSet Sys$ReturnCode 0\n"
	.align
obey_post:
	.string	"\nSet HostCmd$Rc <Sys$ReturnCode>\n"
	.align
obey_cmd_prefix:
	.string	"Obey "
	.align


@ copy_string - append the NUL-terminated string at r5 to [r4] (NUL not copied).
@ Advances r4 past the last byte written; advances r5. Preserves r0.
copy_string:
	stmfd	sp!, {r0, lr}
cs_loop:
	ldrb	r0, [r5], #1
	teq	r0, #0
	beq	cs_done
	strb	r0, [r4], #1
	b	cs_loop
cs_done:
	ldmfd	sp!, {r0, pc}


@ ring_put_block - append r5 bytes from r4 into the output ring (producer side,
@ foreground). Sets WS_OVERFLOW and stops early if the ring fills. Mirrors the
@ single-word publish discipline of wrchv_handler. Preserves r4/r5.
ring_put_block:
	stmfd	sp!, {r0-r6, lr}
rpb_loop:
	teq	r5, #0
	beq	rpb_done
	ldr	r1, [wp, #WS_RING_W]
	ldr	r2, [wp, #WS_RING_R]
	add	r3, r1, #1
	mov	r3, r3, lsl #(32 - RING_SHIFT)	@ (W+1) & (RING_SIZE-1)
	mov	r3, r3, lsr #(32 - RING_SHIFT)
	teq	r3, r2			@ would this write fill the ring?
	beq	rpb_overflow
	ldrb	r0, [r4], #1
	ldr	r6, =WS_RING		@ (offset too large for an add immediate)
	add	r6, wp, r6
	strb	r0, [r6, r1]		@ ring[W] = byte
	str	r3, [wp, #WS_RING_W]	@ publish new write index
	sub	r5, r5, #1
	b	rpb_loop
rpb_overflow:
	mov	r0, #1
	str	r0, [wp, #WS_OVERFLOW]
rpb_done:
	ldmfd	sp!, {r0-r6, pc}

	.ltorg
