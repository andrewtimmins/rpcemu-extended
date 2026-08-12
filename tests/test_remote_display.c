/*
 * Where the Manager draws a machine's screen, and where a click in that panel
 * lands on it.
 *
 * The bug this exists for: the paint fitted the guest's screen inside the panel
 * and centred it, while the pointer mapping divided by the whole panel. So the
 * guest pointer started offset by the width of the bar down the left and then
 * moved at rect.w/panel_w of the speed of the hand moving it - about four fifths
 * on an ordinary wide window. The host pointer is hidden while a machine is
 * shown, so that slow pointer was the only one on screen, and it read as the
 * whole Manager being sluggish.
 *
 * The checks that matter are the two edges: the left edge of the drawn picture
 * must be guest column 0 and the right edge must be the last column. Get the
 * rectangle wrong and one or both moves, whatever the middle does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote_display_geometry.h"
#include "remote_display_scale.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
check_eq(const char *what, int got, int expected)
{
	if (got != expected) {
		printf("  %-62s FAIL (got %d, expected %d)\n", what, got, expected);
		failures++;
	} else {
		printf("  %-62s ok\n", what);
	}
}

/* The guest pixel under a panel point, for the given panel and frame size. */
static void
map(int cw, int ch, int fw, int fh, int px, int py, int *gx, int *gy)
{
	const struct remote_display_rect rect = remote_display_rect_for(cw, ch, fw, fh);

	remote_display_point_to_guest(rect, fw, fh, px, py, gx, gy);
}

