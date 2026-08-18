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
 * machine_lock - one running emulator per machine.
 *
 * Nothing used to stop the same machine being started twice, and both copies then
 * write to the same files: `cmos.ram` and the machine's configuration on the way
 * out, where the last to exit silently discards the other's changes, and any hard
 * disc image throughout, where two processes interleaving sector writes corrupts
 * the filesystem. Each write is flushed to the host now, which makes the
 * interleaving more thorough rather than less.
 *
 * It has always been possible to do by hand. What makes it worth guarding is
 * running several machines at once becoming ordinary: a list of machines with a
 * Start button puts the mistake one click away, and the damage is not obvious until
 * ADFS starts complaining much later.
 *
 * The lock is an exclusive lock held by the operating system on a file in the
 * machine's own directory, rather than a file whose existence means "locked".
 * That matters because the kernel drops it when the process dies, however it dies,
 * so a crash or a kill cannot leave a machine permanently unstartable. There is
 * nothing to clean up and no staleness to reason about.
 *
 * The file's contents - the process id and the VNC port - are for people to read,
 * and for anything asking where a running machine can be reached. They are
 * informational only: the lock is the lock.
 */

#ifndef MACHINE_LOCK_H
#define MACHINE_LOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Claim a machine for this process.
 *
 * @param machine_dir The machine's own directory, as rpcemu_get_machine_datadir().
 * @param vnc_port    Recorded in the file so a reader can find the running copy;
 *                    0 if VNC is not in use.
 * @return 1 if the machine is now ours, 0 if another process holds it.
 */
extern int machine_lock_acquire(const char *machine_dir, int vnc_port);

/**
 * Record the port the VNC server actually ended up listening on.
 *
 * The port is not known to be real when the lock is taken: VNC may be switched
 * off, or may fail to bind. Writing the configured port regardless would advertise
 * a port nothing is listening on, so the lock is taken with 0 and corrected once
 * the server is up.
 *
 * @param vnc_port The live port, or 0 if there is no server.
 */
extern void machine_lock_set_vnc_port(int vnc_port);

/**
 * Record the local control-channel endpoint a managed child (see
 * machine_ipc.h) is listening on, so the Manager can find it without a
 * separate discovery step. An AF_UNIX path on Linux/macOS, "tcp:<port>" on
 * Windows. Empty (the default) for every process that is not a managed
 * child - a plain single-window launch has nothing to advertise here.
 */
extern void machine_lock_set_ipc_endpoint(const char *ipc_endpoint);

/**
 * Release it. Safe to call when nothing was acquired.
 *
 * Called on an orderly exit for tidiness; the operating system would do it anyway.
 */
extern void machine_lock_release(void);

/**
 * Who holds a machine, for a message worth reading.
 *
 * Reads the file rather than the lock, so treat the answer as a hint: the holder
 * may have exited between the failed acquire and this call.
 *
 * @return 1 if something was read, 0 otherwise.
 */
extern int machine_lock_read_owner(const char *machine_dir, long *pid, int *vnc_port);

/**
 * The running owner's IPC endpoint, if it has one (see machine_lock_set_ipc_endpoint).
 *
 * @return 1 if an endpoint was read, 0 if there is no lock file or no managed
 *         child has recorded one (an ordinary single-window machine, most of
 *         the time).
 */
extern int machine_lock_read_ipc_endpoint(const char *machine_dir, char *endpoint_out, size_t endpoint_out_size);

/**
 * Is the process that wrote a lock file still there?
 *
 * The lock file is only a record. The lock itself is held by the kernel and
 * dropped the moment its holder dies, so a machine that crashed or was killed
 * leaves the file behind saying it is still running. Anything that believes the
 * file rather than checking is talking about a process that no longer exists -
 * which is how the Manager came to attach to dead machines and then report that
 * they had failed to start.
 *
 * Asked of the pid rather than by taking the lock, deliberately. Taking it would
 * be the stronger answer, but it would also mean grabbing the lock of a machine
 * that is in the middle of starting and turning it away.
 *
 * @param pid Process id from machine_lock_read_owner()
 * @return    1 if that process exists, 0 if it does not (or pid is not valid)
 */
extern int machine_lock_owner_alive(long pid);

/**
 * Record where this machine's debugger is listening, so a tool can find it.
 *
 * A machine may be configured with any socket path, so nothing can work one out
 * from the outside; this is the machine saying where it actually bound.
 */
extern void machine_lock_set_debug_endpoint(const char *debug_endpoint);

/**
 * Record where this machine's network capture socket is listening.
 *
 * Same reason as the debugger's: a machine may be configured with any path, so
 * nothing outside can work one out. This is the machine saying where it bound.
 */
extern void machine_lock_set_netcap_endpoint(const char *netcap_endpoint);

/**
 * Record where this machine's HostCmd channel is listening.
 *
 * A tool cannot work the answer out for itself: the path may be named in the
 * machine's configuration, and on Windows it is a TCP port that may have moved
 * because another machine already held the one asked for. Looking for a socket
 * file instead finds one left behind by a machine that was killed, which is a
 * machine that is not running answering for one that is.
 */
extern void machine_lock_set_hostcmd_endpoint(const char *hostcmd_endpoint);

/**
 * Whether a recorded endpoint names a filesystem socket rather than a TCP port.
 *
 * An endpoint is one of two things and every tool has to tell them apart, so the
 * rule lives here, beside the code that writes them, rather than in three copies
 * that drift: an absolute path means AF_UNIX, anything else is host:port. On
 * Windows it is always the latter, because there is no AF_UNIX to record and the
 * port may have moved from the one the configuration asked for.
 */
extern int machine_lock_endpoint_is_path(const char *endpoint);

/**
 * The running machine's HostCmd endpoint, if it recorded one.
 *
 * @return 1 if an endpoint was read, 0 otherwise
 */
extern int machine_lock_read_hostcmd_endpoint(const char *machine_dir,
                                             char *endpoint_out,
                                             size_t endpoint_out_size);

/**
 * The running machine's debugger endpoint, if it recorded one.
 *
 * @return 1 if an endpoint was read, 0 otherwise
 */
extern int machine_lock_read_debug_endpoint(const char *machine_dir,
                                            char *endpoint_out, size_t endpoint_out_size);

/**
 * The running machine's network capture endpoint, if it recorded one.
 *
 * @return 1 if an endpoint was read, 0 otherwise
 */
extern int machine_lock_read_netcap_endpoint(const char *machine_dir,
                                             char *endpoint_out, size_t endpoint_out_size);

#ifdef __cplusplus
}
#endif

#endif /* MACHINE_LOCK_H */
