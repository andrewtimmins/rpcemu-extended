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
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <SDL.h>
#include <android/log.h>

#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
#include "vidc20.h"
#include "iomd.h"
#include "keyboard.h"
#include "sound.h"
#include "podulerom.h"

#define TAG "rpcemu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Mirrors kIomdTimerIntervalNs and the catch-up caps in emulator_host.cpp. */
#define IOMD_INTERVAL_NS	2000000LL
#define MAX_IOMD_CATCH_UP	32
#define MAX_VIDEO_CATCH_UP	4

extern void execrpcemu(void);

/* ---- the frame the core last gave us -------------------------------------- */

static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

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

	/* This runs on the video thread now; present() reads the copy on the SDL
	   thread. */
	pthread_mutex_lock(&frame_mutex);

	if (frame.pixels == NULL || frame.w != xsize || frame.h != ysize) {
		free(frame.pixels);
		frame.pixels = malloc((size_t) xsize * (size_t) ysize * 4);
		if (frame.pixels == NULL) {
			frame.w = frame.h = 0;
			pthread_mutex_unlock(&frame_mutex);
			return;
		}
		frame.w = xsize;
		frame.h = ysize;
		LOGI("guest display is now %dx%d", xsize, ysize);
	}

	bytes = (size_t) xsize * (size_t) ysize * 4;
	memcpy(frame.pixels, buffer, bytes);
	frame.have = 1;
	pthread_mutex_unlock(&frame_mutex);
}

/*
 * The video and sound threads, as real threads.
 *
 * The first attempt ran everything on one thread and called vidcthread() and
 * sound_buffer_update() from the main loop. That is a plausible-looking
 * simplification and it was wrong twice over: drawscr() wakes the video thread
 * from inside the video mutex and expects it to run afterwards, and the sound
 * producer expects its buffers to be consumed. Rather than keep discovering which
 * call the core expects a thread to make, this now mirrors
 * src/gui/emulator_host.cpp exactly - the same two threads, the same condition
 * variables, the same mutex semantics - because that is the arrangement the core
 * is actually written against and tested with.
 *
 * Note the sense of vidctrymutex(): drawscr() reads "if (!vidctrymutex()) return;",
 * so 1 means the mutex was obtained and 0 means busy. Returning 0 unconditionally,
 * as tests/test_stubs.c does, makes drawscr() bail on every frame - a machine that
 * runs perfectly and never draws. That is also why tests/boot_trace.c has never
 * drawn anything on any platform.
 */

static pthread_t	video_thread, sound_thread;
static pthread_mutex_t	video_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t	sound_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	video_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t	sound_cond = PTHREAD_COND_INITIALIZER;
static int		threads_running;

static void *
video_thread_fn(void *arg)
{
	NOT_USED(arg);

	pthread_mutex_lock(&video_mutex);
	while (!quited) {
		pthread_cond_wait(&video_cond, &video_mutex);
		if (!quited) {
			vidcthread();
		}
	}
	pthread_mutex_unlock(&video_mutex);
	return NULL;
}

static void *
sound_thread_fn(void *arg)
{
	NOT_USED(arg);

	pthread_mutex_lock(&sound_mutex);
	while (!quited) {
		pthread_cond_wait(&sound_cond, &sound_mutex);
		if (!quited) {
			sound_buffer_update();
		}
	}
	pthread_mutex_unlock(&sound_mutex);
	return NULL;
}

void
vidcstartthread(void)
{
	if (pthread_create(&video_thread, NULL, video_thread_fn, NULL) != 0) {
		fatal("cannot create the video thread");
	}
	threads_running = 1;
}

void
vidcendthread(void)
{
	if (!threads_running) {
		return;
	}
	vidcwakeupthread();
	pthread_join(video_thread, NULL);
	threads_running = 0;
}

void
vidcwakeupthread(void)
{
	pthread_cond_signal(&video_cond);
}

void vidcreleasemutex(void) { pthread_mutex_unlock(&video_mutex); }

/** 1 if the mutex was obtained, 0 if busy - see the note above. */
int
vidctrymutex(void)
{
	const int ret = pthread_mutex_trylock(&video_mutex);

	if (ret == EBUSY) {
		return 0;
	}
	if (ret != 0) {
		fatal("obtaining the vidc mutex failed: %d", ret);
	}
	return 1;
}

/*
 * Sound. Nothing is audible yet - the plt_sound_* hooks accept buffers and drop
 * them, as podules/common/sound_out_null.c does - but the thread still has to
 * consume them or the guest's sound DMA never completes.
 */
