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
 * test_netcapture - the frames the machine sends and receives.
 *
 * Two things here are worth testing without a machine. The pcap headers,
 * because a file another program has to open is either exactly right or
 * useless and the failure is silent until somebody tries Wireshark; and the
 * ring, because it is read by a thread that is not the one writing it and its
 * whole job is to say honestly what the reader missed.
 *
 * Every expected byte below is taken from the pcap format's own definition,
 * written out by hand, not from asking the code what it produced.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcapture.h"

static int failures;

#define CHECK(what, got, want)                                                 \
	do {                                                                   \
		const long long g = (long long) (got), w = (long long) (want); \
									       \
		if (g != w) {                                                  \
			printf("  FAIL %-22s got %lld, wanted %lld\n", what,   \
			    g, w);                                             \
			failures++;                                            \
		}                                                              \
	} while (0)

/** Read a host-order 32-bit value back out of a buffer. */
static uint32_t
get32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static uint16_t
get16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

int
main(void)
{
	printf("test_netcapture\n");

	/*
	 * The 24-byte file header, field by field, from the pcap definition:
	 * magic, two version halfwords, a zone, a sigfigs, the snap length and
	 * the link type. Ethernet is link type 1.
	 *
	 * The magic goes out in the host's own byte order deliberately - that is
	 * how the format tells a reader which way round everything else is - so
	 * it is compared as a value rather than as bytes.
	 */
	printf("pcap file header\n");
	{
		uint8_t h[NETCAP_PCAP_HEADER_LEN];

		memset(h, 0xee, sizeof(h));
		netcap_pcap_file_header(h);

		CHECK("magic", get32(h + 0), 0xa1b2c3d4u);
		CHECK("version major", get16(h + 4), 2);
		CHECK("version minor", get16(h + 6), 4);
		CHECK("thiszone", get32(h + 8), 0);
		CHECK("sigfigs", get32(h + 12), 0);
		CHECK("snaplen", get32(h + 16), NETCAP_SNAPLEN);
		CHECK("linktype", get32(h + 20), 1);
	}

	/*
	 * The 16-byte record header: seconds, microseconds, how much was kept,
	 * how long the frame really was. The last two differ only for a frame
	 * longer than the snap length, and keeping the real length is the whole
	 * reason the format carries both.
	 */
	printf("pcap record header\n");
	{
		uint8_t r[NETCAP_PCAP_RECORD_LEN];

		memset(r, 0xee, sizeof(r));
		netcap_pcap_record_header(r, 0x11223344u, 999999u, 60u, 1514u);

		CHECK("ts_sec", get32(r + 0), 0x11223344u);
		CHECK("ts_usec", get32(r + 4), 999999u);
		CHECK("incl_len", get32(r + 8), 60);
		CHECK("orig_len", get32(r + 12), 1514);
	}

	/*
	 * A capture file, written and read back.
	 *
	 * Two frames of known contents, so the check is on the bytes rather than
	 * on the call having returned. The expected size is worked out here: the
	 * file header, then a record header and the frame for each.
	 */
	printf("writing a file\n");
	{
		static const uint8_t frame_a[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0x06, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x06 };
		static const uint8_t frame_b[] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x52, 0x55, 0x0a, 0x0a, 0x0a, 0x02, 0x08, 0x00, 0x45 };
		const char *path = "test_netcapture.pcap";
		uint8_t buf[512];
		size_t got;
		FILE *f;
		NetcapStats st;

		netcap_init();
		netcap_clear();
		CHECK("start", netcap_file_start(path, 0), 1);
		CHECK("active", netcap_file_active(), 1);

		netcap_frame(NETCAP_TX, frame_a, sizeof(frame_a));
		netcap_frame(NETCAP_RX, frame_b, sizeof(frame_b));

		netcap_get_stats(&st);
		CHECK("frames", st.frames, 2);
		CHECK("tx", st.frames_tx, 1);
		CHECK("rx", st.frames_rx, 1);
		CHECK("bytes", st.bytes, sizeof(frame_a) + sizeof(frame_b));

		netcap_file_stop();
		CHECK("stopped", netcap_file_active(), 0);

		f = fopen(path, "rb");
		if (f == NULL) {
			printf("  FAIL could not reopen the capture\n");
			failures++;
		} else {
			got = fread(buf, 1, sizeof(buf), f);
			fclose(f);

			/* 24 + (16 + 14) + (16 + 15) */
			CHECK("file size", got,
			    NETCAP_PCAP_HEADER_LEN +
			    NETCAP_PCAP_RECORD_LEN + sizeof(frame_a) +
			    NETCAP_PCAP_RECORD_LEN + sizeof(frame_b));
			CHECK("file magic", get32(buf), 0xa1b2c3d4u);
			/* First record: 14 bytes kept, 14 real, then the frame itself. */
			CHECK("rec1 incl", get32(buf + 24 + 8), sizeof(frame_a));
			CHECK("rec1 orig", get32(buf + 24 + 12), sizeof(frame_a));
			CHECK("rec1 data",
			    memcmp(buf + 24 + 16, frame_a, sizeof(frame_a)), 0);
			/* Second follows immediately, no padding between records. */
			CHECK("rec2 incl",
			    get32(buf + 24 + 16 + sizeof(frame_a) + 8), sizeof(frame_b));
			CHECK("rec2 data",
			    memcmp(buf + 24 + 16 + sizeof(frame_a) + 16, frame_b,
			        sizeof(frame_b)), 0);
		}
		remove(path);
	}

	/*
	 * The size limit.
	 *
	 * Checked before each frame, so the file stops at or below the limit and
	 * never part-way through a record - a truncated final record makes the
	 * whole file unreadable to some tools rather than merely short.
	 */
	printf("stopping when full\n");
	{
		static const uint8_t frame[64] = { 0 };
		const char *path = "test_netcapture_full.pcap";
		NetcapStats st;
		int i;

		netcap_clear();
		/* Header 24, then 80 a frame. A limit of 200 leaves room for two
		   (24 + 80 + 80 = 184) and refuses the third at 264. */
		CHECK("start capped", netcap_file_start(path, 200), 1);
		for (i = 0; i < 10; i++) {
			netcap_frame(NETCAP_TX, frame, sizeof(frame));
		}
		netcap_get_stats(&st);
		CHECK("closed itself", st.file_active, 0);
		CHECK("said why", st.file_stopped_full, 1);
		CHECK("within limit", st.file_bytes <= 200, 1);
		CHECK("wrote two", st.file_bytes,
		    NETCAP_PCAP_HEADER_LEN + 2 * (NETCAP_PCAP_RECORD_LEN + 64));
		netcap_file_stop();
		remove(path);
	}

	/*
	 * The ring.
	 *
	 * A reader asks for whatever is newer than the last frame it saw. The
	 * serial numbers are what make that answerable: they count from one and
	 * are never reused, so a gap is unambiguous.
	 */
	printf("frames kept in memory\n");
	{
		static const uint8_t frame[32] = { 0 };
		NetcapFrame *out = malloc(sizeof(*out) * NETCAP_RING_FRAMES);
		unsigned count = 0;
		uint64_t oldest;
		int i;

		if (out == NULL) {
			printf("  FAIL out of memory\n");
			return EXIT_FAILURE;
		}

		netcap_clear();
		CHECK("ring off", netcap_ring_enabled(), 0);
		/*
		 * Off by default, so nothing is kept and a reader gets nothing.
		 *
		 * The counters must not move either. That is not bookkeeping
		 * pedantry: it is the observable half of the early return that keeps
		 * netcap_frame() down to two loads when nobody is capturing, and
		 * without it every frame would take the lock forever after - a
		 * diagnostic quietly becoming a cost on the path it exists to watch.
		 */
		netcap_frame(NETCAP_TX, frame, sizeof(frame));
		netcap_copy_since(0, out, NETCAP_RING_FRAMES, &count);
		CHECK("nothing kept", count, 0);
		{
			NetcapStats idle;

			netcap_get_stats(&idle);
			CHECK("no frames counted", idle.frames, 0);
			CHECK("no bytes counted", idle.bytes, 0);
		}

		netcap_ring_enable(1);
		netcap_clear();
		CHECK("ring on", netcap_ring_enabled(), 1);

		for (i = 0; i < 5; i++) {
			netcap_frame((i & 1) ? NETCAP_RX : NETCAP_TX, frame, sizeof(frame));
		}
		oldest = netcap_copy_since(0, out, NETCAP_RING_FRAMES, &count);
		CHECK("five kept", count, 5);
		CHECK("oldest is 1", oldest, 1);
		/* In the order they happened, numbered from one. */
		CHECK("first serial", out[0].serial, 1);
		CHECK("last serial", out[4].serial, 5);
		CHECK("first is tx", out[0].direction, NETCAP_TX);
		CHECK("second is rx", out[1].direction, NETCAP_RX);
		CHECK("length kept", out[0].length, sizeof(frame));

		/* Asking again with the last serial seen returns only what is new. */
		netcap_copy_since(5, out, NETCAP_RING_FRAMES, &count);
		CHECK("nothing new", count, 0);
		netcap_frame(NETCAP_TX, frame, sizeof(frame));
		netcap_copy_since(5, out, NETCAP_RING_FRAMES, &count);
		CHECK("one new", count, 1);
		CHECK("it is 6", out[0].serial, 6);

		free(out);
	}

	/*
	 * A reader that falls behind.
	 *
	 * Filling the ring twice over must not silently pretend nothing was lost:
	 * the oldest serial still held is how a reader learns it has a gap, and
	 * the dropped count is how a person does.
	 */
	printf("a reader that falls behind\n");
	{
		static const uint8_t frame[32] = { 0 };
		NetcapFrame *out = malloc(sizeof(*out) * NETCAP_RING_FRAMES);
		unsigned count = 0;
		uint64_t oldest;
		NetcapStats st;
		int i;

		if (out == NULL) {
			printf("  FAIL out of memory\n");
			return EXIT_FAILURE;
		}
		netcap_clear();
		for (i = 0; i < NETCAP_RING_FRAMES + 10; i++) {
			netcap_frame(NETCAP_TX, frame, sizeof(frame));
		}
		oldest = netcap_copy_since(0, out, NETCAP_RING_FRAMES, &count);

		CHECK("ring is full", count, NETCAP_RING_FRAMES);
		/* 4106 frames through a 4096 ring: the first ten are gone, so the
		   oldest still held is number 11. */
		CHECK("oldest serial", oldest, 11);
		CHECK("newest serial", out[count - 1].serial, NETCAP_RING_FRAMES + 10);
		netcap_get_stats(&st);
		CHECK("ten overwritten", st.dropped_ring, 10);

		free(out);
		netcap_ring_enable(0);
		CHECK("ring off again", netcap_ring_enabled(), 0);
	}

	netcap_close();

	if (failures != 0) {
		printf("FAILED: %d check(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("PASS\n");
	return EXIT_SUCCESS;
}
