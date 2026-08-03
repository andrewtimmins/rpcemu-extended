/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
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

#include <assert.h>
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>	/* RTL_OSVERSIONINFOW comes with this, via winnt.h */
#else
#include <sys/statvfs.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <sys/sysctl.h>		/* sysctlbyname(), for the macOS product version */
#endif
#endif

#include "rpcemu.h"
#include "mem.h"
#include "sound.h"
#include "vidc20.h"




/**
 * Return disk space information about a file system.
 *
 * @param path Pathname of object within file system
 * @param d    Pointer to disk_info structure that will be filled in
 * @return     On success 1 is returned, on error 0 is returned
 */
int
path_disk_info(const char *path, disk_info *d)
{
	assert(path != NULL);
	assert(d != NULL);

#ifdef _WIN32
	{
		ULARGE_INTEGER avail, total;

		/* free bytes available to the caller, total bytes on the volume */
		if (!GetDiskFreeSpaceExA(path, &avail, &total, NULL)) {
			return 0;
		}
		d->size = (uint64_t) total.QuadPart;
		d->free = (uint64_t) avail.QuadPart;
	}
#else
	{
		struct statvfs s;

		if (statvfs(path, &s) != 0) {
			return 0;
		}
		d->size = (uint64_t) s.f_blocks * (uint64_t) s.f_frsize;
		d->free = (uint64_t) s.f_bavail * (uint64_t) s.f_frsize;
	}
#endif

	return 1;
}

/**
 * Log details about the current Operating System version.
 *
 * Called during program start-up.
 *
 * "Windows 11" or "macOS 15.3" is what a bug report says, so that is what is
 * wanted here. Neither comes from the obvious call: GetVersionEx() is
 * deprecated and reports Windows 8 on anything newer unless the application
 * carries a manifest saying otherwise, and uname() on a Mac gives the Darwin
 * kernel version, which no user recognises.
 */
void
rpcemu_log_os(void)
{
#ifdef _WIN32
	/* RtlGetVersion() is the one call that answers truthfully with no
	   manifest. It lives in ntdll rather than in an import library, so it is
	   looked up rather than linked - the same approach podules.c takes. */
	typedef LONG (WINAPI *rtl_get_version)(PRTL_OSVERSIONINFOW);

	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	rtl_get_version fn = ntdll != NULL
	    ? (rtl_get_version) (void *) GetProcAddress(ntdll, "RtlGetVersion")
	    : NULL;

	if (fn != NULL) {
		RTL_OSVERSIONINFOW info;

		memset(&info, 0, sizeof(info));
		info.dwOSVersionInfoSize = sizeof(info);
		if (fn(&info) == 0) {
			/* Windows 11 reports itself as 10.0; the build number is
			   what tells the two apart, so it is logged either way. */
			rpclog("OS: SysName = Windows %s\n",
			       info.dwBuildNumber >= 22000 ? "11" : "10");
			rpclog("OS: Release = %lu.%lu build %lu\n",
			       (unsigned long) info.dwMajorVersion,
			       (unsigned long) info.dwMinorVersion,
			       (unsigned long) info.dwBuildNumber);
			return;
		}
	}
	rpclog("OS: SysName = Windows\n");
#else
	struct utsname u;

	if (uname(&u) == -1) {
		rpclog("OS: Could not determine: %s\n", strerror(errno));
		return;
	}

	rpclog("OS: SysName = %s\n", u.sysname);
	rpclog("OS: Release = %s\n", u.release);
	rpclog("OS: Version = %s\n", u.version);
	rpclog("OS: Machine = %s\n", u.machine);

#ifdef __APPLE__
	/* uname() has given the Darwin release above, which does not match what
	   the machine calls itself. Both are logged: the kernel version is still
	   the one an ARM/Intel difference shows up in. */
	{
		/* Sized as the kernel declares them: osproductversion is char[48],
		   and osversion is OSVERSIZE, which is 256. */
		char product[48];
		char build[256];
		size_t len = sizeof(product);

		if (sysctlbyname("kern.osproductversion", product, &len, NULL, 0) == 0) {
			rpclog("OS: macOS = %s\n", product);
		}
		len = sizeof(build);
		if (sysctlbyname("kern.osversion", build, &len, NULL, 0) == 0) {
			rpclog("OS: macOS build = %s\n", build);
		}
	}
#endif
#endif
}


