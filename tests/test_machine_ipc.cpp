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
 * Exercises the two halves of machine_ipc.h in-process: a SharedFramebuffer
 * writer/reader pair standing in for a --managed child and the Manager
 * window, and a MachineIpcServer/MachineIpcClient pair carrying requests and
 * events over a real local socket. Nothing here needs a display, ROMs or a
 * running emulator - it is purely the transport the Manager window and a
 * managed machine talk over.
 */

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/gui/machine_ipc.h"

static int failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

static void test_shared_framebuffer()
{
	char name[64];
	std::snprintf(name, sizeof(name), "/rpcemu-test-fb-%ld", (long) getpid());

	SharedFramebuffer writer;
	CHECK(writer.CreateNew(name));

	SharedFramebuffer reader;
	CHECK(reader.OpenExisting(name));

	/* Nothing published yet. */
	std::vector<uint32_t> out;
	int w = 0, h = 0;
	CHECK(!reader.ReadInto(&out, &w, &h));

	std::vector<uint32_t> frame(64u * 48u);
	for (size_t i = 0; i < frame.size(); i++) {
		frame[i] = 0xFF000000u | (uint32_t) (i & 0xFFFFFFu);
	}
	writer.Publish(frame.data(), 64, 48);

	CHECK(reader.ReadInto(&out, &w, &h));
	CHECK(w == 64);
	CHECK(h == 48);
	CHECK(out == frame);

	/* A second, differently-sized frame: the reader must pick up both the
	   new pixels and the new dimensions together, never a mix of the two. */
	std::vector<uint32_t> frame2(32u * 16u, 0x11223344u);
	writer.Publish(frame2.data(), 32, 16);

	CHECK(reader.ReadInto(&out, &w, &h));
	CHECK(w == 32);
	CHECK(h == 16);
	CHECK(out == frame2);

	writer.Close();
}

/*
 * Regression test for a real bug caught by manually driving the Manager
 * window: MachineIpcNameFor() used to call getpid() internally with no way
 * to override it, so the managed child (creating the segment, its own pid)
 * and the Manager (opening it, the Manager's own pid) computed two different
 * names for the same machine and OpenExisting() always failed. The name must
 * depend only on the machine directory and an explicitly passed pid - the
 * child's pid, supplied by both sides - never on whichever process happens
 * to be calling.
 */
static void test_ipc_name_for()
{
	const std::string dir = "/tmp/some/machine/dir/";

	CHECK(MachineIpcNameFor(dir, 111) == MachineIpcNameFor(dir, 111));
	CHECK(MachineIpcNameFor(dir, 111) != MachineIpcNameFor(dir, 222));
	CHECK(MachineIpcNameFor(dir, 111) != MachineIpcNameFor("/tmp/other/dir/", 111));
}

static void test_ipc_roundtrip()
{
#ifdef _WIN32
	const std::string endpoint;	/* Start() ignores it and picks a TCP port */
#else
	char path[128];
	std::snprintf(path, sizeof(path), "/tmp/rpcemu-test-ipc-%ld.sock", (long) getpid());
	const std::string endpoint = path;
#endif

	std::mutex m;
	std::condition_variable cv;
	std::vector<IpcRequest> received;
	std::vector<IpcEvent> events;

	MachineIpcServer server;
	const bool server_ok = server.Start(endpoint, [&](const IpcRequest &req) {
		std::lock_guard<std::mutex> lock(m);
		received.push_back(req);
		cv.notify_all();
	});
	CHECK(server_ok);
	CHECK(!server.BoundEndpoint().empty());

	MachineIpcClient client;
	const bool client_ok = client.Connect(server.BoundEndpoint(), [&](const IpcEvent &ev) {
		std::lock_guard<std::mutex> lock(m);
		events.push_back(ev);
		cv.notify_all();
	});
	CHECK(client_ok);

	IpcRequest key_request;
	key_request.type = IpcRequestType::KeyPress;
	key_request.arg1 = 42;
	client.Send(key_request);

	IpcRequest disc_request;
	disc_request.type = IpcRequestType::LoadDisc0;
	std::snprintf(disc_request.path, sizeof(disc_request.path), "%s", "/tmp/disc.adf");
	client.Send(disc_request);

	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait_for(lock, std::chrono::seconds(5), [&] { return received.size() >= 2; });
	}
	CHECK(received.size() == 2);
	if (received.size() == 2) {
		CHECK(received[0].type == IpcRequestType::KeyPress);
		CHECK(received[0].arg1 == 42);
		CHECK(received[1].type == IpcRequestType::LoadDisc0);
		CHECK(std::string(received[1].path) == "/tmp/disc.adf");
	}

	IpcEvent frame_event;
	frame_event.type = IpcEventType::FrameReady;
	server.SendEvent(frame_event);

	IpcEvent quit_event;
	quit_event.type = IpcEventType::Quit;
	server.SendEvent(quit_event);

	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait_for(lock, std::chrono::seconds(5), [&] { return events.size() >= 2; });
	}
	CHECK(events.size() == 2);
	if (events.size() == 2) {
		CHECK(events[0].type == IpcEventType::FrameReady);
		CHECK(events[1].type == IpcEventType::Quit);
	}

	client.Disconnect();
	server.Stop();
}

