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

/* See machine_lock.h for what this protects and why the lock is an OS lock. */

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#endif

#include "rpcemu.h"
#include "machine_lock.h"

#define LOCK_FILE "running.lock"

/* Remembered so the port can be corrected later without re-deriving the path. */
static int lock_vnc_port;

/* Remembered so it survives into a later rewrite of the file (e.g. when the
   VNC port is corrected) without the caller having to repeat itself. Empty
   when this process is not running as a managed machine, i.e. every case
   except the manager's own child processes - see machine_ipc.h. */
static char lock_ipc_endpoint[512];
static char lock_debug_endpoint[700];
static char lock_netcap_endpoint[700];

static void
set_str_field(char *out, size_t size, const char *value)
{
	if (value == NULL) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s", value);
}

/** Render the file's contents. `ipc_endpoint` may be NULL or empty. */
static void
format_lock_text(char *out, size_t size, long pid, int vnc_port, const char *ipc_endpoint)
{
	int n = snprintf(out, size, "pid=%ld\nvnc_port=%d\n", pid, vnc_port);

	if (ipc_endpoint != NULL && ipc_endpoint[0] != '\0' && n > 0 && (size_t) n < size) {
		n += snprintf(out + n, size - (size_t) n, "ipc=%s\n", ipc_endpoint);
	}

	/*
	 * ★ Where the debugger is actually listening.
	 *
	 * A machine may be given any socket path it likes, so a tool cannot work
	 * one out - rpcemu-debug looked for the default name and found nothing on
	 * a machine whose configuration named its own. The machine knows, and this
	 * file is where it already tells everything else what it is doing.
	 */
	if (lock_debug_endpoint[0] != '\0' && n > 0 && (size_t) n < size) {
		n += snprintf(out + n, size - (size_t) n, "dbg=%s\n", lock_debug_endpoint);
	}
	if (lock_netcap_endpoint[0] != '\0' && n > 0 && (size_t) n < size) {
		snprintf(out + n, size - (size_t) n, "cap=%s\n", lock_netcap_endpoint);
	}
}

/** Join a machine directory and the lock file name, tolerating a missing separator. */
static void
lock_path(char *out, size_t size, const char *machine_dir)
{
	size_t len;

	if (machine_dir == NULL || machine_dir[0] == '\0') {
		machine_dir = "./";
	}
	len = strlen(machine_dir);
	if (len > 0 && (machine_dir[len - 1] == '/' || machine_dir[len - 1] == '\\')) {
		snprintf(out, size, "%s%s", machine_dir, LOCK_FILE);
	} else {
		snprintf(out, size, "%s/%s", machine_dir, LOCK_FILE);
	}
}

#ifdef _WIN32

#include <windows.h>

static HANDLE lock_handle = INVALID_HANDLE_VALUE;

int
machine_lock_acquire(const char *machine_dir, int vnc_port)
{
	char path[1024];
	char text[1600];
	DWORD written = 0;

	lock_path(path, sizeof(path), machine_dir);

	/*
	 * Exclusive by share mode rather than by a separate locking call, and Windows
	 * closes the handle when the process ends however it ends.
	 *
	 * FILE_SHARE_READ, not 0. Sharing nothing does lock the machine, but it also
	 * stops anyone reading the file - including machine_lock_read_owner() in the
	 * very process that has just been refused, which is the only time anybody
	 * wants to read it. The "already running" message is meant to say which
	 * process holds the machine and on which VNC port it can be reached, and on
	 * Windows it never could: the read failed with a sharing violation and the
	 * message came out bare. Found by test_machine_lock on the first CI run it
	 * ever had.
	 *
	 * The lock is unaffected. A second machine_lock_acquire() asks for
	 * GENERIC_WRITE, which FILE_SHARE_READ refuses, so the second instance is
	 * still turned away; only readers get in.
	 */
	lock_handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (lock_handle == INVALID_HANDLE_VALUE) {
		return 0;
	}

	SetFilePointer(lock_handle, 0, NULL, FILE_BEGIN);
	SetEndOfFile(lock_handle);
	format_lock_text(text, sizeof(text), (long) GetCurrentProcessId(), vnc_port,
	    lock_ipc_endpoint);
	WriteFile(lock_handle, text, (DWORD) strlen(text), &written, NULL);
	FlushFileBuffers(lock_handle);
	return 1;
}

