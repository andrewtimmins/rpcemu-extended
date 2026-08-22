/*
 * The OPEN Bus co-processor card: the register window, the aperture, the
 * timeslice, the interrupt and bus mastering.
 *
 * This is the card, not the processors: tests/test_cpu_rv32i.c,
 * tests/test_cpu_6502.c and tests/test_cpu_z80.c cover those, so what is left
 * here is everything a card does AROUND a core. It links the bus itself and
 * substitutes a fake host memory and a fake interrupt controller for the
 * emulator, exactly as tests/test_openbus.c does for the stub card, so it runs on
 * every platform with no machine and no display.
 *
 * The rules being pinned down are the ones that would be expensive to get wrong
 * once a guest module is talking to this card:
 *
 *   - one card design, three cores: the CORE register says which is fitted, and
 *     every other register behaves the same whichever it is. Each of the three is
 *     run to a halt through the same sequence, because a register that only
 *     works for the core the author happened to test is the whole failure this
 *     project keeps having.
 *   - the aperture, which is how a program is loaded without the guest ever
 *     needing a physical address. Byte and word access, and the auto-increment.
 *   - the interrupt is raised ONCE per stop, not once per timeslice, or a guest
 *     would spend its life in an interrupt handler.
 *   - bus mastering both ways, and that a transfer which would run outside card
 *     RAM does not start and leaves the length alone so the guest can tell.
 *   - narrow reads see the addressed byte of a register, not its low byte.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_6502.h"
#include "cpu_rv32i.h"
#include "cpu_z80.h"
#include "openbus.h"
#include "openbus_coproc.h"
#include "copro_bus.h"
#include "openbus_stub.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_u32(const char *what, uint32_t got, uint32_t want)
{
	const int ok = (got == want);

	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf("      got 0x%08x, wanted 0x%08x\n", got, want);
		failures++;
	}
}

/* ---- a fake machine ----------------------------------------------------- */

#define FAKE_BASE	0x10000000u
#define FAKE_WORDS	64

static uint32_t fake_ram[FAKE_WORDS];
static int irq_line, fiq_line;
static int irq_calls;

static uint32_t
fake_read32(uint32_t phys)
{
	const uint32_t index = (phys - FAKE_BASE) >> 2;

	if (phys < FAKE_BASE || index >= FAKE_WORDS) {
		return 0xdeadbeefu;
	}
	return fake_ram[index];
}

static void
fake_write32(uint32_t phys, uint32_t val)
{
	const uint32_t index = (phys - FAKE_BASE) >> 2;

	if (phys < FAKE_BASE || index >= FAKE_WORDS) {
		return;
	}
	fake_ram[index] = val;
}

static void
fake_set_irq(int state)
{
	irq_line = state;
	irq_calls++;
}

static void
fake_set_fiq(int state)
{
	fiq_line = state;
}

static const openbus_host_ops fake_ops = {
	.read32 = fake_read32,
	.write32 = fake_write32,
	.set_irq = fake_set_irq,
	.set_fiq = fake_set_fiq,
};

/* ---- talking to the card ------------------------------------------------ */

static uint32_t
rd(uint32_t offset)
{
	return openbus_reg_read(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_32);
}

static void
wr(uint32_t offset, uint32_t val)
{
	openbus_reg_write(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_32, val);
}

static uint32_t
rd8(uint32_t offset)
{
	return openbus_reg_read(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_8);
}

static void
wr8(uint32_t offset, uint32_t val)
{
	openbus_reg_write(OPENBUS_REG_BASE + offset, OPENBUS_SIZE_8, val);
}

/** Start again with the named core fitted. */
static int
fit(const char *core)
{
	openbus_remove();
	memset(fake_ram, 0, sizeof(fake_ram));
	irq_line = fiq_line = 0;
	irq_calls = 0;

	if (openbus_coproc_request(core) != 0) {
		return 0;
	}
	return openbus_coproc_fit() == 0;
}

/** Load a program into card RAM through the aperture, a byte at a time. */
static void
load_program(const uint8_t *prog, unsigned len)
{
	unsigned i;

	wr(OPENBUS_COPROC_REG_ADDR, 0);
	for (i = 0; i < len; i++) {
		wr8(OPENBUS_COPROC_REG_DATA, prog[i]);
	}
}

/** Give the card timeslices until its core stops, or give up. */
static int
run_to_stop(void)
{
	int slices;

	for (slices = 0; slices < 2000; slices++) {
		const uint32_t status = rd(OPENBUS_COPROC_REG_STATUS);

		if (status & (OPENBUS_COPROC_STATUS_HALTED |
		              OPENBUS_COPROC_STATUS_FAULT)) {
			return 1;
		}
		(void) openbus_run(1000);
	}
	return 0;
}

/* The three programs, one per core, each stopping with 42 as its exit code. */

/* addi a0, x0, 42 ; ecall */
static const uint8_t prog_rv32i[] = {
	0x13, 0x05, 0xa0, 0x02,
	0x73, 0x00, 0x00, 0x00
};

/* lda #42 ; brk */
static const uint8_t prog_6502[] = { 0xa9, 0x2a, 0x00 };

/* ld a,42 ; halt */
static const uint8_t prog_z80[] = { 0x3e, 0x2a, 0x76 };

/* ---- the tests ---------------------------------------------------------- */

