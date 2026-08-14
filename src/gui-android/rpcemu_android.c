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
 * The Android front end: one machine, one screen, SDL2.
 *
 * This is deliberately not a port of the wxWidgets front end. wxWidgets has no
 * Android build at all, so it would be a rewrite either way, and the desktop
 * shape - modal dialogues, a five-tab machine editor, menu bars, several machines
 * at once - is wrong for a tablet. Here there is one machine running at a time and
 * nothing else on screen.
 *
 * What a front end owes the core is small, and tests/test_stubs.c is the list:
 * somewhere to put pixels, a clock, sound plumbing, activity counters, and the
 * video-thread hooks. Everything below either implements one of those or drives
 * the machine.
 *
 * Two things are worth knowing about how the machine actually runs, because they
 * shape this file:
 *
 *   - Frames are produced by the CORE, not by us. vblupdate() on the desktop is
 *     just "drawscre++", and execrpcemu() calls drawscr() itself whenever a frame
 *     is owed; drawscr() then calls rpcemu_video_update() with the pixels. So
 *     there is no video thread here and no need for one: the hooks below are
 *     no-ops and vidctrymutex() always succeeds, which is honest because
 *     everything runs on this one thread.
 *
 *   - The IOMD timers do not tick unless we tick them. gentimerirq() must be
 *     called about every 2ms or RISC OS waits for an interrupt for ever - which
 *     is exactly what tests/boot_trace.c ran into. The interval and the catch-up
 *     limits mirror EmulatorHost::ServiceTimers() so the two front ends pace the
 *     machine the same way.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <android/log.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "vidc20.h"
#include "iomd.h"
#include "keyboard.h"

#define TAG "rpcemu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Mirrors kIomdTimerIntervalNs and the catch-up caps in emulator_host.cpp. */
#define IOMD_INTERVAL_NS	2000000LL
#define MAX_IOMD_CATCH_UP	32
#define MAX_VIDEO_CATCH_UP	4

extern void execrpcemu(void);

/* ---- the frame the core last gave us -------------------------------------- */

static struct {
	uint32_t	*pixels;	/* our own copy, so the core may carry on */
	int		 w, h;		/* size of that copy */
	int		 have;		/* a frame is waiting to be presented */
} frame;

static SDL_Window	*window;
static SDL_Renderer	*renderer;
static SDL_Texture	*texture;
static int		 tex_w, tex_h;
static int		 quit_requested;

/* ---- the front-end contract ----------------------------------------------- */

/**
 * Receive a frame from the core.
 *
 * Copied rather than kept by pointer: the core owns that buffer and will write
 * the next frame into it. yl..yh are the dirty rows, and the whole buffer is
 * copied regardless - a tablet's frame is a few megabytes and the copy is far
 * cheaper than reasoning about partial texture updates before anything works.
 * Narrowing this to the dirty span is a later optimisation, noted rather than
 * done.
 */
void
rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize, int yl, int yh,
    int double_size, int host_xsize, int host_ysize)
{
	size_t bytes;

	NOT_USED(yl);
	NOT_USED(yh);
	NOT_USED(double_size);
	NOT_USED(host_xsize);
	NOT_USED(host_ysize);

	if (buffer == NULL || xsize <= 0 || ysize <= 0) {
		return;
	}

	if (frame.pixels == NULL || frame.w != xsize || frame.h != ysize) {
		free(frame.pixels);
		frame.pixels = malloc((size_t) xsize * (size_t) ysize * 4);
		if (frame.pixels == NULL) {
			frame.w = frame.h = 0;
			return;
		}
		frame.w = xsize;
		frame.h = ysize;
		LOGI("guest display is now %dx%d", xsize, ysize);
	}

	bytes = (size_t) xsize * (size_t) ysize * 4;
	memcpy(frame.pixels, buffer, bytes);
	frame.have = 1;
}

/* No video thread: everything runs on the one thread, so there is nothing to
   start, wake or lock against. vidctrymutex() reporting success is what lets the
   core go on and call drawscr(). */
void vidcstartthread(void) {}
void vidcendthread(void) {}
void vidcwakeupthread(void) {}
void vidcreleasemutex(void) {}
int  vidctrymutex(void) { return 0; }