static void
rewrite_lock_file_win32(void)
{
	char text[1600];
	DWORD written = 0;

	if (lock_handle == INVALID_HANDLE_VALUE) {
		return;
	}
	SetFilePointer(lock_handle, 0, NULL, FILE_BEGIN);
	SetEndOfFile(lock_handle);
	format_lock_text(text, sizeof(text), (long) GetCurrentProcessId(), lock_vnc_port,
	    lock_ipc_endpoint);
	WriteFile(lock_handle, text, (DWORD) strlen(text), &written, NULL);
	FlushFileBuffers(lock_handle);
}

void
machine_lock_set_vnc_port(int vnc_port)
{
	lock_vnc_port = vnc_port;
	rewrite_lock_file_win32();
}

void
machine_lock_set_ipc_endpoint(const char *ipc_endpoint)
{
	set_str_field(lock_ipc_endpoint, sizeof(lock_ipc_endpoint), ipc_endpoint);
	rewrite_lock_file_win32();
}

void
machine_lock_set_debug_endpoint(const char *debug_endpoint)
{
	set_str_field(lock_debug_endpoint, sizeof(lock_debug_endpoint), debug_endpoint);
	rewrite_lock_file_win32();
}

void
machine_lock_set_netcap_endpoint(const char *netcap_endpoint)
{
	set_str_field(lock_netcap_endpoint, sizeof(lock_netcap_endpoint),
	    netcap_endpoint);
	rewrite_lock_file_win32();
}

void
machine_lock_release(void)
{
	if (lock_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(lock_handle);
		lock_handle = INVALID_HANDLE_VALUE;
	}
}

#else /* POSIX */

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static int lock_fd = -1;

int
machine_lock_acquire(const char *machine_dir, int vnc_port)
{
	char path[1024];
	char text[1600];
	int fd;

	lock_path(path, sizeof(path), machine_dir);

	fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		/* Cannot create it - a read-only machine directory, say. Better to run
		   than to refuse over a lock we could not take. */
		rpclog("machine_lock: cannot open %s, running without a lock\n", path);
		return 1;
	}

	/*
	 * flock() rather than a file whose existence is the lock: the kernel drops
	 * this when the process dies, however it dies, so a crash cannot leave a
	 * machine permanently unstartable and there is no staleness to reason about.
	 */
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		return 0;
	}

	if (ftruncate(fd, 0) != 0) {
		/* Not worth failing over: the contents are only a hint. */
	}
	format_lock_text(text, sizeof(text), (long) getpid(), vnc_port, lock_ipc_endpoint);
	if (write(fd, text, strlen(text)) < 0) {
		/* Likewise. The lock is what matters. */
	}

	lock_fd = fd;	/* held for the life of the process */
	return 1;
}

static void
rewrite_lock_file_posix(void)
{
	char text[1600];

	if (lock_fd < 0) {
		return;
	}
	if (lseek(lock_fd, 0, SEEK_SET) < 0 || ftruncate(lock_fd, 0) != 0) {
		return;
	}
	format_lock_text(text, sizeof(text), (long) getpid(), lock_vnc_port, lock_ipc_endpoint);
	if (write(lock_fd, text, strlen(text)) < 0) {
		/* Informational only; the lock is what matters. */
	}
}

void
machine_lock_set_vnc_port(int vnc_port)
{
	lock_vnc_port = vnc_port;
	rewrite_lock_file_posix();
}

void
machine_lock_set_ipc_endpoint(const char *ipc_endpoint)
{
	set_str_field(lock_ipc_endpoint, sizeof(lock_ipc_endpoint), ipc_endpoint);
	rewrite_lock_file_posix();
}

void
machine_lock_set_debug_endpoint(const char *debug_endpoint)
{
	set_str_field(lock_debug_endpoint, sizeof(lock_debug_endpoint), debug_endpoint);
	rewrite_lock_file_posix();
}

