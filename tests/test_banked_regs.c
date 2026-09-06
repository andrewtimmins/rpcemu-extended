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
 * The banked registers, as a debugger has to show them.
 *
 * WHY THIS EXISTS. Asked for on discussion #223: stepping through an exception
 * path means wanting the mode's own R13 and R14 beside the ones the interrupted
 * code was using. Reading them out is not a loop over the bank arrays, and the
 * way it goes wrong is nasty.
 *
 * The mode the machine is IN has its registers in arm.reg[]; its bank array
 * holds whatever was there when it was last switched out of, which can be very
 * stale. updatemode() writes them back on the way out, not as it goes. So a
 * naive reader shows five modes correctly and the one being debugged wrongly -
 * and the one being debugged is the only one anybody is looking at.
 *
 * R8-R12 have the same shape one level down. Every mode except FIQ shares them
 * with User, so while the machine is in IRQ or SVC the shared copy is the live
 * arm.reg[8..12] and arm.user_reg[8..12] is stale; only when the machine is in
 * FIQ does the User bank hold them.
 *
 * Both of those are checked here from both sides: the same register is read
 * while its mode is current and while it is not, and the answers have to agree.
 *
 * Links rpcemu_core for the real updatemode() and register banks, which are the
 * only thing that makes this meaningful - the banking rules are the emulator's,
 * not this file's.
 */

#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "arm.h"
#include "arm_common.h"
#include "mem.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_hex(const char *what, uint32_t got, uint32_t want)
{
	printf("  %-62s %s", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("  (got %08x, want %08x)", (unsigned) got, (unsigned) want);
		failures++;
	}
	printf("\n");
}

/** The bank with this name, or NULL. */
static const DebugBankedRegs *
bank(const DebugBankedRegs *banks, uint32_t count, const char *name)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(banks[i].name, name) == 0) {
			return &banks[i];
		}
	}
	return NULL;
}

/* Values chosen so that a register read from the wrong bank is obvious. */
#define SVC_R13	0x5C000013u
#define SVC_R14	0x5C000014u
#define IRQ_R13	0x1B000013u
#define IRQ_R14	0x1B000014u
#define FIQ_R8	0xF1000008u
#define FIQ_R13	0xF1000013u
#define FIQ_R14	0xF1000014u
#define USR_R13	0x05000013u
#define USR_R14	0x05000014u
#define SHARED_R8 0x58000008u

int
main(void)
{
	DebugBankedRegs banks[DEBUG_BANK_COUNT];
	uint32_t count;
	const DebugBankedRegs *b;

	arm_init();
	mem_init();
	machine.model = Model_RPCSA110;
	mem_reset(16, 2);
	prog32 = 1;

	/*
	 * Fill each bank by entering its mode and writing its registers, which is
	 * the only way to fill them: writing the bank arrays directly would test
	 * this file's idea of the banking rules rather than the emulator's.
	 */
	updatemode(0x10 | USER);
	arm.reg[13] = USR_R13;
	arm.reg[14] = USR_R14;
	arm.reg[8] = SHARED_R8;

	updatemode(0x10 | IRQ);
	arm.reg[13] = IRQ_R13;
	arm.reg[14] = IRQ_R14;

	updatemode(0x10 | FIQ);
	arm.reg[8] = FIQ_R8;
	arm.reg[13] = FIQ_R13;
	arm.reg[14] = FIQ_R14;

	updatemode(0x10 | SUPERVISOR);
	arm.reg[13] = SVC_R13;
	arm.reg[14] = SVC_R14;

	printf("Stopped in Supervisor\n");
	count = debugger_get_banked_registers(banks, DEBUG_BANK_COUNT);
	check("every mode is reported", count == DEBUG_BANK_COUNT);

	b = bank(banks, count, "SVC");
	check("SVC is the current mode", b != NULL && b->is_current);
	/* The live registers, NOT arm.super_reg[], which still holds whatever was
	   there before this mode was entered. */
	check_hex("SVC R13 is the live register", b ? b->r13 : 0, SVC_R13);
	check_hex("SVC R14 is the live register", b ? b->r14 : 0, SVC_R14);

	b = bank(banks, count, "IRQ");
	check("IRQ is not current", b != NULL && !b->is_current);
	check_hex("IRQ R13 comes from its bank", b ? b->r13 : 0, IRQ_R13);
	check_hex("IRQ R14 comes from its bank", b ? b->r14 : 0, IRQ_R14);

	b = bank(banks, count, "FIQ");
	check_hex("FIQ R13 comes from its bank", b ? b->r13 : 0, FIQ_R13);
	check_hex("FIQ R14 comes from its bank", b ? b->r14 : 0, FIQ_R14);
	check("FIQ reports R8-R12", b != NULL && b->banks_r8_r12);
	check_hex("FIQ R8 comes from its bank", b ? b->r8_r12[0] : 0, FIQ_R8);

	b = bank(banks, count, "USR");
	check_hex("User R13 comes from its bank", b ? b->r13 : 0, USR_R13);
	check_hex("User R14 comes from its bank", b ? b->r14 : 0, USR_R14);
	/*
	 * The shared five. The machine is in Supervisor, so R8-R12 ARE the live
	 * registers and arm.user_reg[8..12] is stale - reading the bank array here
	 * is the mistake this checks for.
	 */
	check_hex("the shared R8 is the live register", b ? b->r8_r12[0] : 0,
	          SHARED_R8);
	check("User has no SPSR", b != NULL && !b->has_spsr);
	check("SVC has one", bank(banks, count, "SVC")->has_spsr);

	/*
	 * Now from the other side. In FIQ, the same registers have to come back
	 * with the same values - FIQ's from arm.reg[], everyone else's from their
	 * banks, and the shared five from the User bank because FIQ is the one
	 * mode that does not share them.
	 */
	printf("\nStopped in FIQ, where R8-R12 change hands\n");
	updatemode(0x10 | FIQ);
	count = debugger_get_banked_registers(banks, DEBUG_BANK_COUNT);

	b = bank(banks, count, "FIQ");
	check("FIQ is now the current mode", b != NULL && b->is_current);
	check_hex("FIQ R13 is the live register", b ? b->r13 : 0, FIQ_R13);
	check_hex("FIQ R8 is the live register", b ? b->r8_r12[0] : 0, FIQ_R8);

	b = bank(banks, count, "SVC");
	check("SVC is no longer current", b != NULL && !b->is_current);
	check_hex("SVC R13 now comes from its bank", b ? b->r13 : 0, SVC_R13);

	b = bank(banks, count, "USR");
	check_hex("the shared R8 now comes from the User bank",
	          b ? b->r8_r12[0] : 0, SHARED_R8);
	check_hex("User R13 is unchanged", b ? b->r13 : 0, USR_R13);

	/* System shares User's bank, so a machine in System mode shows the User
	   row as current rather than showing it a stale copy. */
	printf("\nStopped in System, which shares User's bank\n");
	updatemode(0x10 | SYSTEM);
	count = debugger_get_banked_registers(banks, DEBUG_BANK_COUNT);
	b = bank(banks, count, "USR");
	check("the User row is current in System mode", b != NULL && b->is_current);

	printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