static void
test_identity(void)
{
	printf("Identity, and one card design across three cores:\n");

	check("the rv32i card fits", fit("rv32i"));
	check_u32("ID says this card", rd(OPENBUS_COPROC_REG_ID),
	          OPENBUS_COPROC_ID);
	check_u32("CORE says RV32", rd(OPENBUS_COPROC_REG_CORE),
	          OPENBUS_COPROC_CORE_RV32I_ID);
	check_u32("with a megabyte of card RAM",
	          rd(OPENBUS_COPROC_REG_RAMSIZE), 1024u * 1024u);
	check("and a name naming the core",
	      strstr(openbus_name(), "RV32IM") != NULL);

	check("the 6502 card fits", fit("6502"));
	check_u32("CORE says 6502", rd(OPENBUS_COPROC_REG_CORE),
	          OPENBUS_COPROC_CORE_6502_ID);
	check_u32("with the 64K that is its whole address space",
	          rd(OPENBUS_COPROC_REG_RAMSIZE), 64u * 1024u);

	check("the z80 card fits", fit("z80"));
	check_u32("CORE says Z80", rd(OPENBUS_COPROC_REG_CORE),
	          OPENBUS_COPROC_CORE_Z80_ID);
	check_u32("with 64K as well", rd(OPENBUS_COPROC_REG_RAMSIZE),
	          64u * 1024u);

	/* Names are matched without regard to case, and rubbish is refused rather
	   than quietly starting a machine with no card in it. */
	check("a core name is matched whatever its case",
	      openbus_coproc_request("RV32I") == 0 &&
	      openbus_coproc_requested_core() == OPENBUS_COPROC_RV32I);
	check("an unknown core is refused", openbus_coproc_request("6809") != 0);
	check("a name that is a prefix of one is refused",
	      openbus_coproc_request("rv32") != 0);
	check("a name that extends one is refused",
	      openbus_coproc_request("z80x") != 0);
	check("and so is nothing at all", openbus_coproc_request(NULL) != 0);
	check("the request survives a refusal",
	      openbus_coproc_requested() &&
	      openbus_coproc_requested_core() == OPENBUS_COPROC_RV32I);

	check("every core has a name for the command line",
	      openbus_coproc_core_name(OPENBUS_COPROC_RV32I) != NULL &&
	      openbus_coproc_core_name(OPENBUS_COPROC_6502) != NULL &&
	      openbus_coproc_core_name(OPENBUS_COPROC_Z80) != NULL);
}

static void
test_aperture(void)
{
	printf("\nThe aperture, which is how a program gets in:\n");

	(void) fit("z80");

	wr(OPENBUS_COPROC_REG_ADDR, 0x100);
	wr(OPENBUS_COPROC_REG_DATA, 0x44332211u);
	check_u32("a word write advances the aperture by four",
	          rd(OPENBUS_COPROC_REG_ADDR), 0x104);

	wr(OPENBUS_COPROC_REG_ADDR, 0x100);
	check_u32("and reads back what was written",
	          rd(OPENBUS_COPROC_REG_DATA), 0x44332211u);
	check_u32("a word read advances it too", rd(OPENBUS_COPROC_REG_ADDR), 0x104);

	/* Little-endian, so the low byte of the word is at the low address. That
	   matters because a guest loading a program writes bytes in file order. */
	wr(OPENBUS_COPROC_REG_ADDR, 0x100);
	check_u32("the low byte of the word is at the low address",
	          rd8(OPENBUS_COPROC_REG_DATA), 0x11);
	check_u32("a byte access advances the aperture by one",
	          rd(OPENBUS_COPROC_REG_ADDR), 0x101);
	check_u32("and the next byte follows", rd8(OPENBUS_COPROC_REG_DATA), 0x22);

	wr(OPENBUS_COPROC_REG_ADDR, 0x200);
	wr8(OPENBUS_COPROC_REG_DATA, 0xaa);
	wr8(OPENBUS_COPROC_REG_DATA, 0xbb);
	wr(OPENBUS_COPROC_REG_ADDR, 0x200);
	check_u32("bytes written one at a time make the word they should",
	          rd(OPENBUS_COPROC_REG_DATA), 0x0000bbaau);

	/* The aperture wraps rather than faulting, and an address written past the
	   end of RAM is folded into it, so a guest cannot aim it at nothing. */
	wr(OPENBUS_COPROC_REG_ADDR, 64u * 1024u);
	check_u32("an aperture address past the end of RAM wraps",
	          rd(OPENBUS_COPROC_REG_ADDR), 0);
}

static void
test_narrow_reads(void)
{
	printf("\nNarrow reads of a register:\n");

	(void) fit("rv32i");

	/* 'OBCP' is 0x4f424350, so little-endian its bytes are 50 43 42 4f. A
	   card that answered the whole word whatever the width would give 0x50
	   for all four, which is why two different offsets are checked. */
	check_u32("byte 0 of the ID register", rd8(OPENBUS_COPROC_REG_ID + 0), 0x50);
	check_u32("byte 1", rd8(OPENBUS_COPROC_REG_ID + 1), 0x43);
	check_u32("byte 2", rd8(OPENBUS_COPROC_REG_ID + 2), 0x42);
	check_u32("byte 3", rd8(OPENBUS_COPROC_REG_ID + 3), 0x4f);

	/* Past the end of the register map. It was 0x80 until the emulator-facing
	   registers were added and 0x80 became CTLADDR, which is a real register
	   reading zero - so this check started failing and was right to. */
	check_u32("an offset nothing drives reads as an undriven bus",
	          rd(0x100), 0xffffffffu);
	check_u32("and so does the write-only interrupt-clear register",
	          rd(OPENBUS_COPROC_REG_IRQCLEAR), 0xffffffffu);
}

/**
 * Run one core's program through the whole sequence a guest would use.
 *
 * Done for all three rather than for the one that happened to be convenient:
 * "it worked on the core I was testing" is the failure this repeatedly has.
 */
