/*
 * Typing a host file from its extension.
 *
 * HostFS calls anything without a ",xxx" suffix Text, which is right for a
 * file RISC OS wrote and wrong for one dropped into the folder from the host:
 * a PNG arrived as a Text file called "photo/png", because HostFS also swaps
 * '.' and '/' over.
 *
 * Two things are being guarded here, and the second matters more than the
 * first. One is that the recognised extensions produce the right numbers - a
 * wrong filetype is silently wrong, so the numbers are asserted rather than
 * eyeballed. The other is everything the table must NOT claim: source files,
 * in particular, are Text on RISC OS and are written as "main/c" there, so
 * mapping ".c" would change the type of files RISC OS development already
 * depends on. The default is Text anyway, so leaving them alone is both the
 * safe answer and the correct one.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "hostfs_filetype.h"

static int failures;

static void
check_type(const char *leaf, uint32_t want)
{
	uint32_t got = 0;
	int found = hostfs_filetype_from_leafname(leaf, &got);

	if (found && got == want) {
		printf("ok: %-24s -> &%03X\n", leaf, (unsigned) want);
		return;
	}
	if (!found) {
		printf("FAIL: %-24s -> not recognised, wanted &%03X\n",
		       leaf, (unsigned) want);
	} else {
		printf("FAIL: %-24s -> &%03X, wanted &%03X\n",
		       leaf, (unsigned) got, (unsigned) want);
	}
	failures++;
}

static void
check_untyped(const char *leaf, const char *why)
{
	uint32_t got = 0;

	if (!hostfs_filetype_from_leafname(leaf, &got)) {
		printf("ok: %-24s left alone (%s)\n", leaf, why);
		return;
	}
	printf("FAIL: %-24s was typed &%03X; it should be left alone (%s)\n",
	       leaf, (unsigned) got, why);
	failures++;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	/* ---- the numbers ------------------------------------------------- */

	check_type("photo.png",    0xb60);
	check_type("photo.jpg",    0xc85);
	check_type("photo.jpeg",   0xc85);
	check_type("anim.gif",     0x695);
	check_type("image.bmp",    0x69c);
	check_type("scan.tif",     0xff0);
	check_type("scan.tiff",    0xff0);
	check_type("manual.pdf",   0xadf);
	check_type("index.html",   0xfaf);
	check_type("index.htm",    0xfaf);
	check_type("data.csv",     0xdfe);
	check_type("letter.rtf",   0xc32);
	check_type("tune.mp3",     0x1ad);
	check_type("beep.wav",     0xfb1);

	/*
	 * .zip is Archive &DDC, the type SparkFS and ArcFS use. &A91 is quoted
	 * for it in some places and is not what the registry says; this is here
	 * so that mistake cannot be made twice.
	 */
	check_type("backup.zip",   0xddc);

	/* ---- case ------------------------------------------------------- */

	check_type("PHOTO.PNG",    0xb60);
	check_type("Photo.Png",    0xb60);

	/* ---- what must be left alone ------------------------------------- */

	check_untyped("main.c",       "C source is Text on RISC OS");
	check_untyped("main.h",       "so is a header");
	check_untyped("startup.s",    "and assembler");
	check_untyped("notes.txt",    "already the default");
	check_untyped("ReadMe",       "no extension at all");
	check_untyped("archive.tar",  "not in the table");
	check_untyped(".profile",     "a name, not an extension");
	check_untyped("",             "empty");
	check_untyped("trailing.",    "nothing after the dot");

	/* A directory further up the path must not decide a file's type. */
	check_untyped("/home/me/photos.png/ReadMe",
	              "the leaf has no extension, whatever the directory is called");
	{
		uint32_t got = 0;

		if (hostfs_filetype_from_leafname("/home/me/pics/photo.png", &got)
		    && got == 0xb60) {
			printf("ok: a full path still types on its own leaf\n");
		} else {
			printf("FAIL: a full path did not type on its own leaf\n");
			failures++;
		}
	}

	/* NULL must be handled: the caller passes whatever the filesystem gave. */
	{
		uint32_t got = 0;

		if (!hostfs_filetype_from_leafname(NULL, &got)) {
			printf("ok: a NULL leafname is refused rather than crashing\n");
		} else {
			printf("FAIL: a NULL leafname was accepted\n");
			failures++;
		}
	}

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