void
machine_lock_set_netcap_endpoint(const char *netcap_endpoint)
{
	set_str_field(lock_netcap_endpoint, sizeof(lock_netcap_endpoint),
	    netcap_endpoint);
	rewrite_lock_file_posix();
}

void
machine_lock_release(void)
{
	if (lock_fd >= 0) {
		/* Closing releases the flock; done explicitly so an orderly exit does not
		   rely on process teardown ordering. */
		close(lock_fd);
		lock_fd = -1;
	}
}

#endif /* _WIN32 */

int
machine_lock_read_owner(const char *machine_dir, long *pid, int *vnc_port)
{
	char path[1024];
	char line[128];
	FILE *f;
	int found = 0;

	if (pid != NULL) {
		*pid = 0;
	}
	if (vnc_port != NULL) {
		*vnc_port = 0;
	}

	lock_path(path, sizeof(path), machine_dir);
	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		long value;

		if (sscanf(line, "pid=%ld", &value) == 1) {
			if (pid != NULL) {
				*pid = value;
			}
			found = 1;
		} else if (sscanf(line, "vnc_port=%ld", &value) == 1) {
			if (vnc_port != NULL) {
				*vnc_port = (int) value;
			}
			found = 1;
		}
	}
	fclose(f);
	return found;
}

/* One reader for both endpoints: the file is the same and only the key differs. */
static int
machine_lock_read_field(const char *machine_dir, const char *key,
    char *endpoint_out, size_t endpoint_out_size)
{
	char path[1024];
	char line[1600];	/* wide enough for a key plus a full AF_UNIX path */
	FILE *f;
	int found = 0;

	if (endpoint_out != NULL && endpoint_out_size > 0) {
		endpoint_out[0] = '\0';
	}
	if (endpoint_out == NULL || endpoint_out_size == 0) {
		return 0;
	}

	lock_path(path, sizeof(path), machine_dir);
	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const size_t prefix_len = strlen(key);

		if (strncmp(line, key, prefix_len) == 0) {
			size_t len = strlen(line + prefix_len);

			while (len > 0 && (line[prefix_len + len - 1] == '\n' ||
			                   line[prefix_len + len - 1] == '\r')) {
				len--;
			}
			if (len >= endpoint_out_size) {
				len = endpoint_out_size - 1;
			}
			memcpy(endpoint_out, line + prefix_len, len);
			endpoint_out[len] = '\0';
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int
machine_lock_owner_alive(long pid)
{
	if (pid <= 0) {
		return 0;
	}

#ifdef _WIN32
	{
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
		    (DWORD) pid);
		DWORD code = 0;

		if (h == NULL) {
			return 0;
		}
		/* Open alone is not enough: a process that has exited but whose
		   handle is still held reports STILL_ACTIVE only while running. */
		if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) {
			CloseHandle(h);
			return 1;
		}
		CloseHandle(h);
		return 0;
	}
#else
	/*
	 * Signal 0 performs the existence and permission checks without sending
	 * anything. EPERM means the process is there but belongs to somebody
	 * else, which for this question is still "there".
	 */
	if (kill((pid_t) pid, 0) == 0) {
		return 1;
	}
	return errno == EPERM;
#endif
}

int
machine_lock_read_ipc_endpoint(const char *machine_dir, char *endpoint_out,
    size_t endpoint_out_size)
{
	return machine_lock_read_field(machine_dir, "ipc=", endpoint_out,
	    endpoint_out_size);
}

int
machine_lock_read_debug_endpoint(const char *machine_dir, char *endpoint_out,
    size_t endpoint_out_size)
{
	return machine_lock_read_field(machine_dir, "dbg=", endpoint_out,
	    endpoint_out_size);
}

int
machine_lock_read_netcap_endpoint(const char *machine_dir, char *endpoint_out,
    size_t endpoint_out_size)
{
	return machine_lock_read_field(machine_dir, "cap=", endpoint_out,
	    endpoint_out_size);
}
