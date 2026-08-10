/*
  RPCEmu - An Acorn system emulator

  Guessing a RISC OS filetype from a host filename extension.

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 */

/*
 * WHY THIS EXISTS
 *
 * HostFS takes a file's RISC OS type from a ",xxx" suffix on the host name and
 * calls everything else Text. That is right for a file RISC OS itself wrote,
 * and wrong for one you dropped into the folder from Finder or Explorer: a PNG
 * arrives as a text file, so double-clicking it does the wrong thing. It is
 * compounded by HostFS swapping '.' and '/' over, which makes "photo.png"
 * appear in RISC OS as a Text file called "photo/png" - a name that looks
 * broken as well as a type that is wrong.
 *
 * WHAT IS AND IS NOT IN THE TABLE
 *
 * Only formats that arrive from outside RISC OS and have one obvious type:
 * images, documents, archives and sound.
 *
 * Source-like extensions are deliberately absent, and this is the important
 * one. On HostFS a host file called "main.c" is RISC OS "main/c", which is how
 * RISC OS has always written C sources, and it genuinely is a text file.
 * Mapping ".c" - or ".h", ".s", ".txt" and the rest - would change the type of
 * files that RISC OS development already relies on being Text. The default is
 * already Text, so leaving them out is also the correct answer for them.
 *
 * WHERE THE NUMBERS CAME FROM
 *
 * Each type below was taken from the published allocations rather than from
 * memory, and cross-checked between two independent lists. Two cases are worth
 * recording because they are the ones a guess gets wrong:
 *
 *   - ".zip" is &DDC (Archive, as used by SparkFS and ArcFS), not the &A91
 *     that is sometimes quoted.
 *   - MPEG video is deliberately absent: the two sources disagree on its
 *     number, so it is left out rather than have one of them be wrong.
 *
 * XML and CSS are absent because they have no allocation in the registry.
 */

#include <stdint.h>
#include <string.h>

#include "hostfs_filetype.h"

struct ext_type {
	const char *ext;	/* without the dot, lower case */
	uint32_t    type;
};

static const struct ext_type ext_types[] = {
	/* Images */
	{ "png",  0xb60 },
	{ "jpg",  0xc85 },
	{ "jpeg", 0xc85 },
	{ "gif",  0x695 },
	{ "bmp",  0x69c },
	{ "tif",  0xff0 },
	{ "tiff", 0xff0 },

	/* Documents */
	{ "pdf",  0xadf },
	{ "html", 0xfaf },
	{ "htm",  0xfaf },
	{ "csv",  0xdfe },
	{ "rtf",  0xc32 },

	/* Archives */
	{ "zip",  0xddc },

	/* Sound */
	{ "mp3",  0x1ad },
	{ "wav",  0xfb1 },
};

static int
ascii_casecmp(const char *a, const char *b)
{
	/* Deliberately ASCII-only and locale-independent: tolower() in a Turkish
	   locale does not map 'I' the way this needs, and a filename extension is
	   not text in the user's language. */
	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;

		if (ca >= 'A' && ca <= 'Z') {
			ca = (char) (ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char) (cb - 'A' + 'a');
		}
		if (ca != cb) {
			return 1;
		}
		a++;
		b++;
	}
	return (*a == *b) ? 0 : 1;
}

int
hostfs_filetype_from_leafname(const char *leafname, uint32_t *type)
{
	const char *slash;
	const char *dot;
	size_t i;

	if (leafname == NULL || type == NULL) {
		return 0;
	}

	/* Work on the leaf alone, so a directory called "photos.png" further up
	   the path cannot decide the type of a file inside it. */
	slash = strrchr(leafname, '/');
	if (slash != NULL) {
		leafname = slash + 1;
	}

	dot = strrchr(leafname, '.');
	if (dot == NULL || dot == leafname) {
		/* No extension, or a name that is nothing but an extension
		   (".profile"), which is a name rather than a type. */
		return 0;
	}

	for (i = 0; i < sizeof(ext_types) / sizeof(ext_types[0]); i++) {
		if (ascii_casecmp(dot + 1, ext_types[i].ext) == 0) {
			*type = ext_types[i].type;
			return 1;
		}
	}

	return 0;
}
