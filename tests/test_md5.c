/*
 * MD5, against the answers RFC 1321 publishes.
 *
 * This is what decides whether a downloaded package is the file its index said
 * it would be, so a wrong digest is not a wrong number on a screen: it either
 * rejects every package or accepts a corrupted one. Our own implementation was
 * written from the algorithm rather than taken from a library (see docs/packages.md
 * and the comment at the top of md5.h), which is exactly the case that wants
 * known-answer tests: nothing else can tell a subtly wrong MD5 from a right one,
 * because both produce sixteen plausible-looking bytes.
 *
 * The lengths around 55, 56, 63, 64 and 119 are not arbitrary. MD5 appends a
 * one bit, pads to 56 bytes modulo 64 and then writes an eight-byte length, so
 * those are the inputs where the padding needs an extra block, and they are
 * where an implementation written from the specification goes wrong. Every
 * expected digest here comes from a reference implementation, not from this one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/** Hex-encode a digest the way md5_file_hex does, so the two can be compared. */
static void
hex(const uint8_t digest[16], char *out)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 16; i++) {
		out[i * 2] = digits[digest[i] >> 4];
		out[i * 2 + 1] = digits[digest[i] & 0x0f];
	}
	out[32] = '\0';
}

/** One-shot digest of a buffer, as 32 hex digits. */
static void
md5_hex(const void *data, size_t len, char *out)
{
	MD5Context ctx;
	uint8_t digest[16];

	md5_init(&ctx);
	md5_update(&ctx, (const uint8_t *) data, len);
	md5_final(&ctx, digest);
	hex(digest, out);
}

static void
expect(const char *label, const void *data, size_t len, const char *want)
{
	char got[33];
	char what[96];

	md5_hex(data, len, got);
	snprintf(what, sizeof(what), "%s", label);
	if (strcmp(got, want) != 0) {
		printf("  %-56s FAIL\n", what);
		printf("      wanted %s\n      got    %s\n", want, got);
		failures++;
	} else {
		printf("  %-56s ok\n", what);
	}
}

