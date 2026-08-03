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

#ifdef RPCEMU_VNC

#include "vnc_server.h"

#include "emulator_host.h"

extern "C" {
#include "rpcemu.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

VncServer *g_vnc_server = nullptr;

void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl);
void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl);
enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl);
void vnc_client_gone_callback(rfbClientPtr cl);

VncServer::VncServer(EmulatorHost *emulator_host)
{
	emulator_host_.store(emulator_host);
}

VncServer::~VncServer()
{
	stop();
}

void VncServer::configurePixelFormat()
{
	if (!rfb_screen_) {
		return;
	}

	rfb_screen_->serverFormat.bitsPerPixel = 32;
	rfb_screen_->serverFormat.depth = 24;
	rfb_screen_->serverFormat.bigEndian = FALSE;
	rfb_screen_->serverFormat.trueColour = TRUE;
	rfb_screen_->serverFormat.redMax = 255;
	rfb_screen_->serverFormat.greenMax = 255;
	rfb_screen_->serverFormat.blueMax = 255;
	rfb_screen_->serverFormat.redShift = 16;
	rfb_screen_->serverFormat.greenShift = 8;
	rfb_screen_->serverFormat.blueShift = 0;
}

void VncServer::copyFrameLines(const uint32_t *buffer, int width, int start_y, int end_y)
{
	if (!rfb_screen_ || !rfb_screen_->frameBuffer || !buffer || width <= 0) {
		return;
	}

	for (int y = start_y; y < end_y; ++y) {
		const uint32_t *src_row = buffer + (y * width);
		auto *dst_row = reinterpret_cast<uint32_t *>(rfb_screen_->frameBuffer + (y * width * 4));
		for (int x = 0; x < width; ++x) {
			const uint32_t pixel = src_row[x];
			const uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xff);
			const uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xff);
			const uint8_t b = static_cast<uint8_t>(pixel & 0xff);
			dst_row[x] = static_cast<uint32_t>(b) |
			               (static_cast<uint32_t>(g) << 8) |
			               (static_cast<uint32_t>(r) << 16);
		}
	}
}

bool VncServer::start(int port, const std::string &password)
{
	bool restart_needed;

	{
		std::lock_guard<std::mutex> lock(mutex_);

		if (running_ && listen_port_ == port && current_password_ == password) {
			return true;
		}
		restart_needed = running_;
	}

	/* Settings > VNC Server can change the port or password while the server is
	   running. Returning early in that case would leave it listening on the old
	   port with the old password while the config claimed otherwise, so rebind
	   instead. stop() takes the lock itself, hence the scoped block above. */
	if (restart_needed) {
		stop();
	}

	std::lock_guard<std::mutex> lock(mutex_);

	current_password_ = password;
	password_list_[0] = nullptr;
	password_list_[1] = nullptr;

	int argc = 0;
	char *argv[] = {nullptr};

	rfb_screen_ = rfbGetScreen(&argc, argv, current_width_, current_height_, 8, 3, 4);
	if (!rfb_screen_) {
		return false;
	}

	/* libvncserver keeps the pointer rather than copying, so this outlives it. */
	desktop_name_ = config.name[0] != '\0'
	    ? std::string("RPCEmu Extended - ") + config.name
	    : std::string("RPCEmu Extended");
	rfb_screen_->desktopName = const_cast<char *>(desktop_name_.c_str());
	rfb_screen_->alwaysShared = TRUE;
	rfb_screen_->deferUpdateTime = 0;
	rfb_screen_->port = port;
	rfb_screen_->ipv6port = port;
	rfb_screen_->frameBuffer = static_cast<char *>(malloc(static_cast<size_t>(current_width_) * static_cast<size_t>(current_height_) * 4));
	if (!rfb_screen_->frameBuffer) {
		rfbScreenCleanup(rfb_screen_);
		rfb_screen_ = nullptr;
		return false;
	}
	memset(rfb_screen_->frameBuffer, 0, static_cast<size_t>(current_width_) * static_cast<size_t>(current_height_) * 4);

	configurePixelFormat();

	rfb_screen_->kbdAddEvent = vnc_kbd_callback;
	rfb_screen_->ptrAddEvent = vnc_ptr_callback;
	rfb_screen_->newClientHook = vnc_new_client_callback;
	rfb_screen_->screenData = this;

	if (!current_password_.empty()) {
		password_list_[0] = strdup(current_password_.c_str());
		password_list_[1] = nullptr;
		rfb_screen_->authPasswdData = password_list_;
		rfb_screen_->passwordCheck = rfbCheckPasswordByList;
	} else {
		rfb_screen_->authPasswdData = nullptr;
	}

	rfbInitServer(rfb_screen_);

	/* rfbInitServer() returns void and leaves socketState at READY even when the
	   port was taken, so the listening sockets are what distinguish a bind that
	   worked from one that lost the port. Compared against RFB_INVALID_SOCKET
	   rather than -1: rfbSocket is an unsigned SOCKET on Windows, where "< 0" is
	   never true. */
	if (rfb_screen_->listenSock == RFB_INVALID_SOCKET &&
	    rfb_screen_->listen6Sock == RFB_INVALID_SOCKET) {
		rpclog("VNC: could not listen on port %d (is it already in use?)\n", port);
		free(rfb_screen_->frameBuffer); /* freed here, as stop() does, not by the library */
		rfb_screen_->frameBuffer = nullptr;
		rfbScreenCleanup(rfb_screen_);
		rfb_screen_ = nullptr;
		if (password_list_[0] != nullptr) {
			free(password_list_[0]);
			password_list_[0] = nullptr;
		}
		return false;
	}

	listen_port_ = port;
	running_ = true;
	event_thread_ = std::thread([this]() { eventLoop(); });

	rpclog("VNC: server started on port %d\n", port);
	return true;
}

