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

/* See machine_selector.h. */

#include <stdio.h>
#include <string.h>

#include "console_font.h"	/* the cell size the layout is built from */
#include "machine_selector.h"

/* X11 keysyms RFB delivers. Named here rather than including X headers, which
   would drag a dependency into a file whose whole point is not having any. */
#define XK_Return	0xff0d
#define XK_KP_Enter	0xff8d
#define XK_Escape	0xff1b
#define XK_Up		0xff52
#define XK_Down		0xff54
#define XK_Page_Up	0xff55
#define XK_Page_Down	0xff56
#define XK_Home		0xff50
#define XK_End		0xff57

/* Grey on dark blue: legible on a bad connection, and obviously not RISC OS, so
   nobody mistakes the selector for a booted machine. */
#define COLOUR_BG	0x00102040u
#define COLOUR_TITLE	0x00ffffffu
#define COLOUR_TEXT	0x00c0c8d0u
#define COLOUR_DIM	0x007080a0u
#define COLOUR_SEL_BG	0x00d0d8e0u
#define COLOUR_SEL_FG	0x00102040u

#define ROW_HEIGHT	CONSOLE_FONT_HEIGHT
#define MARGIN_X	(CONSOLE_FONT_WIDTH * 2)

void
machine_selector_init(MachineSelector *m)
{
	memset(m, 0, sizeof(*m));
}

int
machine_selector_add(MachineSelector *m, const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return 0;
	}

	m->total++;
	if (m->count >= MACHINE_SELECTOR_MAX) {
		return 0;
	}

	snprintf(m->names[m->count], MACHINE_SELECTOR_NAME, "%s", name);
	m->count++;
	return 1;
}

const char *
machine_selector_current(const MachineSelector *m)
{
	if (m->count == 0 || m->selected < 0 || m->selected >= m->count) {
		return NULL;
	}
	return m->names[m->selected];
}

/** Keep the selection inside the list and inside the visible window. */
static void
scroll_to_selection(MachineSelector *m, int visible_rows)
{
	if (m->selected < 0) {
		m->selected = 0;
	}
	if (m->selected >= m->count) {
		m->selected = m->count - 1;
	}
	if (visible_rows < 1) {
		visible_rows = 1;
	}
	if (m->selected < m->first_visible) {
		m->first_visible = m->selected;
	}
	if (m->selected >= m->first_visible + visible_rows) {
		m->first_visible = m->selected - visible_rows + 1;
	}
	if (m->first_visible < 0) {
		m->first_visible = 0;
	}
}

MachineSelectorAction
machine_selector_key(MachineSelector *m, uint32_t keysym)
{
	const int page = 10;
	int before = m->selected;

	if (m->count == 0) {
		/* Nothing to choose. Escape still works, so a client is not stuck. */
		return (keysym == XK_Escape) ? MACHINE_SELECTOR_CANCELLED
		                             : MACHINE_SELECTOR_IGNORED;
	}

	switch (keysym) {
	case XK_Return:
	case XK_KP_Enter:
		return MACHINE_SELECTOR_CHOSEN;

	case XK_Escape:
		return MACHINE_SELECTOR_CANCELLED;

	case XK_Up:
		m->selected--;
		break;
	case XK_Down:
		m->selected++;
		break;
	case XK_Page_Up:
		m->selected -= page;
		break;
	case XK_Page_Down:
		m->selected += page;
		break;
	case XK_Home:
		m->selected = 0;
		break;
	case XK_End:
		m->selected = m->count - 1;
		break;

	default:
		/*
		 * A digit jumps to that entry, because the list is numbered and typing
		 * the number is faster than arrowing to it. 1-9 then 0 for the tenth,
		 * matching how the list is labelled.
		 */
		if (keysym >= '1' && keysym <= '9') {
			const int want = (int) (keysym - '1');

			if (want < m->count) {
				m->selected = want;
				break;
			}
			return MACHINE_SELECTOR_IGNORED;
		}
		if (keysym == '0' && m->count >= 10) {
			m->selected = 9;
			break;
		}
		return MACHINE_SELECTOR_IGNORED;
	}

	/* Clamped after the move, so Up on the first entry stays put rather than
	   wrapping to the end. Wrapping in a list this short surprises people. */
	if (m->selected < 0) {
		m->selected = 0;
	}
	if (m->selected >= m->count) {
		m->selected = m->count - 1;
	}

	return (m->selected != before) ? MACHINE_SELECTOR_REDRAW
	                              : MACHINE_SELECTOR_IGNORED;
}

