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
 * netcapture.c - see netcapture.h for what this is for.
 *
 * ★ The pcap format is written by hand, on purpose.
 *
 * It is a 24-byte file header and a 16-byte header per frame, and that is the
 * whole of it. Depending on libpcap for forty bytes of structure would put a
 * library on the build of all three platforms - and a Windows one that does
 * not exist without WinPcap or Npcap - to save writing them out.
 *
 * The magic number is written in the host's own byte order, which is not
 * laziness: it is how the format tells a reader which order the rest of the
 * fields are in. A reader that sees it byte-swapped swaps everything else too.
 */

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include "rpcemu.h"
#include "netcapture.h"

#define PCAP_MAGIC		0xa1b2c3d4u
#define PCAP_VERSION_MAJOR	2
#define PCAP_VERSION_MINOR	4
#define PCAP_LINKTYPE_ETHERNET	1

typedef struct {
	pthread_mutex_t lock;
	int initialised;

	/* The file. */
	FILE *file;
	uint64_t file_bytes;
	uint64_t file_max_bytes;	/* 0 = no limit */
	int file_stopped_full;
	char file_path[512];

	/* The frames kept in memory. */
	int ring_active;
	NetcapFrame *ring;		/* NETCAP_RING_FRAMES entries, or NULL */
	unsigned ring_next;		/* where the next frame goes */
	uint64_t ring_oldest_serial;	/* serial of the oldest frame still held */

	/* Counters. */
	uint64_t serial;		/* frames seen, and the last serial issued */
	uint64_t bytes;
	uint64_t frames_tx;
	uint64_t frames_rx;
	uint64_t dropped_ring;
} NetCapture;

static NetCapture nc;

/**
 * A little-endian-agnostic 32-bit write: the value goes out in the host's own
 * order, which is what the magic number tells the reader to expect.
 */
static void
put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static void
put16(uint8_t *p, uint16_t v)
{
	memcpy(p, &v, sizeof(v));
}

void
netcap_pcap_file_header(uint8_t *out)
{
	put32(out + 0, PCAP_MAGIC);
	put16(out + 4, PCAP_VERSION_MAJOR);
	put16(out + 6, PCAP_VERSION_MINOR);
	put32(out + 8, 0);			/* thiszone: timestamps are UTC */
	put32(out + 12, 0);			/* sigfigs, always zero in practice */
	put32(out + 16, NETCAP_SNAPLEN);
	put32(out + 20, PCAP_LINKTYPE_ETHERNET);
}

void
netcap_pcap_record_header(uint8_t *out, uint32_t sec, uint32_t usec,
                          uint32_t captured, uint32_t length)
{
	put32(out + 0, sec);
	put32(out + 4, usec);
	put32(out + 8, captured);
	put32(out + 12, length);
}

/** Now, in the two fields pcap wants. Zero for both if the clock refuses. */
static void
timestamp_now(uint32_t *sec, uint32_t *usec)
{
#ifdef _WIN32
	/*
	 * Windows has no gettimeofday. GetSystemTimeAsFileTime counts 100ns
	 * intervals from 1601, so the epoch difference comes off before the
	 * split - 11644473600 seconds, which is why this is done in 64 bits.
	 */
	FILETIME ft;
	uint64_t t;

	GetSystemTimeAsFileTime(&ft);
	t = ((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	t /= 10;				/* to microseconds */
	t -= UINT64_C(11644473600000000);	/* 1601 -> 1970 */
	*sec = (uint32_t) (t / 1000000u);
	*usec = (uint32_t) (t % 1000000u);
#else
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0) {
		*sec = 0;
		*usec = 0;
		return;
	}
	*sec = (uint32_t) tv.tv_sec;
	*usec = (uint32_t) tv.tv_usec;
#endif
}

void
netcap_init(void)
{
	if (nc.initialised) {
		return;
	}
	memset(&nc, 0, sizeof(nc));
	pthread_mutex_init(&nc.lock, NULL);
	nc.initialised = 1;
}

void
netcap_reset(void)
{
	/*
	 * A reset is the machine restarting, not the capture ending: somebody
	 * watching a boot wants to keep watching it. The file and the ring stay
	 * as they are.
	 */
}

void
netcap_close(void)
{
	if (!nc.initialised) {
		return;
	}
	netcap_file_stop();
	pthread_mutex_lock(&nc.lock);
	free(nc.ring);
	nc.ring = NULL;
	nc.ring_active = 0;
	pthread_mutex_unlock(&nc.lock);
	pthread_mutex_destroy(&nc.lock);
	nc.initialised = 0;
}

int
netcap_file_start(const char *path, uint64_t max_bytes)
{
	uint8_t header[NETCAP_PCAP_HEADER_LEN];
	FILE *f;

	if (path == NULL || path[0] == '\0') {
		return 0;
	}
	netcap_init();
	netcap_file_stop();

	f = fopen(path, "wb");
	if (f == NULL) {
		rpclog("Netcap: could not open '%s' to capture into\n", path);
		return 0;
	}
	netcap_pcap_file_header(header);
	if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
		rpclog("Netcap: could not write the header of '%s'\n", path);
		fclose(f);
		return 0;
	}
	(void) fflush(f);

	pthread_mutex_lock(&nc.lock);
	nc.file = f;
	nc.file_bytes = sizeof(header);
	nc.file_max_bytes = max_bytes;
	nc.file_stopped_full = 0;
	strncpy(nc.file_path, path, sizeof(nc.file_path) - 1);
	nc.file_path[sizeof(nc.file_path) - 1] = '\0';
	pthread_mutex_unlock(&nc.lock);

	rpclog("Netcap: capturing to '%s'\n", path);
	return 1;
}

