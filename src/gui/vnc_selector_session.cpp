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

/* See vnc_selector_session.h. */

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <wx/dialog.h>

extern "C" {
#include "rpcemu.h"
#include "machine_selector.h"
#include "text_screen.h"
}

#include "vnc_app.h"
#include "vnc_selector_session.h"
#include "vnc_server.h"

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

/*
 * State shared between the VNC server's event thread, the drawing thread and the
 * GUI thread. Small enough to keep behind one mutex; the alternative is three
 * atomics and a subtle ordering argument.
 */
struct Shared {
	std::mutex mutex;
	std::deque<uint32_t> keys;
	std::string chosen;
	bool finished = false;
};

Shared *g_shared = nullptr;

void queue_key(uint32_t keysym, bool down)
{
	if (!down || g_shared == nullptr) {
		return;	/* on press only, or a release would double every keystroke */
	}
	std::lock_guard<std::mutex> lock(g_shared->mutex);

	if (g_shared->keys.size() < 20) {
		g_shared->keys.push_back(keysym);
	}
}

/*
 * The list is drawn and the keys acted on here, off the GUI thread, because the
 * GUI thread is inside a modal event loop and cannot run anything of ours.
 */
void serve(Shared *shared, std::vector<std::string> names, wxDialog *dialog)
{
	MachineSelector selector;
	std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0);
	TextScreen screen{ pixels.data(), kWidth, kHeight, kWidth };
	VncServer &server = VncAppServer();

	machine_selector_init(&selector);
	for (const std::string &n : names) {
		machine_selector_add(&selector, n.c_str());
	}
	machine_selector_draw(&selector, &screen);

	for (;;) {
		std::string picked;
		bool stop = false;
		bool redraw = false;

		server.primeFramebuffer(pixels.data(), kWidth, kHeight);

		{
			std::lock_guard<std::mutex> lock(shared->mutex);

			if (shared->finished) {
				break;
			}
			while (!shared->keys.empty()) {
				const uint32_t keysym = shared->keys.front();

				shared->keys.pop_front();
				switch (machine_selector_key(&selector, keysym)) {
				case MACHINE_SELECTOR_CHOSEN: {
					const char *name = machine_selector_current(&selector);

					if (name != nullptr) {
						picked = name;
						shared->chosen = name;
					}
					break;
				}
				case MACHINE_SELECTOR_CANCELLED:
					/* A remote Escape closes the remote list only. The person at
					   the keyboard has not cancelled anything. */
					stop = true;
					break;
				case MACHINE_SELECTOR_REDRAW:
					redraw = true;
					break;
				case MACHINE_SELECTOR_IGNORED:
					break;
				}
				if (!picked.empty() || stop) {
					break;
				}
			}
		}

		if (!picked.empty()) {
			/*
			 * End the local dialogue from the GUI thread. CallAfter is the only
			 * safe way to touch a window from here, and wxID_CANCEL rather than
			 * wxID_OK because the dialogue has no selection of its own to report -
			 * the caller checks chosen() first and treats it as the answer.
			 */
			if (dialog != nullptr) {
				dialog->CallAfter([dialog]() { dialog->EndModal(wxID_CANCEL); });
			}
			break;
		}
		if (stop) {
			break;
		}
		if (redraw) {
			machine_selector_draw(&selector, &screen);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

std::thread g_thread;

} /* namespace */

VncSelectorSession::VncSelectorSession(const std::vector<std::string> &names,
                                       wxDialog *dialog)
{
	/*
	 * Started only if VNC is already meant to be on. This is not the moment to
	 * open a port the user has not asked for: it is a listening socket, and one
	 * that presently lets whoever reaches it choose which machine boots.
	 */
	if (!VncAppStart(false)) {
		return;
	}

	g_shared = new Shared();
	VncAppServer().setKeySymHook(queue_key);
	g_thread = std::thread(serve, g_shared, names, dialog);
	active_ = true;
	rpclog("vnc_selector: offering %zu machines on port %d\n", names.size(),
	    VncAppPort());
}

VncSelectorSession::~VncSelectorSession()
{
	if (!active_) {
		delete g_shared;
		g_shared = nullptr;
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_shared->mutex);
		g_shared->finished = true;
	}
	if (g_thread.joinable()) {
		g_thread.join();
	}

	/* The hook goes before the machine does, or the guest would never see a key.
	   VncAppAttach() also clears it, but not until a machine exists. */
	VncAppServer().setKeySymHook(nullptr);

	delete g_shared;
	g_shared = nullptr;
	active_ = false;
}

std::string VncSelectorSession::chosen() const
{
	if (g_shared == nullptr) {
		return std::string();
	}
	std::lock_guard<std::mutex> lock(g_shared->mutex);
	return g_shared->chosen;
}