static void
run_one_core(const char *name, const uint8_t *prog, unsigned len,
             uint32_t expect_core_id)
{
	printf("\nLoading and running a program on the %s core:\n", name);

	check("the card fits", fit(name));
	check_u32("with the core asked for", rd(OPENBUS_COPROC_REG_CORE),
	          expect_core_id);

	load_program(prog, len);

	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	check_u32("after a reset nothing has been executed",
	          rd(OPENBUS_COPROC_REG_CYCLES), 0);
	check_u32("the program counter is at the entry point",
	          rd(OPENBUS_COPROC_REG_PC), 0);
	check_u32("and the card is not running",
	          rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_RUNNING, 0);

	check_u32("a card with nothing to do uses no cycles",
	          (uint32_t) openbus_run(100), 0);

	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN |
	                            OPENBUS_COPROC_CTRL_IRQ_ON_HALT);
	check("the card reports itself running",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_RUNNING) != 0);

	check("it reaches a stop", run_to_stop());
	check("which is a halt and not a fault",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_HALTED) != 0 &&
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_FAULT) == 0);
	check_u32("the exit code arrived in the mailbox",
	          rd(OPENBUS_COPROC_REG_MBOX_RX), 42);
	check("instructions were counted", rd(OPENBUS_COPROC_REG_CYCLES) >= 2);
	check_u32("no fault is reported", rd(OPENBUS_COPROC_REG_FAULT), 0);

	/* ★ Once per stop, not once per timeslice. */
	check("the card raised the host's interrupt", irq_line == 1);
	check("and says so in its status",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_IRQ) != 0);
	{
		const int calls_before = irq_calls;

		(void) openbus_run(1000);
		(void) openbus_run(1000);
		check("further timeslices do not raise it again",
		      irq_calls == calls_before);
	}

	wr(OPENBUS_COPROC_REG_IRQCLEAR, 1);
	check("clearing it drops the line", irq_line == 0);
	check("and the status agrees",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_IRQ) == 0);

	check("running is clear now the core has stopped",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_RUNNING) == 0);

	/* ★ And the RUN bit itself is cleared, not merely masked out of STATUS.
	   Checking STATUS alone cannot tell: it computes RUNNING as "run bit set
	   AND not stopped", so a card that left the bit set would still read as
	   not running. The register is what a guest sees. */
	check("and the run bit in CTRL was cleared, not just hidden",
	      (rd(OPENBUS_COPROC_REG_CTRL) & OPENBUS_COPROC_CTRL_RUN) == 0);

	/* ★ A guest that sets RUN again on a core that has already stopped must
	   not get another interrupt for the same stop, or a poll loop becomes an
	   interrupt storm. */
	{
		const int calls_before = irq_calls;
		const uint32_t cycles_before = rd(OPENBUS_COPROC_REG_CYCLES);

		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN |
		                            OPENBUS_COPROC_CTRL_IRQ_ON_HALT);
		(void) openbus_run(1000);
		(void) openbus_run(1000);
		check("setting RUN on a stopped core raises no further interrupt",
		      irq_calls == calls_before && irq_line == 0);
		check("and executes nothing",
		      rd(OPENBUS_COPROC_REG_CYCLES) == cycles_before);
	}

	check("the fiq line was never touched", fiq_line == 0);
}

static void
test_step_and_fault(void)
{
	printf("\nSingle stepping, and a core that faults:\n");

	(void) fit("6502");
	load_program(prog_6502, sizeof(prog_6502));
	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);

	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_STEP);
	/* One instruction, charged what it costs: the program starts with an
	   LDA #n, which is two cycles. CYCLES counts cycles rather than
	   instructions, so a step advances it by the instruction's own timing -
	   the program counter is what says only one instruction ran. */
	check_u32("a step charges the instruction's own cycles",
	          rd(OPENBUS_COPROC_REG_CYCLES), 2);
	check_u32("and the program counter moved over exactly one instruction",
	          rd(OPENBUS_COPROC_REG_PC), 2);
	check("the step bit does not stick",
	      (rd(OPENBUS_COPROC_REG_CTRL) & OPENBUS_COPROC_CTRL_STEP) == 0);

	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_STEP);
	check("a second step reached the BRK",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_HALTED) != 0);
	check_u32("and the mailbox has the exit code",
	          rd(OPENBUS_COPROC_REG_MBOX_RX), 42);

	{
		/* An undocumented opcode: the card must report the fault rather
		   than looking like a core that is still thinking about it. */
		static const uint8_t bad[] = { 0x02 };

		(void) fit("6502");
		load_program(bad, sizeof(bad));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN |
		                            OPENBUS_COPROC_CTRL_IRQ_ON_HALT);
		check("it stops", run_to_stop());
		check("with the fault bit set and the halt bit clear",
		      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_FAULT) != 0 &&
		      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_HALTED) == 0);
		check_u32("the cause is reported", rd(OPENBUS_COPROC_REG_FAULT),
		          CPU6502_FAULT_ILLEGAL);
		check_u32("and the offending opcode",
		          rd(OPENBUS_COPROC_REG_FAULTADDR), 0x02);
		check("a fault raises the interrupt as a halt does", irq_line == 1);
		check("but the mailbox is not given an exit code it never had",
		      rd(OPENBUS_COPROC_REG_MBOX_RX) == 0);
	}
}