int
main(void)
{
	printf("Manager display geometry\n");

	/* A panel exactly the guest's shape: no bars, and the rectangle is the
	   whole panel. This is the case the old code got right, which is why the
	   fault went unnoticed. */
	{
		const struct remote_display_rect r = remote_display_rect_for(1200, 900, 1600, 1200);
		int gx, gy;

		check_eq("4:3 guest in a 4:3 panel: x", r.x, 0);
		check_eq("4:3 guest in a 4:3 panel: y", r.y, 0);
		check_eq("4:3 guest in a 4:3 panel: width", r.w, 1200);
		check_eq("4:3 guest in a 4:3 panel: height", r.h, 900);

		map(1200, 900, 1600, 1200, 0, 0, &gx, &gy);
		check("top left maps to 0,0", gx == 0 && gy == 0);
		map(1200, 900, 1600, 1200, 1199, 899, &gx, &gy);
		check_eq("right edge maps to the last column", gx, 1599);
		check_eq("bottom edge maps to the last row", gy, 1199);
	}

	/*
	 * A panel wider than the guest's screen: bars down the sides.
	 *
	 * 1600x1200 in 1200x700 fits to the height, so the picture is 933x700 at
	 * x=133. The old mapping used 1600/1200 = 1.33 guest pixels per panel
	 * pixel where 1600/933 = 1.71 is needed, and started at guest 177 rather
	 * than 0.
	 */
	{
		const struct remote_display_rect r = remote_display_rect_for(1200, 700, 1600, 1200);
		int gx, gy;

		check_eq("wide panel: picture width", r.w, 933);
		check_eq("wide panel: picture height", r.h, 700);
		check_eq("wide panel: bar on the left", r.x, 133);
		check_eq("wide panel: no bar on top", r.y, 0);

		map(1200, 700, 1600, 1200, r.x, 0, &gx, &gy);
		check_eq("wide panel: left edge of the picture is guest column 0", gx, 0);

		map(1200, 700, 1600, 1200, r.x + r.w - 1, 0, &gx, &gy);
		check_eq("wide panel: right edge of the picture is the last column", gx, 1599);

		/* Halfway across the picture is halfway across the screen, within the
		   rounding of one guest pixel per panel pixel. */
		map(1200, 700, 1600, 1200, r.x + r.w / 2, r.h / 2, &gx, &gy);
		check("wide panel: centre of the picture is the centre of the screen",
		      abs(gx - 800) <= 2 && abs(gy - 600) <= 2);

		/* Anywhere in the bars is clamped to the edge, not wrapped round or
		   left to run negative. */
		map(1200, 700, 1600, 1200, 0, 350, &gx, &gy);
		check_eq("wide panel: the bar on the left clamps to column 0", gx, 0);
		map(1200, 700, 1600, 1200, 1199, 350, &gx, &gy);
		check_eq("wide panel: the bar on the right clamps to the last column", gx, 1599);
	}

	/* A panel taller than the guest's screen: bars top and bottom, and it is
	   the vertical mapping that has to account for them. */
	{
		const struct remote_display_rect r = remote_display_rect_for(800, 900, 1600, 1200);
		int gx, gy;

		check_eq("tall panel: picture width", r.w, 800);
		check_eq("tall panel: picture height", r.h, 600);
		check_eq("tall panel: bar on top", r.y, 150);

		map(800, 900, 1600, 1200, 0, r.y, &gx, &gy);
		check_eq("tall panel: top of the picture is guest row 0", gy, 0);
		map(800, 900, 1600, 1200, 0, r.y + r.h - 1, &gx, &gy);
		check_eq("tall panel: bottom of the picture is the last row", gy, 1199);
		map(800, 900, 1600, 1200, 0, 0, &gx, &gy);
		check_eq("tall panel: the bar on top clamps to row 0", gy, 0);
	}

	/*
	 * The pointer must cross the guest's screen at the same rate the hand
	 * crosses the picture - the property that was actually broken. Measured as
	 * the guest distance covered between the two edges of the drawn picture,
	 * which has to be the full width of the screen however the panel is shaped.
	 */
	{
		const int shapes[][2] = {
			{ 1200, 700 }, { 1000, 750 }, { 1920, 600 }, { 640, 900 }, { 1600, 1200 },
		};
		size_t i;

		for (i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
			const int cw = shapes[i][0], ch = shapes[i][1];
			const struct remote_display_rect r =
			    remote_display_rect_for(cw, ch, 1600, 1200);
			int x0, y0, x1, y1;
			char label[128];

			map(cw, ch, 1600, 1200, r.x, r.y, &x0, &y0);
			map(cw, ch, 1600, 1200, r.x + r.w - 1, r.y + r.h - 1, &x1, &y1);

			snprintf(label, sizeof(label),
			    "%dx%d panel: the picture spans the whole guest screen", cw, ch);
			check(label, x0 == 0 && y0 == 0 && x1 == 1599 && y1 == 1199);
		}
	}

	/* Degenerate sizes: a panel not laid out yet, and a machine that has not
	   said how big its screen is. Neither may divide by zero. */
	{
		const struct remote_display_rect r = remote_display_rect_for(0, 0, 1600, 1200);
		int gx = -1, gy = -1;

		check("unlaid-out panel gives an empty rectangle", r.w == 0 && r.h == 0);
		remote_display_point_to_guest(r, 1600, 1200, 10, 10, &gx, &gy);
		check("mapping against an empty rectangle gives 0,0", gx == 0 && gy == 0);

		const struct remote_display_rect r2 = remote_display_rect_for(800, 600, 0, 0);

		check("frame of no size gives an empty rectangle", r2.w == 0 && r2.h == 0);
	}

	printf("\nScaling a machine's screen down to the panel\n");

	/* A screen of one colour must come out that colour, whatever the scale:
	   the simplest thing a box average can get wrong is its own arithmetic. */
	{
		const int sw = 320, sh = 240, dw = 133, dh = 100;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *dst = malloc((size_t) dw * dh * 3);
		int i, flat = 1;

		for (i = 0; i < sw * sh; i++) {
			src[i] = 0x00336699;
		}
		check("scaling reports success", remote_display_scale_argb(src, sw, sh, dst, dw, dh) == 1);

		for (i = 0; i < dw * dh; i++) {
			if (dst[3 * i + 0] != 0x33 || dst[3 * i + 1] != 0x66 ||
			    dst[3 * i + 2] != 0x99) {
				flat = 0;
				break;
			}
		}
		check("one colour in, the same colour out, and in R,G,B order", flat);
		free(src);
		free(dst);
	}

	/*
	 * A one-pixel chequerboard, scaled by exactly a half, must come out mid
	 * grey: every destination pixel averages two black and two white source
	 * pixels. This is the property a fixed four-tap bilinear filter loses as
	 * the scale factor grows, and it is what keeps single-pixel text legible
	 * rather than letting it break up.
	 */
	{
		const int sw = 256, sh = 256, dw = 128, dh = 128;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *dst = malloc((size_t) dw * dh * 3);
		int x, y, i, grey = 1;

		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				src[y * sw + x] = ((x + y) & 1) ? 0x00ffffff : 0;
			}
		}
		remote_display_scale_argb(src, sw, sh, dst, dw, dh);

		for (i = 0; i < dw * dh; i++) {
			if (dst[3 * i] < 0x7e || dst[3 * i] > 0x80) {
				grey = 0;
				break;
			}
		}
		check("a 1px chequerboard halved comes out mid grey, not aliased", grey);
		free(src);
		free(dst);
	}

	/*
	 * The same again, but with stripes, which pin down each direction on its
	 * own - a chequerboard comes out grey even if only one axis is being
	 * averaged, because the other axis still mixes a light and a dark pixel.
	 * One-pixel horizontal rules are what a RISC OS window's furniture is made
	 * of, and dropping every other one is exactly how this looked wrong.
	 */
	{
		const int sw = 256, sh = 256, dw = 128, dh = 128;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *dst = malloc((size_t) dw * dh * 3);
		int x, y, i, grey;

		/* Rows alternating light and dark: only averaging down the columns
		   can turn these grey. */
		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				src[y * sw + x] = (y & 1) ? 0x00ffffff : 0;
			}
		}
		remote_display_scale_argb(src, sw, sh, dst, dw, dh);
		grey = 1;
		for (i = 0; i < dw * dh; i++) {
			if (dst[3 * i] < 0x7e || dst[3 * i] > 0x80) {
				grey = 0;
				break;
			}
		}
		check("1px horizontal rules halved: averaged down, not dropped", grey);

		/* And columns alternating, which only averaging across can turn
		   grey. */
		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				src[y * sw + x] = (x & 1) ? 0x00ffffff : 0;
			}
		}
		remote_display_scale_argb(src, sw, sh, dst, dw, dh);
		grey = 1;
		for (i = 0; i < dw * dh; i++) {
			if (dst[3 * i] < 0x7e || dst[3 * i] > 0x80) {
				grey = 0;
				break;
			}
		}
		check("1px vertical rules halved: averaged across, not dropped", grey);
		free(src);
		free(dst);
	}

	/* It must write every destination pixel and not one byte more. Canaries
	   either side, and a fill that would show through as magenta. */
	{
		const int sw = 500, sh = 400, dw = 173, dh = 139;
		const size_t bytes = (size_t) dw * dh * 3;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *buffer = malloc(bytes + 32);
		unsigned char *dst = buffer + 16;
		int i, wrote_all = 1, canary_ok = 1;

		for (i = 0; i < sw * sh; i++) {
			src[i] = 0x00202020;
		}
		memset(buffer, 0xcd, bytes + 32);
		remote_display_scale_argb(src, sw, sh, dst, dw, dh);

		for (i = 0; i < 16; i++) {
			if (buffer[i] != 0xcd || buffer[16 + bytes + i] != 0xcd) {
				canary_ok = 0;
				break;
			}
		}
		check("nothing written outside the destination image", canary_ok);

		for (i = 0; i < (int) bytes; i++) {
			if (dst[i] != 0x20) {
				wrote_all = 0;
				break;
			}
		}
		check("every destination pixel written, at an awkward scale", wrote_all);
		free(src);
		free(buffer);
	}

	/* Sizes that would divide by zero or walk off the end, and a destination
	   the same size as the source (the scale factor of 1 the panel can hit
	   when a window happens to match the guest exactly). */
	{
		const int sw = 64, sh = 48;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *dst = malloc((size_t) sw * sh * 3);
		int i, same = 1;

		for (i = 0; i < sw * sh; i++) {
			src[i] = (uint32_t) ((i & 0xff) << 16 | (i & 0xff) << 8 | (i & 0xff));
		}
		check("a zero-sized destination is refused",
		      remote_display_scale_argb(src, sw, sh, dst, 0, 0) == 0);
		check("a zero-sized source is refused",
		      remote_display_scale_argb(src, 0, 0, dst, sw, sh) == 0);
		check("a null destination is refused",
		      remote_display_scale_argb(src, sw, sh, NULL, sw, sh) == 0);

		remote_display_scale_argb(src, sw, sh, dst, sw, sh);
		for (i = 0; i < sw * sh; i++) {
			const unsigned char expected = (unsigned char) (i & 0xff);

			if (dst[3 * i] != expected || dst[3 * i + 1] != expected ||
			    dst[3 * i + 2] != expected) {
				same = 0;
				break;
			}
		}
		check("1:1 is an exact copy, pixel for pixel", same);
		free(src);
		free(dst);
	}

	printf("\nScaling only the rows the guest redrew\n");

	/*
	 * Which destination rows a change to source rows affects. The bottom has to
	 * round up: a source row contributing any part of a destination row means
	 * that destination row must be redone, and rounding down leaves a
	 * one-pixel line of the previous frame along the bottom edge of everything
	 * that moved.
	 */
	{
		int top, bottom;

		remote_display_scale_rows_for(0, 1200, 1200, 700, &top, &bottom);
		check("a whole screen covers every destination row",
		      top == 0 && bottom == 700);

		remote_display_scale_rows_for(600, 612, 1200, 700, &top, &bottom);
		check("a 12-row band covers a small band, rounded outwards",
		      top == 350 && bottom >= 357 && bottom <= 358);

		remote_display_scale_rows_for(1199, 1200, 1200, 700, &top, &bottom);
		check("the last source row covers the last destination row",
		      bottom == 700 && top == 699);

		remote_display_scale_rows_for(0, 1, 1200, 700, &top, &bottom);
		check("the first source row covers the first destination row",
		      top == 0 && bottom == 1);

		remote_display_scale_rows_for(50, 50, 1200, 700, &top, &bottom);
		check("an empty range covers nothing", top == 0 && bottom == 0);

		remote_display_scale_rows_for(-40, 5000, 1200, 700, &top, &bottom);
		check("a range outside the screen is clamped to it",
		      top == 0 && bottom == 700);
	}

	/*
	 * The band scaler and the whole-image scaler have to agree: scaling a
	 * changed band must give the same pixels as scaling everything, or a
	 * dragged window leaves a seam behind it. Checked by scaling one image
	 * whole and building the other band by band.
	 */
	{
		const int sw = 640, sh = 480, dw = 373, dh = 280;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *whole = malloc((size_t) dw * dh * 3);
		unsigned char *bands = malloc((size_t) dw * dh * 3);
		int x, y, band, top, bottom;

		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				src[y * sw + x] = (uint32_t) (((x * 7) & 0xff) << 16 |
				                              ((y * 3) & 0xff) << 8 |
				                              ((x + y) & 0xff));
			}
		}

		remote_display_scale_argb(src, sw, sh, whole, dw, dh);
		memset(bands, 0, (size_t) dw * dh * 3);

		/* Sixteen source rows at a time, as a redraw would arrive. */
		for (band = 0; band < sh; band += 16) {
			remote_display_scale_rows_for(band, band + 16, sh, dh, &top, &bottom);
			remote_display_scale_argb_rows(src, sw, sh, bands, dw, dh, top, bottom);
		}

		check("band by band gives the same picture as all at once",
		      memcmp(whole, bands, (size_t) dw * dh * 3) == 0);

		/* And a band leaves the rest of the destination alone. */
		memset(bands, 0x11, (size_t) dw * dh * 3);
		remote_display_scale_rows_for(240, 256, sh, dh, &top, &bottom);
		remote_display_scale_argb_rows(src, sw, sh, bands, dw, dh, top, bottom);
		{
			int untouched = 1, i;

			for (i = 0; i < top * dw * 3; i++) {
				if (bands[i] != 0x11) {
					untouched = 0;
					break;
				}
			}
			for (i = bottom * dw * 3; i < dw * dh * 3; i++) {
				if (bands[i] != 0x11) {
					untouched = 0;
					break;
				}
			}
			check("rows outside the band are left exactly as they were", untouched);
		}
		free(src);
		free(whole);
		free(bands);
	}

	printf("\nThe cheap filter used while things are moving\n");

	{
		const int sw = 640, sh = 480, dw = 320, dh = 240;
		uint32_t *src = malloc((size_t) sw * sh * sizeof(uint32_t));
		unsigned char *dst = malloc((size_t) dw * dh * 3);
		int x, y, i, exact = 1;

		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				src[y * sw + x] = ((x + y) & 1) ? 0x00ffffff : 0;
			}
		}
		check("nearest scaling reports success",
		      remote_display_scale_argb_nearest_rows(src, sw, sh, dst, dw, dh,
		          0, dh) == 1);

		/* Nearest neighbour picks a source pixel rather than averaging, so
		   every output pixel is one of the two input colours. That is the
		   trade being made, and it is what makes it cheap. */
		for (i = 0; i < dw * dh; i++) {
			if (dst[3 * i] != 0x00 && dst[3 * i] != 0xff) {
				exact = 0;
				break;
			}
		}
		check("nearest picks source pixels rather than averaging them", exact);

		/* It must respect a row range as the box average does, or the fast
		   filter would wipe the part of the picture it was not asked for. */
		memset(dst, 0x22, (size_t) dw * dh * 3);
		remote_display_scale_argb_nearest_rows(src, sw, sh, dst, dw, dh, 100, 120);
		{
			int untouched = 1;

			for (i = 0; i < 100 * dw * 3; i++) {
				if (dst[i] != 0x22) {
					untouched = 0;
					break;
				}
			}
			for (i = 120 * dw * 3; i < dw * dh * 3; i++) {
				if (dst[i] != 0x22) {
					untouched = 0;
					break;
				}
			}
			check("nearest leaves rows outside its range alone", untouched);
		}

		/* Upscaling is what it is also for: the box average has nothing to
		   average, and this must not read off the end of the source. */
		{
			unsigned char *big = malloc((size_t) (sw * 2) * (sh * 2) * 3);

			check("nearest can scale up",
			      remote_display_scale_argb_nearest_rows(src, sw, sh, big,
			          sw * 2, sh * 2, 0, sh * 2) == 1);
			free(big);
		}
		free(src);
		free(dst);
	}

	printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
