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

/* See machine_ipc.h. */

#include "machine_ipc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

#include "socket-compat.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/un.h>
#endif

/* ----------------------------------------------------------------------
 * SharedFramebuffer
 * -------------------------------------------------------------------- */

size_t SharedFramebuffer::TotalSize()
{
	const size_t header_size = ((sizeof(Header) + 63) / 64) * 64;	/* cache-line align */
	const size_t slot_size = (size_t) kMaxWidth * (size_t) kMaxHeight * sizeof(uint32_t);

	return header_size + (size_t) kBufferCount * slot_size;
}

bool SharedFramebuffer::Map(const std::string &name, bool create)
{
	const size_t total = TotalSize();
	const size_t header_size = ((sizeof(Header) + 63) / 64) * 64;
	const size_t slot_size = (size_t) kMaxWidth * (size_t) kMaxHeight * sizeof(uint32_t);
	void *mem = nullptr;

#ifdef _WIN32
	const std::string full = "Local\\" + name;
	HANDLE handle;

	if (create) {
		handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		    0, (DWORD) total, full.c_str());
	} else {
		handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full.c_str());
	}
	if (handle == nullptr) {
		return false;
	}
	mem = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, total);
	if (mem == nullptr) {
		CloseHandle(handle);
		return false;
	}
	file_mapping_handle_ = handle;
#else
	const std::string full = "/" + name;	/* shm_open names begin with one */
	int flags = create ? (O_CREAT | O_RDWR | O_EXCL) : O_RDWR;
	int fd = shm_open(full.c_str(), flags, 0600);

	if (fd < 0 && create && errno == EEXIST) {
		/* A segment from a previous run of this pid survived somehow (the
		   name already includes the pid, so a genuine collision would mean
		   pid reuse racing us, vanishingly unlikely) - clear it rather than
		   fail outright. */
		shm_unlink(full.c_str());
		fd = shm_open(full.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
	}
	if (fd < 0) {
		return false;
	}
	if (create && ftruncate(fd, (off_t) total) != 0) {
		close(fd);
		shm_unlink(full.c_str());
		return false;
	}
	mem = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);	/* the mapping keeps the segment alive; the fd is not needed after mmap */
	if (mem == MAP_FAILED) {
		if (create) {
			shm_unlink(full.c_str());
		}
		return false;
	}
#endif

	name_ = name;
	owner_ = create;
	mapping_ = mem;
	header_ = reinterpret_cast<Header *>(mem);

	uint8_t *base = reinterpret_cast<uint8_t *>(mem);
	for (int i = 0; i < kBufferCount; i++) {
		slots_[i] = reinterpret_cast<uint32_t *>(base + header_size + (size_t) i * slot_size);
	}

	if (create) {
		/*
		 * The header is treated as already-constructed atomics laid directly
		 * over shared memory rather than placement-newed: std::atomic<uint32_t>
		 * has the same object representation as uint32_t on every mainstream
		 * platform this builds for, which is the ordinary way atomics are used
		 * in a cross-process shared segment. Every field gets an explicit
		 * starting value through the atomic API below (no reliance on the
		 * segment's initial zero-fill from the OS).
		 */
		header_->front_slot.store(0, std::memory_order_relaxed);
		header_->prev_slot.store(kBufferCount - 1, std::memory_order_relaxed);
		for (int i = 0; i < kBufferCount; i++) {
			header_->slot_width[i].store(0, std::memory_order_relaxed);
			header_->slot_height[i].store(0, std::memory_order_relaxed);
		}
	}

	return true;
}

bool SharedFramebuffer::CreateNew(const std::string &name)
{
	Close();
	return Map(name, true);
}

bool SharedFramebuffer::OpenExisting(const std::string &name)
{
	Close();
	return Map(name, false);
}

