/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2026 Andy Timmins

  The AArch64 (arm64) dynarec backend is new code by Andy Timmins, based on the
  RPCEmu dynamic recompiler and its x86/amd64 code generators by Sarah Walker.

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
 * AArch64 (arm64) dynarec backend - shared declarations.
 *
 * See docs/arm64-dynarec.md for the overall design. This mirrors the
 * block model used by codegen_amd64.h / codegen_x86.h so the target-independent
 * front-end (arm_dynarec.c) is unchanged.
 */

#ifndef CODEGEN_ARM64_H
#define CODEGEN_ARM64_H

#define isblockvalid(l)	(dcache)

#define BLOCKS 1024

#if defined(__APPLE__) && defined(__aarch64__)
/* Apple Silicon: the code cache is a MAP_JIT allocation (see codegen_arm64.c),
   so rcodeblock is a pointer to it rather than a static array. */
extern uint8_t (*rcodeblock)[1792];
#else
extern uint8_t rcodeblock[BLOCKS][1792];
#endif
extern uint32_t codeblockpc[0x8000];
extern int codeblocknum[0x8000];

extern uint8_t flaglookup[16][16];

/* A block is entered as void (*)(void) at offset BLOCKSTART. The epilogue lives
   at offset 0 (branched to on exit); the prologue is at BLOCKSTART and falls
   into the body. AArch64 instructions are 4 bytes, so BLOCKSTART must be a
   multiple of 4 and leave room for the epilogue (5 instructions = 20 bytes). */
#define BLOCKSTART 32

#define HASH(l) (((l)>>2)&0x7FFF)

#endif /* CODEGEN_ARM64_H */
