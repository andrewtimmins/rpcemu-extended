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
 * Vertical sync must happen whether or not a frame of pixels is produced.
 *
 * WHY THIS EXISTS. It used to be raised as a side effect of a frame reaching the
 * host: rpcemu_video_update() posted it, and the video thread returns without
 * delivering anything when the dirty buffer is empty. So a screen that was not
 * changing produced no vertical sync at all - and that deadlocks a guest
 * outright. A game clears the screen, waits for vertical sync before drawing its
 * first frame, and the sync it waits for could only be produced by it drawing.
 * It waited for ever, screen black, with no abort and nothing in any log.
 *
 * That is what made every ADFFS game hang, pinned in the kernel's OS_Byte 19
 * wait loop, and it cost a long investigation precisely because every instrument
 * pointed at a healthy machine: no aborts, no fault, frames being requested 60
 * times a second.
 *
 * The invariant is one line long and worth pinning permanently: calling
 * drawscr() raises the flyback interrupt, on every path, including the ones that
 * draw nothing at all. tests/test_stubs.c has vidctrymutex() return 0, so
 * drawscr() here takes its earliest exit and produces no pixels whatsoever -
 * which is exactly the case that used to be silent.
 */

#include <stdio.h>
#include <stdlib.h>

#include "rpcemu.h"
#include "iomd.h"
#include "vidc20.h"

#define IOMD_0x010_IRQSTA	0x010	/* IRQA status */
#define IOMD_0x014_IRQRQA	0x014	/* IRQA request/clear */
#define IOMD_IRQA_FLYBACK	0x08

static int failures = 0;

static void
check(const char *what, int ok)
{
	printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/**
 * Clear a latched flyback interrupt the way the guest's handler does, by
 * writing the bit back to IRQA request/clear.
 */
static void
acknowledge_flyback(void)
{
	iomd_write(IOMD_0x014_IRQRQA, IOMD_IRQA_FLYBACK);
}

static int
flyback_pending(void)
{
	return (iomd_read(IOMD_0x010_IRQSTA) & IOMD_IRQA_FLYBACK) != 0;
}

int
main(void)
{
	int i;

	printf("Vertical sync\n\n");

	iomd_reset(IOMDType_IOMD);
	initvideo();

	acknowledge_flyback();
	check("no flyback latched to start with", !flyback_pending());

	/* The whole point: no pixels are produced on this path at all. */
	drawscr();
	check("drawscr() raises the flyback interrupt with nothing drawn",
	      flyback_pending());

	/* And it must keep coming, once per frame, rather than latching once and
	   stopping - a guest that waits for a NEW sync every frame, which is what
	   pacing a game means, needs one every time round. */
	for (i = 0; i < 5; i++) {
		acknowledge_flyback();
		if (flyback_pending()) {
			break;		/* acknowledge did not work; reported below */
		}
		drawscr();
		if (!flyback_pending()) {
			break;
		}
	}
	check("it is raised again on every later frame, not just the first",
	      i == 5);

	/* The pollable flyback bit in IOMD's control register must move too: some
	   guest code watches that instead of taking the interrupt. */
	check("the pollable flyback bit in IOMD control is set as well",
	      (iomd_read(0x000) & 0x80) != 0);

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