void
netcap_file_stop(void)
{
	FILE *f;

	if (!nc.initialised) {
		return;
	}
	pthread_mutex_lock(&nc.lock);
	f = nc.file;
	nc.file = NULL;
	nc.file_path[0] = '\0';
	pthread_mutex_unlock(&nc.lock);

	/* Closed outside the lock: fclose flushes, and the emulator thread must
	   not wait behind a disc write to record its next frame. */
	if (f != NULL) {
		fclose(f);
	}
}

int
netcap_file_active(void)
{
	return nc.initialised && nc.file != NULL;
}

void
netcap_ring_enable(int enable)
{
	netcap_init();
	pthread_mutex_lock(&nc.lock);
	if (enable) {
		if (nc.ring == NULL) {
			nc.ring = calloc(NETCAP_RING_FRAMES, sizeof(nc.ring[0]));
		}
		nc.ring_active = (nc.ring != NULL);
	} else {
		nc.ring_active = 0;
		free(nc.ring);
		nc.ring = NULL;
		nc.ring_next = 0;
		nc.ring_oldest_serial = 0;
	}
	pthread_mutex_unlock(&nc.lock);
}

int
netcap_ring_enabled(void)
{
	return nc.initialised && nc.ring_active;
}

void
netcap_clear(void)
{
	if (!nc.initialised) {
		return;
	}
	pthread_mutex_lock(&nc.lock);
	nc.ring_next = 0;
	nc.ring_oldest_serial = 0;
	nc.serial = 0;
	nc.bytes = 0;
	nc.frames_tx = 0;
	nc.frames_rx = 0;
	nc.dropped_ring = 0;
	if (nc.ring != NULL) {
		memset(nc.ring, 0, (size_t) NETCAP_RING_FRAMES * sizeof(nc.ring[0]));
	}
	pthread_mutex_unlock(&nc.lock);
}

