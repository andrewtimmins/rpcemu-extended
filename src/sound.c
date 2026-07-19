/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

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

/* Sound emulation */
#include <assert.h>
#include <stdint.h>

#include "rpcemu.h"
#include "mem.h"
#include "iomd.h"

#include "sound.h"
#include "savestate.h"

uint32_t soundaddr[4];
static uint32_t samplefreq = 41666;
int soundinited, soundlatch, soundcount;

#define BUFFERLENSAMPLES (4410)
#define BUFFERLENBYTES (BUFFERLENSAMPLES * 2)
static int16_t bigsoundbuffer[4][BUFFERLENSAMPLES]; /**< Temp store, used to buffer
                                                      data between the emulated sound
                                                       and platform */
static int bigsoundpos = 0;
static int bigsoundbufferhead = 0; // sound buffer being written to
static int bigsoundbuffertail = 0; // sound buffer being read from


/**
 * Called on program startup to initialise the sound system
 */
void
sound_init(void)
{
	/* Call the platform code to create a thread for handing sound updates */
	sound_thread_start();

	/* The initial default sample rate for the Risc PC is not 44100 */
	samplefreq = 41666;

	/* Call the platform specific code to start the audio playing */
	plt_sound_init(BUFFERLENBYTES);
	if (config.soundenabled) {
		plt_sound_restart();
	}
}

/**
 * Called when the user turns the sound on via the GUI
 */
void
sound_restart(void)
{
	assert(config.soundenabled);

	/* Pass the call on to the platform specific code */
	plt_sound_restart();
}

/**
 * Called when the user turns the sound off via the GUI
 */
void
sound_pause(void)
{
	assert(!config.soundenabled);

	/* Pass the call on to the platform specific code */
	plt_sound_pause();
}

/**
 * Called when the VIDC registers controlling
 * sample frequency have changed
 *
 * @param newsamplefreq Sample frequency in Hz (e.g. 44100)
 */
void
sound_samplefreq_change(int newsamplefreq)
{
	if((uint32_t) newsamplefreq != samplefreq) {
		int i;
		/* to prevent queued data being played at the wrong frequency
		   blank it */
		for(i = 0; i < 4; i++) {
			memset(bigsoundbuffer[i], 0, BUFFERLENBYTES);
		}

		samplefreq = (uint32_t) newsamplefreq;
	}
}

/**
 * Copy data from the emulated sound data into a temp store.
 * Also generates sound interrupts.
 *
 * Called from gentimerirq (iomd.c)
 * @thread emulator
 */
void
sound_irq_update(void)
{
	const uint32_t *ramp; /**< Pointer to which bank of RAM 'page' is in */
        uint32_t page,start,end,temp;
        int offset = (iomd.sndstat & IOMD_DMA_STATUS_BUFFER) << 1;
        int len;
        unsigned int c;

        // If bigsoundbufferhead is 1 less than bigsoundbuffertail, then
        // the buffer list is full.
        if (((bigsoundbufferhead + 1) & 3) == bigsoundbuffertail)
        {
                soundcount += 4000;
                // kick the sound thread to clear the list
                sound_thread_wakeup();
                return;
        }
        page  = soundaddr[offset] & 0xFFFFF000; /** TODO This should also be & with phys_space_mask */
        start = soundaddr[offset] & 0xFF0;
        end   = (soundaddr[offset + 1] & 0xFF0) + 16;
        len   = (end - start) >> 2;
        
        soundlatch = (int) (((float) len / (float) samplefreq) * 2000000.0f);

        iomd.irqdma.status |= IOMD_IRQDMA_SOUND_0;
        updateirqs();

        iomd.sndstat |= (IOMD_DMA_STATUS_INTERRUPT | IOMD_DMA_STATUS_OVERRUN);
        iomd.sndstat ^= IOMD_DMA_STATUS_BUFFER; /* Swap between buffer A and B */

	/* Handle sound data all over physical RAM */
	if (page & 0x08000000) {
		ramp =  ram1;
	} else if (page & 0x04000000) {
		ramp = ram01;
	} else {
		ramp = ram00;
	}

	/* The second SIMM (ram1) is only present on 256MB machines; on smaller
	   configurations a DMA page resolving to that bank leaves ramp NULL, so
	   skip the transfer rather than dereferencing a NULL pointer. */
	if (ramp == NULL) {
		return;
	}

        for (c = start; c < end; c += 4)
        {
                temp = ramp[((c + page) & mem_rammask) >> 2];
                bigsoundbuffer[bigsoundbufferhead][bigsoundpos++] = (int16_t)(temp & 0xFFFF);
                bigsoundbuffer[bigsoundbufferhead][bigsoundpos++] = (int16_t)(temp >> 16);
                if (bigsoundpos >= (BUFFERLENSAMPLES))
                {
                        bigsoundbufferhead++;
                        bigsoundbufferhead &= 3; /* if (bigsoundbufferhead > 3) { bigsoundbufferhead = 0; } */
                        bigsoundpos = 0;
                        sound_thread_wakeup();
                }
        }
}

/**
 * Copy data from the temp store into the platform specific output sound buffer.
 *
 * Called from host platform-specific sound thread function.
 * @thread sound 
 */
void
sound_buffer_update(void)
{
	while (bigsoundbuffertail != bigsoundbufferhead) {
		if(plt_sound_buffer_free() >= (BUFFERLENBYTES)) {
			if (config.soundenabled) {
				plt_sound_buffer_play(samplefreq, (const char *) bigsoundbuffer[bigsoundbuffertail], BUFFERLENBYTES);  // write one buffer
			}

			bigsoundbuffertail++;
			bigsoundbuffertail &= 3; /* if (bigsoundbuffertail > 3) { bigsoundbuffertail = 0; } */
		} else {
			/* Still playing previous block of data, no need to fill it up yet */
			break;
		}
	}
}


/**
 * Write the sound DMA state to a suspend snapshot.
 */
void
sound_savestate(FILE *f)
{
	int c;

	for (c = 0; c < 4; c++) {
		savestate_write_u32(f, soundaddr[c]);
	}
	savestate_write_u32(f, samplefreq);
	savestate_write_i32(f, soundlatch);
	savestate_write_i32(f, soundcount);
}

/**
 * Restore the sound DMA state from a suspend snapshot.
 *
 * The host-side sample buffers are discarded (momentary silence until the
 * restored DMA state refills them).
 */
void
sound_loadstate(FILE *f)
{
	int c;

	for (c = 0; c < 4; c++) {
		soundaddr[c] = savestate_read_u32(f);
	}
	samplefreq = savestate_read_u32(f);
	soundlatch = savestate_read_i32(f);
	soundcount = savestate_read_i32(f);

	for (c = 0; c < 4; c++) {
		memset(bigsoundbuffer[c], 0, BUFFERLENBYTES);
	}
	bigsoundpos = 0;
	bigsoundbufferhead = 0;
	bigsoundbuffertail = 0;
}
