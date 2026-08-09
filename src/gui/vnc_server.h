/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#ifndef HOST_VNC_SERVER_H
#define HOST_VNC_SERVER_H

#ifdef RPCEMU_VNC

#include <atomic>
#include <cstdint>
#include <mutex>
#include <functional>
#include <set>
#include <string>
#include <thread>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

class EmulatorHost;

class VncServer {
public:
	explicit VncServer(EmulatorHost *emulator_host);
	~VncServer();

	bool start(int port, const std::string &password);

	/*
	 * Divert keysyms away from the guest.
	 *
	 * The hook sees every key first and says whether it swallowed it: true means
	 * the guest never hears about it, false means carry on as normal. That lets
	 * something the emulator draws itself - the machine selector before a machine
	 * exists, or an overlay over a running one - take input while a machine that
	 * is not covered up keeps working. It is also why the server can usefully be
	 * constructed with no EmulatorHost at all.
	 *
	 * The hook is called on the server's own event thread, so it must be cheap
	 * and thread-safe; queue and return. Set it before start() to avoid racing
	 * the first client.
	 */
	using KeySymHook = std::function<bool(uint32_t keysym, bool down)>;
	void setKeySymHook(KeySymHook hook) { keysym_hook_ = std::move(hook); }
	void stop();
	bool isRunning() const { return running_; }
	int getPort() const { return running_ ? listen_port_ : 0; }

	/* Bind one specific port. start() calls this for each candidate. */
	bool startOnPort(int port, const std::string &password);
	int getClientCount() const { return client_count_.load(); }

	void updateFramebuffer(const uint32_t *buffer, int width, int height, int yl, int yh);

	/*
	 * Put an image in the framebuffer whether or not anyone is watching.
	 *
	 * updateFramebuffer() skips the copy when no client is connected, which is
	 * right for a guest: there is no point converting frames nobody will see, and
	 * the next connection gets a full update anyway. It is wrong for a still
	 * image the emulator drew itself, such as the machine selector, because
	 * nothing will redraw it in response to a client arriving - so the client
	 * connects and is shown an empty buffer.
	 */
	void primeFramebuffer(const uint32_t *buffer, int width, int height);

	/*
	 * Point the server at a machine, or at none.
	 *
	 * Input goes to whatever is attached, so detaching leaves a connected client
	 * able to look but not touch - which is what should happen between machines.
	 */
	void setEmulatorHost(EmulatorHost *host) { emulator_host_.store(host); }

	/*
	 * While an overlay is up, guest frames are not copied to the client: the
	 * emulator carries on running and drawing, but what the client sees is
	 * whatever the overlay primed. Clearing it forces a full update so the guest's
	 * screen comes back in one go rather than a line at a time as it happens to
	 * change.
	 */
	/*
	 * Tell the guest that every key it thinks is held is now up.
	 *
	 * Needed whenever input stops being passed through mid-keystroke, which is
	 * what happens when an overlay opens: the presses reached the guest and the
	 * releases will not, so without this it is left believing keys are still down.
	 * Held modifiers are the worst of it, because everything typed afterwards is
	 * interpreted through them.
	 */
	void releaseAllKeys();

	void setOverlayActive(bool active);
	bool overlayActive() const { return overlay_active_.load(); }

	/** Send the whole screen on the next update, not just the changed lines. */
	void forceFullUpdate() { force_full_update_.store(true); }

	/** The framebuffer's current size, so an overlay can match it. */
	int width() const { return current_width_; }
	int height() const { return current_height_; }
	EmulatorHost *emulatorHost() const { return emulator_host_.load(); }
	void processEvents();

	friend enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl);
	friend void vnc_client_gone_callback(rfbClientPtr cl);
	friend void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl);
	friend void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl);

private:
	unsigned int keysymToScanCode(rfbKeySym keysym) const;
	bool resizeFramebuffer(int width, int height);
	void configurePixelFormat();
	void copyFrameLines(const uint32_t *buffer, int width, int start_y, int end_y);
	void eventLoop();

	/*
	 * The emulator to deliver input to, or nullptr when there is none.
	 *
	 * Atomic because it is read on the server's event thread every time a key or
	 * a mouse event arrives, and written on the GUI thread when a machine starts
	 * or stops. The server now outlives any particular machine, which is what
	 * lets it show the selector before one exists and keep the same connection
	 * across a machine starting.
	 */
	std::atomic<EmulatorHost *> emulator_host_{nullptr};
	rfbScreenInfoPtr rfb_screen_ = nullptr;
	KeySymHook keysym_hook_;
	std::thread event_thread_;
	std::mutex mutex_;
	int current_width_ = 640;
	int current_height_ = 480;
	/* How many consecutive ports to try before giving up. Enough for every
	   machine that can run at once (NET_SLOT_MAX) with room to spare. */
	static constexpr int kPortAttempts = 16;

	int listen_port_ = 5900;
	std::atomic<int> client_count_{0};
	std::atomic<bool> force_full_update_{false};
	std::atomic<bool> overlay_active_{false};

	/* Keysyms currently down as far as the guest is concerned, so they can be
	   released if input is diverted before they come back up. Touched only on the
	   server's event thread. */
	std::set<uint32_t> keys_down_;
	bool running_ = false;
	char *password_list_[2] = {nullptr, nullptr};
	std::string current_password_;
	std::string desktop_name_;
};

extern VncServer *g_vnc_server;

#endif /* RPCEMU_VNC */

#endif /* HOST_VNC_SERVER_H */
