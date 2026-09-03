/*
 * The ARM disassembler, and the instruction description the debugger stands on.
 *
 * WHY THIS EXISTS. The disassembler had no test at all, which was tolerable
 * while its only job was painting text into an inspector window - a wrong
 * mnemonic is obvious to the person reading it, and costs nothing but a
 * double-take. That stopped being true once arm_decode() started answering
 * questions the debugger acts on: is this instruction a call, is it a return,
 * where does this branch go. Those answers drive step-over and step-out, so a
 * decoding mistake no longer shows up as an odd-looking line. It shows up as a
 * debugger that runs away when you asked it to step, and the person at the
 * other end blames the emulator.
 *
 * Two distinct things are pinned down here:
 *
 *   - TEXT, for a sample of every encoding class. These are regression locks,
 *     not a specification: the point is that a refactor of the decode path
 *     cannot quietly change what a user sees.
 *
 *   - EFFECTS, the ArmInsnInfo fields. This is the part with teeth. is_call
 *     must be true for BL and false for a plain B, or step-over will step into
 *     every loop. is_return must catch all three ways ARM code returns (MOV
 *     PC,LR / BX LR / LDM with PC in the list) or step-out will never fire.
 *
 * FPA deserves special mention. The FPA10 is emulated in fpa.c and RISC OS C
 * code is thick with its instructions, but until recently they disassembled as
 * generic CDP/LDC on coprocessors 1 and 2 - unreadable exactly where floating
 * point bugs live. The encodings checked here were assembled by hand from the
 * FPA operation tables in fpa.c, so this file is also the record of what those
 * bit patterns are supposed to mean.
 */

#include <stdio.h>
#include <string.h>

#include "arm_disasm.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/** The address every instruction is decoded at, unless a test says otherwise. */
#define AT	0x8000u

/**
 * Check that an opcode renders as expected.
 */
static void
check_text(uint32_t opcode, const char *expected)
{
	char buf[256];
	char what[320];

	arm_disasm(opcode, AT, buf, sizeof(buf));
	snprintf(what, sizeof(what), "%08X -> \"%s\"", opcode, expected);
	check(what, strcmp(buf, expected) == 0);

	if (strcmp(buf, expected) != 0) {
		printf("      got \"%s\"\n", buf);
	}
}

/**
 * Check the class an opcode decodes to.
 */
static void
check_class(uint32_t opcode, ArmInsnClass expected)
{
	ArmInsnInfo info;
	char what[128];

	arm_decode(opcode, AT, &info);
	snprintf(what, sizeof(what), "%08X decodes as class %d", opcode, (int) expected);
	check(what, info.cls == expected);
}

/* ---- text ---------------------------------------------------------------- */

static void
test_data_processing(void)
{
	printf("Data processing\n");

	check_text(0xE3A00001, "MOV R0, #1");
	check_text(0xE1A00001, "MOV R0, R1");
	check_text(0xE0812002, "ADD R2, R1, R2");
	check_text(0xE2411001, "SUB R1, R1, #1");
	check_text(0xE3500000, "CMP R0, #0");
	check_text(0xE1A01082, "MOV R1, R2, LSL #1");
	check_text(0xE1A01F82, "MOV R1, R2, LSL #31");
	check_text(0xE1A00060, "MOV R0, R0, RRX");
	check_text(0xE1B00001, "MOVS R0, R1");
	check_text(0x01A00001, "MOVEQ R0, R1");
	check_text(0xE1A0F00E, "MOV PC, R14");
}

static void
test_transfers(void)
{
	printf("Load and store\n");

	check_text(0xE5910000, "LDR R0, [R1]");
	check_text(0xE5910004, "LDR R0, [R1, #4]");
	check_text(0xE5110004, "LDR R0, [R1, #-4]");
	check_text(0xE5B10004, "LDR R0, [R1, #4]!");
	check_text(0xE4910004, "LDR R0, [R1], #4");
	check_text(0xE5C10000, "STRB R0, [R1]");
	check_text(0xE1D100B0, "LDRH R0, [R1]");
	check_text(0xE1C100B0, "STRH R0, [R1]");
	check_text(0xE1D100D0, "LDRSB R0, [R1]");
	check_text(0xE1D100F0, "LDRSH R0, [R1]");
	check_text(0xE1017092, "SWP R7, R2, [R1]");

	printf("Block transfer\n");

	check_text(0xE92D4010, "STMDB R13!, {R4, R14}");
	check_text(0xE8BD8010, "LDMIA R13!, {R4, PC}");
	check_text(0xE8900007, "LDMIA R0, {R0, R1, R2}");
}