static void
test_z80_mailbox(void)
{
	/* ld a,(0) is not an instruction: the Z80 reads its mailbox through the
	   port space, which is the only one of the three cores that can. */
	static const uint8_t prog[] = {
		0xdb, 0x00,		/* in a,(0) - reads MBOX_TX */
		0xd3, 0x00,		/* out (0),a - posts to MBOX_RX */
		0x3c,			/* inc a */
		0x76			/* halt */
	};

	printf("\nThe Z80's mailbox, which the other two cores do not have:\n");

	(void) fit("z80");
	load_program(prog, sizeof(prog));
	wr(OPENBUS_COPROC_REG_MBOX_TX, 0x37);
	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	check_u32("a reset clears the mailbox the guest wrote",
	          rd(OPENBUS_COPROC_REG_MBOX_TX), 0);

	/* So write it after the reset, which is the order a guest would use
	   anyway: reset, then hand over the parameters, then run.

	   Stepped rather than run, because the two things being checked cannot
	   both be seen at the end: the halt overwrites the mailbox with the exit
	   code, so a test that only looked afterwards would see the exit code and
	   conclude nothing about the OUT that came before it. */
	wr(OPENBUS_COPROC_REG_MBOX_TX, 0x37);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_STEP);	/* in a,(0) */
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_STEP);	/* out (0),a */
	check_u32("the core read the mailbox the guest wrote and posted it back",
	          rd(OPENBUS_COPROC_REG_MBOX_RX), 0x37);

	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
	check("it runs to a halt", run_to_stop());
	check_u32("and the halt then overwrote it with the exit code",
	          rd(OPENBUS_COPROC_REG_MBOX_RX), 0x38);
	check("no interrupt was raised, since it was not asked for",
	      irq_line == 0);
}

static void
test_dma(void)
{
	unsigned i;
	int ok;

	printf("\nBus mastering, which is what makes this a second bus master:\n");

	(void) fit("z80");

	for (i = 0; i < 8; i++) {
		fake_ram[i] = 0x11111111u * (i + 1u);
	}

	wr(OPENBUS_COPROC_REG_DMAHOST, FAKE_BASE);
	wr(OPENBUS_COPROC_REG_DMALOCAL, 0x400);
	wr(OPENBUS_COPROC_REG_DMALEN, 8);
	wr(OPENBUS_COPROC_REG_DMACTRL, OPENBUS_COPROC_DMA_START);

	check("a transfer in progress says so",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_DMA) != 0);
	check("and it costs the host cycles rather than being free",
	      openbus_run(4) == 4);
	check_u32("four of the eight words have moved",
	          rd(OPENBUS_COPROC_REG_DMALEN), 4);

	(void) openbus_run(100);
	check_u32("then it finishes", rd(OPENBUS_COPROC_REG_DMALEN), 0);
	check("and stops reporting a transfer",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_DMA) == 0);

	ok = 1;
	wr(OPENBUS_COPROC_REG_ADDR, 0x400);
	for (i = 0; i < 8; i++) {
		if (rd(OPENBUS_COPROC_REG_DATA) != 0x11111111u * (i + 1u)) {
			ok = 0;
		}
	}
	check("host memory arrived in card RAM, word for word", ok);

	/* And back the other way, into a part of host memory that was clear. */
	wr(OPENBUS_COPROC_REG_DMAHOST, FAKE_BASE + 0x80u);
	wr(OPENBUS_COPROC_REG_DMALOCAL, 0x400);
	wr(OPENBUS_COPROC_REG_DMALEN, 4);
	wr(OPENBUS_COPROC_REG_DMACTRL, OPENBUS_COPROC_DMA_START |
	                               OPENBUS_COPROC_DMA_TO_HOST);
	(void) openbus_run(100);

	ok = 1;
	for (i = 0; i < 4; i++) {
		if (fake_ram[32 + i] != 0x11111111u * (i + 1u)) {
			ok = 0;
		}
	}
	check("and card RAM reached host memory", ok);
	check_u32("nothing was written past the transfer", fake_ram[36], 0);

	/* A request that would run off the end of card RAM does not start, and
	   the length is left alone so the guest can see that nothing happened.
	   Clamping would move somebody's data by an amount they did not ask for. */
	wr(OPENBUS_COPROC_REG_DMALOCAL, 64u * 1024u - 8u);
	wr(OPENBUS_COPROC_REG_DMALEN, 100);
	wr(OPENBUS_COPROC_REG_DMACTRL, OPENBUS_COPROC_DMA_START);
	check("a transfer that would overrun card RAM does not start",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_DMA) == 0);
	check_u32("and its length is untouched, so the guest can tell",
	          rd(OPENBUS_COPROC_REG_DMALEN), 100);

	wr(OPENBUS_COPROC_REG_DMALOCAL, 0x401);	/* not word aligned */
	wr(OPENBUS_COPROC_REG_DMALEN, 1);
	wr(OPENBUS_COPROC_REG_DMACTRL, OPENBUS_COPROC_DMA_START);
	check("nor does a misaligned one",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_DMA) == 0);

	wr(OPENBUS_COPROC_REG_DMALOCAL, 0x400);
	wr(OPENBUS_COPROC_REG_DMALEN, 0);
	wr(OPENBUS_COPROC_REG_DMACTRL, OPENBUS_COPROC_DMA_START);
	check("and a transfer of nothing is not a transfer",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_DMA) == 0);
}