void SharedFramebuffer::Close()
{
	if (mapping_ != nullptr) {
#ifdef _WIN32
		UnmapViewOfFile(mapping_);
#else
		munmap(mapping_, TotalSize());
#endif
		mapping_ = nullptr;
	}
#ifdef _WIN32
	if (file_mapping_handle_ != nullptr) {
		CloseHandle((HANDLE) file_mapping_handle_);
		file_mapping_handle_ = nullptr;
	}
	/* Windows frees the mapping's backing store automatically once every
	   handle to it is closed, so there is nothing analogous to shm_unlink()
	   to do here even for the owner. */
#else
	if (owner_ && !name_.empty()) {
		const std::string full = "/" + name_;

		shm_unlink(full.c_str());
	}
#endif
	header_ = nullptr;
	for (int i = 0; i < kBufferCount; i++) {
		slots_[i] = nullptr;
	}
}

SharedFramebuffer::~SharedFramebuffer()
{
	Close();
}

void SharedFramebuffer::Publish(const uint32_t *pixels, int width, int height)
{
	if (header_ == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
		return;
	}
	if (width > kMaxWidth) {
		width = kMaxWidth;
	}
	if (height > kMaxHeight) {
		height = kMaxHeight;
	}

	const uint32_t front = header_->front_slot.load(std::memory_order_relaxed);
	const uint32_t prev = header_->prev_slot.load(std::memory_order_relaxed);
	uint32_t target = 0;

	/* Triple buffering: write into whichever slot is neither the published
	   frame nor the one before it, so a reader mid-copy of either is never
	   overwritten underneath it. With three slots there is always exactly
	   one such slot free. */
	for (uint32_t i = 0; i < kBufferCount; i++) {
		if (i != front && i != prev) {
			target = i;
			break;
		}
	}

	const size_t count = (size_t) width * (size_t) height;
	std::memcpy(slots_[target], pixels, count * sizeof(uint32_t));

	header_->slot_width[target].store((uint32_t) width, std::memory_order_relaxed);
	header_->slot_height[target].store((uint32_t) height, std::memory_order_relaxed);
	header_->prev_slot.store(front, std::memory_order_relaxed);
	/* Release: publishes the memcpy above and the two stores just before it
	   together with the slot index, so a reader that acquires front_slot and
	   then reads slot_width/slot_height[front] is guaranteed to see the pixels
	   and dimensions that go together, never a mix of an old buffer with new
	   dimensions or vice versa. */
	header_->front_slot.store(target, std::memory_order_release);
}

bool SharedFramebuffer::ReadInto(std::vector<uint32_t> *out, int *width, int *height) const
{
	if (header_ == nullptr) {
		return false;
	}

	const uint32_t front = header_->front_slot.load(std::memory_order_acquire);
	const uint32_t w = header_->slot_width[front].load(std::memory_order_relaxed);
	const uint32_t h = header_->slot_height[front].load(std::memory_order_relaxed);

	if (w == 0 || h == 0) {
		return false;
	}

	const size_t count = (size_t) w * (size_t) h;
	out->resize(count);
	std::memcpy(out->data(), slots_[front], count * sizeof(uint32_t));
	*width = (int) w;
	*height = (int) h;
	return true;
}

std::string MachineIpcNameFor(const std::string &machine_dir, long pid)
{
	const size_t h = std::hash<std::string>{}(machine_dir);
	char buf[64];

	if (pid < 0) {
#ifdef _WIN32
		pid = (long) GetCurrentProcessId();
#else
		pid = (long) getpid();
#endif
	}

#ifdef _WIN32
	std::snprintf(buf, sizeof(buf), "rpcemu-fb-%08lx-%ld",
	    (unsigned long) (h & 0xffffffffu), pid);
#else
	std::snprintf(buf, sizeof(buf), "/rpcemu-fb-%08lx-%ld",
	    (unsigned long) (h & 0xffffffffu), pid);
#endif
	return std::string(buf);
}

/* ----------------------------------------------------------------------
 * Small I/O helpers shared by the server and client sides.
 * -------------------------------------------------------------------- */

