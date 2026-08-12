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
 * A shared-memory segment name unique to one machine run: the data directory
 * hashed, the machine's name, and the owning process's pid. The pid means two
 * launches of the same machine (which machine_lock already refuses) can never
 * collide, and a stale segment from a killed process is unambiguous to spot.
 *
 * Both the machine and the Manager work this out separately, so it is built
 * from the two things each can state exactly rather than from a path one of
 * them has to reconstruct.
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
std::string MachineIpcNameFor(const std::string &data_dir,
                              const std::string &machine_name, long pid = -1);

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

	/*
	 * ★ One verb for every menu command, rather than a verb per command.
	 *
	 * A managed machine never shows its window, so its menu bar - which is
	 * complete, and whose handlers all work - has nothing to hang off and no
	 * way to be reached. The obvious repair is a request type per command,
	 * which is around forty of them, each needing a handler here that
	 * duplicates the one the menu item already has.
	 *
	 * Instead this carries the menu id itself (a MainFrameMenuId, or a
	 * wxID_* such as wxID_ABOUT) in arg1, and the child turns it back into a
	 * menu event aimed at its own frame. The existing handler then runs,
	 * unchanged and unaware that the click came from another process. A
	 * command added to the machine window in future needs nothing here.
	 *
	 * arg2 carries the checked state for a tick-box item, and path carries a
	 * string argument where one is needed - a file the Manager's own dialogue
	 * chose, for instance, since the Manager is the process with a window to
	 * put a dialogue over.
	 */
	MenuCommand,		/* arg1 = menu id, arg2 = check state, path = optional argument */

	/*
	 * Ask the machine what its tick-boxes currently say, so the Manager can
	 * show them correctly. Answered with an IpcEventType::StateReport.
	 */
	RequestState,
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

	/*
	 * What the machine's own tick-box menu items currently say, so the
	 * Manager's copies of them can agree with the machine rather than with
	 * whatever they happened to be set to when it started.
	 *
	 * Carried in path as "id=0|1" pairs separated by spaces, which fits
	 * comfortably and needs no new struct or versioning: an id the Manager
	 * does not recognise is ignored, and one it expects but does not receive
	 * simply keeps its previous value.
	 */
	StateReport,
};

struct IpcEvent {
	IpcEventType type = IpcEventType::FrameReady;

	/*
	 * For FrameReady: the rows of the guest's screen that changed, as the
	 * half-open range [dirty_top, dirty_bottom) the emulator's own VideoUpdate
	 * uses. Unused by every other event type.
	 *
	 * Here because without it the Manager had to treat every frame as a whole
	 * new screen: copy it, convert it and resample all of it, measured at 23ms
	 * a frame for a 1920x1080 guest, which is most of a GUI thread and is why
	 * the pointer - handled on that same thread - could only be updated about
	 * thirty times a second. A machine's own window has always had these rows
	 * and has always used them.
	 *
	 * A machine that cannot say, or that really did redraw everything, sends
	 * 0 and the screen height, which is what the Manager did for every frame
	 * before this existed.
	 */
	int32_t dirty_top = 0;
	int32_t dirty_bottom = 0;

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
