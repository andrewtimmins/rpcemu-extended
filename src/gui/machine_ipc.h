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
 * machine_ipc - the local transport between the Manager window and a
 * machine running in its own "--managed" process.
 *
 * Two halves, deliberately not VNC/RFB and not any remote-desktop protocol:
 *
 *   - SharedFramebuffer: the video path. The managed child publishes each
 *     completed frame into a memory-mapped region shared with the Manager
 *     process (POSIX shm_open()/mmap(), Windows CreateFileMapping()) - the
 *     Manager reads pixels directly out of it with no socket copy and no
 *     encoding, the same way EmulatorPanel already reads them out of a
 *     buffer that VIDC produced when both lived in one process. Triple-
 *     buffered so a writer can never be observed mid-frame by a reader
 *     without needing a lock on the hot path.
 *
 *   - MachineIpcServer/MachineIpcClient: the control path, a small local
 *     socket (AF_UNIX on Linux/macOS, TCP loopback on Windows, mirroring the
 *     transport selection hostcmd.c and debugcmd.c already use for their own
 *     local control channels). It carries only small fixed-size messages -
 *     input, disc/reset/exit requests, and a handful of status events - never
 *     pixels. Request handlers on the server side call straight into
 *     EmulatorHost's public methods, which are already safe to call from a
 *     foreign thread (see EmulatorHost::PostCommand): the VNC server's own
 *     keyboard/pointer callbacks do exactly this today.
 */

#ifndef MACHINE_IPC_H
#define MACHINE_IPC_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ----------------------------------------------------------------------
 * SharedFramebuffer
 * -------------------------------------------------------------------- */

class SharedFramebuffer {
public:
	/* Generous upper bound on any host/guest display mode; sized once so a
	   resize never needs to remap the segment (which the reader could be
	   touching at the same time). */
	static constexpr int kMaxWidth = 2560;
	static constexpr int kMaxHeight = 1600;
	static constexpr int kBufferCount = 3;	/* see class comment: lock-free triple buffer */

	SharedFramebuffer() = default;
	~SharedFramebuffer();

	SharedFramebuffer(const SharedFramebuffer &) = delete;
	SharedFramebuffer &operator=(const SharedFramebuffer &) = delete;

	/* Managed child: create a fresh segment. `name` should be unique to this
	   machine's run (see MachineIpcNameFor()). */
	bool CreateNew(const std::string &name);

	/* Manager: map an existing segment created by CreateNew() elsewhere. */
	bool OpenExisting(const std::string &name);

	void Close();
	bool IsOpen() const { return header_ != nullptr; }

	/* Writer side. Copies `width`x`height` (whole-frame; simpler and, for
	   RISC OS resolutions, cheap enough than tracking a dirty rect through
	   shared memory) into the next free slot, then publishes it. Silently
	   clamps/ignores a frame larger than kMaxWidth x kMaxHeight, which
	   should not happen given the host display bound rpcemu already
	   negotiates. */
	void Publish(const uint32_t *pixels, int width, int height);

	/* Reader side. Copies the most recently published frame into `out` and
	   reports its dimensions. Returns false if nothing has been published
	   yet. Safe to call concurrently with Publish() from another process. */
	bool ReadInto(std::vector<uint32_t> *out, int *width, int *height) const;

private:
	struct Header {
		std::atomic<uint32_t> front_slot;	/* index of the newest complete frame */
		std::atomic<uint32_t> prev_slot;	/* the one before it, kept safe from reuse */
		/* Per-slot, not shared: read only after loading front_slot (with
		   acquire), so a reader always sees the dimensions that go with the
		   exact pixels it is about to copy, never a mismatched pair from a
		   frame published concurrently. */
		std::atomic<uint32_t> slot_width[kBufferCount];
		std::atomic<uint32_t> slot_height[kBufferCount];
	};

	static size_t TotalSize();
	bool Map(const std::string &name, bool create);

	std::string name_;
	bool owner_ = false;	/* created (vs opened) the segment; unlinks on Close() */
	void *mapping_ = nullptr;
#ifdef _WIN32
	void *file_mapping_handle_ = nullptr;
#else
	int fd_ = -1;
#endif
	Header *header_ = nullptr;
	uint32_t *slots_[kBufferCount] = {};
	uint32_t next_write_slot_ = 0;	/* writer-only, no synchronisation needed */
};

/*
 * A shared-memory segment name unique to one machine run, derived from its
 * own data directory and the owning process's pid so two launches of the
 * same machine (which machine_lock already refuses) can never collide, and a
 * stale segment from a killed process is unambiguous to spot.
 *
 * `pid` must be the pid of the process that called (or will call)
 * CreateNew() - the managed child - not the caller's own pid. The child
 * itself has no other pid to give (pass -1, the default, to use its own, via
 * getpid()/GetCurrentProcessId()); the Manager must instead pass the pid it
 * read back from machine_lock_read_owner() for that machine. Computing this
 * from the caller's own pid on both sides was tried first and does not work:
 * the Manager's pid is never the child's, so the two processes named two
 * different segments and OpenExisting() always failed.
 */