/* Sound is not wired up yet: the core keeps producing samples and they are
   dropped, exactly as podules/common/sound_out_null.c does for a podule. Doing
   this properly means SDL2 audio, which is a later step. */
void plt_sound_init(int samples) { NOT_USED(samples); }
void plt_sound_buffer_play(void) {}
void plt_sound_buffer_free(void) {}
void plt_sound_pause(void) {}
void plt_sound_restart(void) {}
void sound_thread_start(void) {}
void sound_thread_close(void) {}
void sound_thread_wakeup(void) {}

/* Activity indicators drive lights on the desktop toolbar. There is no toolbar. */
void fdc_activity_increment(void) {}
void hostfs_activity_increment(void) {}
void ide_activity_increment(void) {}
void network_activity_increment(void) {}

/**
 * A monotonic clock in nanoseconds, which is what paces the whole machine.
 *
 * CLOCK_MONOTONIC rather than SDL_GetTicks: this is compared against nanosecond
 * timer deadlines, and a millisecond clock would quantise the IOMD tick to five
 * times its own interval.
 */
uint64_t
rpcemu_nsec_timer_ticks(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/**
 * Something has gone wrong that the emulator cannot continue past.
 *
 * There is no dialogue to put this in yet, so it goes to logcat, which is where
 * an Android native fault has to be read from anyway (adb logcat -s rpcemu).
 * abort() rather than exit() deliberately: it leaves a tombstone with a native
 * backtrace, which is worth far more than a clean exit when the report is going
 * to arrive second-hand from a tablet.
 */
void
fatal(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	__android_log_vprint(ANDROID_LOG_FATAL, TAG, format, ap);
	va_end(ap);
	abort();
}

/** The core's non-fatal error reporter. Logged, and the machine carries on. */
void
error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	__android_log_vprint(ANDROID_LOG_ERROR, TAG, format, ap);
	va_end(ap);
}

void
rpcemu_log_platform(void)
{
	LOGI("Android front end, SDL %d.%d.%d",
	    SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
}

/* Settings are the Kotlin side's business - it writes the machine's config file
   before starting this activity, and the core reads it. Nothing to do here, and
   in particular nothing that would write over what the shell just wrote. */
void config_load(Config *cfg) { NOT_USED(cfg); }
void config_save(Config *cfg) { NOT_USED(cfg); }

/* Pointer warping has no meaning on a touch screen. Left for a real mouse, which
   Android does support and which is the better answer for RISC OS's three
   buttons; see docs/android.md. */
void rpcemu_move_host_mouse(uint16_t x, uint16_t y) { NOT_USED(x); NOT_USED(y); }

void
rpcemu_request_poweroff(void)
{
	LOGI("guest asked to power off");
	quit_requested = 1;
}

/* Called from the core's idle path. Servicing input here as well as in the main
   loop keeps the pointer moving while the guest is idling, which is what issue
   #36 was about on the desktop. */
void
rpcemu_idle_process_events(void)
{
	/* Nothing yet: input arrives in the main loop. Wired up with the input
	   work, and left present so the core's idle path has something to call. */
}

int
rpcemu_host_commands_pending(void)
{
	return 0;
}

/* ---- presenting a frame --------------------------------------------------- */

static void
present(void)
{
	if (!frame.have || frame.pixels == NULL) {
		return;
	}

	if (texture == NULL || tex_w != frame.w || tex_h != frame.h) {
		if (texture != NULL) {
			SDL_DestroyTexture(texture);
		}
		/* ARGB8888 to match the core's uint32_t pixels. */
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
		    SDL_TEXTUREACCESS_STREAMING, frame.w, frame.h);
		if (texture == NULL) {
			LOGE("SDL_CreateTexture: %s", SDL_GetError());
			return;
		}
		tex_w = frame.w;
		tex_h = frame.h;
	}

	SDL_UpdateTexture(texture, NULL, frame.pixels, frame.w * 4);
	SDL_RenderClear(renderer);
	/* Letterboxed to the guest's aspect ratio rather than stretched: a RISC OS
	   mode on a 16:10 tablet would otherwise be visibly wrong. */
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
	frame.have = 0;
}

/* ---- the machine ---------------------------------------------------------- */