static void
test_branches(void)
{
	printf("Branches\n");

	/* Target is address + 8 + (offset << 2) */
	check_text(0xEA000000, "B 0x00008008");
	check_text(0xEB000000, "BL 0x00008008");
	check_text(0xEAFFFFFE, "B 0x00008000");
	check_text(0x1A000002, "BNE 0x00008010");
	check_text(0xE12FFF11, "BX R1");
	check_text(0xE12FFF1E, "BX R14");
}

static void
test_misc(void)
{
	printf("Multiply, PSR and SWI\n");

	check_text(0xE0000291, "MUL R0, R1, R2");
	check_text(0xE0203291, "MLA R0, R1, R2, R3");
	check_text(0xE0810392, "UMULL R0, R1, R2, R3");
	check_text(0xE10F0000, "MRS R0, CPSR");
	check_text(0xE129F000, "MSR CPSR_cf, R0");
	check_text(0xEF000000, "SWI OS_WriteC");
	check_text(0xEF000011, "SWI OS_Exit");
	check_text(0xEF020011, "SWI XOS_Exit");

	printf("Generic coprocessor (not the FPA)\n");

	/* CP15 is the MMU/cache control coprocessor and must stay generic */
	check_text(0xEE110F10, "MRC P15, 0, R0, C1, C0, 0");
	check_text(0xEE010F10, "MCR P15, 0, R0, C1, C0, 0");
}

static void
test_fpa(void)
{
	printf("FPA10 (coprocessors 1 and 2)\n");

	/* Load and store, CP1: precision from bits 22 and 15 */
	check_text(0xED910100, "LDFS F0, [R1]");
	check_text(0xED910108, "LDFS F0, [R1, #32]");
	check_text(0xED2DA102, "STFD F2, [R13, #-8]!");

	/* Multiple register load and store, CP2 */
	check_text(0xED910200, "LFM F0, 4, [R1]");
	check_text(0xED810200, "SFM F0, 4, [R1]");

	/* Dyadic arithmetic */
	check_text(0xEE010102, "ADFS F0, F1, F2");
	check_text(0xEE143189, "MUFD F3, F4, #1.0");
	check_text(0xEE210103, "SUFS F0, F1, F3");
	check_text(0xEE410102, "DVFS F0, F1, F2");

	/* Monadic arithmetic: bit 15 selects the monadic table */
	check_text(0xEE008101, "MVFS F0, F1");
	check_text(0xEE108101, "MNFS F0, F1");
	check_text(0xEE208101, "ABSS F0, F1");
	check_text(0xEE408101, "SQTS F0, F1");

	/* Register transfer */
	check_text(0xEE003110, "FLTS F0, R3");
	check_text(0xEE102175, "FIXZ R2, F5");
	check_text(0xEE203110, "WFS R3");
	check_text(0xEE303110, "RFS R3");

	/* Compare: Rd is 15 and the load bit is set */
	check_text(0xEE91F112, "CMF F1, F2");
	check_text(0xEEB1F112, "CNF F1, F2");
	check_text(0xEED1F112, "CMFE F1, F2");

	printf("FPA classification\n");

	check_class(0xED910100, ARM_CLASS_FPA_LDST);
	check_class(0xEE010102, ARM_CLASS_FPA_DATA);
	check_class(0xEE003110, ARM_CLASS_FPA_REG);

	/* CP15 must not be mistaken for the FPA */
	check_class(0xEE110F10, ARM_CLASS_COPROC_REG);
}

/* ---- effects ------------------------------------------------------------- */