int
main(int argc, char *argv[])
{
	const char *dir = (argc > 1) ? argv[1] : ".";
	char path[1024];

	printf("the RFC 1321 test suite\n");
	expect("empty string", "", 0,
	    "d41d8cd98f00b204e9800998ecf8427e");
	expect("\"a\"", "a", 1,
	    "0cc175b9c0f1b6a831c399e269772661");
	expect("\"abc\"", "abc", 3,
	    "900150983cd24fb0d6963f7d28e17f72");
	expect("\"message digest\"", "message digest", 14,
	    "f96b697d7cb7938d525a2f31aaf161d0");
	expect("the lower-case alphabet",
	    "abcdefghijklmnopqrstuvwxyz", 26,
	    "c3fcd3d76192e4007dfb496cca67e13b");
	expect("alphanumerics, both cases",
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62,
	    "d174ab98d277d9f5a5611c2c9f419d9f");
	expect("eight repeats of \"1234567890\"",
	    "1234567890123456789012345678901234567890"
	    "1234567890123456789012345678901234567890", 80,
	    "57edf4a22be3c955ac49da2e2107b67a");

	printf("\nlengths where the padding changes shape\n");
	{
		/* Digests of 'a' repeated N times, from a reference implementation. */
		static const struct {
			size_t len;
			const char *digest;
		} cases[] = {
			{   54, "eced9e0b81ef2bba605cbc5e2e76a1d0" },
			{   55, "ef1772b6dff9a122358552954ad0df65" },
			{   56, "3b0c8ac703f828b04c6c197006d17218" },
			{   57, "652b906d60af96844ebd21b674f35e93" },
			{   63, "b06521f39153d618550606be297466d5" },
			{   64, "014842d480b571495a4a0363793f7367" },
			{   65, "c743a45e0d2e6a95cb859adae0248435" },
			{  119, "8a7bd0732ed6a28ce75f6dabc90e1613" },
			{  120, "5f61c0ccad4cac44c75ff505e1f1e537" },
			{  121, "f6acfca2d47c87f2b14ca038234d3614" },
			{  127, "020406e1d05cdc2aa287641f7ae2cc39" },
			{  128, "e510683b3f5ffe4093d021808bc6ff70" },
			{  129, "b325dc1c6f5e7a2b7cf465b9feab7948" },
			{ 1000, "cabe45dcc9ae5b66ba86600cca6b8ba8" },
		};
		char buf[1000];
		char label[64];
		size_t i;

		memset(buf, 'a', sizeof(buf));
		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			snprintf(label, sizeof(label), "%4zu bytes", cases[i].len);
			expect(label, buf, cases[i].len, cases[i].digest);
		}
	}

	printf("\nfed in pieces, the answer is the same\n");
	/* A package is hashed in whatever chunks the reader hands over, so the
	   buffering between update() calls has to be invisible. A byte at a time is
	   the worst case for it. */
	{
		char buf[300];
		char one_shot[33];
		char piecemeal[33];
		MD5Context ctx;
		uint8_t digest[16];
		size_t i;
		int same_all = 1;

		for (i = 0; i < sizeof(buf); i++) {
			buf[i] = (char) (i * 7 + 3);
		}
		md5_hex(buf, sizeof(buf), one_shot);

		md5_init(&ctx);
		for (i = 0; i < sizeof(buf); i++) {
			md5_update(&ctx, (const uint8_t *) buf + i, 1);
		}
		md5_final(&ctx, digest);
		hex(digest, piecemeal);
		check("one byte at a time", strcmp(one_shot, piecemeal) == 0);

		/* And at every chunk size up to two blocks, which is where a
		   buffer-fill boundary can be crossed mid-chunk. */
		for (i = 1; i <= 130; i++) {
			size_t off;

			md5_init(&ctx);
			for (off = 0; off < sizeof(buf); off += i) {
				size_t n = sizeof(buf) - off;

				if (n > i) {
					n = i;
				}
				md5_update(&ctx, (const uint8_t *) buf + off, n);
			}
			md5_final(&ctx, digest);
			hex(digest, piecemeal);
			if (strcmp(one_shot, piecemeal) != 0) {
				printf("      chunk size %zu disagrees\n", i);
				same_all = 0;
			}
		}
		check("every chunk size from 1 to 130", same_all);

		/* A zero-length update in the middle must change nothing: a reader
		   that returns 0 bytes without being at the end is ordinary. */
		md5_init(&ctx);
		md5_update(&ctx, (const uint8_t *) buf, 100);
		md5_update(&ctx, (const uint8_t *) buf, 0);
		md5_update(&ctx, (const uint8_t *) buf + 100, sizeof(buf) - 100);
		md5_final(&ctx, digest);
		hex(digest, piecemeal);
		check("a zero-length update is ignored",
		    strcmp(one_shot, piecemeal) == 0);
	}

	printf("\nmd5_file_hex\n");
	{
		char got[33];
		FILE *f;

		snprintf(path, sizeof(path), "%s/md5-test-file", dir);
		f = fopen(path, "wb");
		if (f == NULL) {
			printf("  cannot write %s\n", path);
			return 1;
		}
		fputs("abc", f);
		fclose(f);
		check("a file hashes to the same as its contents",
		    md5_file_hex(path, got) == 0 &&
		    strcmp(got, "900150983cd24fb0d6963f7d28e17f72") == 0);

		/* Larger than whatever it reads in one go, so the loop is exercised. */
		f = fopen(path, "wb");
		if (f != NULL) {
			size_t i;

			for (i = 0; i < 100000; i++) {
				fputc('a', f);
			}
			fclose(f);
		}
		check("100000 bytes read in however many reads it takes",
		    md5_file_hex(path, got) == 0 &&
		    strcmp(got, "1af6d6f2f682f76f80e606aeaaee1680") == 0);

		/* Empty is a real case: it has a digest, and it is not an error. */
		f = fopen(path, "wb");
		if (f != NULL) {
			fclose(f);
		}
		check("an empty file is hashed, not refused",
		    md5_file_hex(path, got) == 0 &&
		    strcmp(got, "d41d8cd98f00b204e9800998ecf8427e") == 0);

		remove(path);
		check("a missing file reports failure", md5_file_hex(path, got) == -1);
	}

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
