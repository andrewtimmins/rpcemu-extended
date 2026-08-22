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
 * Instruction timings for the 6502, the Z80 and the 6809, against published
 * values.
 *
 * ★ WHY THESE ARE WORTH A TEST OF THEIR OWN. A program emulating a machine paces
 * its display and its sound from the processor's clock: a PAL C64 raster line is
 * 63 cycles, a Spectrum frame is 69888 T-states. A table of 256 numbers written
 * from memory has mistakes in it, and a mistake here is a display that drifts
 * rather than anything that looks like a bug.
 *
 * The tables in the cores were derived mechanically - the 6502's from the
 * addressing mode each opcode uses, the Z80's from the memory accesses the core
 * actually makes, the 6809's from which column of its opcode grid an instruction
 * sits in - and the figures below are the independent half of that: values from
 * the published timings, checked against what the core charges.
 *
 * ★ The 6809 needs this more than the others, not less, because a derivation
 * from the grid is exactly the kind of rule that is right about 250 opcodes and
 * wrong about the six that are exceptions to it. Its indexed instructions are
 * also the only ones here whose cost is not a property of the opcode at all: the
 * postbyte that follows decides, so the same LDA costs anything from four cycles
 * to eleven.
 *
 * ★ AND BOTH POLARITIES OF EVERY CONDITIONAL, which is not padding. The first
 * version of the Z80 table was derived from a probe run whose flags came from a
 * reset, where they are all ones, so the Z-set conditionals were measured taken
 * and the NZ ones not taken. Half the table was wrong and a sample that tested
 * one polarity of each agreed with it.
 */

#include "cpu_6502.h"
#include "cpu_6809.h"
#include "cpu_z80.h"

#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t ram[65536];

static void
check(const char *what, int got, int want)
{
	if (got != want) {
		printf("  FAIL %-32s got %d, published %d\n", what, got, want);
		failures++;
	}
}

/* ------------------------------------------------------------------ 6502 */

static void
m6502(const char *what, const uint8_t *code, size_t len, int want,
      void (*setup)(cpu6502_state *))
{
	cpu6502_state c;

	memset(ram, 0, sizeof(ram));
	memcpy(ram + 0x200, code, len);
	memset(&c, 0, sizeof(c));
	cpu6502_init(&c, ram, sizeof(ram));
	cpu6502_reset(&c, 0x200);
	if (setup != NULL) {
		setup(&c);
	}
	check(what, cpu6502_step(&c), want);
}

static void m6502_x_ff(cpu6502_state *c) { c->x = 0xff; }
static void m6502_y_ff(cpu6502_state *c) { c->y = 0xff; }
static void m6502_no_carry(cpu6502_state *c) { c->p &= (uint8_t) ~CPU6502_FLAG_C; }

static void
test_6502(void)
{
	printf("6502, against the published timings:\n");
	{ const uint8_t p[] = {0xa9,0x00};      m6502("LDA #n", p, 2, 2, NULL); }
	{ const uint8_t p[] = {0xa5,0x10};      m6502("LDA zp", p, 2, 3, NULL); }
	{ const uint8_t p[] = {0xb5,0x10};      m6502("LDA zp,X", p, 2, 4, NULL); }
	{ const uint8_t p[] = {0xad,0x00,0x30}; m6502("LDA abs", p, 3, 4, NULL); }
	{ const uint8_t p[] = {0xbd,0x00,0x30}; m6502("LDA abs,X, no carry", p, 3, 4, NULL); }
	{ const uint8_t p[] = {0xbd,0x01,0x30}; m6502("LDA abs,X, carrying", p, 3, 5, m6502_x_ff); }
	{ const uint8_t p[] = {0x9d,0x01,0x30}; m6502("STA abs,X is fixed at 5", p, 3, 5, m6502_x_ff); }
	{ const uint8_t p[] = {0xb9,0x01,0x30}; m6502("LDA abs,Y, carrying", p, 3, 5, m6502_y_ff); }
	{ const uint8_t p[] = {0xa1,0x10};      m6502("LDA (zp,X)", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0xb1,0x10};      m6502("LDA (zp),Y, no carry", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x91,0x10};      m6502("STA (zp),Y is fixed at 6", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0xe6,0x10};      m6502("INC zp", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0xee,0x00,0x30}; m6502("INC abs", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0xfe,0x00,0x30}; m6502("INC abs,X", p, 3, 7, NULL); }
	{ const uint8_t p[] = {0x4c,0x00,0x30}; m6502("JMP abs", p, 3, 3, NULL); }
	{ const uint8_t p[] = {0x6c,0x00,0x30}; m6502("JMP (abs)", p, 3, 5, NULL); }
	{ const uint8_t p[] = {0x20,0x00,0x30}; m6502("JSR abs", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0x60};           m6502("RTS", p, 1, 6, NULL); }
	{ const uint8_t p[] = {0x48};           m6502("PHA", p, 1, 3, NULL); }
	{ const uint8_t p[] = {0x68};           m6502("PLA", p, 1, 4, NULL); }
	{ const uint8_t p[] = {0xea};           m6502("NOP", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x00};           m6502("BRK", p, 1, 7, NULL); }
	{ const uint8_t p[] = {0x90,0x02};      m6502("BCC taken, same page", p, 2, 3, m6502_no_carry); }
	{ const uint8_t p[] = {0x90,0xfd};      m6502("BCC taken, over a page", p, 2, 4, m6502_no_carry); }
	{ const uint8_t p[] = {0xb0,0x02};      m6502("BCS not taken", p, 2, 2, m6502_no_carry); }
}

