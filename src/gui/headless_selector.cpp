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
 * headless_selector - choose a machine over VNC before one is running.
 *
 * Headless mode used to insist on --machine, because the selector was a wx
 * dialogue and headless has no wx. That made a remote emulator worse than a local
 * one in a way nothing about the machine required: changing machine meant killing
 * the process and starting it with different arguments.
 *
 * This puts the list on the VNC display instead. It runs before any machine
 * exists, which is only possible because the port and password are now the
 * emulator's own settings rather than a machine's (see src/app_settings.c), and
 * because the VNC server can be built with no EmulatorHost and its keys diverted
 * to a hook.
 *
 * The drawing and the list logic live in machine_selector.c, with no VNC in them,
 * so they can be tested by rendering into a buffer. This file is only the plumbing
 * between that and a socket.
 *
 * The server it uses belongs to the process (see vnc_app.h), so it is the same one
 * the machine then attaches to and a client keeps its connection across the
 * handover.
 */

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rpcemu.h"
#include "app_settings.h"
#include "machine_selector.h"
#include "text_screen.h"
#include "vnc_app.h"
#include "vnc_server.h"
#include "headless_selector.h"

namespace {

/* A size every client copes with, and enough rows for a long list. The machine
   that follows will resize the framebuffer to whatever mode it selects. */
constexpr int kWidth = 640;
constexpr int kHeight = 480;

/* Keys arrive on the VNC server's event thread, so they are queued rather than
   acted on there. */
std::mutex g_key_mutex;
std::deque<uint32_t> g_keys;

bool queue_key(uint32_t keysym, bool down)
{
	/* Always swallowed: there is no machine to pass anything on to. */
	if (!down) {
		return true;	/* act on press; a release would double every keystroke */
	}
	std::lock_guard<std::mutex> lock(g_key_mutex);

	/* A client holding a key while nothing drains it must not grow this without
	   bound. Twenty is far more than a selector can be behind. */
	if (g_keys.size() < 20) {
		g_keys.push_back(keysym);
	}
	return true;
}

bool next_key(uint32_t *keysym)
{
	std::lock_guard<std::mutex> lock(g_key_mutex);

	if (g_keys.empty()) {
		return false;
	}
	*keysym = g_keys.front();
	g_keys.pop_front();
	return true;
}

} /* namespace */

std::string HeadlessChooseMachine(const std::vector<std::string> &names)
{
	MachineSelector selector;
	std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0);
	/* Trailing 0 is the scale: outside 1..TEXT_SCREEN_MAX_SCALE, which
	   text_screen.h defines as unscaled. machine_selector_draw() sets a real
	   one from text_screen_auto_scale() before it draws, so the value here only
	   has to be a defined one. Written out rather than left off so that is a
	   choice rather than whatever the language supplies. */
	TextScreen screen{ pixels.data(), kWidth, kHeight, kWidth, 0 };
	Config app{};
	std::string chosen;

	machine_selector_init(&selector);
	for (const std::string &n : names) {
		machine_selector_add(&selector, n.c_str());
	}

	/*
	 * The port and password come from the app settings, because there is no
	 * machine to ask. This is the case that made moving them worthwhile.
	 */
	app.vnc_port = 5900;
	app.vnc_password[0] = '\0';
	app_settings_load(rpcemu_get_datadir(), &app);

	VncServer &server = VncAppServer();

	server.setKeySymHook(queue_key);
	if (!VncAppStart(true)) {
		fprintf(stderr, "error: could not start the VNC server on port %d for the\n"
		                "       machine selector.\n", app.vnc_port);
		return std::string();
	}

	fprintf(stderr, "Connect a VNC client to port %d to choose a machine.\n",
	        app.vnc_port);
	if (app.vnc_password[0] == '\0') {
		fprintf(stderr, "The server has no password set.\n");
	}
	fflush(stderr);

	machine_selector_draw(&selector, &screen);

	for (;;) {
		uint32_t keysym;
		bool redraw = false;

		/*
		 * Pushed every time round rather than only when something changes, and
		 * with primeFramebuffer() rather than updateFramebuffer(), which skips
		 * the copy while no client is connected. Nothing here will redraw in
		 * response to a client arriving, so without this the first client sees an
		 * empty buffer. At 640x480 and five times a second the copy costs nothing
		 * worth measuring.
		 */
		server.primeFramebuffer(pixels.data(), kWidth, kHeight);

		while (next_key(&keysym)) {
			switch (machine_selector_key(&selector, keysym)) {
			case MACHINE_SELECTOR_CHOSEN: {
				const char *name = machine_selector_current(&selector);

				if (name != nullptr) {
					chosen = name;
				}
				break;
			}
			case MACHINE_SELECTOR_CANCELLED:
				VncAppStop();
				return std::string();
			case MACHINE_SELECTOR_REDRAW:
				redraw = true;
				break;
			case MACHINE_SELECTOR_IGNORED:
				break;
			}
			if (!chosen.empty()) {
				break;
			}
		}

		if (!chosen.empty()) {
			break;
		}
		if (redraw) {
			machine_selector_draw(&selector, &screen);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	/* Left running: the machine attaches to this same server, so the client that
	   just chose keeps its connection. Only the hook is cleared, by VncAppAttach(). */
	return chosen;
}
