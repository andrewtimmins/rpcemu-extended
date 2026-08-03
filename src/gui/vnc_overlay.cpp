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

/* See vnc_overlay.h. */

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "rpcemu.h"
#include "console_font.h"
#include "text_screen.h"
}

#include "emulator_host.h"
#include "vnc_app.h"
#include "vnc_overlay.h"
#include "vnc_server.h"

namespace {

/* X11 keysyms. Prefixed rather than called XK_*, because libvncserver's headers
   already define those as macros and a constexpr cannot be named by one. */
constexpr uint32_t kKeyEscape    = 0xff1b;
constexpr uint32_t kKeyReturn    = 0xff0d;
constexpr uint32_t kKeyKPEnter  = 0xff8d;
constexpr uint32_t kKeyUp        = 0xff52;
constexpr uint32_t kKeyDown      = 0xff54;
constexpr uint32_t kKeyShiftL   = 0xffe1;
constexpr uint32_t kKeyShiftR   = 0xffe2;
constexpr uint32_t kKeyControlL = 0xffe3;
constexpr uint32_t kKeyControlR = 0xffe4;
constexpr uint32_t kKeyAltL     = 0xffe9;
constexpr uint32_t kKeyAltR     = 0xffea;
constexpr uint32_t kKeyMetaL    = 0xffe7;
constexpr uint32_t kKeyMetaR    = 0xffe8;

constexpr uint32_t COLOUR_BG    = 0x00102040u;
constexpr uint32_t COLOUR_TITLE = 0x00ffffffu;
constexpr uint32_t COLOUR_TEXT  = 0x00c0c8d0u;
constexpr uint32_t COLOUR_DIM   = 0x007080a0u;
constexpr uint32_t COLOUR_SEL_BG = 0x00d0d8e0u;
constexpr uint32_t COLOUR_SEL_FG = 0x00102040u;

constexpr int ROW = CONSOLE_FONT_HEIGHT;

enum class Item {
	Reset,
	Close,
	Quit,
	Count
};

struct Entry {
	Item item;
	const char *label;
	const char *detail;
};

const Entry kEntries[] = {
	{ Item::Reset, "Reset the machine",
	  "As the Reset item on the File menu. Anything unsaved is lost." },
	{ Item::Close, "Return to the machine",
	  "Close this menu. Ctrl+Alt+Shift+M brings it back." },
	{ Item::Quit,  "Shut the emulator down",
	  "Saves CMOS, discs and settings on the way out, as quitting normally does." },
};

struct State {
	std::mutex mutex;
	EmulatorHost *host = nullptr;
	bool visible = false;
	int selected = 0;
	bool ctrl = false;
	bool alt = false;
	bool shift = false;
	std::vector<uint32_t> pixels;
};

State g_state;

void track_modifier(uint32_t keysym, bool down)
{
	switch (keysym) {
	case kKeyControlL: case kKeyControlR: g_state.ctrl = down; break;
	case kKeyShiftL:   case kKeyShiftR:   g_state.shift = down; break;
	case kKeyAltL:     case kKeyAltR:
	case kKeyMetaL:    case kKeyMetaR:    g_state.alt = down; break;
	default: break;
	}
}

/** Draw the menu at whatever size the client's framebuffer currently is. */
void draw(VncServer &server)
{
	const int w = server.width() > 0 ? server.width() : 640;
	const int h = server.height() > 0 ? server.height() : 480;
	TextScreen screen;
	int y;

	g_state.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
	screen.pixels = g_state.pixels.data();
	screen.width = w;
	screen.height = h;
	screen.stride = w;
	screen.scale = 1;

	/* The guest picks the resolution, so the menu has to look deliberate at both
	   640x480 and 1920x1080. Scale the cell with the screen, and centre the block
	   rather than pinning it to a corner where it looks lost on a large display. */
	text_screen_set_scale(&screen, text_screen_auto_scale(&screen));
	{
		const int row = CONSOLE_FONT_HEIGHT * screen.scale;
		const int items = static_cast<int>(Item::Count);
		const int block = row * (items * 2 - 1);
		const int indent = CONSOLE_FONT_WIDTH * screen.scale * 3;

		text_screen_clear(&screen, COLOUR_BG);
		text_screen_centre(&screen, row * 2, "RPCEmu Extended", COLOUR_TITLE);
		text_screen_centre(&screen, row * 3,
		    config.name[0] != '\0' ? config.name : "(unnamed machine)", COLOUR_TEXT);

		y = (h - block) / 2;
		for (int i = 0; i < items; i++) {
			if (i == g_state.selected) {
				text_screen_fill(&screen, indent - row / 2, y,
				    w - 2 * (indent - row / 2), row, COLOUR_SEL_BG);
				text_screen_string_bg(&screen, indent, y, kEntries[i].label,
				    COLOUR_SEL_FG, COLOUR_SEL_BG);
			} else {
				text_screen_string(&screen, indent, y, kEntries[i].label,
				    COLOUR_TEXT);
			}
			y += row * 2;
		}

		/* What the highlighted item will do, so nothing destructive is chosen from
		   a three-word label alone. */
		text_screen_centre(&screen, h - row * 5,
		    kEntries[g_state.selected].detail, COLOUR_DIM);
		text_screen_centre(&screen, h - row * 3,
		    "Up and Down to move, Enter to choose", COLOUR_DIM);
		text_screen_centre(&screen, h - row * 2,
		    "Escape or Ctrl+Alt+Shift+M returns to the machine", COLOUR_DIM);
	}

	server.primeFramebuffer(g_state.pixels.data(), w, h);
}

void close_overlay(VncServer &server)
{
	g_state.visible = false;
	server.setOverlayActive(false);
}

/** Act on the highlighted item. Called with the lock held. */
void activate(VncServer &server)
{
	EmulatorHost *host = g_state.host;

	switch (kEntries[g_state.selected].item) {
	case Item::Reset:
		if (host != nullptr) {
			/* Posts a command; safe from this thread, which is the server's. */
			host->Reset();
			rpclog("vnc_overlay: machine reset from a VNC client\n");
		}
		close_overlay(server);
		break;

	case Item::Close:
		close_overlay(server);
		break;

	case Item::Quit:
		if (host != nullptr) {
			rpclog("vnc_overlay: shutdown requested from a VNC client\n");
			host->RequestExit();
		}
		close_overlay(server);
		break;

	case Item::Count:
		break;
	}
}

bool on_key(uint32_t keysym, bool down)
{
	VncServer &server = VncAppServer();
	std::lock_guard<std::mutex> lock(g_state.mutex);

	track_modifier(keysym, down);

	/* The shortcut. Swallowed in both directions so the guest never sees half of
	   it, which would leave it thinking a modifier was still held. */
	if (down && g_state.ctrl && g_state.alt && g_state.shift &&
	    (keysym == 'm' || keysym == 'M')) {
		g_state.visible = !g_state.visible;
		server.setOverlayActive(g_state.visible);
		if (g_state.visible) {
			/*
			 * The guest saw Ctrl, Alt and Shift go down and is about to not see
			 * them come up, because everything is swallowed from here. Left alone
			 * it believes all three are still held and interprets everything
			 * afterwards through them, which looks like keys being pressed by
			 * themselves.
			 */
			server.releaseAllKeys();
			g_state.selected = 0;
			draw(server);
		}
		return true;
	}

	if (!g_state.visible) {
		/* Closed: the guest gets everything, including the modifiers, which were
		   only being watched rather than taken. */
		return false;
	}

	if (!down) {
		return true;	/* the machine must not see releases for keys it never got */
	}

	switch (keysym) {
	case kKeyUp:
		g_state.selected = (g_state.selected + static_cast<int>(Item::Count) - 1) %
		    static_cast<int>(Item::Count);
		draw(server);
		break;
	case kKeyDown:
		g_state.selected = (g_state.selected + 1) % static_cast<int>(Item::Count);
		draw(server);
		break;
	case kKeyReturn:
	case kKeyKPEnter:
		activate(server);
		break;
	case kKeyEscape:
		close_overlay(server);
		break;
	default:
		break;
	}
	return true;
}

} /* namespace */

void VncOverlayAttach(EmulatorHost *host)
{
	VncServer &server = VncAppServer();
	std::lock_guard<std::mutex> lock(g_state.mutex);

	g_state.host = host;
	if (host == nullptr) {
		if (g_state.visible) {
			close_overlay(server);
		}
		server.setKeySymHook(nullptr);
		return;
	}
	server.setKeySymHook(on_key);
}

bool VncOverlayVisible()
{
	std::lock_guard<std::mutex> lock(g_state.mutex);

	return g_state.visible;
}