/*
 * A forwarded menu command, and the tick-box state that comes back.
 *
 * This is the whole of the Manager's menu bar in one message type: the id says
 * which command, arg2 carries a tick-box's new state (or Create Disc's chosen
 * file type), and path carries a file the Manager's own dialogue picked. If any
 * of those three failed to survive the trip, the command that ran in the machine
 * would not be the one the user chose - and it would report success either way,
 * which is exactly the failure this is here to catch.
 *
 * The reply direction matters as much: a StateReport the Manager cannot read
 * leaves its tick-boxes showing something the machine does not agree with.
 */
void test_menu_command_roundtrip()
{
	std::printf("\nForwarded menu commands\n");

#ifdef _WIN32
	const std::string endpoint;	/* Start() ignores it and picks a TCP port */
#else
	char path[128];
	std::snprintf(path, sizeof(path), "/tmp/rpcemu-test-menu-%ld.sock", (long) getpid());
	const std::string endpoint = path;
#endif

	MachineIpcServer server;
	MachineIpcClient client;
	std::mutex m;
	std::condition_variable cv;
	std::vector<IpcRequest> received;
	std::vector<IpcEvent> events;

	const bool server_ok = server.Start(endpoint, [&](const IpcRequest &req) {
		std::lock_guard<std::mutex> lock(m);
		received.push_back(req);
		cv.notify_all();
	});
	CHECK(server_ok);

	const bool client_ok = client.Connect(server.BoundEndpoint(), [&](const IpcEvent &ev) {
		std::lock_guard<std::mutex> lock(m);
		events.push_back(ev);
		cv.notify_all();
	});
	CHECK(client_ok);

	/* A tick-box: the id and the state it was moved to. */
	IpcRequest toggle;
	toggle.type = IpcRequestType::MenuCommand;
	toggle.arg1 = 5006;		/* a menu id; the value is opaque to the transport */
	toggle.arg2 = 1;		/* now ticked */
	client.Send(toggle);

	/* A command carrying a file chosen in the Manager, and - for Create Disc -
	   which entry of the file-type list was chosen, in the same field a
	   tick-box would use. No command needs both. */
	IpcRequest with_file;
	with_file.type = IpcRequestType::MenuCommand;
	with_file.arg1 = 5011;
	with_file.arg2 = 3;		/* the fourth disc type */
	std::snprintf(with_file.path, sizeof(with_file.path), "%s",
	    "/tmp/a directory with spaces/blank disc.adf");
	client.Send(with_file);

	IpcRequest ask_state;
	ask_state.type = IpcRequestType::RequestState;
	client.Send(ask_state);

	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait_for(lock, std::chrono::seconds(5), [&] { return received.size() >= 3; });
	}
	CHECK(received.size() == 3);
	if (received.size() == 3) {
		CHECK(received[0].type == IpcRequestType::MenuCommand);
		CHECK(received[0].arg1 == 5006);
		CHECK(received[0].arg2 == 1);

		CHECK(received[1].type == IpcRequestType::MenuCommand);
		CHECK(received[1].arg1 == 5011);
		CHECK(received[1].arg2 == 3);
		CHECK(std::string(received[1].path) ==
		    "/tmp/a directory with spaces/blank disc.adf");

		CHECK(received[2].type == IpcRequestType::RequestState);
	}

	/* The answer: the pairs the Manager parses to set its own tick-boxes. */
	IpcEvent state;
	state.type = IpcEventType::StateReport;
	std::snprintf(state.path, sizeof(state.path), "%s", "5006=1 5007=0 5008=1");
	server.SendEvent(state);

	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait_for(lock, std::chrono::seconds(5), [&] { return !events.empty(); });
	}
	CHECK(!events.empty());
	if (!events.empty()) {
		CHECK(events[0].type == IpcEventType::StateReport);
		CHECK(std::string(events[0].path) == "5006=1 5007=0 5008=1");
	}

	client.Disconnect();
	server.Stop();
}

int main()
{
	test_shared_framebuffer();
	test_ipc_name_for();
	test_ipc_roundtrip();
	test_menu_command_roundtrip();

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("All machine_ipc tests passed\n");
	return 0;
}
