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
 * netcapcmd.h - the control socket for network capture.
 *
 * The third of the sockets, beside HostCmd (drive the guest's command line)
 * and DebugCmd (inspect and control the CPU), and built to the same shape: a
 * line of text in, a line of JSON out, serviced from the emulator thread.
 *
 * ★ It can also stop speaking text and start speaking pcap.
 *
 * `pcap` turns the connection into a live capture stream - the file header,
 * then a record per frame as it happens - which is the whole point of it:
 *
 *     rpcemu-netcap --socket <sock> --pcap - | wireshark -k -i -
 *
 * gives real Wireshark, live, with every dissector it has. A window of our own
 * is worth having for people who have not got Wireshark, but it is never going
 * to be a better Wireshark, and a socket that speaks the format costs almost
 * nothing to provide.
 *
 * Verbs, all newline-terminated:
 *
 *   ping                    pong, for checking the socket is alive
 *   help                    the list
 *   status                  what is being captured and how much, as JSON
 *   start <path> [maxbytes] begin writing a pcap file, 0 = no limit
 *   stop                    close it
 *   clear                   forget the frames held in memory, zero the counts
 *   ring on|off             keep frames in memory for tail/follow
 *   tail [n]                the last n frames as JSON, one per line, then "end"
 *   follow                  every frame from now on, as JSON, until the client goes
 *   pcap                    the same but as a raw pcap stream, binary from here
 *   quit                    disconnect
 */

#ifndef NETCAPCMD_H
#define NETCAPCMD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void netcapcmd_init(void);
void netcapcmd_reset(void);
void netcapcmd_close(void);

/** Serviced from the emulator thread, beside hostcmd_poll and debugcmd_poll. */
void netcapcmd_poll(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* NETCAPCMD_H */
