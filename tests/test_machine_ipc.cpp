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
#include <winsock2.h>
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
	writer.Publish(frame.data(), 64, 48, 0, 48);

	CHECK(reader.ReadInto(&out, &w, &h));
	CHECK(w == 64);
	CHECK(h == 48);
	CHECK(out == frame);

	/* A second, differently-sized frame: the reader must pick up both the
	   new pixels and the new dimensions together, never a mix of the two. */
	std::vector<uint32_t> frame2(32u * 16u, 0x11223344u);
	writer.Publish(frame2.data(), 32, 16, 0, 16);

	CHECK(reader.ReadInto(&out, &w, &h));
	CHECK(w == 32);
	CHECK(h == 16);
	CHECK(out == frame2);

	writer.Close();
}

/*
 * Publishing only the rows that changed must still hand the reader a whole
 * frame.
 *
 * Publish() copies [dirty_top, dirty_bottom) into whichever slot is free, so a
 * slot that has sat out a few frames is behind by the rows those frames changed
 * and has to catch up on them - it carries the union of the ranges it missed. A
 * mistake there does not fail loudly: it leaves a band of rows holding a frame
 * two or three old, which on a real screen looks like a patch that will not
 * repaint. So this drives a long sequence of arbitrary ranges against a model of
 * what the frame should be, and compares every published frame in full.
 */
static void test_shared_framebuffer_dirty_rows()
{
	char name[64];
	std::snprintf(name, sizeof(name), "/rpcemu-test-fbd-%ld", (long) getpid());

	SharedFramebuffer writer;
	CHECK(writer.CreateNew(name));
	SharedFramebuffer reader;
	CHECK(reader.OpenExisting(name));

	const int w = 40, h = 30;
	std::vector<uint32_t> source((size_t) w * (size_t) h, 0u);
	std::vector<uint32_t> out;
	int gw = 0, gh = 0;
	uint32_t tick = 1;

	/* A deliberately fixed sequence rather than a random one, so a failure is
	   the same failure every time it is run. */
	static const int ranges[][2] = {
		{ 0, 30 }, { 0, 1 }, { 29, 30 }, { 10, 11 }, { 10, 12 },
		{ 0, 5 }, { 25, 30 }, { 12, 13 }, { 3, 4 }, { 15, 16 },
		{ 0, 30 }, { 7, 8 }, { 8, 9 }, { 9, 10 }, { 1, 2 },
		{ 20, 21 }, { 20, 22 }, { 4, 6 }, { 0, 2 }, { 28, 29 },
	};

	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		const int top = ranges[i][0];
		const int bottom = ranges[i][1];

		/* Change the source exactly where we are about to say it changed. */
		for (int y = top; y < bottom; y++) {
			for (int x = 0; x < w; x++) {
				source[(size_t) y * (size_t) w + (size_t) x] =
				    (tick << 8) | (uint32_t) y;
			}
		}
		tick++;

		writer.Publish(source.data(), w, h, top, bottom);

		CHECK(reader.ReadInto(&out, &gw, &gh));
		CHECK(gw == w);
		CHECK(gh == h);
		/* The whole frame, not just the rows just published. */
		CHECK(out == source);
	}

	/* An empty range means the whole frame, as it did before the range
	   existed. Change everything and say nothing changed. */
	for (size_t i = 0; i < source.size(); i++) {
		source[i] = 0xDEADBEEFu;
	}
	writer.Publish(source.data(), w, h, 0, 0);
	CHECK(reader.ReadInto(&out, &gw, &gh));
	CHECK(out == source);

	/* So does a range the wrong way round, or one off the end. */
	for (size_t i = 0; i < source.size(); i++) {
		source[i] = 0x5A5A5A5Au;
	}
	writer.Publish(source.data(), w, h, 20, 5);
	CHECK(reader.ReadInto(&out, &gw, &gh));
	CHECK(out == source);

	for (size_t i = 0; i < source.size(); i++) {
		source[i] = 0x0BADF00Du;
	}
	writer.Publish(source.data(), w, h, 0, h + 10);
	CHECK(reader.ReadInto(&out, &gw, &gh));
	CHECK(out == source);

	/* A geometry change leaves every slot holding nothing usable, so the next
	   frame at the new size must be copied whole however small its range. */
	const int w2 = 24, h2 = 18;
	std::vector<uint32_t> small((size_t) w2 * (size_t) h2, 0x12345678u);
	writer.Publish(small.data(), w2, h2, 4, 5);
	CHECK(reader.ReadInto(&out, &gw, &gh));
	CHECK(gw == w2);
	CHECK(gh == h2);
	CHECK(out == small);

	writer.Close();
}

/*
 * Regression test for a real bug caught by manually driving the Manager
 * window: MachineIpcNameFor() used to call getpid() internally with no way
 * to override it, so the managed child (creating the segment, its own pid)
 * and the Manager (opening it, the Manager's own pid) computed two different
 * names for the same machine and OpenExisting() always failed. The name must
 * depend only on what both sides can state exactly - the data directory, the
 * machine's name, and an explicitly passed pid - never on whichever process
 * happens to be calling.
 */
static void test_ipc_name_for()
{
	const std::string data = "/tmp/some/datadir/";

	CHECK(MachineIpcNameFor(data, "Alpha", 111) == MachineIpcNameFor(data, "Alpha", 111));
	CHECK(MachineIpcNameFor(data, "Alpha", 111) != MachineIpcNameFor(data, "Alpha", 222));
	CHECK(MachineIpcNameFor(data, "Alpha", 111) != MachineIpcNameFor(data, "Beta", 111));

	/* Two RPCEmus on different --datadir trees, each holding a machine of the
	   same name. */
	CHECK(MachineIpcNameFor(data, "Alpha", 111) !=
	      MachineIpcNameFor("/tmp/other/datadir/", "Alpha", 111));

	/* The machine's name is in the segment's, so one can be traced back to the
	   other. */
	CHECK(MachineIpcNameFor(data, "Alpha", 111).find("Alpha") != std::string::npos);

	/*
	 * A name too long to embed whole is cut rather than overrunning the buffer
	 * or the shortest platform limit. Two machines whose names differ only past
	 * the cut therefore share a segment name until the pid is taken into
	 * account - which it always is, and only one of them can be running,
	 * machine_lock seeing to that.
	 */
	const std::string long_a(80, 'a');
	const std::string long_b = long_a + "-different-tail";

	CHECK(MachineIpcNameFor(data, long_a, 111).size() < 100);
	CHECK(MachineIpcNameFor(data, long_a, 111) == MachineIpcNameFor(data, long_a, 111));
	CHECK(MachineIpcNameFor(data, long_a, 111) != MachineIpcNameFor(data, long_a, 222));
	CHECK(MachineIpcNameFor(data, long_a, 111) == MachineIpcNameFor(data, long_b, 111));
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
#ifdef _WIN32
	/*
	 * ★ Nothing on Windows can open a socket until Winsock is started, and
	 * this is not the emulator.
	 *
	 * rpcemu.c does this at startup, so the IPC works perfectly well in the
	 * running program - but a test binary is its own process and had never
	 * done it. Every socket call here failed, which showed as the server
	 * simply refusing to start. The test linked, which is as far as anyone had
	 * checked; it has never actually passed on Windows until now.
	 */
	{
		WSADATA wsadata;

		if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
			std::fprintf(stderr, "WSAStartup failed\n");
			return 1;
		}
	}
#endif

	test_shared_framebuffer();
	test_shared_framebuffer_dirty_rows();
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