void VncServer::stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_) {
			return;
		}
		running_ = false;
	}

	if (event_thread_.joinable()) {
		event_thread_.join();
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (rfb_screen_) {
		rfbShutdownServer(rfb_screen_, TRUE);
		if (rfb_screen_->frameBuffer) {
			free(rfb_screen_->frameBuffer);
		}
		rfbScreenCleanup(rfb_screen_);
		rfb_screen_ = nullptr;
	}

	if (password_list_[0]) {
		free(password_list_[0]);
		password_list_[0] = nullptr;
	}
	current_password_.clear();
	client_count_.store(0);
	force_full_update_.store(false);
	rpclog("VNC: server stopped\n");
}

void VncServer::updateFramebuffer(const uint32_t *buffer, int width, int height, int yl, int yh)
{
	if (!running_ || !rfb_screen_ || !buffer || client_count_.load() == 0) {
		return;
	}

	/* An overlay owns the screen while it is up: the machine keeps running and
	   keeps drawing, but the client sees the overlay rather than a guest frame
	   painted over the top of it. */
	if (overlay_active_.load()) {
		return;
	}

	int start_y = std::max(0, yl);
	int end_y = std::min(height, yh + 1);
	if (force_full_update_.exchange(false)) {
		start_y = 0;
		end_y = height;
	}
	if (start_y >= end_y) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (width != current_width_ || height != current_height_) {
		if (!resizeFramebuffer(width, height)) {
			return;
		}
	}

	copyFrameLines(buffer, width, start_y, end_y);
	rfbMarkRectAsModified(rfb_screen_, 0, start_y, width, end_y);
}

