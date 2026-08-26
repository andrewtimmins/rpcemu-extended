/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * test_multifs_fat12.c - MultiFS's FAT12 entries, run as ARM code
 *
 * A FAT16 entry is two bytes and a FAT32 entry is four, so both divide into a
 * 512-byte sector and reading one is an aligned load of a known width. A FAT12
 * entry is a byte and a half: it shares one of its two bytes with the entry
 * beside it, which half it owns depends on whether its cluster number is odd,
 * and because 512 is not a multiple of three halves one entry in every 341 has
 * its two bytes in DIFFERENT SECTORS. Every one of those is a place to get the
 * arithmetic wrong, and getting it wrong on the write side does not produce a
 * wrong answer - it destroys the neighbouring entry, which is somebody else's
 * file, on a volume chkdsk may not be able to put back.
 *
 * Reading the assembler and agreeing with it is not evidence. So this loads the
 * assembled module into the emulator's own RAM and RUNS it: fat12_get,
 * fat12_put, and fat_next and fat_raw above them, executed instruction by
 * instruction by the same interpreter that runs a guest, against a FAT12 image
 * built here. Only read_sector and write_sector are replaced, by stubs that
 * move sectors to and from that image - the two routines whose bodies are not
 * what is being tested, and which would otherwise want SCSIFS and a USB stack.
 *
 * The answers are checked against a reference reader written a deliberately
 * different way: the assembler thinks in byte pairs and nibbles, and the
 * reference treats the FAT as a stream of 12-bit fields and moves one bit at a
 * time. Two formulations that disagree about nibble order cannot both pass, and
 * the reference itself was checked against a 1.44MB FAT12 volume formatted by
 * newfs_msdos and written by macOS's own msdos driver - pass its image as
 * argv[2] and the chains on it are walked with this same ARM code.
 *
 * The module is assembled here rather than taken from the committed image, so
 * what runs is the source in the tree. Registered only where the ARM binutils
 * are, which is the same condition tests/check-guest-modules.sh runs under.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "cp15.h"

extern int arm_exec(void);

/* Where things go in the emulated machine. RAM starts at 0x10000000 and the
   first bank is half of what mem_reset() is given, so 8MB here; everything
   below is inside that. */
#define RAM_BASE	0x10000000u
#define MODULE_BASE	0x10001000u
#define STACK_TOP	0x100f0000u
#define WS_BASE		0x10100000u
#define VOL_BASE	0x10180000u
#define HALT_ADDR	0x101f0000u
#define IMAGE_BASE	0x10200000u

/* The geometry of the image built below: a 1.44MB floppy, one sector to a
   cluster, which is what newfs_msdos produces for that size and what puts the
   most straddling entries inside the smallest volume. */
#define BPS		512
#define SPC		1
#define RESERVED	1
#define NFATS		2
#define SPF		9
#define ROOTENTS	512
#define TOTAL_SECTORS	2880
#define NCLUSTERS	2829	/* what newfs_msdos -F 12 -c 1 reports */
#define FAT_BYTES	(SPF * BPS)
#define IMAGE_BYTES	(TOTAL_SECTORS * BPS)

static int failures;

static void
check(int cond, const char *what)
{
	printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) {
		failures++;
	}
}

/* ------------------------------------------------------------------------- */
/* The reference: a FAT12 as a stream of 12-bit fields.                      */
/*                                                                           */
/* Field k occupies bits 12k to 12k+11, counting from the least significant   */
/* bit of the lowest byte upwards. Moving a bit at a time is slow and says    */
/* nothing about nibbles or byte pairs, which is the point: it cannot share a */
/* misconception with the code it is checking.                               */
/* ------------------------------------------------------------------------- */

static unsigned
ref_get(const uint8_t *fat, unsigned cluster)
{
	unsigned bit = cluster * 12;
	unsigned v = 0;
	unsigned i;

	for (i = 0; i < 12; i++) {
		unsigned b = bit + i;

		v |= ((fat[b >> 3] >> (b & 7)) & 1u) << i;
	}
	return v;
}

