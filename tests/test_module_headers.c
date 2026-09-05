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

/**
 * Validate the guest module images this fork assembles itself.
 *
 * Every fault this checks for was found the hard way, by booting a guest and
 * watching it fail, because a malformed module is silent at assembly time and
 * mostly silent at load time too. The worst of them took four other modules
 * down with it: RISC OS abandons the whole expansion-card scan when a module's
 * initialisation returns to a bad address, so a boot lost RPCEmuCoPro,
 * RPCEmuUSBSupport, RPCEmuHostFS, SyncClock and the graphics card's RPCEmuGfx
 * and reported one prefetch abort by way of explanation.
 *
 * What is checked, and why each one is here:
 *
 *  - Every offset in the module header points inside the image. A stray one is
 *    a wild branch or a string read from nowhere.
 *
 *  - The module flags word exists and says 32-bit compatible. RISC OS 5 will
 *    not run a module that does not claim it.
 *
 *  - No 26-bit return idiom appears anywhere in the code. "ORR LR,LR,#V_bit"
 *    and "BIC LR,LR,#V_bit" followed by "MOV PC,LR" were how a 26-bit module
 *    set and cleared V as it returned, because the flags lived in the top bits
 *    of R15; in 32-bit code they corrupt bit 28 OF THE RETURN ADDRESS. That is
 *    the fault above: &fc00dce8 became &ec00dce8. "MOVS PC,LR" is the same
 *    era's other habit and copies SPSR into CPSR, which is an exception return
 *    and not a subroutine return at all.
 *
 *  - Each command table entry's information word has its minimum no greater
 *    than its maximum. The kernel reads bits 0-7 as the MINIMUM number of
 *    parameters and bits 16-23 as the MAXIMUM (Oscli: "MOV R6, R4, LSR #16 ;
 *    max no parms" then "AND R4, R4, #&ff ; min no parms"), which is the
 *    opposite of how it reads left to right. Written the other way round, a
 *    command asks for a minimum above its maximum, no argument count can ever
 *    satisfy it, and RISC OS prints the syntax message instead of ever calling
 *    the command's code.
 *
 *  - Each entry's syntax and help pointers are non-zero and inside the image.
 *    These two words were once the other way round, which put the syntax
 *    string where *Help looks and left a null pointer where RISC OS reads the
 *    syntax message.
 *
 * WHAT IT CANNOT SEE. Only modules with an assembler source in riscos-progs/ are
 * checked, because the instruction scan needs the image to be plain ARM code.
 * SharedClipboard is built with the DDE inside a guest and is modsqz'd, so it is
 * compressed and scanning it for instruction patterns would be looking at the
 * wrong bytes; it is covered by nothing here. Neither are the ROM images in
 * usbroms/ that come from RISC OS Open rather than from us. And nothing here
 * says a module works - only that these specific structural mistakes are absent.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_HEADER_WORDS	13	/* Start .. Module Flags */

/* Header word indices, from the PRM's module header layout. */
#define HDR_START		0
#define HDR_INIT		1
#define HDR_FINAL		2
#define HDR_SERVICE		3
#define HDR_TITLE		4
#define HDR_HELP		5
#define HDR_COMMANDS		6
#define HDR_SWI_CHUNK		7
#define HDR_SWI_HANDLER		8
#define HDR_SWI_TABLE		9
#define HDR_SWI_CODE		10
#define HDR_MESSAGES		11
#define HDR_FLAGS		12

/* The instruction words a 32-bit module must never contain. */
#define INSN_MOVS_PC_LR		0xe1b0f00eu	/* MOVS PC, LR              */
#define INSN_ORR_LR_V		0xe38ee201u	/* ORR LR, LR, #1 << 28     */
#define INSN_BIC_LR_V		0xe3cee201u	/* BIC LR, LR, #1 << 28     */

/* Information word fields, as the kernel's Oscli reads them. */
#define INFO_MIN(w)		((w) & 0xffu)
#define INFO_MAX(w)		(((w) >> 16) & 0xffu)
#define INFO_CONFIGURE		0x40000000u
#define INFO_STATUS		0x80000000u

static int failures = 0;

static void
fail(const char *module, const char *fmt, ...)
{
	va_list ap;

	printf("FAIL %s: ", module);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

static uint32_t
word_at(const uint8_t *image, size_t offset)
{
	return (uint32_t) image[offset]
	     | ((uint32_t) image[offset + 1] << 8)
	     | ((uint32_t) image[offset + 2] << 16)
	     | ((uint32_t) image[offset + 3] << 24);
}

/**
 * Read a whole file, or return NULL. *len is only valid on success.
 */
static uint8_t *
read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long size;

	if (f == NULL) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc((size_t) size);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t) size, f) != (size_t) size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t) size;
	return buf;
}

/**
 * An offset of zero means "not present" for every header field, so only a
 * non-zero one is checked. A field pointing at the very last byte is still
 * wrong for anything read as a word, hence the size argument.
 */
