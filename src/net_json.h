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
 * net_json - Ethernet frames over Charles Ferguson's JSON tun/tap server, so a
 * machine here and a RISC OS Pyromaniac can sit on the same virtual network.
 *
 * Pyromaniac has had this for years (its `etherdriverjson` configuration group)
 * and the server is a single Python file:
 *
 *     https://github.com/gerph/tuntap-json-server
 *
 * It listens on TCP, replicates every frame it is given to every other client,
 * and optionally to a real tap interface. That last part is the reason to
 * support it rather than only our own loopback wire: the server can run on a
 * Linux box where creating a tap is straightforward, and Windows and macOS
 * emulators can join it over the network without any of them needing a tap of
 * their own. It also gives one place to watch the whole virtual LAN, which a
 * peer-to-peer design cannot.
 *
 * ★ Not used at the same time as net_switch.
 *
 * Both are hubs. An instance on both would send each frame to its loopback
 * peers AND to the server, which replicates it to the same peers if they are
 * also connected, and every frame would arrive twice. So a machine configured
 * with a server uses that and nothing else; network-nat.c enforces it.
 *
 * WIRE FORMAT. One JSON object per line, and the Ethernet header is taken
 * apart rather than sent whole:
 *
 *     {"frame_type": 2048, "src": [6,2,3,4,5,6],
 *      "dst": [255,255,255,255,255,255], "data": "<base64 payload>"}
 *
 * `data` is the payload only - everything from byte 14 onwards. dst is bytes
 * 0..5, src is 6..11, and frame_type is the big-endian halfword at 12..13. The
 * server does the same split when it reads from a tap, so this is the format
 * and not an interpretation of it.
 *
 * ADDRESSES. This carries frames and nothing else. Two machines that are to
 * talk IP still have to agree addresses between them: guests here are given
 * 10.10.10.10 + slot by their own SLiRP, so anything else joining the same wire
 * needs an address on that network that does not collide. Likewise MAC
 * addresses, which are derived from the slot unless the machine sets one.
 */

#ifndef NET_JSON_H
#define NET_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to the configured server.
 *
 * Does nothing and reports failure when the machine has no server configured,
 * which is how network-nat.c decides whether to use the loopback wire instead.
 * A server that is configured but cannot be reached is reported and the machine
 * carries on without it.
 *
 * @return 0 if connected, -1 otherwise
 */
extern int net_json_init(void);

/**
 * Disconnect and release the socket.
 */
extern void net_json_close(void);

/**
 * Whether frames are going to a server rather than to the loopback wire.
 *
 * @return 1 if connected, 0 if not
 */
extern int net_json_is_connected(void);

/**
 * Send a frame the guest has transmitted.
 *
 * @param frame     Complete Ethernet frame
 * @param frame_len Length in bytes
 */
extern void net_json_tx(const uint8_t *frame, int frame_len);

/**
 * Deliver frames from the server into this guest.
 *
 * The server does not send a client its own frames, so everything arriving here
 * came from somewhere else. Frames not addressed to this machine are dropped as
 * a card would drop them.
 *
 * @return the number of frames delivered
 */
extern int net_json_poll(void);

/*
 * The two halves of the wire format, exposed so they can be tested without a
 * server to talk to. A mistake in either is invisible from inside: the frame is
 * sent, and the other end quietly makes something else of it.
 */

/**
 * Turn an Ethernet frame into the server's JSON line, including the newline.
 *
 * @param frame     Complete Ethernet frame, at least 14 bytes
 * @param frame_len Length in bytes
 * @param out       Where to write the line
 * @param out_size  Size of that buffer
 * @return          Length written, or -1 if the frame is too short or would not fit
 */
extern int net_json_encode(const uint8_t *frame, int frame_len, char *out, size_t out_size);

/**
 * Turn one of the server's JSON lines back into an Ethernet frame.
 *
 * @param line      One line, with or without its newline
 * @param out       Where to write the frame
 * @param out_size  Size of that buffer
 * @return          Frame length, or -1 if the line is malformed or would not fit
 */
extern int net_json_decode(const char *line, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NET_JSON_H */
