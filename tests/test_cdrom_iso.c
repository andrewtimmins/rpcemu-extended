/*
 * Ejecting a CD-ROM image must forget the file handle.
 *
 * iso_exit() (reached from the menu as Disc > CD-ROM > Empty, and again when
 * a second image is loaded or the emulator shuts down) used to fclose() the
 * handle and leave the pointer behind, and iso_init() does not clear it
 * either. Ejecting twice in a row therefore called fclose() a second time on
 * a FILE that had already been freed.
 *
 * A double fclose() is undefined behaviour, so this test does not try to
 * detect it by crashing - that is the sanitiser job's business, and it will
 * catch the second close directly. What is asserted here is the observable
 * state that goes with a correct release: once the image has been ejected the
 * drive reports empty. Before the fix it reported a disc still present,
 * because only the handle was closed and iso_empty was left alone, which is
 * also what let a later read reach a freed handle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ide.h"
#include "cdrom-iso.h"

/* cdrom-iso.c assigns to this and calls back into the IDE layer; ide.c is not
   linked in, so both are supplied here. The callback only tells the emulated
   drive a disc changed, which has no bearing on handle lifetime. */
ATAPI *atapi;
void atapi_discchanged(void) { }

static int failures;

static void
check(int cond, const char *what)
{
	printf("%s: %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) {
		failures++;
	}
}

/* A file just large enough to be opened and seeked; the contents do not
   matter, only that fopen() succeeds so a handle exists to be leaked. */
static int
make_image(char *path, size_t path_size)
{
	FILE *f;
	static const char sector[2048];

	/* A fixed name, not a pid-derived one: the tests run in their own
	   build directory and this has to compile on Windows too, where
	   getpid() is neither in unistd.h nor spelled the same. */
	snprintf(path, path_size, "test_cdrom_iso.iso");
	f = fopen(path, "wb");
	if (f == NULL) {
		return 0;
	}
	fwrite(sector, sizeof(sector), 1, f);
	fclose(f);
	return 1;
}

int
main(void)
{
	char path[256];

	if (!make_image(path, sizeof(path))) {
		printf("FAIL: could not create a test image\n");
		return 1;
	}

	iso_init();
	check(atapi != NULL, "iso_init() installs the ISO driver");

	check(iso_open(path) == 0, "an image opens");

	/* The first ready() reports the disc change and clears it; the second
	   reports the disc that is now present. */
	atapi->ready();
	check(atapi->ready() != 0, "the drive reports a disc present");

	/* Eject. */
	atapi->exit();
	check(atapi->ready() == 0,
	      "the drive reports empty once the image is ejected");

	/*
	 * Eject again, which is two menu clicks and was a second fclose() on a
	 * freed handle. Reaching the next line at all is the result; under the
	 * sanitisers this is where the original defect is reported.
	 */
	atapi->exit();
	check(atapi->ready() == 0, "a second eject is safe and still empty");

	/* And a read arriving after the eject must not touch the handle. */
	{
		uint8_t buf[2048];

		memset(buf, 0xaa, sizeof(buf));
		atapi->readsector(buf, 0);
		check(buf[0] == 0xaa, "a read after eject is ignored, not served");
	}

	remove(path);

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
