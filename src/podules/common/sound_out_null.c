/*
  RPCEmu - An Acorn system emulator

  Copyright (C) Sarah Walker
  Copyright (C) 2026 Andy Timmins

  Part of the podule subsystem, derived from Arculator 2.2 by Sarah Walker
  (https://b-em.bbcmicro.com/arculator/), and distributed under the GNU GPL v2.

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
  Sound output for a podule where there is no SDL2 to play it with: accept the
  samples and drop them.

  The counterpart of sound_in_null.c and midi_stub.c, and needed for the same
  reason. sound_out_sdl2.c includes <SDL.h> unconditionally, so before this
  existed the core could not be compiled at all without SDL2 development headers,
  even though CMake treated the SDL2 *link* as optional. That only went unnoticed
  because every machine the emulator had been built on happened to have them.

  A podule that produces sound still initialises, still runs, and still asks this
  to play buffers; nothing is heard. That is the same bargain sound_in_null.c
  makes for capture, and it is what lets the core build for a target whose audio
  arrives another way - the Android front end takes its audio from the Java side
  rather than from SDL2 (see docs/android.md).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "podule_api.h"
#include "sound_out.h"

typedef struct sound_out_null_t
{
	void	*p;
} sound_out_null_t;

void *
sound_out_init(void *p, int freq, int buffer_size,
    void (*log)(const char *format, ...),
    const podule_callbacks_t *podule_callbacks, podule_t *podule)
{
	sound_out_null_t *sound_out = malloc(sizeof(sound_out_null_t));

	if (sound_out == NULL) {
		return NULL;
	}
	memset(sound_out, 0, sizeof(sound_out_null_t));
	sound_out->p = p;

	return sound_out;
}

void
sound_out_close(void *p)
{
	sound_out_null_t *sound_out = p;

	free(sound_out);
}

void
sound_out_buffer(void *p, int16_t *buffer, int len)
{
	/* Nowhere to send it. Deliberately not an error: the podule is entitled to
	   keep producing samples, and refusing them would change its timing. */
}