namespace {

bool SendAll(int fd, const void *buf, size_t len)
{
	const uint8_t *p = static_cast<const uint8_t *>(buf);
	size_t sent = 0;

	while (sent < len) {
		const int n = (int) send(fd, (const char *) p + sent, (int) (len - sent), MSG_NOSIGNAL);

		if (n > 0) {
			sent += (size_t) n;
			continue;
		}
		if (n < 0 && (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN ||
		              sock_errno() == SOCK_EINTR)) {
			struct pollfd pfd;
			pfd.fd = fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			poll(&pfd, 1, 1000);
			continue;
		}
		return false;
	}
	return true;
}

/* Reads exactly `len` bytes, polling in short bursts so `keep_running` (set
   to false by Stop()/Disconnect() on another thread) is noticed promptly
   without needing to interrupt a blocking recv(). */
bool RecvExact(int fd, void *buf, size_t len, const std::atomic<bool> *keep_running)
{
	uint8_t *p = static_cast<uint8_t *>(buf);
	size_t got = 0;

	while (got < len) {
		if (keep_running != nullptr && !keep_running->load()) {
			return false;
		}

		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		const int rc = poll(&pfd, 1, 200);

		if (rc == 0) {
			continue;
		}
		if (rc < 0) {
			if (sock_errno() == SOCK_EINTR) {
				continue;
			}
			return false;
		}

		const int n = (int) recv(fd, (char *) p + got, (int) (len - got), 0);

		if (n > 0) {
			got += (size_t) n;
			continue;
		}
		if (n == 0) {
			return false;	/* peer closed */
		}
		if (sock_errno() == SOCK_EWOULDBLOCK || sock_errno() == SOCK_EAGAIN ||
		    sock_errno() == SOCK_EINTR) {
			continue;
		}
		return false;
	}
	return true;
}

} /* namespace */

/* ----------------------------------------------------------------------
 * MachineIpcServer
 * -------------------------------------------------------------------- */

bool MachineIpcServer::Start(const std::string &endpoint,
                              std::function<void(const IpcRequest &)> on_request)
{
	if (running_.load()) {
		return false;
	}
	on_request_ = std::move(on_request);

#ifdef _WIN32
	(void) endpoint;
	const int fd = (int) socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;	/* OS-assigned; discovered below */
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
	    listen(fd, 1) != 0) {
		closesocket(fd);
		return false;
	}
	socklen_t alen = sizeof(addr);
	getsockname(fd, (struct sockaddr *) &addr, &alen);
	bound_endpoint_ = "tcp:" + std::to_string((int) ntohs(addr.sin_port));
	socket_set_nonblocking(fd);
	listen_fd_ = fd;
#else
	struct sockaddr_un addr;
	if (endpoint.empty() || endpoint.size() >= sizeof(addr.sun_path)) {
		return false;
	}
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	std::memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
	unlink(endpoint.c_str());	/* clear a stale socket from a killed previous run */
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		close(fd);
		return false;
	}
	if (listen(fd, 1) != 0) {
		close(fd);
		unlink(endpoint.c_str());
		return false;
	}
	socket_set_nonblocking(fd);
	listen_fd_ = fd;
	sock_path_ = endpoint;
	bound_endpoint_ = endpoint;
#endif

	running_ = true;
	accept_thread_ = std::thread([this]() { AcceptLoop(); });
	return true;
}

void MachineIpcServer::AcceptLoop()
{
	while (running_.load()) {
		struct pollfd pfd;
		pfd.fd = listen_fd_;
		pfd.events = POLLIN;
		pfd.revents = 0;
		const int rc = poll(&pfd, 1, 200);

		if (rc <= 0) {
			continue;
		}
		const int cfd = (int) accept(listen_fd_, nullptr, nullptr);
		if (cfd < 0) {
			continue;
		}

		/* One client at a time - the Manager. A fresh connection (e.g. the
		   Manager restarting after a crash) replaces whatever was there,
		   since only one thing should be driving a machine's input. */
		{
			std::lock_guard<std::mutex> lock(client_mutex_);
			if (client_fd_ >= 0) {
				closesocket(client_fd_);
			}
			client_fd_ = cfd;
		}
		socket_set_nonblocking(cfd);
		ClientLoop(cfd);
	}
}

