/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

//#define isblockvalid(l) (((l)&0xFFC00000)==0x3800000)
#define isblockvalid(l) (dcache)

#define BLOCKS 1024

extern uint8_t rcodeblock[BLOCKS][1792+512+64];
extern uint32_t codeblockpc[0x8000];
extern int codeblocknum[0x8000];
/*
 * Which CPU word size each cached block was compiled for: 1 for 32-bit program
 * mode, 0 for 26-bit.
 *
 * The generators bake arm.r15_mask into what they emit, and use it at
 * generation time to decide whether to mask R15 at all; they also bake in the
 * flags-register pointer, which moves between R15 and R16 with the word size.
 * A block compiled in one word size is therefore wrong in the other - issue
 * #154, where a PC-relative LDR under a 26-bit OS computed an address that
 * still had the flags in it. So a block is only reused when this matches
 * jit_word32.
 *
 * It used to be handled by throwing the entire cache away on a word-size
 * change, and that was ruinous. RISC OS 3.7 and 4.x set PROG32 in the CP15
 * control register while running in 26-bit modes, so the CPU changes word size
 * twice per SWI: measured at some 63,000 round trips a second on RISC OS 3.71,
 * which emptied the cache 126,000 times a second and left the recompiler
 * compiling 3.7 million blocks a second only to discard them. Checking one byte
 * instead leaves both sets of blocks in place. That is issue #181.
 */
extern uint8_t codeblockword[0x8000];
extern int jit_word32;

extern uint8_t flaglookup[16][16];

#define BLOCKSTART 16
//uint32_t blocks[1024];

#define HASH(l) (((l)>>2)&0x7FFF)
//#define callblock(l) (((codeblockpc[0][HASH(l)]==l)||(codeblockpc[1][HASH(l)]==l))?codecallblock(l):0)
