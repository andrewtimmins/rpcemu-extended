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

#ifdef __cplusplus
}
#endif

#endif /* MACHINE_LOCK_H */
