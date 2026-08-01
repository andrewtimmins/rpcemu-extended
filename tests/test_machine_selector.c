/*
 * The machine selector a VNC client sees when no machine was named.
 *
 * Two halves. The navigation is checked as a state machine, and the drawing is
 * rendered into a buffer and written out as an image, because "does this look
 * right" is not something an assertion can answer and a selector nobody can read
 * is no use however correct its indices are.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine_selector.h"
#include "text_screen.h"

#define XK_Return	0xff0d
#define XK_Escape	0xff1b
#define XK_Up		0xff52
#define XK_Down		0xff54
#define XK_Home		0xff50
#define XK_End		0xff57
#define XK_Page_Down	0xff56

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_str(const char *what, const char *got, const char *want)
{
	const int ok = (got != NULL && want != NULL) ? strcmp(got, want) == 0
	                                            : got == want;

	printf("  %-52s %s", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf(" (got %s, wanted %s)", got ? got : "NULL", want ? want : "NULL");
	}
	printf("\n");
	if (!ok) {
		failures++;
	}
}

/* A PPM, so the result can be looked at without linking an image library into a
   unit test. Converted to PNG outside if anyone wants to view it. */
static void
write_ppm(const char *path, const TextScreen *s)
{
	FILE *f = fopen(path, "wb");
	int y, x;

	if (f == NULL) {
		printf("  (could not write %s)\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
	for (y = 0; y < s->height; y++) {
		for (x = 0; x < s->width; x++) {
			const uint32_t p = s->pixels[y * s->stride + x];
			const unsigned char rgb[3] = {
				(unsigned char) ((p >> 16) & 0xff),
				(unsigned char) ((p >> 8) & 0xff),
				(unsigned char) (p & 0xff)
			};

			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("  wrote %s\n", path);
}

int
main(int argc, char *argv[])
{
	MachineSelector m;
	/* Zeroed, not merely assigned field by field. TextScreen has grown a member
	   since this was written and an uninitialised one made the whole test hang on
	   macOS while passing on Linux, which is the worst way to find out. */
	TextScreen s = { 0 };
	const int w = 640, h = 480;
	int i;

	printf("navigation\n");
	machine_selector_init(&m);
	check("empty list has no current machine",
	    machine_selector_current(&m) == NULL);
	check("a key on an empty list is ignored",
	    machine_selector_key(&m, XK_Down) == MACHINE_SELECTOR_IGNORED);
	check("Escape still works on an empty list",
	    machine_selector_key(&m, XK_Escape) == MACHINE_SELECTOR_CANCELLED);

	machine_selector_add(&m, "Default");
	machine_selector_add(&m, "RISC OS 3.71");
	machine_selector_add(&m, "usbtest");
	machine_selector_add(&m, "Kinetic 512MB");
	check_str("first entry is selected", machine_selector_current(&m), "Default");

	check("Down moves", machine_selector_key(&m, XK_Down) == MACHINE_SELECTOR_REDRAW);
	check_str("...to the second entry", machine_selector_current(&m),
	    "RISC OS 3.71");
	check("Up moves back", machine_selector_key(&m, XK_Up) == MACHINE_SELECTOR_REDRAW);
	check_str("...to the first", machine_selector_current(&m), "Default");

	/* Clamping rather than wrapping: in a short list, wrapping surprises. */
	check("Up on the first entry does nothing",
	    machine_selector_key(&m, XK_Up) == MACHINE_SELECTOR_IGNORED);
	check_str("...and stays put", machine_selector_current(&m), "Default");
	machine_selector_key(&m, XK_End);
	check_str("End goes to the last", machine_selector_current(&m),
	    "Kinetic 512MB");
	check("Down on the last does nothing",
	    machine_selector_key(&m, XK_Down) == MACHINE_SELECTOR_IGNORED);
	check("Page Down past the end clamps",
	    machine_selector_key(&m, XK_Page_Down) == MACHINE_SELECTOR_IGNORED);
	machine_selector_key(&m, XK_Home);
	check_str("Home goes back to the first", machine_selector_current(&m),
	    "Default");

	check("a digit jumps", machine_selector_key(&m, '3') == MACHINE_SELECTOR_REDRAW);
	check_str("...to that entry", machine_selector_current(&m), "usbtest");
	check("a digit past the end is ignored",
	    machine_selector_key(&m, '9') == MACHINE_SELECTOR_IGNORED);
	check_str("...leaving the selection alone", machine_selector_current(&m),
	    "usbtest");
	check("an unrelated key is ignored",
	    machine_selector_key(&m, 'q') == MACHINE_SELECTOR_IGNORED);
	check("Enter chooses",
	    machine_selector_key(&m, XK_Return) == MACHINE_SELECTOR_CHOSEN);
	check_str("...the highlighted machine", machine_selector_current(&m),
	    "usbtest");

	printf("\noverflow\n");
	{
		MachineSelector big;

		machine_selector_init(&big);
		for (i = 0; i < MACHINE_SELECTOR_MAX + 10; i++) {
			char name[32];

			snprintf(name, sizeof(name), "machine%d", i);
			machine_selector_add(&big, name);
		}
		check("stores up to the limit", big.count == MACHINE_SELECTOR_MAX);
		check("counts everything found", big.total == MACHINE_SELECTOR_MAX + 10);
	}

	printf("\na long name cannot overflow its cell\n");
	{
		MachineSelector longname;
		char name[256];

		machine_selector_init(&longname);
		memset(name, 'A', sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		machine_selector_add(&longname, name);
		check("stored truncated, not overflowed",
		    strlen(machine_selector_current(&longname))
		        == MACHINE_SELECTOR_NAME - 1);
	}

	printf("\ndrawing\n");
	s.pixels = calloc((size_t) w * (size_t) h, sizeof(uint32_t));
	s.width = w;
	s.height = h;
	s.stride = w;
	if (s.pixels == NULL) {
		printf("  out of memory\n");
		return 1;
	}

	machine_selector_draw(&m, &s);
	check("something was drawn", s.pixels[0] != 0);
	/* The highlight must actually be a different colour from the background, or
	   the selection is invisible however correct the index is. */
	{
		uint32_t bg = s.pixels[0];
		int differing = 0;
		size_t n;

		for (n = 0; n < (size_t) w * (size_t) h; n++) {
			if (s.pixels[n] != bg) {
				differing++;
			}
		}
		check("more than a background was painted", differing > 1000);
	}
	if (argc > 1) {
		write_ppm(argv[1], &s);
	}

	/* A client can be any size, including absurd. Neither must fault. */
	{
		TextScreen tiny = { s.pixels, 64, 32, w };
		TextScreen wide = { s.pixels, 640, 480, w };

		machine_selector_draw(&m, &tiny);
		check("drawing into a tiny screen does not fault", 1);
		{
			MachineSelector none;

			machine_selector_init(&none);
			machine_selector_draw(&none, &wide);
			check("the empty-list screen draws", 1);
		}
		if (argc > 2) {
			write_ppm(argv[2], &wide);
		}
	}

	/* The macOS CI hang: a caller that never set `scale` handed over stack rubbish,
	   and a large positive value turned one draw into CONSOLE_FONT_HEIGHT * scale
	   iterations per glyph. Nothing was wrong with the picture; the test simply
	   never finished. Checked here because a hang is not something the rest of the
	   suite would report as a failure. */
	printf("\na daft scale is refused rather than believed\n");
	{
		TextScreen bad = { s.pixels, w, h, w, 200000000 };

		check("an absurd scale reads back as unscaled", text_screen_scale(&bad) == 1);
		bad.scale = -1;
		check("a negative scale reads back as unscaled", text_screen_scale(&bad) == 1);
		bad.scale = 0;
		check("zero reads back as unscaled", text_screen_scale(&bad) == 1);
		bad.scale = 3;
		check("a sane scale is honoured", text_screen_scale(&bad) == 3);
		text_screen_set_scale(&bad, 999);
		check("setting an absurd scale clamps it",
		    text_screen_scale(&bad) <= TEXT_SCREEN_MAX_SCALE);

		/* Draws, and returns. If the clamp were gone this would not come back. */
		bad.scale = 200000000;
		machine_selector_draw(&m, &bad);
		check("drawing with an absurd scale still returns", 1);
	}

	free(s.pixels);
	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