void
netcap_frame(NetcapDirection direction, const void *data, size_t length)
{
	uint32_t sec, usec;
	uint32_t captured;

	/*
	 * The cheap way out, taken for every frame when nobody is capturing.
	 * Read without the lock on purpose: both are set by a person choosing
	 * something in a dialogue, so the worst a race can do is miss one frame
	 * at the moment capture is switched on.
	 */
	if (!nc.initialised || (nc.file == NULL && !nc.ring_active)) {
		return;
	}
	if (data == NULL || length == 0) {
		return;
	}

	captured = (length > NETCAP_SNAPLEN) ? NETCAP_SNAPLEN : (uint32_t) length;
	timestamp_now(&sec, &usec);

	pthread_mutex_lock(&nc.lock);

	nc.serial++;
	nc.bytes += length;
	if (direction == NETCAP_TX) {
		nc.frames_tx++;
	} else {
		nc.frames_rx++;
	}

	if (nc.ring != NULL && nc.ring_active) {
		NetcapFrame *slot = &nc.ring[nc.ring_next];

		/* Overwriting a frame nobody has read is worth counting: it is the
		   difference between "nothing happened" and "you were not looking". */
		if (slot->serial != 0) {
			nc.dropped_ring++;
		}
		slot->serial = nc.serial;
		slot->sec = sec;
		slot->usec = usec;
		slot->length = (uint32_t) length;
		slot->captured = captured;
		slot->direction = (uint8_t) direction;
		memcpy(slot->data, data, captured);

		nc.ring_next = (nc.ring_next + 1u) % NETCAP_RING_FRAMES;
		if (nc.serial > NETCAP_RING_FRAMES) {
			nc.ring_oldest_serial = nc.serial - NETCAP_RING_FRAMES + 1;
		} else {
			nc.ring_oldest_serial = 1;
		}
	}

	if (nc.file != NULL) {
		const uint64_t want = NETCAP_PCAP_RECORD_LEN + captured;

		if (nc.file_max_bytes != 0 &&
		    nc.file_bytes + want > nc.file_max_bytes)
		{
			/*
			 * Full. Closed here rather than left to write short records,
			 * and said in the log, because a capture that quietly stops is
			 * indistinguishable from a network that quietly went idle -
			 * which is exactly what somebody would be trying to diagnose.
			 */
			FILE *f = nc.file;

			nc.file = NULL;
			nc.file_stopped_full = 1;
			pthread_mutex_unlock(&nc.lock);
			fclose(f);
			rpclog("Netcap: capture file reached its %llu byte limit and was "
			       "closed\n", (unsigned long long) nc.file_max_bytes);
			return;
		}

		{
			uint8_t record[NETCAP_PCAP_RECORD_LEN];

			netcap_pcap_record_header(record, sec, usec, captured,
			    (uint32_t) length);
			if (fwrite(record, 1, sizeof(record), nc.file) == sizeof(record) &&
			    fwrite(data, 1, captured, nc.file) == captured)
			{
				nc.file_bytes += want;
			}
			/*
			 * Flushed per frame. A capture is nearly always being read while
			 * it is still being written - by Wireshark, or by somebody who
			 * has just reproduced the fault and gone to look - and a frame
			 * still in a stdio buffer is a frame that appears not to have
			 * happened.
			 */
			(void) fflush(nc.file);
		}
	}

	pthread_mutex_unlock(&nc.lock);
}

uint64_t
netcap_copy_since(uint64_t after_serial, NetcapFrame *out, unsigned max,
                  unsigned *count)
{
	uint64_t oldest;
	unsigned n = 0;
	unsigned i;

	if (count != NULL) {
		*count = 0;
	}
	if (!nc.initialised || out == NULL || max == 0) {
		return 0;
	}

	pthread_mutex_lock(&nc.lock);
	oldest = nc.ring_oldest_serial;
	if (nc.ring != NULL && nc.ring_active) {
		/*
		 * Walked oldest-first from the slot after the newest, so the frames
		 * come out in the order they happened whatever state the ring is in.
		 */
		for (i = 0; i < NETCAP_RING_FRAMES && n < max; i++) {
			const NetcapFrame *slot =
			    &nc.ring[(nc.ring_next + i) % NETCAP_RING_FRAMES];

			if (slot->serial == 0 || slot->serial <= after_serial) {
				continue;
			}
			out[n++] = *slot;
		}
	}
	pthread_mutex_unlock(&nc.lock);

	if (count != NULL) {
		*count = n;
	}
	return oldest;
}

void
netcap_get_stats(NetcapStats *stats)
{
	if (stats == NULL) {
		return;
	}
	memset(stats, 0, sizeof(*stats));
	if (!nc.initialised) {
		return;
	}
	pthread_mutex_lock(&nc.lock);
	stats->frames = nc.serial;
	stats->bytes = nc.bytes;
	stats->frames_tx = nc.frames_tx;
	stats->frames_rx = nc.frames_rx;
	stats->file_bytes = nc.file_bytes;
	stats->dropped_ring = nc.dropped_ring;
	stats->file_active = (nc.file != NULL);
	stats->file_stopped_full = nc.file_stopped_full;
	stats->ring_active = nc.ring_active;
	strncpy(stats->file_path, nc.file_path, sizeof(stats->file_path) - 1);
	stats->file_path[sizeof(stats->file_path) - 1] = '\0';
	pthread_mutex_unlock(&nc.lock);
}