void VncServer::primeFramebuffer(const uint32_t *buffer, int width, int height)
{
	if (!running_ || !rfb_screen_ || !buffer || width <= 0 || height <= 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (width != current_width_ || height != current_height_) {
		if (!resizeFramebuffer(width, height)) {
			return;
		}
	}
	copyFrameLines(buffer, width, 0, height);
	rfbMarkRectAsModified(rfb_screen_, 0, 0, width, height);
}

void VncServer::releaseAllKeys()
{
	EmulatorHost *host = emulator_host_.load();

	if (host == nullptr) {
		keys_down_.clear();
		return;
	}
	for (const uint32_t keysym : keys_down_) {
		const unsigned int scan_code = keysymToScanCode(keysym);

		if (scan_code != 0xff) {
			host->KeyRelease(scan_code);
		}
	}
	keys_down_.clear();
}

void VncServer::setOverlayActive(bool active)
{
	const bool was = overlay_active_.exchange(active);

	if (was && !active) {
		/* Coming back to the guest: ask for the whole screen, because the parts
		   the overlay covered are unchanged as far as the guest is concerned and
		   would otherwise not be resent until something happened to touch them. */
		force_full_update_.store(true);
	}
}

void VncServer::processEvents()
{
	if (!running_ || !rfb_screen_) {
		return;
	}

	// Must not hold mutex_ here: libvncserver invokes callbacks (e.g. newClientHook)
	// from within rfbProcessEvents, and those must not deadlock on the same lock.
	rfbProcessEvents(rfb_screen_, 0);
}

void VncServer::eventLoop()
{
	while (running_) {
		processEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool VncServer::resizeFramebuffer(int width, int height)
{
	if (!rfb_screen_) {
		return false;
	}

	char *new_buffer = static_cast<char *>(realloc(rfb_screen_->frameBuffer, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
	if (!new_buffer) {
		return false;
	}

	rfb_screen_->frameBuffer = new_buffer;
	memset(rfb_screen_->frameBuffer, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
	rfbNewFramebuffer(rfb_screen_, rfb_screen_->frameBuffer, width, height, 8, 3, 4);
	configurePixelFormat();
	current_width_ = width;
	current_height_ = height;
	return true;
}

unsigned int VncServer::keysymToScanCode(rfbKeySym keysym) const
{
	// Map X11/VNC keysyms to the host native scancodes used by keyboard_x.c.
	switch (keysym) {
	case XK_Escape: return 0x09;
	case XK_F1: return 0x43;
	case XK_F2: return 0x44;
	case XK_F3: return 0x45;
	case XK_F4: return 0x46;
	case XK_F5: return 0x47;
	case XK_F6: return 0x48;
	case XK_F7: return 0x49;
	case XK_F8: return 0x4a;
	case XK_F9: return 0x4b;
	case XK_F10: return 0x4c;
	case XK_F11: return 0x5f;
	case XK_F12: return 0x60;
	case XK_Scroll_Lock: return 0x4e;

	case XK_grave:
	case XK_asciitilde: return 0x31;
	case XK_1:
	case XK_exclam: return 0x0a;
	case XK_2:
	case XK_at: return 0x0b;
	case XK_3:
	case XK_numbersign: return 0x0c;
	case XK_4:
	case XK_dollar: return 0x0d;
	case XK_5:
	case XK_percent: return 0x0e;
	case XK_6:
	case XK_asciicircum: return 0x0f;
	case XK_7:
	case XK_ampersand: return 0x10;
	case XK_8:
	case XK_asterisk: return 0x11;
	case XK_9:
	case XK_parenleft: return 0x12;
	case XK_0:
	case XK_parenright: return 0x13;
	case XK_minus:
	case XK_underscore: return 0x14;
	case XK_equal:
	case XK_plus: return 0x15;
	case XK_sterling: return 0x33;
	case XK_BackSpace: return 0x16;
	case XK_Insert: return 0x76;

	case XK_Tab: return 0x17;
	case XK_q:
	case XK_Q: return 0x18;
	case XK_w:
	case XK_W: return 0x19;
	case XK_e:
	case XK_E: return 0x1a;
	case XK_r:
	case XK_R: return 0x1b;
	case XK_t:
	case XK_T: return 0x1c;
	case XK_y:
	case XK_Y: return 0x1d;
	case XK_u:
	case XK_U: return 0x1e;
	case XK_i:
	case XK_I: return 0x1f;
	case XK_o:
	case XK_O: return 0x20;
	case XK_p:
	case XK_P: return 0x21;
	case XK_bracketleft:
	case XK_braceleft: return 0x22;
	case XK_bracketright:
	case XK_braceright: return 0x23;
	case XK_Return: return 0x24;
	case XK_Delete: return 0x77;
	case XK_End: return 0x73;
	case XK_Page_Down: return 0x75;

	case XK_Caps_Lock: return 0x42;
	case XK_a:
	case XK_A: return 0x26;
	case XK_s:
	case XK_S: return 0x27;
	case XK_d:
	case XK_D: return 0x28;
	case XK_f:
	case XK_F: return 0x29;
	case XK_g:
	case XK_G: return 0x2a;
	case XK_h:
	case XK_H: return 0x2b;
	case XK_j:
	case XK_J: return 0x2c;
	case XK_k:
	case XK_K: return 0x2d;
	case XK_l:
	case XK_L: return 0x2e;
	case XK_semicolon:
	case XK_colon: return 0x2f;
	case XK_apostrophe:
	case XK_quotedbl: return 0x30;
	case XK_Home: return 0x6e;
	case XK_Page_Up: return 0x70;
	case XK_Num_Lock: return 0x4d;

	case XK_Shift_L: return 0x32;
	case XK_backslash:
	case XK_bar: return 0x5e;
	case XK_z:
	case XK_Z: return 0x34;
	case XK_x:
	case XK_X: return 0x35;
	case XK_c:
	case XK_C: return 0x36;
	case XK_v:
	case XK_V: return 0x37;
	case XK_b:
	case XK_B: return 0x38;
	case XK_n:
	case XK_N: return 0x39;
	case XK_m:
	case XK_M: return 0x3a;
	case XK_comma:
	case XK_less: return 0x3b;
	case XK_period:
	case XK_greater: return 0x3c;
	case XK_slash:
	case XK_question: return 0x3d;
	case XK_Shift_R: return 0x3e;
	case XK_Up: return 0x6f;

	case XK_Control_L: return 0x25;
	case XK_Alt_L: return 0x40;
	case XK_space: return 0x41;
	case XK_Alt_R: return 0x40;
	case XK_Control_R: return 0x69;
	case XK_Left: return 0x71;
	case XK_Down: return 0x74;
	case XK_Right: return 0x72;

	case XK_KP_Divide: return 0x6a;
	case XK_KP_Multiply: return 0x3f;
	case XK_KP_Subtract: return 0x52;
	case XK_KP_7:
	case XK_KP_Home: return 0x4f;
	case XK_KP_8:
	case XK_KP_Up: return 0x50;
	case XK_KP_9:
	case XK_KP_Page_Up: return 0x51;
	case XK_KP_Add: return 0x56;
	case XK_KP_4:
	case XK_KP_Left: return 0x53;
	case XK_KP_5: return 0x54;
	case XK_KP_6:
	case XK_KP_Right: return 0x55;
	case XK_KP_1:
	case XK_KP_End: return 0x57;
	case XK_KP_2:
	case XK_KP_Down: return 0x58;
	case XK_KP_3:
	case XK_KP_Page_Down: return 0x59;
	case XK_KP_Enter: return 0x68;
	case XK_KP_0:
	case XK_KP_Insert: return 0x5a;
	case XK_KP_Decimal:
	case XK_KP_Delete: return 0x5b;

	default: return 0xff;
	}
}

void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl)
{
	auto *server = static_cast<VncServer *>(cl->screen->screenData);
	if (!server) {
		return;
	}

	/* Offered first to whatever the emulator is drawing itself. It returns true
	   if it swallowed the key, which is how an overlay can watch for its own
	   shortcut without stealing everything else from the guest. Checked before the
	   host, so this works with no emulator at all. */
	if (server->keysym_hook_ &&
	    server->keysym_hook_(static_cast<uint32_t>(keysym), down != FALSE)) {
		return;
	}

	EmulatorHost *host = server->emulator_host_.load();

	if (!host) {
		return;
	}

	const unsigned int scan_code = server->keysymToScanCode(keysym);
	if (scan_code == 0xff) {
		return;
	}

	/* Remembered so releaseAllKeys() can undo them if input is diverted before
	   the key comes back up. */
	if (down) {
		server->keys_down_.insert(static_cast<uint32_t>(keysym));
		host->KeyPress(scan_code);
	} else if (server->keys_down_.erase(static_cast<uint32_t>(keysym)) > 0) {
		host->KeyRelease(scan_code);
	}
	/*
	 * A release for a key the guest never saw pressed is dropped. That happens
	 * every time an overlay closes: the shortcut's modifiers went down before it
	 * opened, were released for the guest as it opened, and the client's real
	 * releases arrive afterwards. Sending those on would be telling the machine to
	 * let go of keys it is not holding.
	 */
}

void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl)
{
	auto *server = static_cast<VncServer *>(cl->screen->screenData);
	EmulatorHost *host = server ? server->emulator_host_.load() : nullptr;

	if (!host) {
		return;
	}

	host->MouseMove(x, y);

	/* RFB reports buttons as bit 0 = left, bit 1 = middle, bit 2 = right. The
	   guest button encoding (matching the native panel's MapClickButton) is
	   left = 1, right = 2, middle = 4, so remap rather than pass the raw mask -
	   otherwise the right button acts as Menu and the middle does nothing. */
	int guest = 0;
	if (buttonMask & (1 << 0)) guest |= 1; /* left   */
	if (buttonMask & (1 << 1)) guest |= 4; /* middle */
	if (buttonMask & (1 << 2)) guest |= 2; /* right  */

	static int last_buttons = 0;
	const int pressed = guest & ~last_buttons;
	const int released = last_buttons & ~guest;
	if (pressed) {
		host->MousePress(pressed);
	}
	if (released) {
		host->MouseRelease(released);
	}
	last_buttons = guest;

	/* The scroll wheel is delivered as momentary clicks of RFB buttons 4 (up)
	   and 5 (down). Send one wheel notch on each press edge, matching the
	   native panel which forwards a wxWidgets wheel rotation of +/-120. */
	static int last_wheel = 0;
	const int wheel = buttonMask & ((1 << 3) | (1 << 4));
	const int wheel_pressed = wheel & ~last_wheel;
	if (wheel_pressed & (1 << 3)) {
		host->MouseWheel(120);
	}
	if (wheel_pressed & (1 << 4)) {
		host->MouseWheel(-120);
	}
	last_wheel = wheel;
}

enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl)
{
	auto *server = static_cast<VncServer *>(cl->screen->screenData);
	if (!server) {
		return RFB_CLIENT_REFUSE;
	}

	cl->clientGoneHook = vnc_client_gone_callback;
	server->client_count_.fetch_add(1);
	server->force_full_update_.store(true);
	if (cl->host) {
		rpclog("VNC: client connected from %s\n", cl->host);
	}

	return RFB_CLIENT_ACCEPT;
}

void vnc_client_gone_callback(rfbClientPtr cl)
{
	auto *server = static_cast<VncServer *>(cl->screen->screenData);
	if (!server) {
		return;
	}
	server->client_count_.fetch_sub(1);
	if (cl->host) {
		rpclog("VNC: client disconnected from %s\n", cl->host);
	}
}

#endif /* RPCEMU_VNC */
