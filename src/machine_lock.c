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

#include "rpcemu.h"
#include "machine_lock.h"

#define LOCK_FILE "running.lock"

/* Remembered so the port can be corrected later without re-deriving the path. */
static int lock_vnc_port;

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
	char text[128];
	DWORD written = 0;

	lock_path(path, sizeof(path), machine_dir);

	/*
	 * Exclusive by share mode rather than by a separate locking call: opening with
	 * dwShareMode 0 means no other process can open it at all, and Windows closes
	 * the handle when the process ends however it ends.
	 */
	lock_handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (lock_handle == INVALID_HANDLE_VALUE) {
		return 0;
	}

	SetFilePointer(lock_handle, 0, NULL, FILE_BEGIN);
	SetEndOfFile(lock_handle);
	snprintf(text, sizeof(text), "pid=%lu\nvnc_port=%d\n",
	    (unsigned long) GetCurrentProcessId(), vnc_port);
	WriteFile(lock_handle, text, (DWORD) strlen(text), &written, NULL);
	FlushFileBuffers(lock_handle);
	return 1;
}

void
machine_lock_set_vnc_port(int vnc_port)
{
	char text[128];
	DWORD written = 0;

	lock_vnc_port = vnc_port;
	if (lock_handle == INVALID_HANDLE_VALUE) {
		return;
	}
	SetFilePointer(lock_handle, 0, NULL, FILE_BEGIN);
	SetEndOfFile(lock_handle);
	snprintf(text, sizeof(text), "pid=%lu\nvnc_port=%d\n",
	    (unsigned long) GetCurrentProcessId(), vnc_port);
	WriteFile(lock_handle, text, (DWORD) strlen(text), &written, NULL);
	FlushFileBuffers(lock_handle);
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
	char text[128];
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
	snprintf(text, sizeof(text), "pid=%ld\nvnc_port=%d\n", (long) getpid(),
	    vnc_port);
	if (write(fd, text, strlen(text)) < 0) {
		/* Likewise. The lock is what matters. */
	}

	lock_fd = fd;	/* held for the life of the process */
	return 1;
}

void
machine_lock_set_vnc_port(int vnc_port)
{
	char text[128];

	lock_vnc_port = vnc_port;
	if (lock_fd < 0) {
		return;
	}
	if (lseek(lock_fd, 0, SEEK_SET) < 0 || ftruncate(lock_fd, 0) != 0) {
		return;
	}
	snprintf(text, sizeof(text), "pid=%ld\nvnc_port=%d\n", (long) getpid(),
	    vnc_port);
	if (write(lock_fd, text, strlen(text)) < 0) {
		/* Informational only; the lock is what matters. */
	}
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