/* ------------------------------------------------------------------ 65C02 */

static void
c02(const char *what, const uint8_t *code, size_t len, int want,
    void (*setup)(cpu6502_state *))
{
	cpu6502_state c;

	memset(ram, 0, sizeof(ram));
	memcpy(ram + 0x200, code, len);
	memset(&c, 0, sizeof(c));
	cpu6502_init(&c, ram, sizeof(ram));
	cpu6502_set_cmos(&c, 1);
	cpu6502_reset(&c, 0x200);
	if (setup != NULL) {
		setup(&c);
	}
	check(what, cpu6502_step(&c), want);
}

static void c02_decimal(cpu6502_state *c) { c->p |= CPU6502_FLAG_D; }

static void
test_65c02(void)
{
	printf("65C02, where it differs from the NMOS part:\n");
	{ const uint8_t p[] = {0x80,0x02};      c02("BRA", p, 2, 3, NULL); }
	{ const uint8_t p[] = {0x1a};           c02("INC A", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x3a};           c02("DEC A", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x5a};           c02("PHY", p, 1, 3, NULL); }
	{ const uint8_t p[] = {0x7a};           c02("PLY", p, 1, 4, NULL); }
	{ const uint8_t p[] = {0xda};           c02("PHX", p, 1, 3, NULL); }
	{ const uint8_t p[] = {0xfa};           c02("PLX", p, 1, 4, NULL); }
	{ const uint8_t p[] = {0x64,0x10};      c02("STZ zp", p, 2, 3, NULL); }
	{ const uint8_t p[] = {0x74,0x10};      c02("STZ zp,X", p, 2, 4, NULL); }
	{ const uint8_t p[] = {0x9c,0x00,0x30}; c02("STZ abs", p, 3, 4, NULL); }
	{ const uint8_t p[] = {0x9e,0x00,0x30}; c02("STZ abs,X is fixed at 5", p, 3, 5, NULL); }
	{ const uint8_t p[] = {0x04,0x10};      c02("TSB zp", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x0c,0x00,0x30}; c02("TSB abs", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0x14,0x10};      c02("TRB zp", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x1c,0x00,0x30}; c02("TRB abs", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0x89,0xff};      c02("BIT #n", p, 2, 2, NULL); }
	{ const uint8_t p[] = {0x34,0x10};      c02("BIT zp,X", p, 2, 4, NULL); }
	{ const uint8_t p[] = {0x3c,0x00,0x30}; c02("BIT abs,X", p, 3, 4, NULL); }
	{ const uint8_t p[] = {0xb2,0x10};      c02("LDA (zp)", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x92,0x10};      c02("STA (zp)", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x72,0x10};      c02("ADC (zp)", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0x7c,0x00,0x30}; c02("JMP (abs,X)", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0xdb};           c02("STP", p, 1, 3, NULL); }

	/* JMP (abs) costs six on the CMOS part and five on the NMOS one, because
	   it no longer has to reproduce the page-boundary bug. */
	{ const uint8_t p[] = {0x6c,0x00,0x30}; c02("JMP (abs)", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0x6c,0x00,0x30}; m6502("JMP (abs) on NMOS", p, 3, 5, NULL); }

	/* Decimal arithmetic costs a CMOS part one cycle more. */
	{ const uint8_t p[] = {0x69,0x01};      c02("ADC #n in decimal", p, 2, 3, c02_decimal); }
	{ const uint8_t p[] = {0x69,0x01};      m6502("ADC #n in decimal on NMOS", p, 2, 2, c02_decimal); }
}