static void
ref_put(uint8_t *fat, unsigned cluster, unsigned value)
{
	unsigned bit = cluster * 12;
	unsigned i;

	for (i = 0; i < 12; i++) {
		unsigned b = bit + i;
		uint8_t mask = (uint8_t) (1u << (b & 7));

		if ((value >> i) & 1u) {
			fat[b >> 3] |= mask;
		} else {
			fat[b >> 3] = (uint8_t) (fat[b >> 3] & ~mask);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Symbols, out of the ELF the assembler just produced.                      */
/* ------------------------------------------------------------------------- */

#define MAX_SYMS 4096

static struct {
	char		name[64];
	uint32_t	value;
} syms[MAX_SYMS];
static int nsyms;

static int
load_symbols(const char *nmfile)
{
	char line[256];
	FILE *f = fopen(nmfile, "r");

	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL && nsyms < MAX_SYMS) {
		unsigned long value;
		char type;
		char name[64];

		if (sscanf(line, "%lx %c %63s", &value, &type, name) == 3) {
			syms[nsyms].value = (uint32_t) value;
			strncpy(syms[nsyms].name, name, sizeof(syms[nsyms].name) - 1);
			syms[nsyms].name[sizeof(syms[nsyms].name) - 1] = '\0';
			nsyms++;
		}
	}
	fclose(f);
	return nsyms > 0;
}

/** A symbol's value, or abort: a missing one means the source moved. */
static uint32_t
sym(const char *name)
{
	int i;

	for (i = 0; i < nsyms; i++) {
		if (strcmp(syms[i].name, name) == 0) {
			return syms[i].value;
		}
	}
	printf("FAILED: '%s' is not in the module's symbol table.\n", name);
	printf("        The FAT12 code has been renamed or removed; this test is\n");
	printf("        checking something that is no longer there.\n");
	exit(1);
}

/* ------------------------------------------------------------------------- */
/* Emulated memory, poked directly.                                          */
/* ------------------------------------------------------------------------- */

static uint8_t *
host_ptr(uint32_t addr)
{
	return ((uint8_t *) ram00) + ((addr - RAM_BASE) & mem_rammask);
}

/*
 * Point the emulated MMU at the first bank of RAM.
 *
 * Guest loads and stores go through a per-page table of host displacements
 * (see cp15.c), which a running guest fills in from its own page tables. There
 * are none here, so every page the module touches - its own code, the stack,
 * the workspace and the disc image - is mapped by hand. One displacement does
 * for all of them because the window is linear in ram00. Both privilege levels
 * are set because the maps are kept per level.
 */
static void
map_ram(uint32_t bytes)
{
	uintptr_t disp = (uintptr_t) ((uint8_t *) ram00) - (uintptr_t) RAM_BASE;
	uint32_t page;
	int m;

	for (page = 0; page < bytes >> 12; page++) {
		uint32_t addr = RAM_BASE + (page << 12);

		for (m = 0; m < 2; m++) {
			mem_read_map(m)[addr >> 12] = disp;
			mem_write_map(m)[addr >> 12] = disp;
		}
	}
}

static void
put_word(uint32_t addr, uint32_t val)
{
	uint8_t *p = host_ptr(addr);

	p[0] = (uint8_t) val;
	p[1] = (uint8_t) (val >> 8);
	p[2] = (uint8_t) (val >> 16);
	p[3] = (uint8_t) (val >> 24);
}

/* ------------------------------------------------------------------------- */
/* The sector stubs.                                                         */
/*                                                                           */
/* read_sector and write_sector take R0 = drive, R1 = sector, R2 = a 512-byte */
/* buffer, and set V on failure. These move the sector between the buffer and */
/* the image resident at IMAGE_BASE, preserve R0-R4 as the real routines do,  */
/* and clear V. Hand-assembled, and checked by calling them before anything   */
/* else is run - a wrong encoding fails there rather than being blamed on the */
/* code under test.                                                          */
/* ------------------------------------------------------------------------- */

static void
install_stub(uint32_t at, int writing)
{
	static const uint32_t code[] = {
		0xe92d401fu,	/* stmfd sp!, {r0-r4, lr}		*/
		0xe59f301cu,	/* ldr   r3, [pc, #28]  (IMAGE_BASE)	*/
		0xe0833481u,	/* add   r3, r3, r1, lsl #9		*/
		0xe3a04c02u,	/* mov   r4, #512			*/
		0xe4d30001u,	/* ldrb  r0, [r3], #1	 (read)		*/
		0xe4c20001u,	/* strb  r0, [r2], #1	 (read)		*/
		0xe2544001u,	/* subs  r4, r4, #1			*/
		0x1afffffbu,	/* bne   the ldrb			*/
		0xe35f0000u,	/* cmp   pc, #0		 (clears V)	*/
		0xe8bd801fu,	/* ldmfd sp!, {r0-r4, pc}		*/
		IMAGE_BASE,	/* the literal the ldr above reaches	*/
	};
	unsigned i;

	for (i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
		uint32_t insn = code[i];

		/* Writing runs the copy the other way: out of the caller's
		   buffer in R2 and into the image at R3. */
		if (writing && i == 4) {
			insn = 0xe4d20001u;	/* ldrb r0, [r2], #1 */
		}
		if (writing && i == 5) {
			insn = 0xe4c30001u;	/* strb r0, [r3], #1 */
		}
		put_word(at + i * 4, insn);
	}
}

/* ------------------------------------------------------------------------- */
/* Calling into the module.                                                  */
/* ------------------------------------------------------------------------- */

/** Run from `entry` until it returns to the halt instruction. */
static uint32_t
call_arm(uint32_t entry, uint32_t r0, uint32_t r1, uint32_t r2, int *vset)
{
	int i;

	resetcodeblocks();
	arm.reg[0] = r0;
	arm.reg[1] = r1;
	arm.reg[2] = r2;
	arm.reg[12] = WS_BASE;		/* wp */
	arm.reg[13] = STACK_TOP;	/* sp */
	arm.reg[14] = HALT_ADDR;	/* lr */
	arm.reg[15] = entry + 8;
	arm.event = 0;

	for (i = 0; i < 20000; i++) {
		if ((arm.reg[15] - 8) == HALT_ADDR) {
			break;
		}
		arm_exec();
	}
	if ((arm.reg[15] - 8) != HALT_ADDR) {
		printf("FAILED: call to 0x%08x did not return (pc=0x%08x)\n",
		       entry, arm.reg[15]);
		exit(1);
	}
	if (vset != NULL) {
		/* The flags live in arm.reg[cpsr]: which register that is depends on
		   whether the core is in 26- or 32-bit mode, so it is not hardcoded. */
		*vset = (arm.reg[cpsr] & VFLAG) ? 1 : 0;
	}
	return arm.reg[0];
}

int
main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "riscos-progs/MultiFS";
	const char *hostimage = (argc > 2) ? argv[2] : NULL;
	char tmpl[] = "/tmp/multifs-fat12-XXXXXX";
	char cmd[2048];
	char *tmp;
	const char *prefix = NULL;
	static const char *const prefixes[] = {
		"arm-linux-gnueabi-", "arm-none-eabi-", NULL
	};
	uint8_t *image;
	uint8_t *fat;
	uint8_t *modimage;
	long modsize;
	FILE *f;
	uint32_t vol;
	unsigned c;
	int i;
	int mismatches;

	/* Unbuffered, so what has been printed survives a crash in the emulated
	   code rather than being lost with the buffer. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Which ARM binutils. The same two, in the same order, as
	   tests/check-guest-modules.sh. */
	for (i = 0; prefixes[i] != NULL; i++) {
		snprintf(cmd, sizeof(cmd), "command -v %sas >/dev/null 2>&1", prefixes[i]);
		if (system(cmd) == 0) {
			prefix = prefixes[i];
			break;
		}
	}
	if (prefix == NULL) {
		printf("FAILED: no ARM binutils found. This test is registered only "
		       "where they exist, so\n        finding none here means the "
		       "build configured it wrongly.\n");
		return 1;
	}

	tmp = mkdtemp(tmpl);
	if (tmp == NULL) {
		printf("FAILED: cannot make a temporary directory\n");
		return 1;
	}

	/* Assemble out of a copy, so the test never writes into the tree. The copy
	   brings any object files with it and cp gives them all the same
	   timestamp, so make would be entitled to consider the object up to date
	   and hand back a module assembled from an older source. Hence the clean:
	   without it this test can pass against code that is no longer there. */
	snprintf(cmd, sizeof(cmd), "cp -R '%s'/. '%s'/ >/dev/null 2>&1", dir, tmp);
	if (system(cmd) != 0) {
		printf("FAILED: cannot copy '%s'\n", dir);
		return 1;
	}
	snprintf(cmd, sizeof(cmd),
	         "cd '%s' && make clean >/dev/null 2>&1 && "
	         "make multifs.elf AS=%sas LD=%sld OBJCOPY=%sobjcopy "
	         ">/dev/null 2>&1 && %snm multifs.elf > syms.txt 2>/dev/null && "
	         "%sobjcopy -O binary multifs.elf multifs.bin >/dev/null 2>&1",
	         tmp, prefix, prefix, prefix, prefix, prefix);
	if (system(cmd) != 0) {
		printf("FAILED: cannot assemble MultiFS in '%s'\n", tmp);
		return 1;
	}

	snprintf(cmd, sizeof(cmd), "%s/syms.txt", tmp);
	if (!load_symbols(cmd)) {
		printf("FAILED: no symbols read from '%s'\n", cmd);
		return 1;
	}
	printf("MultiFS assembled with %s* (%d symbols)\n", prefix, nsyms);

	snprintf(cmd, sizeof(cmd), "%s/multifs.bin", tmp);
	f = fopen(cmd, "rb");
	if (f == NULL) {
		printf("FAILED: cannot open '%s'\n", cmd);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	modsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	modimage = malloc((size_t) modsize);
	if (modimage == NULL || fread(modimage, 1, (size_t) modsize, f) != (size_t) modsize) {
		printf("FAILED: cannot read '%s'\n", cmd);
		return 1;
	}
	fclose(f);

	/* ------------------------------------------------------------------ */
	/* An emulated machine with nothing in it but RAM.                    */
	/* ------------------------------------------------------------------ */
	arm_init();
	mem_init();
	machine.model = Model_RPCARM710;
	machine.iomd_type = IOMDType_IOMD;
	mem_reset(16, 2);
	arm_reset(CPUModel_SA110);
	initcodeblocks();
	dcache = 0;			/* interpret: no recompiled blocks */
	updatemode(0x10 | SUPERVISOR);	/* 32-bit, privileged */
	map_ram(8 * 1024 * 1024);	/* the whole of SIMM 0 bank 0 */

	/* ------------------------------------------------------------------ */
	/* The image. A FAT of deliberately awkward entries: every straddling */
	/* cluster, its neighbours either side, values whose nibbles differ   */
	/* so an inverted pair cannot pass, and the reserved codes.          */
	/* ------------------------------------------------------------------ */
	image = calloc(1, IMAGE_BYTES);
	if (image == NULL) {
		printf("FAILED: out of memory\n");
		return 1;
	}
	fat = image + RESERVED * BPS;

	/* Entries 0 and 1 are the media descriptor and the end marker. */
	ref_put(fat, 0, 0xff0);
	ref_put(fat, 1, 0xfff);

	/* A chain per cluster, so no entry is left at its initial zero except
	   where a free cluster is wanted. The values are made from the cluster
	   number in a way that puts different nibbles in every position. */
	for (c = 2; c < NCLUSTERS + 2; c++) {
		unsigned v;

		if (c % 97 == 0) {
			v = 0;			/* free */
		} else if (c % 89 == 0) {
			v = 0xff7;		/* bad cluster */
		} else if (c % 83 == 0) {
			v = 0xfff;		/* end of chain */
		} else {
			v = ((c * 7u) ^ 0xa5cu) & 0xfffu;
			if (v == 0) {
				v = 0x123;
			}
		}
		ref_put(fat, c, v);
	}

	/* The second copy of the FAT is a copy, as it is on a real volume. */
	memcpy(image + (RESERVED + SPF) * BPS, fat, FAT_BYTES);

	memcpy(host_ptr(IMAGE_BASE), image, IMAGE_BYTES);
	memcpy(host_ptr(MODULE_BASE), modimage, (size_t) modsize);
	put_word(HALT_ADDR, 0xeafffffeu);	/* b . */

	install_stub(MODULE_BASE + sym("read_sector"), 0);
	install_stub(MODULE_BASE + sym("write_sector"), 1);

	/* The volume record and the workspace. */
	memset(host_ptr(WS_BASE), 0, sym("WS_SIZE"));
	memset(host_ptr(VOL_BASE), 0, sym("VOL_SIZE"));
	vol = VOL_BASE;
	put_word(vol + sym("VOL_TYPE"), 12);
	put_word(vol + sym("VOL_DRIVE"), 0);
	put_word(vol + sym("VOL_FATSEC0"), RESERVED);
	put_word(vol + sym("VOL_FATSZ"), SPF);
	put_word(vol + sym("VOL_NFATS"), NFATS);

	printf("\nThe sector stubs\n");
	{
		/* Read a sector that is not the first, so a wrong shift in the
		   stub cannot pass by reading sector 0 either way. */
		uint32_t buf = WS_BASE + sym("WS_SIZE") + 4096;
		int v;

		call_arm(MODULE_BASE + sym("read_sector"), 0, 7, buf, &v);
		check(v == 0, "read_sector returns without error");
		check(memcmp(host_ptr(buf), image + 7 * BPS, BPS) == 0,
		      "read_sector delivers the sector asked for");

		memset(host_ptr(buf), 0x5a, BPS);
		call_arm(MODULE_BASE + sym("write_sector"), 0, 2500, buf, &v);
		check(v == 0, "write_sector returns without error");
		check(memcmp(host_ptr(IMAGE_BASE + 2500 * BPS), host_ptr(buf), BPS) == 0,
		      "write_sector puts the sector where it belongs");
		/* Put the image back the way it was. */
		memcpy(host_ptr(IMAGE_BASE), image, IMAGE_BYTES);
		put_word(WS_BASE + sym("WS_FATSEC_LBA"), 0);
	}

	/* ------------------------------------------------------------------ */
	printf("\nfat12_get, every entry on the volume\n");
	mismatches = 0;
	for (c = 0; c < NCLUSTERS + 2; c++) {
		unsigned want = ref_get(fat, c);
		int v;
		uint32_t got = call_arm(MODULE_BASE + sym("fat12_get"), vol, c, 0, &v);

		if (v != 0 || got != want) {
			if (mismatches < 8) {
				printf("    cluster %4u: want &%03x, got &%03x%s\n",
				       c, want, got, v ? " (V set)" : "");
			}
			mismatches++;
		}
	}
	{
		char what[80];

		snprintf(what, sizeof(what), "all %d entries read back correctly",
		         NCLUSTERS + 2);
		check(mismatches == 0, what);
	}

	/* The straddling entries again, named, because they are the ones that
	   would fail on their own and a count does not say which. */
	printf("\nThe entries whose two bytes are in different sectors\n");
	{
		int straddlers = 0;

		for (c = 2; c < NCLUSTERS + 2; c++) {
			unsigned off = c + (c >> 1);

			if ((off & 511) != 511) {
				continue;
			}
			straddlers++;
			{
				char what[80];
				int v;
				uint32_t got = call_arm(MODULE_BASE + sym("fat12_get"),
				                        vol, c, 0, &v);

				snprintf(what, sizeof(what),
				         "cluster %u, bytes in sectors %u and %u",
				         c, RESERVED + (off >> 9),
				         RESERVED + ((off + 1) >> 9));
				check(v == 0 && got == ref_get(fat, c), what);
			}
		}
		check(straddlers >= 5, "the volume has straddling entries to check");
	}

	/* ------------------------------------------------------------------ */
	/* fat_next and fat_raw: the dispatch above the helper. fat_next turns */
	/* the reserved end-of-chain codes into zero and fat_raw does not.     */
	/* ------------------------------------------------------------------ */
	printf("\nfat_next and fat_raw on a FAT12 volume\n");
	{
		unsigned eoc = 0, chain = 0;
		int v;
		uint32_t got;

		for (c = 2; c < NCLUSTERS + 2 && (eoc == 0 || chain == 0); c++) {
			unsigned val = ref_get(fat, c);

			if (val >= 0xff8 && eoc == 0) {
				eoc = c;
			}
			if (val != 0 && val < 0xff8 && chain == 0) {
				chain = c;
			}
		}

		got = call_arm(MODULE_BASE + sym("fat_next"), vol, chain, 0, &v);
		check(v == 0 && got == ref_get(fat, chain),
		      "fat_next hands back an ordinary link unchanged");

		got = call_arm(MODULE_BASE + sym("fat_next"), vol, eoc, 0, &v);
		check(v == 0 && got == 0, "fat_next turns an end marker into zero");

		got = call_arm(MODULE_BASE + sym("fat_raw"), vol, eoc, 0, &v);
		check(v == 0 && got == ref_get(fat, eoc),
		      "fat_raw hands the end marker back as it stands");
	}

	/* ------------------------------------------------------------------ */
	/* The other formats must NOT reach any of this.                       */
	/*                                                                    */
	/* fat_next's exFAT case falls through to its exit, and fat_set's copy  */
	/* loop falls through to its own, so a FAT12 block put next to either   */
	/* becomes part of the path it was meant to sit beside - and the symptom */
	/* is a FAT32 volume having its entries rewritten twelve bits at a time. */
	/* Both happened while this was being written and neither showed up in   */
	/* the FAT12 checks above, so the other widths are checked here.        */
	/* ------------------------------------------------------------------ */
	printf("\nFAT16, FAT32 and exFAT do not take the FAT12 path\n");
	{
		static const unsigned widths[] = { 16, 32, 64 };
		static const char *const names[] = { "FAT16", "FAT32", "exFAT" };
		size_t w;

		for (w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
			char what[80];
			int v;
			unsigned cl = 5;
			uint32_t val = 0x123;
			uint8_t *before = malloc(IMAGE_BYTES);
			uint8_t *after = malloc(IMAGE_BYTES);
			int ok;

			if (before == NULL || after == NULL) {
				printf("FAILED: out of memory\n");
				return 1;
			}

			memcpy(host_ptr(IMAGE_BASE), image, IMAGE_BYTES);
			put_word(WS_BASE + sym("WS_FATSEC_LBA"), 0);
			put_word(vol + sym("VOL_TYPE"), widths[w]);
			memcpy(before, host_ptr(IMAGE_BASE), IMAGE_BYTES);

			call_arm(MODULE_BASE + sym("fat_set"), vol, cl, val, &v);
			memcpy(after, host_ptr(IMAGE_BASE), IMAGE_BYTES);

			/* Whatever the entry width is, cluster 5 sits at byte 10
			   for FAT16 and byte 20 for FAT32/exFAT - never at byte 7,
			   where a twelve-bit write would put it. So: the bytes a
			   write of this width owns may change, and no others. */
			{
				unsigned wide = (widths[w] == 16) ? 2 : 4;
				unsigned lo = cl * wide;
				unsigned hi = lo + wide;
				unsigned b;

				ok = 1;
				for (b = 0; b < FAT_BYTES; b++) {
					int changed = before[RESERVED * BPS + b] !=
					              after[RESERVED * BPS + b];

					if (changed && (b < lo || b >= hi)) {
						ok = 0;
					}
				}
			}

			snprintf(what, sizeof(what),
			         "%s: fat_set touches only its own %u-bit entry",
			         names[w], widths[w] == 16 ? 16 : 32);
			check(ok, what);

			/* And fat_next must read it back at that width, not as
			   twelve bits of the byte before it. */
			{
				uint32_t got = call_arm(MODULE_BASE + sym("fat_next"),
				                        vol, cl, 0, &v);

				snprintf(what, sizeof(what),
				         "%s: fat_next reads back what fat_set wrote",
				         names[w]);
				check(v == 0 && got == val, what);
			}

			free(before);
			free(after);
		}

		/* Back to FAT12 for anything after this. */
		put_word(vol + sym("VOL_TYPE"), 12);
		memcpy(host_ptr(IMAGE_BASE), image, IMAGE_BYTES);
		put_word(WS_BASE + sym("WS_FATSEC_LBA"), 0);
	}

	/* ------------------------------------------------------------------ */
	/* fat12_put. The entry has to change, both copies of the FAT have to  */
	/* change, and NOTHING else may - least of all the four bits of the    */
	/* neighbour sitting in the same byte.                                 */
	/* ------------------------------------------------------------------ */
	printf("\nfat12_put: the entry changes and nothing else does\n");
	{
		static const unsigned clusters[] = {
			2, 3, 4, 5,		/* both parities, low down */
			340, 341, 342,		/* either side of a straddler */
			681, 682, 683,
			1364, 1365, 1366,
			1705, 1706, 1707,
			2388, 2389, 2390,
			2727, 2728, 2729, 2730, 2731,
			2828			/* the last cluster */
		};
		static const unsigned values[] = { 0x000, 0x001, 0xfff, 0xabc, 0x123, 0x800, 0x07f };
		uint8_t *expect = malloc(IMAGE_BYTES);
		int bad_entry = 0, bad_neighbour = 0, bad_second = 0, bad_elsewhere = 0;
		size_t k, j;

		if (expect == NULL) {
			printf("FAILED: out of memory\n");
			return 1;
		}

		for (k = 0; k < sizeof(clusters) / sizeof(clusters[0]); k++) {
			for (j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
				unsigned cl = clusters[k];
				unsigned val = values[j];
				int v;
				uint8_t *got_fat;
				uint8_t *got_fat2;

				/* Start from a known image every time, so one
				   failure cannot hide behind another. */
				memcpy(host_ptr(IMAGE_BASE), image, IMAGE_BYTES);
				put_word(WS_BASE + sym("WS_FATSEC_LBA"), 0);

				/* What the image should look like afterwards. */
				memcpy(expect, image, IMAGE_BYTES);
				ref_put(expect + RESERVED * BPS, cl, val);
				memcpy(expect + (RESERVED + SPF) * BPS,
				       expect + RESERVED * BPS, FAT_BYTES);

				call_arm(MODULE_BASE + sym("fat12_put"), vol, cl, val, &v);
				if (v != 0) {
					bad_entry++;
					continue;
				}

				got_fat = host_ptr(IMAGE_BASE) + RESERVED * BPS;
				got_fat2 = host_ptr(IMAGE_BASE) + (RESERVED + SPF) * BPS;

				if (ref_get(got_fat, cl) != val) {
					if (bad_entry < 4) {
						printf("    cluster %u := &%03x read back as &%03x\n",
						       cl, val, ref_get(got_fat, cl));
					}
					bad_entry++;
				}
				if (cl > 0 && ref_get(got_fat, cl - 1) != ref_get(fat, cl - 1)) {
					if (bad_neighbour < 4) {
						printf("    cluster %u := &%03x damaged %u: "
						       "&%03x became &%03x\n", cl, val, cl - 1,
						       ref_get(fat, cl - 1),
						       ref_get(got_fat, cl - 1));
					}
					bad_neighbour++;
				}
				if (ref_get(got_fat, cl + 1) != ref_get(fat, cl + 1)) {
					if (bad_neighbour < 4) {
						printf("    cluster %u := &%03x damaged %u: "
						       "&%03x became &%03x\n", cl, val, cl + 1,
						       ref_get(fat, cl + 1),
						       ref_get(got_fat, cl + 1));
					}
					bad_neighbour++;
				}
				if (memcmp(got_fat2, got_fat, FAT_BYTES) != 0) {
					bad_second++;
				}
				if (memcmp(host_ptr(IMAGE_BASE), expect, IMAGE_BYTES) != 0) {
					bad_elsewhere++;
				}
			}
		}

		check(bad_entry == 0, "the entry written is the entry read back");
		check(bad_neighbour == 0, "the entries either side are untouched");
		check(bad_second == 0, "the second copy of the FAT is written too");
		check(bad_elsewhere == 0, "not one other byte of the volume moved");
		free(expect);
	}

	/* ------------------------------------------------------------------ */
	/* A volume from somewhere else entirely, if one was handed over.      */
	/* ------------------------------------------------------------------ */
	if (hostimage != NULL) {
		printf("\nA real FAT12 volume: %s\n", hostimage);
		f = fopen(hostimage, "rb");
		if (f == NULL) {
			printf("FAILED: cannot open '%s'\n", hostimage);
			return 1;
		}
		{
			uint8_t *real = calloc(1, IMAGE_BYTES);
			size_t n = fread(real, 1, IMAGE_BYTES, f);
			unsigned res, spf, nclus, root_sec, rootents, spc, files = 0;
			unsigned chains_ok = 0, chains_bad = 0;
			uint8_t *rfat;

			fclose(f);
			check(n >= 512, "the image was read");

			res = (unsigned) real[14] | ((unsigned) real[15] << 8);
			spf = (unsigned) real[22] | ((unsigned) real[23] << 8);
			spc = real[13];
			rootents = (unsigned) real[17] | ((unsigned) real[18] << 8);
			nclus = (((unsigned) real[19] | ((unsigned) real[20] << 8))
			         - res - spf * real[16] - (rootents * 32 + 511) / 512) / spc;
			root_sec = res + spf * real[16];
			rfat = real + res * BPS;

			printf("  %u clusters, %u sectors a cluster, FAT at %u for %u\n",
			       nclus, spc, res, spf);

			/* Point the module at this volume instead. */
			memcpy(host_ptr(IMAGE_BASE), real, IMAGE_BYTES);
			put_word(WS_BASE + sym("WS_FATSEC_LBA"), 0);
			put_word(vol + sym("VOL_FATSEC0"), res);
			put_word(vol + sym("VOL_FATSZ"), spf);
			put_word(vol + sym("VOL_NFATS"), real[16]);

			/* Every entry, against the reference. */
			mismatches = 0;
			for (c = 0; c < nclus + 2; c++) {
				int v;
				uint32_t got = call_arm(MODULE_BASE + sym("fat12_get"), vol, c, 0, &v);

				if (v != 0 || got != ref_get(rfat, c)) {
					mismatches++;
				}
			}
			check(mismatches == 0, "every entry agrees with the reference");

			/* And the part no reference of mine can fake: each file's
			   chain, followed with fat_next, has to be exactly as long
			   as the size in its directory entry says. */
			for (i = 0; i < (int) rootents; i++) {
				const uint8_t *e = real + root_sec * BPS + i * 32;
				unsigned start, size, want, len = 0, cl;

				if (e[0] == 0x00) {
					break;
				}
				if (e[0] == 0xe5 || (e[11] & 0x0f) == 0x0f || (e[11] & 0x18) != 0) {
					continue;	/* deleted, a long name, or not a file */
				}
				start = (unsigned) e[26] | ((unsigned) e[27] << 8);
				size = (unsigned) e[28] | ((unsigned) e[29] << 8) |
				       ((unsigned) e[30] << 16) | ((unsigned) e[31] << 24);
				if (start < 2 || size == 0) {
					continue;
				}
				want = (size + spc * BPS - 1) / (spc * BPS);

				cl = start;
				while (cl >= 2 && cl < nclus + 2 && len <= want + 2) {
					int v;

					len++;
					cl = call_arm(MODULE_BASE + sym("fat_next"), vol, cl, 0, &v);
					if (v != 0) {
						len = 0;
						break;
					}
					if (cl == 0) {
						break;	/* end of chain */
					}
				}
				files++;
				if (len == want) {
					chains_ok++;
				} else {
					chains_bad++;
					printf("    %.11s: %u bytes wants %u clusters, "
					       "the chain from %u is %u long\n",
					       (const char *) e, size, want, start, len);
				}
			}
			printf("  %u files in the root directory, %u chains followed\n",
			       files, chains_ok);
			check(files > 0, "there were files to follow");
			check(chains_bad == 0,
			      "every file's chain is as long as its size demands");
			free(real);
		}
	}

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	(void) system(cmd);
	free(image);
	free(modimage);

	printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