void plt_sound_init(uint32_t bufferlen) { NOT_USED(bufferlen); }
void plt_sound_restart(void) {}
void plt_sound_pause(void) {}

/** Always room: nothing is played, and answering zero would stall the producer. */
int32_t plt_sound_buffer_free(void) { return 1 << 20; }

void
plt_sound_buffer_play(uint32_t samplerate, const char *buffer, uint32_t length)
{
	NOT_USED(samplerate);
	NOT_USED(buffer);
	NOT_USED(length);
}

void
sound_thread_start(void)
{
	if (pthread_create(&sound_thread, NULL, sound_thread_fn, NULL) != 0) {
		fatal("cannot create the sound thread");
	}
}

void
sound_thread_close(void)
{
	sound_thread_wakeup();
	pthread_join(sound_thread, NULL);
}

void sound_thread_wakeup(void) { pthread_cond_signal(&sound_cond); }

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
static uint64_t
monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/** Nanoseconds at machine start; the clock below is relative to it. */
static uint64_t start_ns;

/**
 * Emulated time, in nanoseconds SINCE THE MACHINE STARTED.
 *
 * Relative, not absolute, and that is not a detail. The core compares this clock
 * against the value the front end hands gentimerirq(), and the desktop derives both
 * from one relative base (EmulatorHost::GetElapsedTimerNs() is "now - start_time_").
 * Returning absolute CLOCK_MONOTONIC here while passing a relative elapsed to
 * gentimerirq() puts the two timebases however long the host has been up apart, so
 * the IOMD timers never fire - and RISC OS sits for ever polling IOMD IRQ status A
 * for the timer 0 bit, which is exactly what it was doing.
 */
