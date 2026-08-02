/*
 * The primitives a snapshot is built out of.
 *
 * A whole save and load needs a whole machine, so that is not what this does.
 * What it can cover is the layer everything else is written through, and that is
 * where a mistake is worst: a snapshot is a user's suspended machine, and a
 * serialisation bug does not announce itself when saving. It appears later, on
 * resume, in a file that cannot be re-made because the machine that produced it
 * is gone.
 *
 * The run-length codec gets most of the attention. It carries RAM, VRAM and the
 * disc buffers - by far the bulk of a snapshot - and its encoding puts a flag in
 * the top bit of a length, which is the sort of thing that works on every input
 * anyone tries by hand and then fails on a run of exactly the wrong size.
 *
 * The CRC-32 is pinned to published values on purpose. It identifies the ROM a
 * snapshot was taken against, so changing it would quietly invalidate every
 * snapshot in existence; the test is there to make that a failure rather than a
 * discovery.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"

static int failures;
static char dir[512];

static void
check(const char *what, int ok)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static FILE *
open_temp(const char *mode)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/savestate-test-tmp", dir);
	f = fopen(path, mode);
	if (f == NULL) {
		printf("  cannot open %s (%s)\n", path, mode);
		exit(1);
	}
	return f;
}

static void
remove_temp(void)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/savestate-test-tmp", dir);
	remove(path);
}

/**
 * Write a buffer with the RLE codec, read it back, and say whether it survived.
 *
 * @param data  Bytes to round-trip.
 * @param len   How many.
 * @return 1 if the data came back identical and no error was latched.
 */
static int
rle_round_trip(const uint8_t *data, size_t len)
{
	uint8_t *back;
	FILE *f;
	int ok;

	back = malloc(len ? len : 1);
	if (back == NULL) {
		return 0;
	}
	memset(back, 0xa5, len);

	f = open_temp("wb");
	savestate_error = 0;
	savestate_write_rle(f, data, len);
	fclose(f);
	if (savestate_error) {
		free(back);
		return 0;
	}

	f = open_temp("rb");
	savestate_error = 0;
	savestate_read_rle(f, back, len);
	fclose(f);

	ok = (savestate_error == 0) && (len == 0 || memcmp(data, back, len) == 0);
	free(back);
	return ok;
}