/* ------------------------------------------------------------------- Z80 */

static void
z80(const char *what, const uint8_t *code, size_t len, int want,
    void (*setup)(cpu_z80_state *))
{
	cpu_z80_state c;

	memset(ram, 0, sizeof(ram));
	memcpy(ram + 0x100, code, len);
	memset(&c, 0, sizeof(c));
	cpu_z80_init(&c, ram, sizeof(ram));
	cpu_z80_reset(&c, 0x100);
	/* Somewhere harmless for the memory operands to point. */
	c.h = 0x40; c.l = 0x00; c.d = 0x41; c.e = 0x00;
	c.b = 0x02; c.c = 0x03; c.sp = 0x8000; c.ix = 0x4200; c.iy = 0x4300;
	if (setup != NULL) {
		setup(&c);
	}
	check(what, cpu_z80_step(&c), want);
}

static void z80_z_set(cpu_z80_state *c) { c->f |= CPU_Z80_FLAG_Z; }
static void z80_z_clear(cpu_z80_state *c) { c->f &= (uint8_t) ~CPU_Z80_FLAG_Z; }
static void z80_bc_2(cpu_z80_state *c) { c->b = 0; c->c = 2; }

static void
test_z80(void)
{
	printf("Z80, against the published T-states:\n");
	{ const uint8_t p[]={0x00};                z80("NOP",p,1,4,NULL); }
	{ const uint8_t p[]={0x01,0x34,0x12};      z80("LD BC,nn",p,3,10,NULL); }
	{ const uint8_t p[]={0x06,0x55};           z80("LD B,n",p,2,7,NULL); }
	{ const uint8_t p[]={0x0a};                z80("LD A,(BC)",p,1,7,NULL); }
	{ const uint8_t p[]={0x34};                z80("INC (HL)",p,1,11,NULL); }
	{ const uint8_t p[]={0x36,0x99};           z80("LD (HL),n",p,2,10,NULL); }
	{ const uint8_t p[]={0x32,0x00,0x50};      z80("LD (nn),A",p,3,13,NULL); }
	{ const uint8_t p[]={0x76};                z80("HALT",p,1,4,NULL); }
	{ const uint8_t p[]={0x86};                z80("ADD A,(HL)",p,1,7,NULL); }
	{ const uint8_t p[]={0xc3,0x00,0x02};      z80("JP nn",p,3,10,NULL); }
	{ const uint8_t p[]={0xcd,0x00,0x02};      z80("CALL nn",p,3,17,NULL); }
	{ const uint8_t p[]={0xc9};                z80("RET",p,1,10,NULL); }
	{ const uint8_t p[]={0xc5};                z80("PUSH BC",p,1,11,NULL); }
	{ const uint8_t p[]={0xc1};                z80("POP BC",p,1,10,NULL); }
	{ const uint8_t p[]={0xe3};                z80("EX (SP),HL",p,1,19,NULL); }
	{ const uint8_t p[]={0xd3,0xfe};           z80("OUT (n),A",p,2,11,NULL); }
	{ const uint8_t p[]={0xdb,0xfe};           z80("IN A,(n)",p,2,11,NULL); }
	{ const uint8_t p[]={0xf9};                z80("LD SP,HL",p,1,6,NULL); }
	{ const uint8_t p[]={0x18,0x02};           z80("JR e",p,2,12,NULL); }

	/* Both halves of every conditional. */
	{ const uint8_t p[]={0x28,0x02};           z80("JR Z,e taken",p,2,12,z80_z_set); }
	{ const uint8_t p[]={0x28,0x02};           z80("JR Z,e not taken",p,2,7,z80_z_clear); }
	{ const uint8_t p[]={0x20,0x02};           z80("JR NZ,e taken",p,2,12,z80_z_clear); }
	{ const uint8_t p[]={0x20,0x02};           z80("JR NZ,e not taken",p,2,7,z80_z_set); }
	{ const uint8_t p[]={0x10,0x02};           z80("DJNZ taken",p,2,13,NULL); }
	{ const uint8_t p[]={0xc8};                z80("RET Z taken",p,1,11,z80_z_set); }
	{ const uint8_t p[]={0xc8};                z80("RET Z not taken",p,1,5,z80_z_clear); }
	{ const uint8_t p[]={0xc0};                z80("RET NZ taken",p,1,11,z80_z_clear); }
	{ const uint8_t p[]={0xc0};                z80("RET NZ not taken",p,1,5,z80_z_set); }
	{ const uint8_t p[]={0xcc,0x00,0x02};      z80("CALL Z taken",p,3,17,z80_z_set); }
	{ const uint8_t p[]={0xcc,0x00,0x02};      z80("CALL Z not taken",p,3,10,z80_z_clear); }
	{ const uint8_t p[]={0xc4,0x00,0x02};      z80("CALL NZ taken",p,3,17,z80_z_clear); }
	{ const uint8_t p[]={0xca,0x00,0x02};      z80("JP Z taken is still 10",p,3,10,z80_z_set); }
	{ const uint8_t p[]={0xca,0x00,0x02};      z80("JP Z not taken",p,3,10,z80_z_clear); }

	/* The prefixed pages. */
	{ const uint8_t p[]={0xcb,0x00};           z80("RLC B",p,2,8,NULL); }
	{ const uint8_t p[]={0xcb,0x06};           z80("RLC (HL)",p,2,15,NULL); }
	{ const uint8_t p[]={0xcb,0x46};           z80("BIT 0,(HL)",p,2,12,NULL); }
	{ const uint8_t p[]={0xdd,0x7e,0x05};      z80("LD A,(IX+d)",p,3,19,NULL); }
	{ const uint8_t p[]={0xdd,0x34,0x05};      z80("INC (IX+d)",p,3,23,NULL); }
	{ const uint8_t p[]={0xdd,0x36,0x05,0x99}; z80("LD (IX+d),n",p,4,19,NULL); }
	{ const uint8_t p[]={0xdd,0xcb,0x05,0x06}; z80("RLC (IX+d)",p,4,23,NULL); }
	{ const uint8_t p[]={0xdd,0xcb,0x05,0x46}; z80("BIT 0,(IX+d)",p,4,20,NULL); }
	{ const uint8_t p[]={0xed,0x44};           z80("NEG",p,2,8,NULL); }
	{ const uint8_t p[]={0xed,0x52};           z80("SBC HL,DE",p,2,15,NULL); }
	{ const uint8_t p[]={0xed,0x43,0x00,0x50}; z80("LD (nn),BC",p,4,20,NULL); }
	{ const uint8_t p[]={0xed,0x57};           z80("LD A,I",p,2,9,NULL); }
	{ const uint8_t p[]={0xed,0x67};           z80("RRD",p,2,18,NULL); }
	{ const uint8_t p[]={0xed,0x45};           z80("RETN",p,2,14,NULL); }
	{ const uint8_t p[]={0xed,0xa0};           z80("LDI",p,2,16,NULL); }
	{ const uint8_t p[]={0xed,0xb0};           z80("LDIR with more to do",p,2,21,z80_bc_2); }
	{ const uint8_t p[]={0xed,0x78};           z80("IN A,(C)",p,2,12,NULL); }
}