static void
check_offset(const char *module, const char *what, uint32_t offset,
             size_t image_len, size_t size)
{
	if (offset == 0) {
		return;
	}
	if ((size_t) offset + size > image_len) {
		fail(module, "%s offset &%x is outside the %zu-byte image",
		     what, offset, image_len);
	}
}

/**
 * Does the zero-terminated string at this offset begin "Syntax:"? That prefix
 * is what tells a syntax message from a help text, and hence tells whether the
 * command table's last two words are the right way round.
 */
static int
starts_with_syntax(const uint8_t *image, size_t len, uint32_t offset)
{
	static const char prefix[] = "Syntax:";
	size_t i;

	for (i = 0; i < sizeof(prefix) - 1; i++) {
		if ((size_t) offset + i >= len) {
			return 0;
		}
		if (image[offset + i] != (uint8_t) prefix[i]) {
			return 0;
		}
	}
	return 1;
}

static void
check_header(const char *module, const uint8_t *image, size_t len)
{
	uint32_t flags_offset;

	if (len < MODULE_HEADER_WORDS * 4) {
		fail(module, "only %zu bytes: too short for a module header", len);
		return;
	}

	check_offset(module, "Start", word_at(image, HDR_START * 4), len, 4);
	check_offset(module, "Initialisation", word_at(image, HDR_INIT * 4), len, 4);
	check_offset(module, "Finalisation", word_at(image, HDR_FINAL * 4), len, 4);
	check_offset(module, "Service call", word_at(image, HDR_SERVICE * 4), len, 4);
	check_offset(module, "Title", word_at(image, HDR_TITLE * 4), len, 1);
	check_offset(module, "Help", word_at(image, HDR_HELP * 4), len, 1);
	check_offset(module, "Command table", word_at(image, HDR_COMMANDS * 4), len, 1);
	check_offset(module, "SWI handler", word_at(image, HDR_SWI_HANDLER * 4), len, 4);
	check_offset(module, "SWI decoding table", word_at(image, HDR_SWI_TABLE * 4), len, 1);
	check_offset(module, "SWI decoding code", word_at(image, HDR_SWI_CODE * 4), len, 4);
	check_offset(module, "Messages", word_at(image, HDR_MESSAGES * 4), len, 1);

	/* The flags word is the last header field and is what says 32-bit
	   compatible. A module without it does not run on RISC OS 5 at all. */
	flags_offset = word_at(image, HDR_FLAGS * 4);
	if (flags_offset == 0) {
		fail(module, "no module flags word, so it does not claim to be "
		             "32-bit compatible");
	} else if ((size_t) flags_offset + 4 > len) {
		fail(module, "module flags offset &%x is outside the %zu-byte image",
		     flags_offset, len);
	} else if ((word_at(image, flags_offset) & 1u) == 0) {
		fail(module, "module flags &%x does not set bit 0 (32-bit compatible)",
		     word_at(image, flags_offset));
	}
}

static void
check_no_26bit_returns(const char *module, const uint8_t *image, size_t len)
{
	size_t offset;

	for (offset = 0; offset + 4 <= len; offset += 4) {
		const uint32_t insn = word_at(image, offset);
		const char *what = NULL;

		if (insn == INSN_MOVS_PC_LR) {
			what = "MOVS PC, LR, which copies SPSR into CPSR";
		} else if (insn == INSN_ORR_LR_V) {
			what = "ORR LR, LR, #V_bit, which corrupts bit 28 of the "
			       "return address";
		} else if (insn == INSN_BIC_LR_V) {
			what = "BIC LR, LR, #V_bit, which corrupts bit 28 of the "
			       "return address";
		}
		if (what != NULL) {
			fail(module, "26-bit return idiom at &%zx: %s", offset, what);
		}
	}
}

/**
 * Walk the help and command keyword table. Each entry is a zero-terminated
 * name padded to a word boundary, then four words: the code offset, the
 * information word, the syntax message offset and the help text offset. A zero
 * byte where a name would start ends the table.
 */