static void
test_bus_rules(void)
{
	printf("\nHow the card sits in the slot:\n");

	(void) fit("rv32i");
	check("the bus reports a card present", openbus_present());
	check("a second card is refused rather than replacing this one",
	      openbus_coproc_fit() != 0);
	/* ★ Checking the CORE register alone is not enough, and this is worth
	   knowing: the first version of this check read CORE, and it PASSED
	   against a card whose state had been wiped - because a zeroed core
	   selector happens to mean the first core. RAMSIZE and the aperture are
	   what actually prove the card survived, since a wiped card has no RAM at
	   all. */
	check("and the first is still there",
	      openbus_present() &&
	      rd(OPENBUS_COPROC_REG_CORE) == OPENBUS_COPROC_CORE_RV32I_ID);
	check_u32("with its RAM intact rather than freed by the refusal",
	          rd(OPENBUS_COPROC_REG_RAMSIZE), 1024u * 1024u);
	wr(OPENBUS_COPROC_REG_ADDR, 0);
	wr(OPENBUS_COPROC_REG_DATA, 0xc0ffee00u);
	wr(OPENBUS_COPROC_REG_ADDR, 0);
	check_u32("and still usable", rd(OPENBUS_COPROC_REG_DATA), 0xc0ffee00u);

	/* The stub card cannot displace it either, which is what a user asking
	   for both on one command line gets. */
	openbus_stub_request();
	check("the stub card cannot take the slot either", openbus_stub_fit() != 0);

	{
		/* A machine reset resets the core but keeps where the guest said
		   the program starts, and card RAM survives - both because that is
		   what hardware does and because losing either would make a guest
		   reload the program after every reboot. */
		load_program(prog_rv32i, sizeof(prog_rv32i));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
		(void) run_to_stop();
		check("the core halted before the machine reset",
		      (rd(OPENBUS_COPROC_REG_STATUS) &
		       OPENBUS_COPROC_STATUS_HALTED) != 0);

		openbus_reset();
		check("a machine reset clears the halt",
		      (rd(OPENBUS_COPROC_REG_STATUS) &
		       OPENBUS_COPROC_STATUS_HALTED) == 0);
		check_u32("and the cycle count", rd(OPENBUS_COPROC_REG_CYCLES), 0);
		check_u32("the entry point survives", rd(OPENBUS_COPROC_REG_ENTRY), 0);

		wr(OPENBUS_COPROC_REG_ADDR, 0);
		check_u32("and so does the program in card RAM",
		          rd(OPENBUS_COPROC_REG_DATA), 0x02a00513u);

		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
		check("so it runs again without being reloaded", run_to_stop());
		check_u32("with the same answer", rd(OPENBUS_COPROC_REG_MBOX_RX), 42);
	}

	{
		/* A card removed while holding the host's interrupt would leave it
		   asserted with nothing able to clear it. The bus drops the lines
		   on removal; this checks that it really happens for this card. */
		(void) fit("6502");
		load_program(prog_6502, sizeof(prog_6502));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN |
		                            OPENBUS_COPROC_CTRL_IRQ_ON_HALT);
		(void) run_to_stop();
		check("the card is holding the interrupt", irq_line == 1);

		openbus_remove();
		check("removing it drops the line", irq_line == 0);
		check("and the slot is empty", !openbus_present());
		check_u32("an empty slot reads as an undriven bus",
		          rd(OPENBUS_COPROC_REG_ID), 0xffffffffu);
	}

	/* Fitting again after a removal must not leak the RAM of the last one,
	   and must give a clean card. There is no way to observe a leak from
	   here, so what is checked is that it works at all: the sanitiser job in
	   CI is what would catch the leak, and it runs this test. */
	check("a card fits again after a removal", fit("z80"));
	check_u32("with a fresh, cleared RAM",
	          rd(OPENBUS_COPROC_REG_DATA), 0);
}

/*
 * How much RAM a card may have, which is a per-core question because the ceiling
 * is how many address lines the processor has rather than a preference.
 *
 * The clamping matters more than it looks: the size arrives from a machine's
 * configuration file, which somebody may have edited by hand, and asking a 6502
 * for a megabyte has to produce a working card with 64K rather than a card whose
 * top 960K no address can reach - or a machine that refuses to start.
 */
static void
test_ram_sizes(void)
{
	printf("Card RAM, per core and clamped to what it can address:\n");

	/* What a core can ADDRESS at once is its address lines. */
	check_u32("the 6502 addresses 64K at once",
	          openbus_coproc_ram_flat_limit(OPENBUS_COPROC_6502), 64u * 1024u);
	check_u32("and so does the Z80",
	          openbus_coproc_ram_flat_limit(OPENBUS_COPROC_Z80), 64u * 1024u);

	/* What may be FITTED is larger, because banking pages it through a window
	   exactly as a Spectrum 128 or a sideways ROM does. */
	check("an 8-bit card may carry more than it can address at once",
	      openbus_coproc_ram_max(OPENBUS_COPROC_6502) > 64u * 1024u);
	check("RV32I has a 32-bit space, so it offers more still",
	      openbus_coproc_ram_max(OPENBUS_COPROC_RV32I) >
	      openbus_coproc_ram_max(OPENBUS_COPROC_6502));

	/* Zero means the core's own default, which is what a machine whose
	   configuration predates this setting reads as. */
	check("the 6502 is recognised", openbus_coproc_request("6502") == 0);
	check_u32("zero means the core's default",
	          openbus_coproc_request_ram(0),
	          openbus_coproc_ram_default(OPENBUS_COPROC_6502));
	check_u32("a megabyte is allowed on a 6502 now, for paging",
	          openbus_coproc_request_ram(1024u * 1024u), 1024u * 1024u);
	check_u32("but not more than the card may carry",
	          openbus_coproc_request_ram(64u * 1024u * 1024u),
	          OPENBUS_COPROC_RAM_MAX_8BIT);
	check_u32("an absurdly small size clamps up",
	          openbus_coproc_request_ram(1), OPENBUS_COPROC_RAM_MIN);
	check_u32("a size it can address is honoured exactly",
	          openbus_coproc_request_ram(32u * 1024u), 32u * 1024u);

	/* The same figure means different things to different cores, which is the
	   whole reason the limit is per core. */
	check("rv32i is recognised", openbus_coproc_request("rv32i") == 0);
	check_u32("RV32I may have a megabyte",
	          openbus_coproc_request_ram(1024u * 1024u), 1024u * 1024u);

	openbus_coproc_request_ram(0);
}