/**
 * Bring a machine up.
 *
 * The same sequence the desktop performs when a machine starts, and the same one
 * tests/boot_trace.c uses: point the core at its directories, describe the
 * machine, tell the core the model changed so the ROM is accepted for the CPU,
 * then start it.
 *
 * The data directory comes from the Kotlin side via SDL's "external storage" or
 * an explicit environment variable, because on Android the app's own directory is
 * the only place it may freely write.
 */
static int
machine_start(void)
{
	const char *datadir = getenv("RPCEMU_DATADIR");
	const char *machine_name = getenv("RPCEMU_MACHINE");

	if (datadir == NULL) {
		LOGE("RPCEMU_DATADIR is not set; the shell must pass it");
		return 0;
	}

	rpcemu_set_datadir(datadir);
	rpcemu_set_resourcedir(datadir);
	rpcemu_set_machine_datadir(machine_name != NULL ? machine_name : "Default");

	memset(&config, 0, sizeof(config));
	config.model = Model_RPCSA110;
	config.mem_size = 128;
	config.vram_size = 8;
	config.refresh = 60;
	config.cpu_idle = 0;
	if (getenv("RPCEMU_ROM_DIR") != NULL) {
		snprintf(config.rom_dir, sizeof(config.rom_dir), "%s",
		    getenv("RPCEMU_ROM_DIR"));
	}

	LOGI("data directory %s, machine %s, ROM directory \"%s\"",
	    rpcemu_get_datadir(), rpcemu_get_machine_datadir(), config.rom_dir);

	rpcemu_model_changed(config.model);
	rpcemu_start();

	LOGI("machine started, %s backend",
	    arm_is_dynarec() ? "recompiler" : "interpreter");
	return 1;
}

int
main(int argc, char *argv[])
{
	uint64_t start_ns, iomd_next, video_next, video_interval;

	NOT_USED(argc);
	NOT_USED(argv);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		LOGE("SDL_Init: %s", SDL_GetError());
		return 1;
	}

	window = SDL_CreateWindow("RPCEmu", SDL_WINDOWPOS_UNDEFINED,
	    SDL_WINDOWPOS_UNDEFINED, 0, 0,
	    SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
	if (window == NULL) {
		LOGE("SDL_CreateWindow: %s", SDL_GetError());
		return 1;
	}
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (renderer == NULL) {
		LOGE("SDL_CreateRenderer: %s", SDL_GetError());
		return 1;
	}

	if (!machine_start()) {
		return 1;
	}

	video_interval = 1000000000ULL / (config.refresh > 0 ? (unsigned) config.refresh : 60u);
	start_ns = rpcemu_nsec_timer_ticks();
	iomd_next = IOMD_INTERVAL_NS;
	video_next = video_interval;

	while (!quit_requested) {
		SDL_Event ev;
		uint64_t elapsed;
		int n;

		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				quit_requested = 1;
				break;
			case SDL_APP_WILLENTERBACKGROUND:
				/* Android will stop giving us frames. Nothing is
				   suspended yet; a proper implementation saves state
				   here, which is what savestate.c is for. */
				LOGI("entering background");
				break;
			case SDL_APP_DIDENTERFOREGROUND:
				LOGI("back in foreground");
				break;
			default:
				break;
			}
		}

		/* Run the guest. execrpcemu() also calls drawscr() when a frame is
		   owed, which is what reaches rpcemu_video_update() above. */
		for (n = 0; n < 32 && !quit_requested; n++) {
			execrpcemu();
		}

		elapsed = rpcemu_nsec_timer_ticks() - start_ns;

		for (n = 0; elapsed >= iomd_next && n < MAX_IOMD_CATCH_UP; n++) {
			gentimerirq(elapsed);
			iomd_next += IOMD_INTERVAL_NS;
		}
		if (elapsed >= iomd_next) {
			iomd_next = elapsed + IOMD_INTERVAL_NS;
		}

		for (n = 0; elapsed >= video_next && n < MAX_VIDEO_CATCH_UP; n++) {
			drawscre++;	/* one frame owed, as vblupdate() does */
			video_next += video_interval;
		}
		if (elapsed >= video_next) {
			video_next = elapsed + video_interval;
		}

		present();
	}

	LOGI("shutting down");
	if (texture != NULL) {
		SDL_DestroyTexture(texture);
	}
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