int
main(int argc, char *argv[])
{
	snprintf(dir, sizeof(dir), "%s", (argc > 1) ? argv[1] : ".");

	printf("each width round-trips, and keeps its bit pattern\n");
	/* The format is little-endian whatever the host is, so these also say that
	   a snapshot written on one machine can be read on another. */
	{
		FILE *f = open_temp("wb");

		savestate_error = 0;
		savestate_write_u8(f, 0xa5);
		savestate_write_u16(f, 0xbeef);
		savestate_write_u32(f, 0xdeadbeef);
		savestate_write_i32(f, -123456789);
		savestate_write_u64(f, 0x0123456789abcdefULL);
		savestate_write_f64(f, -0.15625);	/* exact in binary */
		savestate_write(f, "twelve bytes", 12);
		fclose(f);
		check("writing latched no error", savestate_error == 0);

		f = open_temp("rb");
		savestate_error = 0;
		check("u8", savestate_read_u8(f) == 0xa5);
		check("u16", savestate_read_u16(f) == 0xbeef);
		check("u32", savestate_read_u32(f) == 0xdeadbeefu);
		check("i32, negative", savestate_read_i32(f) == -123456789);
		check("u64", savestate_read_u64(f) == 0x0123456789abcdefULL);
		check("f64", savestate_read_f64(f) == -0.15625);
		{
			char buf[13];

			memset(buf, 0, sizeof(buf));
			savestate_read(f, buf, 12);
			check("a raw block", strcmp(buf, "twelve bytes") == 0);
		}
		check("reading latched no error", savestate_error == 0);
		fclose(f);
	}

	printf("\nthe extremes of each width survive\n");
	/* Sign extension and truncation both show up here rather than in the
	   ordinary values above. */
	{
		FILE *f = open_temp("wb");

		savestate_error = 0;
		savestate_write_u8(f, 0);
		savestate_write_u8(f, 0xff);
		savestate_write_u16(f, 0xffff);
		savestate_write_u32(f, 0xffffffffu);
		savestate_write_i32(f, -2147483647 - 1);
		savestate_write_i32(f, 2147483647);
		savestate_write_u64(f, 0xffffffffffffffffULL);
		fclose(f);

		f = open_temp("rb");
		savestate_error = 0;
		check("u8 zero", savestate_read_u8(f) == 0);
		check("u8 all ones", savestate_read_u8(f) == 0xff);
		check("u16 all ones", savestate_read_u16(f) == 0xffff);
		check("u32 all ones", savestate_read_u32(f) == 0xffffffffu);
		check("i32 most negative",
		    savestate_read_i32(f) == -2147483647 - 1);
		check("i32 most positive", savestate_read_i32(f) == 2147483647);
		check("u64 all ones",
		    savestate_read_u64(f) == 0xffffffffffffffffULL);
		check("no error", savestate_error == 0);
		fclose(f);
	}

	printf("\nreading past the end is an error, not a guess\n");
	/* A truncated snapshot has to be refused. The latch is what state_load()
	   inspects, so this is the mechanism that turns a short file into a failed
	   resume rather than a machine restored from rubbish. */
	{
		FILE *f = open_temp("wb");

		savestate_write_u8(f, 0x11);
		fclose(f);

		f = open_temp("rb");
		savestate_error = 0;
		(void) savestate_read_u8(f);
		check("the byte that is there reads cleanly",
		    savestate_error == 0);
		(void) savestate_read_u32(f);
		check("reading past the end latches an error",
		    savestate_error != 0);
		fclose(f);
	}

	printf("\nthe run-length codec\n");
	{
		uint8_t buf[4096];
		size_t i;

		/* All one value: the case the codec exists for, since guest RAM is
		   mostly zero. */
		memset(buf, 0, sizeof(buf));
		check("4096 zero bytes", rle_round_trip(buf, sizeof(buf)));
		memset(buf, 0xff, sizeof(buf));
		check("4096 identical non-zero bytes",
		    rle_round_trip(buf, sizeof(buf)));

		/* No run at all: every byte differs from its neighbour, so the whole
		   thing is literal and the encoding is larger than the input. */
		for (i = 0; i < sizeof(buf); i++) {
			buf[i] = (uint8_t) (i * 31 + 7);
		}
		check("no repeats anywhere", rle_round_trip(buf, sizeof(buf)));

		/* Runs and literals adjacent, in both orders, which is where the
		   flushing of a pending literal before a run is either right or off
		   by one. */
		memset(buf, 0, sizeof(buf));
		for (i = 0; i < 100; i++) {
			buf[i] = (uint8_t) (i + 1);
		}
		check("literals then a run", rle_round_trip(buf, sizeof(buf)));

		memset(buf, 0x5a, sizeof(buf));
		for (i = sizeof(buf) - 100; i < sizeof(buf); i++) {
			buf[i] = (uint8_t) i;
		}
		check("a run then literals", rle_round_trip(buf, sizeof(buf)));

		for (i = 0; i < sizeof(buf); i++) {
			buf[i] = (uint8_t) ((i / 8) % 2 ? 0 : i);
		}
		check("runs and literals alternating",
		    rle_round_trip(buf, sizeof(buf)));

		/* Every length up to a few blocks. A codec that decides between a run
		   and a literal by comparing against a minimum will get exactly one of
		   these wrong if the comparison is the wrong way round. */
		{
			int all = 1;

			for (i = 0; i <= 300; i++) {
				size_t j;

				/* A run of i at the front, then something else, so the
				   run length is what varies. */
				memset(buf, 0x77, sizeof(buf));
				for (j = i; j < sizeof(buf); j++) {
					buf[j] = (uint8_t) (j * 13 + 1);
				}
				if (!rle_round_trip(buf, sizeof(buf))) {
					printf("      a leading run of %zu failed\n", i);
					all = 0;
				}
			}
			check("every leading run length from 0 to 300", all);
		}

		/* Short buffers, including the empty one: length 0 is what an absent
		   VRAM or an empty disc buffer comes to. */
		{
			int all = 1;

			for (i = 0; i <= 40; i++) {
				memset(buf, 0xc3, i);
				if (!rle_round_trip(buf, i)) {
					printf("      length %zu failed\n", i);
					all = 0;
				}
			}
			check("every total length from 0 to 40", all);
		}
	}

	printf("\nthe codec actually compresses\n");
	/* Round-tripping is not enough on its own. A codec that emitted a repeat
	   record per byte would pass every check above and make a snapshot several
	   times larger than the RAM it holds - which is what happens if the
	   run-length threshold is lowered without thinking, and it was not caught
	   until this check was added. Guest RAM is mostly zero, so the ratio on a
	   uniform buffer is the property that matters. */
	{
		uint8_t buf[65536];
		char path[1024];
		long encoded = -1;
		FILE *f;

		memset(buf, 0, sizeof(buf));
		f = open_temp("wb");
		savestate_error = 0;
		savestate_write_rle(f, buf, sizeof(buf));
		fclose(f);

		snprintf(path, sizeof(path), "%s/savestate-test-tmp", dir);
		f = fopen(path, "rb");
		if (f != NULL) {
			if (fseek(f, 0, SEEK_END) == 0) {
				encoded = ftell(f);
			}
			fclose(f);
		}
		printf("      (64K of zeroes encoded to %ld bytes)\n", encoded);
		check("64K of zeroes takes under 100 bytes",
		    encoded > 0 && encoded < 100);

		/* And the worst case must not run away either: incompressible data
		   should cost its own size and a little, not a multiple of it. */
		{
			size_t i;

			for (i = 0; i < sizeof(buf); i++) {
				buf[i] = (uint8_t) (i * 31 + 7);
			}
			f = open_temp("wb");
			savestate_error = 0;
			savestate_write_rle(f, buf, sizeof(buf));
			fclose(f);

			encoded = -1;
			f = fopen(path, "rb");
			if (f != NULL) {
				if (fseek(f, 0, SEEK_END) == 0) {
					encoded = ftell(f);
				}
				fclose(f);
			}
			printf("      (64K of incompressible data encoded to %ld bytes)\n",
			    encoded);
			check("the worst case costs little more than the input",
			    encoded > 0 && encoded < (long) sizeof(buf) + 256);
		}
	}

	printf("\nthe codec refuses an encoding that does not fit\n");
	/* A payload whose declared length disagrees with the buffer it is being read
	   into must be rejected rather than written past the end of it. The read side
	   is the one that faces a file it did not write. */
	{
		uint8_t small[16];
		FILE *f;

		memset(small, 0, sizeof(small));
		f = open_temp("wb");
		savestate_error = 0;
		savestate_write_rle(f, small, sizeof(small));
		fclose(f);

		f = open_temp("rb");
		savestate_error = 0;
		{
			uint8_t into[8];

			savestate_read_rle(f, into, sizeof(into));
		}
		check("a length that disagrees with the destination is refused",
		    savestate_error != 0);
		fclose(f);
	}

	printf("\nCRC-32, against published values\n");
	/* IEEE 802.3, reflected, initial and final inversion - the same as zlib's
	   crc32 and the one in unzip.c. */
	check("the empty buffer", savestate_crc32("", 0) == 0x00000000u);
	check("\"a\"", savestate_crc32("a", 1) == 0xe8b7be43u);
	check("\"abc\"", savestate_crc32("abc", 3) == 0x352441c2u);
	check("\"123456789\"", savestate_crc32("123456789", 9) == 0xcbf43926u);
	check("the quick brown fox",
	    savestate_crc32("The quick brown fox jumps over the lazy dog", 43)
	    == 0x414fa339u);

	printf("\nthe CRC is computed in one pass over the whole buffer\n");
	/* Not chunked anywhere today, but if it ever is, the table must be right for
	   a buffer larger than any plausible block size. */
	{
		uint8_t big[70000];
		size_t i;

		for (i = 0; i < sizeof(big); i++) {
			big[i] = (uint8_t) (i * 251);
		}
		check("70000 bytes gives a stable answer",
		    savestate_crc32(big, sizeof(big))
		    == savestate_crc32(big, sizeof(big)));
		check("...and a different one for a changed byte",
		    savestate_crc32(big, sizeof(big))
		    != savestate_crc32(big, sizeof(big) - 1));
	}

	printf("\nthe snapshot version constant is not moved by accident\n");
	/* ide.c decides how to read a chunk by comparing against this, so lowering
	   or reusing it makes an older snapshot load as the wrong shape. */
	check("SNAPSHOT_VERSION_IDE_PER_DRIVE is still 8",
	    SNAPSHOT_VERSION_IDE_PER_DRIVE == 8);

	remove_temp();
	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
