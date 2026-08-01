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
	 * With a hook set, keys go to it and never reach the emulator. It is how
	 * something the emulator draws itself - the machine selector before any
	 * machine exists, and any later overlay - receives input, and it is the
	 * reason the server can usefully be constructed with no EmulatorHost at all.
	 *
	 * The hook is called on the server's own event thread, so it must be cheap
	 * and thread-safe; queue and return. Set it before start() to avoid racing
	 * the first client.
	 */
	using KeySymHook = std::function<void(uint32_t keysym, bool down)>;
	void setKeySymHook(KeySymHook hook) { keysym_hook_ = std::move(hook); }
	void stop();
	bool isRunning() const { return running_; }
	int getPort() const { return running_ ? listen_port_ : 0; }
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

	EmulatorHost *emulator_host_;
	rfbScreenInfoPtr rfb_screen_ = nullptr;
	KeySymHook keysym_hook_;
	std::thread event_thread_;
	std::mutex mutex_;
	int current_width_ = 640;
	int current_height_ = 480;
	int listen_port_ = 5900;
	std::atomic<int> client_count_{0};
	std::atomic<bool> force_full_update_{false};
	bool running_ = false;
	char *password_list_[2] = {nullptr, nullptr};
	std::string current_password_;
};

extern VncServer *g_vnc_server;

#endif /* RPCEMU_VNC */

#endif /* HOST_VNC_SERVER_H */
