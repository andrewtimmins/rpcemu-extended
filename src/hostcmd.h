/*
  RPCEmu - An Acorn system emulator

  HostCmd - expose the guest RISC OS command line to the host.

  The host connects to a local socket and submits command lines; a RISC OS
  gateway module (riscos-progs/HostCmd) polls the emulator for those commands
  via the ArcEm HostCmd SWI, runs each through OS_CLI capturing its output,
  and hands the output back over the same SWI to be streamed to the host.

  Copyright (C) 2025-2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 */

#ifndef HOSTCMD_H
#define HOSTCMD_H

#include <stddef.h>
#include <stdint.h>

#include "hostfs.h"	/* ARMul_State + ARMul_LoadByte/StoreByte shims */

#ifdef __cplusplus
extern "C" {
#endif

/* SWI handler, dispatched on R9. Runs on the emulator thread (from opSWI). */
void hostcmd(ARMul_State *state);

/* Lifecycle - all invoked on the emulator thread, alongside the HostFS
   siblings in rpcemu.c, so no locking is required. */
void hostcmd_init(void);	/**< Create + bind + listen if enabled; else no-op. */
void hostcmd_reset(void);	/**< Abort any in-flight command on machine reset. */
void hostcmd_close(void);	/**< Tear down listener/client and unlink the socket. */
void hostcmd_poll(void);	/**< Non-blocking accept/recv/send; call each tick. */

/*
 * Running a command from inside the emulator.
 *
 * The channel was built for an outside client (rpcemu-run, the MCP server), but
 * the emulator itself has reason to ask the guest things: whether a module a
 * package needs is present, running a package's install triggers, telling the
 * Filer to re-read a directory it has cached.
 *
 * These share the same single-command machinery, so an internal command and an
 * external client take turns rather than interleaving.
 *
 * ALL OF THESE RUN ON THE EMULATOR THREAD, like everything else here, so there
 * is still no locking. A caller on another thread (the GUI) must reach them
 * through the emulator's command queue, and gets the result back through the
 * completion callback below - which is also called on the emulator thread.
 */

/**
 * Called when an internal command finishes.
 *
 * @param rc      The command's return code, as RISC OS reported it.
 * @param output  Everything the command printed. Not NUL-terminated by
 *                contract; use @len. Valid only for the duration of the call.
 * @param len     Length of @output.
 * @param opaque  Whatever was given to hostcmd_internal_submit().
 */
typedef void (*hostcmd_internal_done_fn)(uint32_t rc, const char *output,
                                         size_t len, void *opaque);

/**
 * Queue a command for the guest to run.
 *
 * @return 0 if it was queued; -1 if the channel is busy with another command, if
 *         the command is too long for OS_CLI, or if the guest support module has
 *         never announced itself (so nothing would ever collect it).
 */
int hostcmd_internal_submit(const char *command,
                            hostcmd_internal_done_fn done, void *opaque);

/** True if the guest module has registered, so a command would be collected. */
int hostcmd_internal_ready(void);

/** True while an internal command is queued or running. */
int hostcmd_internal_busy(void);

/**
 * Abandon the internal command, if any.
 *
 * The completion callback is not called. A command already handed to the guest
 * still runs to completion there; its output is simply discarded.
 */
void hostcmd_internal_abandon(void);

#ifdef __cplusplus
}
#endif

#endif /* HOSTCMD_H */
