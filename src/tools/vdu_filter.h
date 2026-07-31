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
 * vdu_filter - turn a RISC OS VDU stream into ordinary text for a host shell.
 *
 * HostCmd captures what the guest writes through the VDU character stream and
 * forwards it byte for byte, which is the right thing for the protocol to do and
 * the wrong thing for a command whose output lands in a pipe. Two properties of
 * that stream surprise everyone who tries it:
 *
 *   - **Lines end LF then CR**, in that order, so every line carries a trailing
 *     carriage return. Text arrives looking correct and then fails `grep`.
 *   - **VDU control codes are in-band**, and most take parameters. `*Status`
 *     emits VDU 14 (paged mode); a program that sets a colour emits VDU 17 and
 *     one parameter byte. Dropping only the introducer leaves the parameters to
 *     appear as text, which corrupts a line invisibly - worse than leaving the
 *     code alone.
 *
 * This filter fixes both. It is deliberately not in the protocol or the guest
 * module: the socket stays byte-exact so anything wanting the real stream can
 * have it, and rpcemu-run --raw turns the filter off.
 *
 * The state lives in a struct because a VDU sequence can be split across two
 * socket reads, and because the newline rules need to know what the previous
 * byte was.
 */

#ifndef VDU_FILTER_H
#define VDU_FILTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	unsigned params;	/**< VDU parameter bytes still to be swallowed */
	unsigned char prev;	/**< previous emitted line terminator, 0 if none */
} VduFilter;

/** Start (or restart) a filter. Also correct for a zeroed struct. */
extern void vdu_filter_init(VduFilter *f);

/**
 * Filter @p n bytes.
 *
 * @param f   Filter state, carried across calls.
 * @param in  Input bytes.
 * @param n   Number of input bytes.
 * @param out Output buffer; must have room for @p n bytes.
 * @return Number of bytes written to @p out, never more than @p n.
 */
extern size_t vdu_filter(VduFilter *f, const uint8_t *in, size_t n, uint8_t *out);

/**
 * Finish a stream.
 *
 * A carriage return at the very end has been held back in case a line feed
 * follows it, so it is emitted here as the newline it turned out to be.
 *
 * @param f   Filter state.
 * @param out Output buffer; must have room for one byte.
 * @return Number of bytes written, 0 or 1.
 */
extern size_t vdu_filter_finish(VduFilter *f, uint8_t *out);

#endif /* VDU_FILTER_H */