/* A card really is given the size asked for, not merely told it. */
static void
test_ram_size_is_what_the_card_gets(void)
{
	printf("A resized card:\n");

	check("the z80 is recognised", openbus_coproc_request("z80") == 0);
	check_u32("16K is honoured", openbus_coproc_request_ram(16u * 1024u),
	          16u * 1024u);
	check("the card fits", fit("z80"));
	check_u32("RAMSIZE reports it", rd(OPENBUS_COPROC_REG_RAMSIZE),
	          16u * 1024u);

	/* The size is real rather than just reported: the aperture wraps at it, so
	   a byte written at the top is the byte read back at the top and the space
	   above does not exist. */
	wr(OPENBUS_COPROC_REG_ADDR, 16u * 1024u - 1u);
	wr8(OPENBUS_COPROC_REG_DATA, 0x5a);
	wr(OPENBUS_COPROC_REG_ADDR, 16u * 1024u - 1u);
	check_u32("and the top byte of it is usable",
	          rd8(OPENBUS_COPROC_REG_DATA), 0x5a);

	/* Left as it was found, so a later test is not run against 16K. */
	openbus_coproc_request_ram(0);
}

/* ---- the emulator-facing registers ------------------------------------- */

/** Write a word into the control area through its aperture. */
static void
ctl_word_write(uint32_t offset, uint32_t value)
{
	wr(OPENBUS_COPROC_REG_CTLADDR, offset);
	wr(OPENBUS_COPROC_REG_CTLDATA, value);
}

static uint32_t
ctl_word_read(uint32_t offset)
{
	wr(OPENBUS_COPROC_REG_CTLADDR, offset);
	return rd(OPENBUS_COPROC_REG_CTLDATA);
}

/** Describe one region, in the control area, as the guest module would. */
static void
map_region(uint32_t at, uint32_t base, uint32_t size, uint32_t kind,
           uint32_t latch)
{
	ctl_word_write(at, base);
	ctl_word_write(at + 4, size);
	ctl_word_write(at + 8, kind);
	ctl_word_write(at + 12, latch);
}

/*
 * The control area and its aperture: the card's own memory, which the core
 * cannot see. Everything the guest sets up lives here, so if the aperture is
 * wrong nothing else can be right.
 */
static void
test_control_area(void)
{
	printf("The control area, which the core cannot see:\n");

	check("a card fits", fit("6502"));
	check_u32("CTLSIZE reports it", rd(OPENBUS_COPROC_REG_CTLSIZE),
	          OPENBUS_COPROC_CTL_SIZE);

	ctl_word_write(0x40, 0xdeadbeefu);
	check_u32("a word written through the aperture reads back",
	          ctl_word_read(0x40), 0xdeadbeefu);

	/* It advances as the card RAM aperture does, so a table is written by
	   setting the address once and then writing words. */
	wr(OPENBUS_COPROC_REG_CTLADDR, 0x80);
	wr(OPENBUS_COPROC_REG_CTLDATA, 0x11111111u);
	wr(OPENBUS_COPROC_REG_CTLDATA, 0x22222222u);
	check_u32("the aperture advanced", rd(OPENBUS_COPROC_REG_CTLADDR), 0x88);
	check_u32("first word", ctl_word_read(0x80), 0x11111111u);
	check_u32("second word", ctl_word_read(0x84), 0x22222222u);

	/* And it is NOT the core's memory: the core's address 0x40 is untouched. */
	wr(OPENBUS_COPROC_REG_ADDR, 0x40);
	check_u32("the core's RAM was not written instead",
	          rd(OPENBUS_COPROC_REG_DATA) & 0xffu, 0);
}

/*
 * A machine described through the registers, and a program's writes collected
 * from the log. This is the whole interface working end to end, driven exactly
 * as the guest module will drive it.
 *
 *   LDA #&aa : STA &4000 : LDA #&bb : STA &4001 : BRK
 */
static void
test_map_and_log_through_registers(void)
{
	static const uint8_t prog[] = {
		0xa9, 0xaa, 0x8d, 0x00, 0x40,
		0xa9, 0xbb, 0x8d, 0x01, 0x40,
		0x00
	};
	const uint32_t map_at = 0x100;
	const uint32_t log_at = 0x400;
	uint32_t head;

	printf("A machine described through the registers:\n");

	check("a 6502 card fits", fit("6502"));
	load_program(prog, sizeof(prog));

	/* &4000 upwards is a screen: writes reach memory and are logged. */
	map_region(map_at, 0x4000, 0x1b00, COPRO_REGION_LOG, 0);
	wr(OPENBUS_COPROC_REG_MAPOFF, map_at);
	wr(OPENBUS_COPROC_REG_MAPCOUNT, 1);
	wr(OPENBUS_COPROC_REG_LOGOFF, log_at);
	wr(OPENBUS_COPROC_REG_LOGENTRIES, 16);

	check_u32("the map is in place", rd(OPENBUS_COPROC_REG_MAPCOUNT), 1);
	check_u32("and the log", rd(OPENBUS_COPROC_REG_LOGENTRIES), 16);

	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
	check("the program runs to its BRK", run_to_stop());

	head = rd(OPENBUS_COPROC_REG_LOGHEAD);
	check_u32("two writes were logged", head, 2);
	check_u32("the guest has read none of them yet",
	          rd(OPENBUS_COPROC_REG_LOGTAIL), 0);

	/* Read them out of the control area, as the guest module will. */
	check_u32("first entry's address", ctl_word_read(log_at + 4), 0x4000);
	check_u32("first entry's value", ctl_word_read(log_at + 8), 0xaa);
	check_u32("second entry's address", ctl_word_read(log_at + 16 + 4), 0x4001);
	check_u32("second entry's value", ctl_word_read(log_at + 16 + 8), 0xbb);

	/* Draining is the guest moving the tail up. */
	wr(OPENBUS_COPROC_REG_LOGTAIL, head);
	check_u32("draining leaves nothing pending",
	          rd(OPENBUS_COPROC_REG_LOGHEAD) - rd(OPENBUS_COPROC_REG_LOGTAIL), 0);

	/* The writes reached memory as well as the log. */
	wr(OPENBUS_COPROC_REG_ADDR, 0x4000);
	check_u32("and the screen memory has the byte",
	          rd(OPENBUS_COPROC_REG_DATA) & 0xffu, 0xaa);
}