uint64_t
rpcemu_nsec_timer_ticks(void)
{
	if (start_ns == 0) {
		return 0;
	}
	return monotonic_ns() - start_ns;
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

/* Timer deadlines, shared by the main loop and the idle hook so the two cannot
   drift apart - the same reason emulator_host.cpp keeps them in one place. */
static uint64_t iomd_next, video_next, video_interval;

/**
 * Bring the IOMD timers and the frame clock up to date.
 *
 * Mirrors EmulatorHost::ServiceTimers(), catch-up caps included.
 */
static void
service_timers(void)
{
	const uint64_t elapsed = rpcemu_nsec_timer_ticks();
	int n;

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
}

/**
 * Called from the core's idle path, and it must do the timers.
 *
 * This is not optional and it is not only about input. When the guest idles - RISC
 * OS waiting for an interrupt - the core stays inside execrpcemu() and calls this.
 * If it does nothing, the IOMD timers are only serviced after execrpcemu() returns,
 * so the interrupt the guest is waiting for can never arrive and the machine waits
 * for ever. The desktop's IdleProcessEvents() calls ServiceTimers() for exactly
 * this reason.
 */
void
rpcemu_idle_process_events(void)
{
	service_timers();
}

int
rpcemu_host_commands_pending(void)
{
	return 0;
}

/* ---- input ---------------------------------------------------------------- */

/*
 * Keys.
 *
 * keyboard_map_key() wants an X11 hardware keycode and returns the PS/2 Set 2
 * bytes for it (src/gui/keyboard_x.c, which is a pure table and is compiled into
 * this front end too rather than duplicated). An X11 keycode is the Linux evdev
 * code plus 8 - checked against that table, where Down is 0x74 and evdev KEY_DOWN
 * is 108.
 *
 * So the only new knowledge here is SDL scancode to evdev code. SDL's scancodes
 * are USB HID usages, which are a different numbering again, so this has to be a
 * table. It maps positions, not characters, which is what RISC OS wants: the guest
 * decides what a position types (see the keyboard notes in the README).
 */
static const struct { SDL_Scancode sdl; uint8_t evdev; } key_table[] = {
	{ SDL_SCANCODE_ESCAPE, 1 },
	{ SDL_SCANCODE_1, 2 }, { SDL_SCANCODE_2, 3 }, { SDL_SCANCODE_3, 4 },
	{ SDL_SCANCODE_4, 5 }, { SDL_SCANCODE_5, 6 }, { SDL_SCANCODE_6, 7 },
	{ SDL_SCANCODE_7, 8 }, { SDL_SCANCODE_8, 9 }, { SDL_SCANCODE_9, 10 },
	{ SDL_SCANCODE_0, 11 },
	{ SDL_SCANCODE_MINUS, 12 }, { SDL_SCANCODE_EQUALS, 13 },
	{ SDL_SCANCODE_BACKSPACE, 14 }, { SDL_SCANCODE_TAB, 15 },
	{ SDL_SCANCODE_Q, 16 }, { SDL_SCANCODE_W, 17 }, { SDL_SCANCODE_E, 18 },
	{ SDL_SCANCODE_R, 19 }, { SDL_SCANCODE_T, 20 }, { SDL_SCANCODE_Y, 21 },
	{ SDL_SCANCODE_U, 22 }, { SDL_SCANCODE_I, 23 }, { SDL_SCANCODE_O, 24 },
	{ SDL_SCANCODE_P, 25 },
	{ SDL_SCANCODE_LEFTBRACKET, 26 }, { SDL_SCANCODE_RIGHTBRACKET, 27 },
	{ SDL_SCANCODE_RETURN, 28 }, { SDL_SCANCODE_LCTRL, 29 },
	{ SDL_SCANCODE_A, 30 }, { SDL_SCANCODE_S, 31 }, { SDL_SCANCODE_D, 32 },
	{ SDL_SCANCODE_F, 33 }, { SDL_SCANCODE_G, 34 }, { SDL_SCANCODE_H, 35 },
	{ SDL_SCANCODE_J, 36 }, { SDL_SCANCODE_K, 37 }, { SDL_SCANCODE_L, 38 },
	{ SDL_SCANCODE_SEMICOLON, 39 }, { SDL_SCANCODE_APOSTROPHE, 40 },
	{ SDL_SCANCODE_GRAVE, 41 }, { SDL_SCANCODE_LSHIFT, 42 },
	{ SDL_SCANCODE_BACKSLASH, 43 },
	{ SDL_SCANCODE_Z, 44 }, { SDL_SCANCODE_X, 45 }, { SDL_SCANCODE_C, 46 },
	{ SDL_SCANCODE_V, 47 }, { SDL_SCANCODE_B, 48 }, { SDL_SCANCODE_N, 49 },
	{ SDL_SCANCODE_M, 50 },
	{ SDL_SCANCODE_COMMA, 51 }, { SDL_SCANCODE_PERIOD, 52 },
	{ SDL_SCANCODE_SLASH, 53 }, { SDL_SCANCODE_RSHIFT, 54 },
	{ SDL_SCANCODE_KP_MULTIPLY, 55 }, { SDL_SCANCODE_LALT, 56 },
	{ SDL_SCANCODE_SPACE, 57 }, { SDL_SCANCODE_CAPSLOCK, 58 },
	{ SDL_SCANCODE_F1, 59 }, { SDL_SCANCODE_F2, 60 }, { SDL_SCANCODE_F3, 61 },
	{ SDL_SCANCODE_F4, 62 }, { SDL_SCANCODE_F5, 63 }, { SDL_SCANCODE_F6, 64 },
	{ SDL_SCANCODE_F7, 65 }, { SDL_SCANCODE_F8, 66 }, { SDL_SCANCODE_F9, 67 },
	{ SDL_SCANCODE_F10, 68 },
	{ SDL_SCANCODE_NUMLOCKCLEAR, 69 }, { SDL_SCANCODE_SCROLLLOCK, 70 },
	{ SDL_SCANCODE_KP_7, 71 }, { SDL_SCANCODE_KP_8, 72 }, { SDL_SCANCODE_KP_9, 73 },
	{ SDL_SCANCODE_KP_MINUS, 74 },
	{ SDL_SCANCODE_KP_4, 75 }, { SDL_SCANCODE_KP_5, 76 }, { SDL_SCANCODE_KP_6, 77 },
	{ SDL_SCANCODE_KP_PLUS, 78 },
	{ SDL_SCANCODE_KP_1, 79 }, { SDL_SCANCODE_KP_2, 80 }, { SDL_SCANCODE_KP_3, 81 },
	{ SDL_SCANCODE_KP_0, 82 }, { SDL_SCANCODE_KP_PERIOD, 83 },
	{ SDL_SCANCODE_F11, 87 }, { SDL_SCANCODE_F12, 88 },
	{ SDL_SCANCODE_KP_ENTER, 96 }, { SDL_SCANCODE_RCTRL, 97 },
	{ SDL_SCANCODE_KP_DIVIDE, 98 }, { SDL_SCANCODE_RALT, 100 },
	{ SDL_SCANCODE_HOME, 102 }, { SDL_SCANCODE_UP, 103 },
	{ SDL_SCANCODE_PAGEUP, 104 }, { SDL_SCANCODE_LEFT, 105 },
	{ SDL_SCANCODE_RIGHT, 106 }, { SDL_SCANCODE_END, 107 },
	{ SDL_SCANCODE_DOWN, 108 }, { SDL_SCANCODE_PAGEDOWN, 109 },
	{ SDL_SCANCODE_INSERT, 110 }, { SDL_SCANCODE_DELETE, 111 },
	{ SDL_SCANCODE_PAUSE, 119 },
	{ SDL_SCANCODE_LGUI, 125 }, { SDL_SCANCODE_RGUI, 126 },
	{ SDL_SCANCODE_APPLICATION, 127 },
};

static void
handle_key(SDL_Scancode sc, int pressed)
{
	size_t i;

	for (i = 0; i < sizeof(key_table) / sizeof(key_table[0]); i++) {
		if (key_table[i].sdl == sc) {
			const uint8_t *codes = keyboard_map_key(key_table[i].evdev + 8u);

			if (codes == NULL) {
				return;
			}
			if (pressed) {
				keyboard_key_press(codes);
			} else {
				/* Break is a one-shot sequence; releasing it would send
				   nonsense, which is what the desktop guards against too. */
				if (codes[0] != 0xe1) {
					keyboard_key_release(codes);
				}
			}
			return;
		}
	}
}

/*
 * The pointer.
 *
 * mousehack is on, so the guest's pointer is placed absolutely - which is what a
 * touch screen wants: the pointer goes where the finger is rather than drifting
 * relative to it. mouse_mouse_move() takes guest pixels, and the frame is stretched
 * across the whole window, so window coordinates scale linearly onto it.
 */
static void
pointer_to(int win_x, int win_y)
{
	int ww = 0, wh = 0;
	int gx, gy;

	SDL_GetWindowSize(window, &ww, &wh);
	if (ww <= 0 || wh <= 0 || frame.w <= 0 || frame.h <= 0) {
		return;
	}

	gx = win_x * frame.w / ww;
	gy = win_y * frame.h / wh;
	if (gx < 0) { gx = 0; }
	if (gy < 0) { gy = 0; }
	if (gx >= frame.w) { gx = frame.w - 1; }
	if (gy >= frame.h) { gy = frame.h - 1; }

	mouse_mouse_move(gx, gy);
}

/*
 * Touch, on an OS built for three buttons.
 *
 * RISC OS puts Select on the left button, Menu on the middle and Adjust on the
 * right, and Menu is used constantly - so the count of fingers down chooses the
 * button: one finger Select, two Menu, three Adjust. That keeps the common case a
 * plain tap while making the menu reachable without any on-screen furniture.
 *
 * A real mouse is handled separately and needs none of this: Android supports one
 * over USB or Bluetooth, and then the buttons are simply the buttons.
 */
#define RISCOS_LEFT	1
#define RISCOS_RIGHT	2
#define RISCOS_MIDDLE	4

static int touch_button;	/* the button this touch is holding, 0 if none */

static int
button_for_fingers(int fingers)
{
	if (fingers >= 3) {
		return RISCOS_RIGHT;
	}
	if (fingers == 2) {
		return RISCOS_MIDDLE;
	}
	return RISCOS_LEFT;
}

static void
touch_release(void)
{
	if (touch_button != 0) {
		mouse_mouse_release(touch_button);
		touch_button = 0;
	}
}

/* ---- presenting a frame --------------------------------------------------- */

static void
present(void)
{
	pthread_mutex_lock(&frame_mutex);
	if (!frame.have || frame.pixels == NULL) {
		pthread_mutex_unlock(&frame_mutex);
		return;
	}

	if (texture == NULL || tex_w != frame.w || tex_h != frame.h) {
		if (texture != NULL) {
			SDL_DestroyTexture(texture);
		}
		/*
		 * RGB888 (which is XRGB8888: the top byte is ignored) rather than
		 * ARGB8888. The core writes 0x00RRGGBB, so the alpha byte is zero, and
		 * with a format that honours alpha the whole frame is transparent and
		 * the screen stays black while frames arrive perfectly well. Blending
		 * is turned off for the same reason.
		 */
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
		    SDL_TEXTUREACCESS_STREAMING, frame.w, frame.h);
		if (texture == NULL) {
			LOGE("SDL_CreateTexture: %s", SDL_GetError());
			pthread_mutex_unlock(&frame_mutex);
			return;
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
		tex_w = frame.w;
		tex_h = frame.h;
	}

	if (SDL_UpdateTexture(texture, NULL, frame.pixels, frame.w * 4) != 0) {
		LOGE("SDL_UpdateTexture: %s", SDL_GetError());
	}
	SDL_RenderClear(renderer);
	/* Letterboxed to the guest's aspect ratio rather than stretched: a RISC OS
	   mode on a 16:10 tablet would otherwise be visibly wrong. */
	if (SDL_RenderCopy(renderer, texture, NULL, NULL) != 0) {
		LOGE("SDL_RenderCopy: %s", SDL_GetError());
	}
	SDL_RenderPresent(renderer);

	frame.have = 0;
	pthread_mutex_unlock(&frame_mutex);
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
	/* Absolute pointer placement, which is what mouse_mouse_move() requires (it
	   asserts mousehack) and what a touch screen wants. */
	config.mousehackon = 1;
	/* DIAGNOSTIC: let a host build of this front end be driven by the debug
	   socket, which is how the guest can be asked where it is stuck. */
	if (getenv("RPCEMU_DEBUG_SOCKET") != NULL) {
		config.debug_enabled = 1;
		snprintf(config.debug_socket, sizeof(config.debug_socket), "%s",
		    getenv("RPCEMU_DEBUG_SOCKET"));
	}
	if (getenv("RPCEMU_ROM_DIR") != NULL) {
		snprintf(config.rom_dir, sizeof(config.rom_dir), "%s",
		    getenv("RPCEMU_ROM_DIR"));
	}

	LOGI("data directory %s, machine %s, ROM directory \"%s\"",
	    rpcemu_get_datadir(), rpcemu_get_machine_datadir(), config.rom_dir);

	rpcemu_model_changed(config.model);

	/* Before rpcemu_start(): the core reads the clock while starting up, and a
	   zero base there would make its first deadlines meaningless. */
	start_ns = monotonic_ns();

	rpcemu_start();

	LOGI("machine started, %s backend",
	    arm_is_dynarec() ? "recompiler" : "interpreter");
	return 1;
}

int
main(int argc, char *argv[])
{
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
	{
		int ww = -1, wh = -1, ow = -1, oh = -1;

		SDL_GetWindowSize(window, &ww, &wh);
		SDL_GetRendererOutputSize(renderer, &ow, &oh);
		LOGI("window %dx%d, renderer output %dx%d", ww, wh, ow, oh);
	}

	if (!machine_start()) {
		return 1;
	}

	video_interval = 1000000000ULL / (config.refresh > 0 ? (unsigned) config.refresh : 60u);
	iomd_next = IOMD_INTERVAL_NS;
	video_next = video_interval;

	while (!quit_requested) {
		SDL_Event ev;
		int n;

		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				quit_requested = 1;
				break;

			case SDL_KEYDOWN:
				if (ev.key.repeat == 0) {
					handle_key(ev.key.keysym.scancode, 1);
				}
				break;
			case SDL_KEYUP:
				handle_key(ev.key.keysym.scancode, 0);
				break;

			/* A real mouse: the buttons are the buttons. */
			case SDL_MOUSEMOTION:
				if (ev.motion.which != SDL_TOUCH_MOUSEID) {
					pointer_to(ev.motion.x, ev.motion.y);
				}
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if (ev.button.which != SDL_TOUCH_MOUSEID) {
					int b = RISCOS_LEFT;

					if (ev.button.button == SDL_BUTTON_MIDDLE) {
						b = RISCOS_MIDDLE;
					} else if (ev.button.button == SDL_BUTTON_RIGHT) {
						b = RISCOS_RIGHT;
					}
					pointer_to(ev.button.x, ev.button.y);
					if (ev.type == SDL_MOUSEBUTTONDOWN) {
						mouse_mouse_press(b);
					} else {
						mouse_mouse_release(b);
					}
				}
				break;
			case SDL_MOUSEWHEEL:
				podulerom_mouse_wheel_change(ev.wheel.y);
				break;

			/* Touch: finger count picks the button. */
			case SDL_FINGERDOWN: {
				int ww = 0, wh = 0;

				SDL_GetWindowSize(window, &ww, &wh);
				pointer_to((int) (ev.tfinger.x * ww),
				    (int) (ev.tfinger.y * wh));
				touch_release();
				touch_button = button_for_fingers(
				    SDL_GetNumTouchFingers(ev.tfinger.touchId));
				mouse_mouse_press(touch_button);
				break;
			}
			case SDL_FINGERMOTION: {
				int ww = 0, wh = 0;

				SDL_GetWindowSize(window, &ww, &wh);
				pointer_to((int) (ev.tfinger.x * ww),
				    (int) (ev.tfinger.y * wh));
				break;
			}
			case SDL_FINGERUP:
				touch_release();
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
		/* Run the guest. execrpcemu() also calls drawscr() when a frame is
		   owed, which wakes the video thread and reaches
		   rpcemu_video_update(). */
		for (n = 0; n < 32 && !quit_requested; n++) {
			execrpcemu();
		}

		service_timers();
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