static void
test_calls_and_returns(void)
{
	ArmInsnInfo info;

	printf("Calls\n");

	/* BL is a call; a plain B is not, however it is written */
	arm_decode(0xEB000000, AT, &info);
	check("BL is a call", info.is_call);
	check("BL is a branch", info.is_branch);
	check("BL writes LR", (info.regs_written & 0x4000) != 0);
	check("BL target is address + 8", info.branch_target_known &&
	      info.branch_target == AT + 8);

	arm_decode(0xEA000000, AT, &info);
	check("B is not a call", !info.is_call);
	check("B is a branch", info.is_branch);
	check("B does not write LR", (info.regs_written & 0x4000) == 0);

	arm_decode(0x0B000000, AT, &info);
	check("BLEQ is a call", info.is_call);
	check("BLEQ is conditional", info.is_conditional);
	check("BL is not conditional", (arm_decode(0xEB000000, AT, &info),
	      !info.is_conditional));

	/* A backwards branch must sign extend */
	arm_decode(0xEAFFFFFE, AT, &info);
	check("B -8 targets itself", info.branch_target == AT);

	printf("Returns\n");

	arm_decode(0xE1A0F00E, AT, &info);
	check("MOV PC, LR is a return", info.is_return);
	check("MOV PC, LR writes PC", info.writes_pc);

	arm_decode(0xE12FFF1E, AT, &info);
	check("BX LR is a return", info.is_return);

	arm_decode(0xE8BD8010, AT, &info);
	check("LDMIA SP!, {R4, PC} is a return", info.is_return);
	check("LDM popping PC writes PC", info.writes_pc);

	/* Things that write PC but are not returns */
	arm_decode(0xE12FFF11, AT, &info);
	check("BX R1 is not a return", !info.is_return);
	check("BX R1 is a branch", info.is_branch);

	arm_decode(0xE1A0F001, AT, &info);
	check("MOV PC, R1 is not a return", !info.is_return);
	check("MOV PC, R1 writes PC", info.writes_pc);

	arm_decode(0xE8BD4010, AT, &info);
	check("LDMIA SP!, {R4, LR} is not a return", !info.is_return);

	/* Ordinary instructions transfer no control */
	arm_decode(0xE0812002, AT, &info);
	check("ADD is not a branch", !info.is_branch);
	check("ADD does not write PC", !info.writes_pc);
	check("ADD is not a return", !info.is_return);
}

static void
test_register_use(void)
{
	ArmInsnInfo info;

	printf("Register use\n");

	/* ADD R2, R1, R2 */
	arm_decode(0xE0812002, AT, &info);
	check("ADD writes R2 only", info.regs_written == (1u << 2));
	check("ADD reads R1 and R2", info.regs_read == ((1u << 1) | (1u << 2)));

	/* MOV R0, #1 reads nothing */
	arm_decode(0xE3A00001, AT, &info);
	check("MOV immediate reads nothing", info.regs_read == 0);
	check("MOV immediate writes R0", info.regs_written == (1u << 0));

	/* CMP writes nothing */
	arm_decode(0xE3500000, AT, &info);
	check("CMP writes nothing", info.regs_written == 0);
	check("CMP reads R0", info.regs_read == (1u << 0));

	/* A register-shifted operand reads the shift register too */
	arm_decode(0xE1A01312, AT, &info);
	check("MOV R1, R2, LSL R3 reads R2 and R3",
	      info.regs_read == ((1u << 2) | (1u << 3)));

	/* LDR R0, [R1] reads the base, writes the destination */
	arm_decode(0xE5910000, AT, &info);
	check("LDR reads base R1", (info.regs_read & (1u << 1)) != 0);
	check("LDR writes R0", (info.regs_written & (1u << 0)) != 0);
	check("LDR without writeback leaves R1 alone",
	      (info.regs_written & (1u << 1)) == 0);

	/* Writeback forms do write the base */
	arm_decode(0xE5B10004, AT, &info);
	check("LDR pre-indexed writeback writes R1",
	      (info.regs_written & (1u << 1)) != 0);
	arm_decode(0xE4910004, AT, &info);
	check("LDR post-indexed writes R1",
	      (info.regs_written & (1u << 1)) != 0);

	/* STR reads the value rather than writing it */
	arm_decode(0xE5810000, AT, &info);
	check("STR reads R0", (info.regs_read & (1u << 0)) != 0);
	check("STR does not write R0", (info.regs_written & (1u << 0)) == 0);

	/* STMDB SP!, {R4, LR} */
	arm_decode(0xE92D4010, AT, &info);
	check("STM reads its register list",
	      (info.regs_read & ((1u << 4) | (1u << 14))) == ((1u << 4) | (1u << 14)));
	check("STM with writeback writes SP",
	      (info.regs_written & (1u << 13)) != 0);

	/* MUL R0, R1, R2 */
	arm_decode(0xE0000291, AT, &info);
	check("MUL writes R0", info.regs_written == (1u << 0));
	check("MUL reads R1 and R2", info.regs_read == ((1u << 1) | (1u << 2)));

	/* MLA also reads the accumulator */
	arm_decode(0xE0203291, AT, &info);
	check("MLA reads R3 as well",
	      info.regs_read == ((1u << 1) | (1u << 2) | (1u << 3)));
}