/* ------------------------------------------------------------------ 8080 */

static void
i8080(const char *what, const uint8_t *code, size_t len, int want,
      void (*setup)(cpu_z80_state *))
{
	cpu_z80_state c;

	memset(ram, 0, sizeof(ram));
	memcpy(ram + 0x100, code, len);
	memset(&c, 0, sizeof(c));
	cpu_z80_init(&c, ram, sizeof(ram));
	cpu_z80_set_8080(&c, 1);
	cpu_z80_reset(&c, 0x100);
	c.h = 0x40; c.l = 0x00; c.d = 0x41; c.e = 0x00;
	c.b = 0x02; c.c = 0x03; c.sp = 0x8000;
	if (setup != NULL) {
		setup(&c);
	}
	check(what, cpu_z80_step(&c), want);
}

static void
test_8080(void)
{
	printf("8080, where it is timed differently from the Z80:\n");
	{ const uint8_t p[]={0x41};           i8080("MOV B,C is 5, not 4", p, 1, 5, NULL); }
	{ const uint8_t p[]={0x46};           i8080("MOV B,M is 7 on both", p, 1, 7, NULL); }
	{ const uint8_t p[]={0x70};           i8080("MOV M,B is 7 on both", p, 1, 7, NULL); }
	{ const uint8_t p[]={0x76};           i8080("HLT is 7, not 4", p, 1, 7, NULL); }
	{ const uint8_t p[]={0x03};           i8080("INX B is 5, not 6", p, 1, 5, NULL); }
	{ const uint8_t p[]={0x04};           i8080("INR B is 5, not 4", p, 1, 5, NULL); }
	{ const uint8_t p[]={0x34};           i8080("INR M is 10, not 11", p, 1, 10, NULL); }
	{ const uint8_t p[]={0x09};           i8080("DAD B is 10, not 11", p, 1, 10, NULL); }
	{ const uint8_t p[]={0xe3};           i8080("XTHL is 18, not 19", p, 1, 18, NULL); }
	{ const uint8_t p[]={0xe9};           i8080("PCHL is 5, not 4", p, 1, 5, NULL); }
	{ const uint8_t p[]={0xf9};           i8080("SPHL is 5, not 6", p, 1, 5, NULL); }
	{ const uint8_t p[]={0xd3,0x10};      i8080("OUT is 10, not 11", p, 2, 10, NULL); }
	{ const uint8_t p[]={0xdb,0x10};      i8080("IN is 10, not 11", p, 2, 10, NULL); }

	/* And the many it agrees with the Z80 about. */
	{ const uint8_t p[]={0x00};           i8080("NOP is 4", p, 1, 4, NULL); }
	{ const uint8_t p[]={0x01,0x34,0x12}; i8080("LXI B is 10", p, 3, 10, NULL); }
	{ const uint8_t p[]={0x06,0x55};      i8080("MVI B is 7", p, 2, 7, NULL); }
	{ const uint8_t p[]={0x36,0x55};      i8080("MVI M is 10", p, 2, 10, NULL); }
	{ const uint8_t p[]={0x32,0x00,0x50}; i8080("STA is 13", p, 3, 13, NULL); }
	{ const uint8_t p[]={0x22,0x00,0x50}; i8080("SHLD is 16", p, 3, 16, NULL); }
	{ const uint8_t p[]={0xc3,0x00,0x02}; i8080("JMP is 10", p, 3, 10, NULL); }
	{ const uint8_t p[]={0xcd,0x00,0x02}; i8080("CALL is 17", p, 3, 17, NULL); }
	{ const uint8_t p[]={0xc9};           i8080("RET is 10", p, 1, 10, NULL); }
	{ const uint8_t p[]={0xc5};           i8080("PUSH B is 11", p, 1, 11, NULL); }
	{ const uint8_t p[]={0xc7};           i8080("RST 0 is 11", p, 1, 11, NULL); }
	{ const uint8_t p[]={0x86};           i8080("ADD M is 7", p, 1, 7, NULL); }
}