void
machine_selector_draw(MachineSelector *m, TextScreen *s)
{
	const int list_top = ROW_HEIGHT * 4;
	const int footer_rows = 4;	/* two hint lines, one gap, one bottom margin */
	int visible_rows = (s->height - list_top) / ROW_HEIGHT - footer_rows;
	int i, y;

	if (visible_rows < 1) {
		visible_rows = 1;
	}
	scroll_to_selection(m, visible_rows);

	/* Set explicitly rather than left to whatever the caller put there: a struct
	   filled in field by field leaves this holding stack rubbish, and the list
	   wants scaling on a large display for the same reason the control menu does. */
	text_screen_set_scale(s, text_screen_auto_scale(s));

	text_screen_clear(s, COLOUR_BG);
	text_screen_centre(s, ROW_HEIGHT, "RPCEmu", COLOUR_TITLE);

	if (m->count == 0) {
		text_screen_centre(s, list_top, "No machines found.", COLOUR_TEXT);
		text_screen_centre(s, list_top + ROW_HEIGHT * 2,
		    "Create one with the graphical interface, or place a",
		    COLOUR_DIM);
		text_screen_centre(s, list_top + ROW_HEIGHT * 3,
		    "configuration in the configs directory.", COLOUR_DIM);
		return;
	}

	text_screen_centre(s, ROW_HEIGHT * 2, "Choose a machine", COLOUR_TEXT);

	y = list_top;
	for (i = m->first_visible;
	     i < m->count && i < m->first_visible + visible_rows;
	     i++, y += ROW_HEIGHT) {
		char line[MACHINE_SELECTOR_NAME + 8];
		const int selected = (i == m->selected);

		/* Numbered 1-9 then 0, so the digit shortcuts are discoverable rather
		   than something you have to be told about. */
		if (i < 9) {
			snprintf(line, sizeof(line), " %d  %s", i + 1, m->names[i]);
		} else if (i == 9) {
			snprintf(line, sizeof(line), " 0  %s", m->names[i]);
		} else {
			snprintf(line, sizeof(line), "    %s", m->names[i]);
		}

		if (selected) {
			/* The highlight spans the full width so it reads as a row, not as a
			   box around the text. */
			text_screen_fill(s, MARGIN_X - CONSOLE_FONT_WIDTH, y,
			    s->width - 2 * (MARGIN_X - CONSOLE_FONT_WIDTH), ROW_HEIGHT,
			    COLOUR_SEL_BG);
			text_screen_string_bg(s, MARGIN_X, y, line, COLOUR_SEL_FG,
			    COLOUR_SEL_BG);
		} else {
			text_screen_string(s, MARGIN_X, y, line, COLOUR_TEXT);
		}
	}

	/* Say so rather than silently truncating. */
	if (m->total > m->count) {
		char more[64];

		snprintf(more, sizeof(more), "... and %d more not shown",
		    m->total - m->count);
		text_screen_string(s, MARGIN_X, y, more, COLOUR_DIM);
	}

	/* One row clear of the bottom edge: drawn flush, the last line is clipped by
	   however much the client's height is not a multiple of the cell. */
	text_screen_centre(s, s->height - ROW_HEIGHT * 3,
	    "Up and Down to move, or type a number", COLOUR_DIM);
	text_screen_centre(s, s->height - ROW_HEIGHT * 2,
	    "Enter to start", COLOUR_DIM);
}