static void
test_swi(void)
{
	ArmInsnInfo info;

	printf("SWI decoding\n");

	arm_decode(0xEF000011, AT, &info);
	check("SWI is flagged", info.is_swi);
	check("SWI number is the comment field", info.swi_number == 0x11);
	check("SWI name resolves", info.swi_name != NULL &&
	      strcmp(info.swi_name, "OS_Exit") == 0);

	/* The X form must resolve to the same name; the caller adds the X */
	arm_decode(0xEF020011, AT, &info);
	check("XSWI number keeps its X bit", info.swi_number == 0x20011);
	check("XSWI resolves to the base name", info.swi_name != NULL &&
	      strcmp(info.swi_name, "OS_Exit") == 0);

	/* An unallocated SWI has no name but is still a SWI */
	arm_decode(0xEF0FFFFF, AT, &info);
	check("unknown SWI is still a SWI", info.is_swi);
	check("unknown SWI has no name", info.swi_name == NULL);
}

static void
test_unknown(void)
{
	ArmInsnInfo info;
	int recognised;

	printf("Unrecognised encodings\n");

	/* NV is undefined on every core this emulator implements */
	recognised = arm_decode(0xF0000000, AT, &info);
	check("NV condition is not decoded", !recognised);
	check("NV yields ARM_CLASS_UNKNOWN", info.cls == ARM_CLASS_UNKNOWN);
	check("NV promises nothing about control flow",
	      !info.is_branch && !info.is_call && !info.is_return);
	check("NV promises nothing about registers",
	      info.regs_read == 0 && info.regs_written == 0);
	check_text(0xF0000000, "DCD 0xF0000000");

	/* A NULL output pointer must not be dereferenced */
	check("arm_decode(NULL) is rejected", arm_decode(0xE3A00001, AT, NULL) == 0);

	/* A NULL buffer must not be written */
	check("arm_disasm(NULL) is rejected",
	      arm_disasm(0xE3A00001, AT, NULL, 16) == NULL);
}

/* ---- symbol annotation --------------------------------------------------- */

static const char *
fake_lookup(uint32_t addr, uint32_t *offset, void *ctx)
{
	(void) ctx;

	if (addr >= 0x8000 && addr < 0x8100) {
		*offset = addr - 0x8000;
		return "main";
	}
	return NULL;
}

static void
test_symbol_annotation(void)
{
	char buf[256];

	printf("Symbol annotation\n");

	/* BL to 0x8008, which is main+8 */
	arm_disasm_sym(0xEB000000, AT, buf, sizeof(buf), fake_lookup, NULL);
	check("BL is annotated with symbol and offset",
	      strcmp(buf, "BL 0x00008008  ; main+0x8") == 0);
	if (strcmp(buf, "BL 0x00008008  ; main+0x8") != 0) {
		printf("      got \"%s\"\n", buf);
	}

	/* A branch landing exactly on the symbol carries no offset */
	arm_disasm_sym(0xEAFFFFFE, AT, buf, sizeof(buf), fake_lookup, NULL);
	check("branch to the symbol base has no offset",
	      strcmp(buf, "B 0x00008000  ; main") == 0);

	/* Outside the symbol, nothing is added */
	arm_disasm_sym(0xEA001000, AT, buf, sizeof(buf), fake_lookup, NULL);
	check("unresolved target is not annotated", strstr(buf, ";") == NULL);

	/* No resolver means no annotation, and the plain entry point must agree */
	arm_disasm_sym(0xEB000000, AT, buf, sizeof(buf), NULL, NULL);
	check("no resolver leaves the text alone", strcmp(buf, "BL 0x00008008") == 0);
}


/* ------------------------------------------------------ rendering options */

/**
 * Check an opcode's rendering with a given set of options in force, and put
 * the defaults back afterwards so no test can leak into the next.
 */
static void
check_opts(const ArmDisasmOptions *opts, uint32_t opcode, uint32_t at,
           const char *expected)
{
	char buf[256];
	char what[400];

	arm_disasm_set_options(opts);
	arm_disasm(opcode, at, buf, sizeof(buf));
	arm_disasm_set_options(NULL);

	snprintf(what, sizeof(what), "%08X -> \"%s\"", opcode, expected);
	check(what, strcmp(buf, expected) == 0);
	if (strcmp(buf, expected) != 0) {
		printf("      got \"%s\"\n", buf);
	}
}