static void
check_command_table(const char *module, const uint8_t *image, size_t len)
{
	uint32_t table = word_at(image, HDR_COMMANDS * 4);
	size_t offset;

	if (table == 0) {
		return;		/* no commands, which is legitimate */
	}
	offset = table;

	while (offset < len && image[offset] != 0) {
		char name[64];
		size_t name_len = 0;
		uint32_t info, syntax, help;

		while (offset + name_len < len && image[offset + name_len] != 0) {
			if (name_len < sizeof(name) - 1) {
				name[name_len] = (char) image[offset + name_len];
			}
			name_len++;
		}
		name[name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1] = '\0';

		/* Step past the name and its terminator, then round up. */
		offset += name_len + 1;
		offset = (offset + 3) & ~(size_t) 3;

		if (offset + 16 > len) {
			fail(module, "command '%s' entry runs past the end of the image",
			     name);
			return;
		}

		info   = word_at(image, offset + 4);
		syntax = word_at(image, offset + 8);
		help   = word_at(image, offset + 12);

		/* A configure or status keyword's word means something else. */
		if ((info & (INFO_CONFIGURE | INFO_STATUS)) == 0
		    && INFO_MIN(info) > INFO_MAX(info)) {
			fail(module, "command '%s' information word &%08x asks for a "
			             "minimum of %u parameters and a maximum of %u, so "
			             "no argument count can satisfy it",
			     name, info, INFO_MIN(info), INFO_MAX(info));
		}

		/* Either may legitimately be absent; several modules here put
		   their syntax line inside the help text and leave the syntax
		   word zero. What is checked is that they are not the wrong way
		   round, which is invisible otherwise: both words are then
		   non-zero and both point somewhere valid. A syntax message
		   begins "Syntax:" and a help text does not. */
		check_offset(module, "syntax message", syntax, len, 1);
		check_offset(module, "help text", help, len, 1);

		if (syntax != 0 && syntax < len
		    && !starts_with_syntax(image, len, syntax)) {
			fail(module, "command '%s' syntax message does not begin "
			             "\"Syntax:\", so it is probably the help text and "
			             "the two words are the wrong way round", name);
		}
		if (help != 0 && help < len
		    && starts_with_syntax(image, len, help)) {
			fail(module, "command '%s' help text begins \"Syntax:\", so it "
			             "is probably the syntax message and the two words "
			             "are the wrong way round", name);
		}

		offset += 16;
	}
}

int
main(int argc, char *argv[])
{
	/* Module image, and the GNU as source whose presence says the image is
	   plain ARM code this fork assembles rather than a compressed or
	   compiler-built one. */
	static const struct {
		const char *image;
		const char *source;
	} modules[] = {
		{ "poduleroms/hostfs,ffa",
		  "riscos-progs/HostFS/hostfs.s" },
		{ "poduleroms/hostfsfiler,ffa",
		  "riscos-progs/HostFS/hostfsfiler.s" },
		{ "poduleroms/rpcemucopro,ffa",
		  "riscos-progs/RPCEmuCoPro/rpcemucopro.s" },
		{ "poduleroms/rpcemupciemulator,ffa",
		  "riscos-progs/RPCEmuPCIEmulator/rpcemupciemulator.s" },
		{ "poduleroms/rpcemusupport,ffa",
		  "riscos-progs/RPCEmuSupport/rpcemusupport.s" },
		{ "poduleroms/rpcemuusbsupport,ffa",
		  "riscos-progs/RPCEmuUSBSupport/rpcemuusbsupport.s" },
		{ "poduleroms/syncclock,ffa",
		  "riscos-progs/SyncClock/syncclock.s" },
		{ "poduleroms/scrollwheel,ffa",
		  "riscos-progs/ScrollWheel/scrollwheel.s" },
		{ "gfxroms/RPCEmuGfx,ffa",
		  "riscos-progs/RPCEmuGfx/rpcemugfx.s" },
		{ "netroms/EtherRPCEm,ffa",
		  "riscos-progs/EtherRPCEm/etherrpcem.s" },
		/* MultiFS is a submodule (riscos-multifs). With it not checked
		   out these two skip, which the source gate below already does
		   correctly - the skip is expected there, not a fault. */
		{ "usbroms/70-multifs,ffa",
		  "riscos-progs/MultiFS/multifs.s" },
		{ "usbroms/80-multifsfiler,ffa",
		  "riscos-progs/MultiFS/multifsfiler.s" },
	};
	const char *root;
	size_t i;
	int checked = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <repository root>\n", argv[0]);
		return 2;
	}
	root = argv[1];

	for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
		char path[1024];
		uint8_t *image;
		size_t len;

		/* The source is the gate: no source, not ours to check. */
		if (snprintf(path, sizeof(path), "%s/%s", root, modules[i].source)
		    >= (int) sizeof(path)) {
			fprintf(stderr, "path too long: %s\n", modules[i].source);
			return 2;
		}
		if (read_file(path, &len) == NULL) {
			printf("skip %s: no GNU as source at %s\n",
			       modules[i].image, modules[i].source);
			continue;
		}

		if (snprintf(path, sizeof(path), "%s/%s", root, modules[i].image)
		    >= (int) sizeof(path)) {
			fprintf(stderr, "path too long: %s\n", modules[i].image);
			return 2;
		}
		image = read_file(path, &len);
		if (image == NULL) {
			fail(modules[i].image, "cannot be read at %s", path);
			continue;
		}

		check_header(modules[i].image, image, len);
		check_no_26bit_returns(modules[i].image, image, len);
		check_command_table(modules[i].image, image, len);
		free(image);
		checked++;
	}

	/* A run that checked nothing passes every assertion it never made. */
	if (checked == 0) {
		printf("FAIL: no module images were checked\n");
		failures++;
	}

	if (failures != 0) {
		printf("%d failure(s) across %d module(s)\n", failures, checked);
		return 1;
	}
	printf("all %d module image(s) pass\n", checked);
	return 0;
}