std::string MachineIpcNameFor(const std::string &machine_dir, long pid = -1);

/* ----------------------------------------------------------------------
 * Control channel wire messages
 * -------------------------------------------------------------------- */

enum class IpcRequestType : uint32_t {
	KeyPress,
	KeyRelease,
	MouseMove,		/* arg1=x, arg2=y (absolute, host panel coordinates) */
	MouseMoveRelative,	/* arg1=dx, arg2=dy */
	MousePress,		/* arg1=button mask */
	MouseRelease,		/* arg1=button mask */
	MouseWheel,		/* arg1=dy */
	Reset,
	Restart,
	RequestExit,
	LoadDisc0,		/* path = disc image path */
	LoadDisc1,
	EjectDisc0,
	EjectDisc1,
	CdromDisabled,
	CdromEmpty,
	CdromLoadIso,		/* path = iso path */
	RequestKeyFrame,	/* ask for the current frame to be republished, e.g.
				   right after a Manager tab switches to this machine */
};

struct IpcRequest {
	IpcRequestType type = IpcRequestType::RequestKeyFrame;
	int32_t arg1 = 0;
	int32_t arg2 = 0;
	char path[512] = {};
};

enum class IpcEventType : uint32_t {
	FrameReady,	/* a new frame is in the shared framebuffer */
	Error,		/* path = message */
	Fatal,		/* path = message; the machine is about to exit */
	Quit,		/* the machine has exited (or is about to, cleanly) */
	TitleChanged,	/* path = new machine name, e.g. after Switch Machine */
};

struct IpcEvent {
	IpcEventType type = IpcEventType::FrameReady;
	char path[512] = {};
};

/* ----------------------------------------------------------------------
 * MachineIpcServer - runs inside the managed child.
 * -------------------------------------------------------------------- */

class MachineIpcServer {
public:
	MachineIpcServer() = default;
	~MachineIpcServer();

	MachineIpcServer(const MachineIpcServer &) = delete;
	MachineIpcServer &operator=(const MachineIpcServer &) = delete;

	/* Starts listening and returns immediately; the accept/read loop runs on
	   its own thread. `endpoint` is an AF_UNIX path on Linux/macOS, or (on
	   Windows, where AF_UNIX is not usable as a well-known rendezvous the
	   same way) the empty string to request an OS-assigned loopback TCP
	   port, discoverable afterwards via BoundEndpoint(). */
	bool Start(const std::string &endpoint,
	           std::function<void(const IpcRequest &)> on_request);

	void Stop();

	/* What a client should actually connect to: the AF_UNIX path given to
	   Start(), or "tcp:<port>" on Windows. Empty if not listening. */
	std::string BoundEndpoint() const { return bound_endpoint_; }

	/* Thread-safe; callable from any thread (the GUI thread for frame
	   mirroring, the emulator thread's error/fatal/quit callbacks). Drops
	   the event silently if no client is connected - there is nothing
	   useful to do with an event nobody can receive, and the framebuffer
	   itself is unaffected. */
	void SendEvent(const IpcEvent &event);

private:
	void AcceptLoop();
	void ClientLoop(int client_fd);

	std::function<void(const IpcRequest &)> on_request_;
	std::thread accept_thread_;
	std::mutex client_mutex_;
	int listen_fd_ = -1;
	int client_fd_ = -1;
	std::atomic<bool> running_{false};
	std::string sock_path_;	/* for unlink() on teardown, POSIX only */
	std::string bound_endpoint_;
};

/* ----------------------------------------------------------------------
 * MachineIpcClient - runs inside the Manager, one per running machine.
 * -------------------------------------------------------------------- */

class MachineIpcClient {
public:
	MachineIpcClient() = default;
	~MachineIpcClient();

	MachineIpcClient(const MachineIpcClient &) = delete;
	MachineIpcClient &operator=(const MachineIpcClient &) = delete;

	/* Connects and starts a background thread reading events. `on_event` is
	   called on that thread; callers touching UI state must hop back to the
	   GUI thread themselves (e.g. via wxCallAfter/CallAfter), exactly as
	   EmulatorHost's own GuiBridge callers already do. */
	bool Connect(const std::string &endpoint,
	             std::function<void(const IpcEvent &)> on_event);

	void Disconnect();
	bool IsConnected() const { return connected_.load(); }

	/* Thread-safe; may be called from the GUI thread freely. */
	void Send(const IpcRequest &request);

private:
	void ReadLoop();

	std::function<void(const IpcEvent &)> on_event_;
	std::thread read_thread_;
	std::mutex send_mutex_;
	int fd_ = -1;
	std::atomic<bool> connected_{false};
};

#endif /* MACHINE_IPC_H */