/*
 * RUNFOR, which is what lets a guest run one frame at a time: the core stops
 * when the budget is spent, whatever the emulator's own timeslices were.
 *
 * The program is an endless loop, so nothing but the budget can stop it.
 */
static void
test_runfor_budget(void)
{
	static const uint8_t prog[] = { 0x4c, 0x00, 0x00 };	/* JMP &0000 */
	int slices;

	printf("Running for a fixed number of cycles:\n");

	check("a 6502 card fits", fit("6502"));
	load_program(prog, sizeof(prog));

	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	wr(OPENBUS_COPROC_REG_RUNFOR, 50);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);

	/* Give it more timeslices than the budget needs; it must still stop. */
	for (slices = 0; slices < 20; slices++) {
		openbus_run(1000);
	}

	/* At least the budget, and over it by at most one instruction: a JMP is
	   three cycles and cannot be split, so 50 becomes 51. Asking for exactly
	   the budget would be asking the card to stop mid-instruction. */
	check("it ran the budget, overshooting by at most one instruction",
	      rd(OPENBUS_COPROC_REG_CYCLES) >= 50 &&
	      rd(OPENBUS_COPROC_REG_CYCLES) <= 50 + 3);
	check_u32("and says the budget is why it stopped",
	          rd(OPENBUS_COPROC_REG_STOPREASON), OPENBUS_COPROC_STOP_BUDGET);
	check("RUN was cleared, so it does not carry on past the frame",
	      (rd(OPENBUS_COPROC_REG_CTRL) & OPENBUS_COPROC_CTRL_RUN) == 0);
	check("and it is neither halted nor faulted",
	      (rd(OPENBUS_COPROC_REG_STATUS) &
	       (OPENBUS_COPROC_STATUS_HALTED | OPENBUS_COPROC_STATUS_FAULT)) == 0);

	/* Another budget carries on from where it was. */
	wr(OPENBUS_COPROC_REG_RUNFOR, 25);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
	for (slices = 0; slices < 20; slices++) {
		openbus_run(1000);
	}
	check("a second budget continues from where the first stopped",
	      rd(OPENBUS_COPROC_REG_CYCLES) >= 75 &&
	      rd(OPENBUS_COPROC_REG_CYCLES) <= 75 + 6);
}

/*
 * A stalling register: the core stops mid-instruction, the guest is told what
 * was wanted, answers it, and the core carries on. This is the path a CIA or a
 * VIA needs, and the one that cannot be done with a log and a latch.
 *
 *   LDA &dc00 : STA &10 : BRK
 */
static void
test_stall_and_ack(void)
{
	static const uint8_t prog[] = {
		0xad, 0x00, 0xdc,	/* LDA &dc00 */
		0x85, 0x10,		/* STA &10   */
		0x00			/* BRK       */
	};
	const uint32_t map_at = 0x100;
	int slices;

	printf("A register the guest has to answer for:\n");

	check("a 6502 card fits", fit("6502"));
	load_program(prog, sizeof(prog));

	map_region(map_at, 0xdc00, 0x0100, COPRO_REGION_STALL, 0);
	wr(OPENBUS_COPROC_REG_MAPOFF, map_at);
	wr(OPENBUS_COPROC_REG_MAPCOUNT, 1);

	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
	for (slices = 0; slices < 5; slices++) {
		openbus_run(1000);
	}

	check("STATUS says an access is waiting",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_WAITING) != 0);
	check_u32("STOPREASON agrees", rd(OPENBUS_COPROC_REG_STOPREASON),
	          OPENBUS_COPROC_STOP_WAITING);
	check_u32("WAITADDR names the register", rd(OPENBUS_COPROC_REG_WAITADDR),
	          0xdc00);
	check_u32("WAITINFO says it is a read",
	          rd(OPENBUS_COPROC_REG_WAITINFO) & 1u, 0);
	check_u32("and nothing has run past it", rd(OPENBUS_COPROC_REG_CYCLES), 0);

	/* Answer it. */
	wr(OPENBUS_COPROC_REG_WAITDATA, 0x5b);
	wr(OPENBUS_COPROC_REG_WAITACK, 1);
	check("the wait is over",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_WAITING) == 0);

	check("the program finishes", run_to_stop());
	wr(OPENBUS_COPROC_REG_ADDR, 0x10);
	check_u32("the answer reached the program",
	          rd(OPENBUS_COPROC_REG_DATA) & 0xffu, 0x5b);
}

/*
 * Interrupts. The 6502 takes one through the vector at &FFFE, which is what a
 * machine's 50Hz or 100Hz tick becomes.
 *
 * The program loops at &0000; the handler at &0300 halts, so the core stopping
 * at all is proof the interrupt was taken and the vector followed.
 */
