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

/* See vnc_app.h. */

#include <memory>
#include <string>

extern "C" {
#include "rpcemu.h"
#include "app_settings.h"
#include "machine_lock.h"
}

#include "vnc_app.h"
#include "vnc_overlay.h"
#include "vnc_server.h"

namespace {

/*
 * Created on first use rather than at static-initialisation time, so it cannot
 * race the data directory being resolved - the port and password come from a file
 * whose location is not known until then.
 */
std::unique_ptr<VncServer> g_server;
int g_port = 0;

} /* namespace */

VncServer &VncAppServer()
{
	if (!g_server) {
		g_server = std::make_unique<VncServer>(nullptr);
		/* The frame-push path in emulator_host.cpp finds the server through this
		   global, so it has to point at the one server for the whole process. */
		g_vnc_server = g_server.get();
	}
	return *g_server;
}

bool VncAppStart(bool force)
{
	VncServer &server = VncAppServer();
	Config app{};

	if (server.isRunning()) {
		return true;
	}

	/*
	 * A machine's VNC server is that machine's setting, so once one is loaded its
	 * values are the answer: config_load() has already settled them, having put the
	 * app settings file underneath as the default for anything the machine does not
	 * mention. Overlaying that file again here would undo the machine's own port,
	 * which is exactly what it used to do.
	 *
	 * With no machine loaded there is nothing to read, and the file is all there
	 * is. That is the headless selector, which has to be reachable before there is
	 * a machine to ask.
	 */
	if (config_machine_loaded()) {
		app.vnc_enabled = config.vnc_enabled;
		app.vnc_port = (config.vnc_port > 0) ? config.vnc_port : 5900;
		snprintf(app.vnc_password, sizeof(app.vnc_password), "%s",
		    config.vnc_password);
	} else {
		app.vnc_enabled = 0;
		app.vnc_port = 5900;
		app.vnc_password[0] = '\0';
		app_settings_load(rpcemu_get_datadir(), &app);
	}
	app_settings_apply_overrides(&app);

	if (!force && !app.vnc_enabled) {
		return false;
	}

	if (!server.start(app.vnc_port, std::string(app.vnc_password))) {
		rpclog("vnc_app: could not listen on port %d\n", app.vnc_port);
		return false;
	}
	/* What it actually got, which may not be what was asked for - see
	   VncServer::start(). Recording the requested port would send anyone
	   reading the lock file to a port nothing is listening on. */
	g_port = server.getPort();

	/* Now it is real: record it so anything reading the machine's lock file is told
	   a port that is actually listening. */
	machine_lock_set_vnc_port(g_port);
	return true;
}

void VncAppStop()
{
	machine_lock_set_vnc_port(0);
	if (g_server) {
		g_server->setEmulatorHost(nullptr);
		g_server->setKeySymHook(nullptr);
		g_server->stop();
	}
	g_port = 0;
}

bool VncAppRunning()
{
	return g_server && g_server->isRunning();
}

void VncAppAttach(EmulatorHost *host)
{
	VncServer &server = VncAppServer();

	/* The selector's hook swallowed every key; a machine needs them, so it is
	   replaced rather than merely cleared. The overlay's hook passes everything
	   through until its own shortcut is pressed, so the guest is unaffected. */
	server.setKeySymHook(nullptr);
	server.setEmulatorHost(host);
	VncOverlayAttach(host);
}

void VncAppDetach()
{
	if (g_server) {
		VncOverlayAttach(nullptr);
		g_server->setEmulatorHost(nullptr);
	}
}

int VncAppPort()
{
	return VncAppRunning() ? g_port : 0;
}