void MachineIpcServer::ClientLoop(int client_fd)
{
	IpcRequest req;

	while (running_.load()) {
		{
			std::lock_guard<std::mutex> lock(client_mutex_);
			if (client_fd_ != client_fd) {
				return;	/* superseded by a newer connection */
			}
		}
		if (!RecvExact(client_fd, &req, sizeof(req), &running_)) {
			break;
		}
		if (on_request_) {
			on_request_(req);
		}
	}

	std::lock_guard<std::mutex> lock(client_mutex_);
	if (client_fd_ == client_fd) {
		closesocket(client_fd_);
		client_fd_ = -1;
	}
}

void MachineIpcServer::SendEvent(const IpcEvent &event)
{
	std::lock_guard<std::mutex> lock(client_mutex_);
	if (client_fd_ < 0) {
		return;	/* nobody to tell; the framebuffer itself is unaffected */
	}
	if (!SendAll(client_fd_, &event, sizeof(event))) {
		closesocket(client_fd_);
		client_fd_ = -1;
	}
}

void MachineIpcServer::Stop()
{
	if (!running_.exchange(false)) {
		return;
	}
	if (accept_thread_.joinable()) {
		accept_thread_.join();
	}
	{
		std::lock_guard<std::mutex> lock(client_mutex_);
		if (client_fd_ >= 0) {
			closesocket(client_fd_);
			client_fd_ = -1;
		}
	}
	if (listen_fd_ >= 0) {
		closesocket(listen_fd_);
		listen_fd_ = -1;
	}
#ifndef _WIN32
	if (!sock_path_.empty()) {
		unlink(sock_path_.c_str());
	}
#endif
}

MachineIpcServer::~MachineIpcServer()
{
	Stop();
}

/* ----------------------------------------------------------------------
 * MachineIpcClient
 * -------------------------------------------------------------------- */

bool MachineIpcClient::Connect(const std::string &endpoint,
                                std::function<void(const IpcEvent &)> on_event)
{
	if (connected_.load()) {
		return false;
	}
	on_event_ = std::move(on_event);

#ifdef _WIN32
	if (endpoint.rfind("tcp:", 0) != 0) {
		return false;
	}
	const int port = std::atoi(endpoint.c_str() + 4);
	const int fd = (int) socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t) port);
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		closesocket(fd);
		return false;
	}
#else
	struct sockaddr_un addr;
	if (endpoint.empty() || endpoint.size() >= sizeof(addr.sun_path)) {
		return false;
	}
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}
	std::memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		close(fd);
		return false;
	}
#endif

	socket_set_nonblocking(fd);
	fd_ = fd;
	connected_ = true;
	read_thread_ = std::thread([this]() { ReadLoop(); });
	return true;
}

void MachineIpcClient::ReadLoop()
{
	IpcEvent ev;

	while (connected_.load()) {
		if (!RecvExact(fd_, &ev, sizeof(ev), &connected_)) {
			/* Losing the connection is the only notice that a machine the
			   Manager attached to, rather than started, has gone: there is no
			   process to watch and nothing sends an explicit Quit. Tested
			   again because Disconnect() clears it, and a disconnection this
			   end asked for is not news. */
			if (connected_.load() && on_event_) {
				IpcEvent gone{};

				gone.type = IpcEventType::Quit;
				on_event_(gone);
			}
			break;
		}
		if (on_event_) {
			on_event_(ev);
		}
	}
	connected_ = false;
}

void MachineIpcClient::Send(const IpcRequest &request)
{
	std::lock_guard<std::mutex> lock(send_mutex_);
	if (fd_ < 0) {
		return;
	}
	/* A send failure here is left for ReadLoop to notice (as a closed peer)
	   and unwind from, rather than torn down from two threads at once. */
	SendAll(fd_, &request, sizeof(request));
}

void MachineIpcClient::Disconnect()
{
	connected_ = false;
	if (fd_ >= 0) {
		/* Before the close, not instead of it: WSAPoll() does not report a
		   handle closed underneath it, so the reader stayed in poll() and the
		   join below waited for ever. */
		shutdown(fd_, SHUT_RDWR);
		closesocket(fd_);
	}
	if (read_thread_.joinable()) {
		read_thread_.join();
	}
	fd_ = -1;
}

MachineIpcClient::~MachineIpcClient()
{
	Disconnect();
}