static void
test_interrupt(void)
{
	static const uint8_t prog[] = { 0x4c, 0x00, 0x00 };	/* JMP &0000 */
	static const uint8_t handler[] = { 0x00 };		/* BRK */
	int slices;

	printf("An interrupt, and the vector it follows:\n");

	check("a 6502 card fits", fit("6502"));
	load_program(prog, sizeof(prog));

	/* The handler, and BOTH vectors pointing at it: the maskable interrupt
	   goes through &FFFE and the non-maskable one through &FFFA, and they are
	   separate on purpose - a handler needs to know which happened. Setting
	   only one and expecting the other to follow it is a mistake this test
	   made first time round. */
	wr(OPENBUS_COPROC_REG_ADDR, 0x0300);
	wr8(OPENBUS_COPROC_REG_DATA, handler[0]);
	wr(OPENBUS_COPROC_REG_ADDR, 0xfffa);
	wr8(OPENBUS_COPROC_REG_DATA, 0x00);	/* NMI vector low  */
	wr8(OPENBUS_COPROC_REG_DATA, 0x03);	/* NMI vector high */
	wr(OPENBUS_COPROC_REG_ADDR, 0xfffe);
	wr8(OPENBUS_COPROC_REG_DATA, 0x00);	/* IRQ vector low  */
	wr8(OPENBUS_COPROC_REG_DATA, 0x03);	/* IRQ vector high */

	wr(OPENBUS_COPROC_REG_ENTRY, 0);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
	wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
	openbus_run(100);

	check("it is running the loop",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_RUNNING) != 0);

	/* A reset leaves the 6502 with interrupts disabled, exactly as the
	   hardware does, so the first one is ignored - which is worth checking,
	   because a core that took it anyway would look like it worked. */
	wr(OPENBUS_COPROC_REG_IRQCTRL, OPENBUS_COPROC_IRQ_ASSERT);
	openbus_run(100);
	check("a masked interrupt is ignored",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_HALTED) == 0);

	/* The non-maskable one is taken whatever the flags say, which is what
	   non-maskable means. */
	wr(OPENBUS_COPROC_REG_IRQCTRL, OPENBUS_COPROC_IRQ_NMI);
	for (slices = 0; slices < 5; slices++) {
		openbus_run(100);
	}
	check("the non-maskable interrupt is taken whatever the flags say",
	      (rd(OPENBUS_COPROC_REG_STATUS) & OPENBUS_COPROC_STATUS_HALTED) != 0);
	check_u32("through its own vector, so it halted in the handler",
	          rd(OPENBUS_COPROC_REG_STOPREASON), OPENBUS_COPROC_STOP_HALTED);
}

/*
 * The two cores that extend cores we already had: they must be distinguishable
 * from the parts they extend, or a machine's configuration cannot mean anything.
 */
static void
test_extended_cores(void)
{
	printf("The 65C02 and the 8080, which extend cores already here:\n");

	check("a 65C02 card fits", fit("65c02"));
	check_u32("and says so, rather than 6502", rd(OPENBUS_COPROC_REG_CORE),
	          OPENBUS_COPROC_CORE_65C02_ID);
	check("with a name naming the part",
	      strstr(openbus_name(), "65C02") != NULL);

	{
		/* BRA, which a 6502 does not have. Two instructions: BRA over an
		   LDA #&ff, then LDA #&2a and BRK. On the CMOS part the answer is
		   &2a; on the NMOS part the opcode faults. */
		static const uint8_t prog[] = {
			0x80, 0x02, 0xa9, 0xff, 0xa9, 0x2a, 0x00
		};

		load_program(prog, sizeof(prog));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
		check("a CMOS-only instruction runs on it", run_to_stop());
		check_u32("and branched", rd(OPENBUS_COPROC_REG_MBOX_RX), 0x2a);

		/* The same program on a 6502 must fault rather than do something
		   plausible. */
		check("a 6502 card fits", fit("6502"));
		load_program(prog, sizeof(prog));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
		(void) run_to_stop();
		check("while on a 6502 it faults",
		      (rd(OPENBUS_COPROC_REG_STATUS) &
		       OPENBUS_COPROC_STATUS_FAULT) != 0);
	}

	check("an 8080 card fits", fit("8080"));
	check_u32("and says so, rather than Z80", rd(OPENBUS_COPROC_REG_CORE),
	          OPENBUS_COPROC_CORE_8080_ID);
	check("with a name naming the part",
	      strstr(openbus_name(), "8080") != NULL);

	{
		/* JR, which an 8080 does not have. */
		static const uint8_t prog[] = { 0x18, 0x00, 0x76 };

		load_program(prog, sizeof(prog));
		wr(OPENBUS_COPROC_REG_ENTRY, 0);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RESET);
		wr(OPENBUS_COPROC_REG_CTRL, OPENBUS_COPROC_CTRL_RUN);
		(void) run_to_stop();
		check("a Z80-only instruction faults on it",
		      (rd(OPENBUS_COPROC_REG_STATUS) &
		       OPENBUS_COPROC_STATUS_FAULT) != 0);
	}
}

int
main(void)
{
	printf("OPEN Bus co-processor card\n");

	openbus_init(&fake_ops);

	test_identity();
	test_aperture();
	test_narrow_reads();
	run_one_core("rv32i", prog_rv32i, sizeof(prog_rv32i),
	             OPENBUS_COPROC_CORE_RV32I_ID);
	run_one_core("6502", prog_6502, sizeof(prog_6502),
	             OPENBUS_COPROC_CORE_6502_ID);
	run_one_core("z80", prog_z80, sizeof(prog_z80),
	             OPENBUS_COPROC_CORE_Z80_ID);
	test_step_and_fault();
	test_z80_mailbox();
	test_dma();
	test_bus_rules();
	test_extended_cores();
	test_ram_sizes();
	test_ram_size_is_what_the_card_gets();
	test_control_area();
	test_map_and_log_through_registers();
	test_runfor_budget();
	test_stall_and_ack();
	test_interrupt();

	openbus_close();

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