/*
 * The four rendering options from discussion #223.
 *
 * WHY THESE ARE TESTED. Each one exists so a disassembly can be read against
 * assembler source without translating every line, and each is a different
 * way of writing the same decoded instruction. A bug here does not look like a
 * bug: it looks like the disassembler disagreeing with your own code, which is
 * exactly the doubt these were added to remove. The defaults are checked
 * alongside every option, because an option that cannot be turned off is as
 * bad as one that does not work.
 */
static void
test_rendering_options(void)
{
	ArmDisasmOptions opts;

	printf("Rendering options\n");

	/* Hex immediates: MOV R0, #128 the RISC OS way. */
	memset(&opts, 0, sizeof(opts));
	check_opts(NULL, 0xE3A00080, AT, "MOV R0, #128");
	opts.hex_immediates = 1;
	check_opts(&opts, 0xE3A00080, AT, "MOV R0, #&80");
	/* And an immediate offset on a load, which goes through the same helper. */
	check_opts(NULL, 0xE5D000FF, AT, "LDRB R0, [R0, #255]");
	check_opts(&opts, 0xE5D000FF, AT, "LDRB R0, [R0, #&FF]");

	/* APCS names. */
	memset(&opts, 0, sizeof(opts));
	opts.apcs_registers = 1;
	check_opts(NULL, 0xE1A0F00E, AT, "MOV PC, R14");
	check_opts(&opts, 0xE1A0F00E, AT, "MOV pc, lr");
	check_opts(&opts, 0xE0812003, AT, "ADD a3, a2, a4");

	/* Register lists as ranges. A run of two is left spelled out, being no
	   longer than the range that would replace it. */
	memset(&opts, 0, sizeof(opts));
	opts.collapse_reglists = 1;
	check_opts(NULL,  0xE92D409F, AT, "STMDB R13!, {R0, R1, R2, R3, R4, R7, R14}");
	check_opts(&opts, 0xE92D409F, AT, "STMDB R13!, {R0-R4, R7, R14}");
	check_opts(&opts, 0xE92D4003, AT, "STMDB R13!, {R0, R1, R14}");
	check_opts(&opts, 0xE92D0007, AT, "STMDB R13!, {R0-R2}");
	/* Every register, which is one range from end to end. */
	check_opts(&opts, 0xE92DFFFF, AT, "STMDB R13!, {R0-PC}");

	/* PC-relative loads resolved. R15 reads eight ahead, so a -120 offset
	   from 0x8000 reaches 0x7F90. */
	memset(&opts, 0, sizeof(opts));
	opts.resolve_pc_relative = 1;
	check_opts(NULL,  0xE51FB078, AT, "LDR R11, [PC, #-120]");
	check_opts(&opts, 0xE51FB078, AT, "LDR R11, &00007F90");
	check_opts(&opts, 0xE59FB004, AT, "LDR R11, &0000800C");
	/* Not off R15, so nothing to resolve. */
	check_opts(&opts, 0xE51CB078, AT, "LDR R11, [R12, #-120]");
	/* Writeback: the address moves, so the registers stay on show. */
	check_opts(&opts, 0xE53FB078, AT, "LDR R11, [PC, #-120]!");

	/* Hex notation is one setting for the whole line, not just the operands
	   that happen to be immediates: a branch target, a SWI number and an MSR
	   immediate all follow it. Mixing "&80" with "0x374C" in one disassembly
	   is the inconsistency this option exists to remove. */
	memset(&opts, 0, sizeof(opts));
	opts.hex_immediates = 1;
	check_opts(NULL,  0xEA00001C, AT, "B 0x00008078");
	check_opts(&opts, 0xEA00001C, AT, "B &00008078");
	/* A named SWI keeps its name whatever the notation - the name is the
	   useful part, and it is not a number to be reformatted. */
	check_opts(&opts, 0xEF000011, AT, "SWI OS_Exit");
	check_opts(NULL,  0xEF012345, AT, "SWI 0x012345");
	check_opts(&opts, 0xEF012345, AT, "SWI &012345");

	/* The options are independent, and combine. */
	memset(&opts, 0, sizeof(opts));
	opts.apcs_registers = 1;
	opts.collapse_reglists = 1;
	opts.hex_immediates = 1;
	check_opts(&opts, 0xE92D409F, AT, "STMDB sp!, {a1-v1, v4, lr}");

	/*
	 * Lower case, and the two things it must NOT touch.
	 *
	 * A SWI's name and a symbol annotation are identifiers belonging to
	 * somebody else: "os_writec" is not a lower-case rendering of "OS_WriteC",
	 * it is the wrong name. That is the whole difficulty of this option and
	 * the reason it was not done with the other three.
	 */
	memset(&opts, 0, sizeof(opts));
	opts.lowercase = 1;
	check_opts(&opts, 0xE3A00080, AT, "mov r0, #128");
	check_opts(&opts, 0xE92D409F, AT, "stmdb r13!, {r0, r1, r2, r3, r4, r7, r14}");
	check_opts(&opts, 0x0A00001C, AT, "beq 0x00008078");
	check_opts(&opts, 0xEF000011, AT, "swi OS_Exit");
	check_opts(&opts, 0xEF020011, AT, "swi XOS_Exit");
	check_opts(&opts, 0xEF012345, AT, "swi 0x012345");

	/* With hex as well, since the digits are ours to lower but the name is not. */
	opts.hex_immediates = 1;
	check_opts(&opts, 0xE3A000FF, AT, "mov r0, #&ff");
	check_opts(&opts, 0xEF000011, AT, "swi OS_Exit");

	/* And setting NULL restores every default. */
	arm_disasm_set_options(&opts);
	arm_disasm_set_options(NULL);
	memset(&opts, 0xff, sizeof(opts));
	arm_disasm_get_options(&opts);
	check("NULL puts every option back to its default",
	      opts.hex_immediates == 0 && opts.apcs_registers == 0 &&
	      opts.collapse_reglists == 0 && opts.resolve_pc_relative == 0);
}

