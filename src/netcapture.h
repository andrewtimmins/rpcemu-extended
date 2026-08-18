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
 * netcapture.h - every Ethernet frame the machine sends or receives.
 *
 * There is one place frames are collected and three things that want them: a
 * pcap file for Wireshark, a window that lists them as they happen, and a
 * control socket that hands them to whatever is listening. Those used to be
 * one thing - a FILE * inside the NAT state - and the other two would each
 * have grown their own copy of the same tap.
 *
 * ★ The frames arrive on the EMULATOR thread and must never make it wait.
 *
 * netcap_frame() is called from the network SWI path, which runs on the thread
 * that is the emulator's whole speed budget. Everything here is therefore
 * bounded and short: a memcpy into a slot that already exists, under a mutex no
 * reader holds for longer than its own memcpy. Nothing allocates, nothing
 * blocks on a reader, and with capture switched off it is two loads and a
 * return.
 */

#ifndef NETCAPTURE_H
#define NETCAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Bytes kept of each frame.
 *
 * A Risc PC's Ethernet is 1514 bytes plus its own headroom, so this keeps
 * every frame whole in practice while still bounding the ring. A frame longer
 * than this is recorded truncated, with its real length kept, which is exactly
 * what pcap's two length fields are for.
 */
#define NETCAP_SNAPLEN		1600

/** Frames held in memory for the analyser and the socket. */
#define NETCAP_RING_FRAMES	4096

/** Which way a frame was going, as the guest sees it. */
typedef enum {
	NETCAP_TX = 0,		/**< The guest sent it */
	NETCAP_RX = 1		/**< The guest received it */
} NetcapDirection;

/**
 * One captured frame.
 *
 * Pointer-free and fixed size so a reader can take a copy without owning
 * anything, the same reason the machine inspector's snapshot is a POD.
 */
typedef struct {
	uint64_t serial;	/**< Counts from 1, never reused: a reader that
				     sees a gap knows exactly what it missed */
	uint32_t sec;		/**< Host wall clock, as pcap records it */
	uint32_t usec;
	uint32_t length;	/**< The frame's real length */
	uint32_t captured;	/**< Bytes present in data[] */
	uint8_t direction;	/**< NetcapDirection */
	uint8_t data[NETCAP_SNAPLEN];
} NetcapFrame;

typedef struct {
	uint64_t frames;	/**< Frames seen since the counters were cleared */
	uint64_t bytes;		/**< Their total real length */
	uint64_t frames_tx;
	uint64_t frames_rx;
	uint64_t file_bytes;	/**< Written to the capture file */
	uint64_t dropped_ring;	/**< Overwritten in the ring before being read */
	int file_active;	/**< A capture file is open */
	int file_stopped_full;	/**< It stopped itself on reaching the size limit */
	int ring_active;
	char file_path[512];	/**< The file being written, or empty */
} NetcapStats;

/* ---- lifecycle -------------------------------------------------------- */

void netcap_init(void);
void netcap_reset(void);
void netcap_close(void);

/* ---- the hot path ------------------------------------------------------ */

/**
 * Record one complete Ethernet frame.
 *
 * Called from the emulator thread for every frame in either direction,
 * whether or not anything is capturing. Returns immediately when nothing is.
 */
void netcap_frame(NetcapDirection direction, const void *data, size_t length);

/* ---- capture to a file ------------------------------------------------- */

/**
 * Start writing a pcap file, replacing any already open.
 *
 * @param path      Where to write. An existing file is overwritten.
 * @param max_bytes Stop at this size, or 0 for no limit. Checked before each
 *                  frame, so the file never exceeds it by more than one frame.
 * @return non-zero if the file was opened
 */
int netcap_file_start(const char *path, uint64_t max_bytes);

void netcap_file_stop(void);
int netcap_file_active(void);

/* ---- frames kept in memory --------------------------------------------- */

/**
 * Whether frames are held in memory for the analyser and the socket.
 *
 * Off by default: with nothing looking at them it is a memcpy per frame for
 * no reason.
 */
void netcap_ring_enable(int enable);
int netcap_ring_enabled(void);

/** Forget every frame held in memory, and zero the counters. */
void netcap_clear(void);

/**
 * Copy out the frames newer than one already seen.
 *
 * @param after_serial Serial of the last frame the caller has, or 0 for all
 *                     of them
 * @param out          Where to put them, caller-owned
 * @param max          How many @p out can hold
 * @param count        Set to how many were copied
 * @return the serial of the oldest frame still held, so a caller that has
 *         fallen behind can tell it has missed some
 */
uint64_t netcap_copy_since(uint64_t after_serial, NetcapFrame *out,
    unsigned max, unsigned *count);

/* ---- counters ---------------------------------------------------------- */

void netcap_get_stats(NetcapStats *stats);

/* ---- pcap, for anything writing the format itself ---------------------- */

/**
 * The 24-byte pcap file header.
 *
 * Exposed because the control socket streams the same format down a socket
 * rather than to a file, and two spellings of one header is how they come to
 * disagree.
 *
 * @param out Buffer of at least NETCAP_PCAP_HEADER_LEN bytes
 */
#define NETCAP_PCAP_HEADER_LEN		24
#define NETCAP_PCAP_RECORD_LEN		16

void netcap_pcap_file_header(uint8_t *out);

/**
 * The 16-byte per-frame record header.
 *
 * @param out Buffer of at least NETCAP_PCAP_RECORD_LEN bytes
 */
void netcap_pcap_record_header(uint8_t *out, uint32_t sec, uint32_t usec,
    uint32_t captured, uint32_t length);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* NETCAPTURE_H */