/* ------------------------------------------------------------------ 6809 */

static void
m6809(const char *what, const uint8_t *code, size_t len, int want,
      void (*setup)(cpu6809_state *))
{
	cpu6809_state c;

	memset(ram, 0, sizeof(ram));
	memcpy(ram + 0x200, code, len);
	memset(&c, 0, sizeof(c));
	cpu6809_init(&c, ram, sizeof(ram));
	cpu6809_reset(&c, 0x200);
	if (setup != NULL) {
		setup(&c);
	}
	check(what, cpu6809_step(&c), want);
}

static void m6809_z_set(cpu6809_state *c) { c->cc |= CPU6809_FLAG_Z; }
static void m6809_z_clear(cpu6809_state *c)
{
	c->cc = (uint8_t) (c->cc & ~CPU6809_FLAG_Z);
}
static void m6809_stack(cpu6809_state *c) { c->s = 0x2000; }

static void
test_6809(void)
{
	printf("6809, against the published timings:\n");

	/* The four addressing modes down one column of the grid, which is the
	   rule the whole table was derived from. */
	{ const uint8_t p[] = {0x86,0x00};           m6809("LDA #n", p, 2, 2, NULL); }
	{ const uint8_t p[] = {0x96,0x10};           m6809("LDA direct", p, 2, 4, NULL); }
	{ const uint8_t p[] = {0xa6,0x84};           m6809("LDA ,X", p, 2, 4, NULL); }
	{ const uint8_t p[] = {0xb6,0x30,0x00};      m6809("LDA extended", p, 3, 5, NULL); }

	/* A sixteen-bit operand costs more in every one of them. */
	{ const uint8_t p[] = {0x8e,0x00,0x00};      m6809("LDX #nn", p, 3, 3, NULL); }
	{ const uint8_t p[] = {0x9e,0x10};           m6809("LDX direct", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0xbe,0x30,0x00};      m6809("LDX extended", p, 3, 6, NULL); }
	{ const uint8_t p[] = {0x83,0x00,0x00};      m6809("SUBD #nn", p, 3, 4, NULL); }
	{ const uint8_t p[] = {0xb3,0x30,0x00};      m6809("SUBD extended", p, 3, 7, NULL); }

	/* A prefix costs one, which is why the page two entries carry it. */
	{ const uint8_t p[] = {0x10,0x8e,0x00,0x00}; m6809("LDY #nn, on page two", p, 4, 4, NULL); }
	{ const uint8_t p[] = {0x10,0xbe,0x30,0x00}; m6809("LDY extended", p, 4, 7, NULL); }
	{ const uint8_t p[] = {0x10,0xce,0x00,0x00}; m6809("LDS #nn", p, 4, 4, NULL); }
	{ const uint8_t p[] = {0x11,0x83,0x00,0x00}; m6809("CMPU #nn, on page three", p, 4, 5, NULL); }

	/* ★ Every postbyte form's own cost, added to the opcode's. */
	{ const uint8_t p[] = {0xa6,0x80};           m6809("LDA ,X+", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0xa6,0x81};           m6809("LDA ,X++", p, 2, 7, NULL); }
	{ const uint8_t p[] = {0xa6,0x82};           m6809("LDA ,-X", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0xa6,0x83};           m6809("LDA ,--X", p, 2, 7, NULL); }
	{ const uint8_t p[] = {0xa6,0x05};           m6809("LDA 5,X, five-bit", p, 2, 5, NULL); }
	/* ★ And the pair that catches a postbyte decoded as the wrong half of the
	   encoding: an explicit zero offset in five bits costs ONE MORE than a
	   bare ",X", because they are different forms and only one of them has an
	   offset to add. Postbyte &00 is that zero offset; ",X+" is &80. */
	{ const uint8_t p[] = {0xa6,0x00};           m6809("LDA 0,X, five-bit zero", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0xa6,0x85};           m6809("LDA B,X", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0xa6,0x86};           m6809("LDA A,X", p, 2, 5, NULL); }
	{ const uint8_t p[] = {0xa6,0x88,0x04};      m6809("LDA n,X, eight-bit", p, 3, 5, NULL); }
	{ const uint8_t p[] = {0xa6,0x89,0x00,0x04}; m6809("LDA n,X, sixteen-bit", p, 4, 8, NULL); }
	{ const uint8_t p[] = {0xa6,0x8b};           m6809("LDA D,X", p, 2, 8, NULL); }
	{ const uint8_t p[] = {0xa6,0x8c,0x04};      m6809("LDA n,PCR, eight-bit", p, 3, 5, NULL); }
	{ const uint8_t p[] = {0xa6,0x8d,0x00,0x04}; m6809("LDA n,PCR, sixteen-bit", p, 4, 9, NULL); }
	{ const uint8_t p[] = {0xa6,0x9f,0x30,0x00}; m6809("LDA [n], extended indirect", p, 4, 9, NULL); }

	/* Indirection is three on top of whatever the form already cost. */
	{ const uint8_t p[] = {0xa6,0x94};           m6809("LDA [,X]", p, 2, 7, NULL); }
	{ const uint8_t p[] = {0xa6,0x91};           m6809("LDA [,X++]", p, 2, 10, NULL); }
	{ const uint8_t p[] = {0xa6,0x98,0x04};      m6809("LDA [n,X], eight-bit", p, 3, 8, NULL); }

	/* The read-modify-write column, in its three forms. */
	{ const uint8_t p[] = {0x40};                m6809("NEGA, inherent", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x00,0x10};           m6809("NEG direct", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0x60,0x84};           m6809("NEG ,X", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0x70,0x30,0x00};      m6809("NEG extended", p, 3, 7, NULL); }
	{ const uint8_t p[] = {0x0e,0x10};           m6809("JMP direct", p, 2, 3, NULL); }
	{ const uint8_t p[] = {0x7e,0x30,0x00};      m6809("JMP extended", p, 3, 4, NULL); }

	/* And the ones that are each their own number. */
	{ const uint8_t p[] = {0x12};                m6809("NOP", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x3d};                m6809("MUL", p, 1, 11, NULL); }
	{ const uint8_t p[] = {0x1d};                m6809("SEX", p, 1, 2, NULL); }
	{ const uint8_t p[] = {0x3a};                m6809("ABX", p, 1, 3, NULL); }
	{ const uint8_t p[] = {0x1f,0x89};           m6809("TFR", p, 2, 6, NULL); }
	{ const uint8_t p[] = {0x1e,0x89};           m6809("EXG", p, 2, 8, NULL); }
	{ const uint8_t p[] = {0x39};                m6809("RTS", p, 1, 5, m6809_stack); }
	{ const uint8_t p[] = {0x8d,0x02};           m6809("BSR", p, 2, 7, m6809_stack); }
	{ const uint8_t p[] = {0xbd,0x30,0x00};      m6809("JSR extended", p, 3, 8, m6809_stack); }
	{ const uint8_t p[] = {0x17,0x00,0x02};      m6809("LBSR", p, 3, 9, m6809_stack); }
	{ const uint8_t p[] = {0x16,0x00,0x02};      m6809("LBRA", p, 3, 5, NULL); }
	{ const uint8_t p[] = {0x3f};                m6809("SWI", p, 1, 19, m6809_stack); }
	{ const uint8_t p[] = {0x30,0x84};           m6809("LEAX ,X", p, 2, 4, NULL); }

	/* ★ A push costs five plus one per byte, so its register list is part of
	   the price and no table can hold it. */
	{ const uint8_t p[] = {0x34,0x02};           m6809("PSHS A, one byte", p, 2, 6, m6809_stack); }
	{ const uint8_t p[] = {0x34,0x10};           m6809("PSHS X, two bytes", p, 2, 7, m6809_stack); }
	{ const uint8_t p[] = {0x34,0xff};           m6809("PSHS everything, twelve", p, 2, 17, m6809_stack); }
	{ const uint8_t p[] = {0x35,0x03};           m6809("PULS CC and A", p, 2, 7, m6809_stack); }

	/*
	 * ★ BOTH POLARITIES, for the reason at the top of this file. A short
	 * branch costs the same either way and a long one does not, which is the
	 * pair a single-polarity sample would get wrong.
	 */
	{ const uint8_t p[] = {0x27,0x02};           m6809("BEQ taken", p, 2, 3, m6809_z_set); }
	{ const uint8_t p[] = {0x27,0x02};           m6809("BEQ not taken", p, 2, 3, m6809_z_clear); }
	{ const uint8_t p[] = {0x26,0x02};           m6809("BNE taken", p, 2, 3, m6809_z_clear); }
	{ const uint8_t p[] = {0x26,0x02};           m6809("BNE not taken", p, 2, 3, m6809_z_set); }
	{ const uint8_t p[] = {0x10,0x27,0x00,0x02}; m6809("LBEQ taken", p, 4, 6, m6809_z_set); }
	{ const uint8_t p[] = {0x10,0x27,0x00,0x02}; m6809("LBEQ not taken", p, 4, 5, m6809_z_clear); }
	{ const uint8_t p[] = {0x10,0x26,0x00,0x02}; m6809("LBNE taken", p, 4, 6, m6809_z_clear); }
	{ const uint8_t p[] = {0x10,0x26,0x00,0x02}; m6809("LBNE not taken", p, 4, 5, m6809_z_set); }
}

int
main(void)
{
	test_6502();
	test_65c02();
	test_z80();
	test_8080();
	test_6809();

	if (failures != 0) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all sampled timings match the published values\n");
	return 0;
}
