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

#ifndef CAPTURED_POINTER_H
#define CAPTURED_POINTER_H

/*
 * Turning host pointer movement into movement for a guest whose pointer is
 * captured, without warping the pointer on every single event.
 *
 * ★ Why this exists, rather than measuring from the centre every time.
 *
 * The obvious way to keep a captured pointer inside the window is to put it back
 * in the middle after every movement and treat its distance from the middle as
 * the movement. That is what both panels did, and it made captured mode
 * noticeably slower than the ordinary one - a stuttering pointer, and dragging a
 * RISC OS window around visibly worse than with the mouse following the host.
 *
 * The cost is the warp. A mouse reporting several hundred times a second means
 * several hundred warps a second, each one an X (or Win32) call that also
 * produces another motion event to process, all on the thread that paints the
 * machine's screen. The painting and the pointer then compete, and both lose.
 *
 * So movement is measured from wherever the pointer was last seen, which needs
 * no warp at all, and the pointer is only put back in the middle when it gets
 * close enough to an edge to be in danger of leaving. During ordinary play that
 * is a handful of warps rather than hundreds a second.
 *
 * Plain arithmetic in a header, with no wx types, so both panels share one copy
 * and tests/test_remote_display.c can hold it to its promises: that the movement
 * reported adds up to the movement made, and that re-centring is rare.
 */

struct captured_pointer {
	int last_x, last_y;	/* where the pointer was at the previous event */
	int have_last;		/* nothing to measure from until the first event */
	int carry_x, carry_y;	/* sub-guest-pixel remainder; see
				   remote_display_delta_to_guest() */
	unsigned long moves;	/* what happened, for the log to report */
	unsigned long recentres;
	unsigned long report_at_ms;	/* when the last report was due */
	unsigned long reported_moves;	/* the count it carried */
};

/* How often the counters are worth a line in the log. */
#define CAPTURED_POINTER_REPORT_MS 5000

/*
 * Whether the counters are worth reporting now, the caller doing the logging.
 *
 * ★ Reported at all because "captured mode feels slower" could not be answered
 * from the outside. The number that mattered was how often the pointer was being
 * warped, and nothing said; movements per re-centre now answers it. It should be
 * in the hundreds - a figure near one means the pointer is being warped on nearly
 * every movement, which is the fault this file was written to remove.
 *
 * The decision lives here rather than in either panel so both report the same
 * way, and so it can be tested; the logging itself stays with the caller, which
 * is the half that needs rpclog and a clock.
 *
 * @param now_ms any monotonic millisecond clock
 * @return non-zero if the caller should log the counters now
 */
static inline int
captured_pointer_should_report(struct captured_pointer *cp, unsigned long now_ms)
{
	if (cp->report_at_ms == 0) {
		cp->report_at_ms = now_ms;
		return 0;
	}
	if (now_ms - cp->report_at_ms < CAPTURED_POINTER_REPORT_MS) {
		return 0;
	}

	cp->report_at_ms = now_ms;

	/* Nothing has moved since the last one, so there is nothing to say: a still
	   mouse and an idle machine write nothing at all. */
	if (cp->moves == cp->reported_moves) {
		return 0;
	}

	cp->reported_moves = cp->moves;
	return 1;
}

/*
 * How close to an edge is too close.
 *
 * Generous on purpose. The pointer is not grabbed at the window level while it
 * is being read this way, so a fast movement that crosses the margin between two
 * events could take it outside the window, where the events stop coming and the
 * guest stops hearing about the mouse. An eighth of the smaller side, and never
 * less than 64 pixels, keeps a fast hand inside while still leaving the middle
 * of the window - which is where the pointer spends its time - warp-free.
 */
static inline int
captured_pointer_margin(int panel_w, int panel_h)
{
	const int smaller = panel_w < panel_h ? panel_w : panel_h;
	int margin = smaller / 8;

	if (margin < 64) {
		margin = 64;
	}
	/* A window smaller than four margins has no warp-free middle to protect,
	   so give it a proportional one and let it warp more often. */
	if (margin * 4 > smaller) {
		margin = smaller / 4;
	}
	return margin;
}

/*
 * Begin, at the pointer's current position.
 *
 * The position is remembered rather than the pointer being moved to the middle:
 * the click that captures should not make the mouse jump, and the first movement
 * afterwards is measured from where the user actually left it.
 */
static inline void
captured_pointer_begin(struct captured_pointer *cp, int x, int y)
{
	cp->last_x = x;
	cp->last_y = y;
	cp->have_last = 1;
	cp->carry_x = 0;
	cp->carry_y = 0;
}

static inline void
captured_pointer_end(struct captured_pointer *cp)
{
	cp->have_last = 0;
}

/*
 * A movement event.
 *
 * Answers the movement in panel pixels - which the caller scales to the guest,
 * each panel knowing its own scaling - and whether the pointer should now be put
 * back in the middle. When it should, the caller warps and the position it warps
 * to is what gets remembered, so the warp's own motion event measures as no
 * movement and reports nothing.
 *
 * @return non-zero if there is any movement to report
 */
static inline int
captured_pointer_motion(struct captured_pointer *cp, int x, int y,
                        int panel_w, int panel_h, int centre_x, int centre_y,
                        int *dx, int *dy, int *recentre)
{
	int margin;

	*dx = 0;
	*dy = 0;
	*recentre = 0;

	if (!cp->have_last) {
		captured_pointer_begin(cp, x, y);
		return 0;
	}

	*dx = x - cp->last_x;
	*dy = y - cp->last_y;
	cp->last_x = x;
	cp->last_y = y;

	margin = captured_pointer_margin(panel_w, panel_h);

	if (x < margin || y < margin ||
	    x >= panel_w - margin || y >= panel_h - margin) {
		*recentre = 1;
		cp->last_x = centre_x;
		cp->last_y = centre_y;
		cp->recentres++;
	}

	if (*dx == 0 && *dy == 0) {
		return 0;
	}
	cp->moves++;
	return 1;
}

#endif /* CAPTURED_POINTER_H */