/*
 * SWI names from a file.
 *
 * A debugger that says "SWI 0x042C40" where a module's own SWI should be is of
 * limited use, and the built-in table only knows RISC OS's own. Asked for in
 * discussion #223. A name from the file must beat the built-in table, so a
 * chunk can be renamed as well as added, and a malformed line must be skipped
 * rather than take the rest of the file with it.
 */
static void
test_user_swi_names(void)
{
	const char *path = "test-swi-names.csv";
	unsigned count = 0;
	const char *err;
	FILE *f;

	printf("SWI names from a file\n");

	check_text(0xEF042C40, "SWI 0x042C40");	/* a number, to begin with */

	f = fopen(path, "w");
	if (f == NULL) {
		check("could not write the test file", 0);
		return;
	}
	fputs("# a comment, and a blank line follow\n\n", f);
	fputs("0x42c40,ADFFS_Info\n", f);
	fputs("  273473 ,  ADFFS_Decimal  \n", f);   /* decimal, and the same number */
	fputs("not a number,ignored\n", f);          /* skipped, not fatal */
	fputs("0x11,MyOwnExit\n", f);                 /* beats the built-in table */
	fputs("\t# an indented comment\n", f);
	fputs("&42C42,ADFFS_Ampersand\n", f);        /* the way RISC OS writes hex */
	fclose(f);

	err = arm_disasm_load_swi_names(path, &count);
	check("the file loads", err == NULL);
	check("and reports how many names it took", count == 4);

	check_text(0xEF042C40, "SWI ADFFS_Info");
	check_text(0xEF042C41, "SWI ADFFS_Decimal");
	check_text(0xEF000011, "SWI MyOwnExit");

	/*
	 * "&62C40" is what RISC OS documents, module headers and assembler
	 * listings write, and strtoul() has never heard of it. The line was being
	 * skipped in silence - the file loaded, said so, and the name was simply
	 * not there.
	 */
	check_text(0xEF042C42, "SWI ADFFS_Ampersand");

	/*
	 * A file lists the chunk base, as a module header declares it; the code
	 * being read calls the X form. Matching only on the number as written
	 * meant the common case - reading a caller - found nothing.
	 */
	check_text(0xEF062C40, "SWI XADFFS_Info");
	check_text(0xEF062C42, "SWI XADFFS_Ampersand");

	arm_disasm_clear_swi_names();
	check_text(0xEF000011, "SWI OS_Exit");

	err = arm_disasm_load_swi_names("no-such-file-here.csv", &count);
	check("a missing file is reported, not fatal", err != NULL);

	remove(path);
}

int
main(void)
{
	printf("ARM disassembler\n\n");

	test_data_processing();
	test_transfers();
	test_branches();
	test_misc();
	test_fpa();
	test_calls_and_returns();
	test_register_use();
	test_swi();
	test_unknown();
	test_symbol_annotation();
	test_rendering_options();
	test_user_swi_names();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");

	return failures != 0;
}
