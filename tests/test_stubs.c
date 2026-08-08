/*
  Stub definitions of the platform / front-end callbacks that the emulator core
  references but the JIT flag tester never exercises. They let the test link
  against rpcemu_core without the wxWidgets/SDL front-end. `fatal` is the only
  one that could plausibly fire (core error paths); it reports and exits so a
  test bug is loud rather than silent.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void
fatal(const char *format, ...)
{
	va_list ap;

	fputs("test stub: fatal() called: ", stderr);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/*
 * The core's non-fatal error reporter (the GUI provides the real one). On
 * glibc/Linux this previously resolved by accident to libc's error(3) GNU
 * extension - a different signature - so no stub was needed; macOS/BSD has no
 * such function, so define it explicitly. Never exercised by the flag tester.
 */
void
error(const char *format, ...)
{
	va_list ap;

	fputs("test stub: error() called: ", stderr);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void rpcemu_log_platform(void) {}

/* Config */
void config_load(void *config) { (void) config; }
void config_save(void *config) { (void) config; }

/* Video / input / timing / NAT front-end hooks */
void rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize, int yl,
    int yh, int double_size, int host_xsize, int host_ysize)
{
	(void) buffer; (void) xsize; (void) ysize; (void) yl; (void) yh;
	(void) double_size; (void) host_xsize; (void) host_ysize;
}
void rpcemu_move_host_mouse(uint16_t x, uint16_t y) { (void) x; (void) y; }
void rpcemu_idle_process_events(void) {}
void rpcemu_request_poweroff(void) {}
/* Emulated time. Most tests never run a timer and are happy with zero, but a
   test that boots a machine must have time advance or the IOMD timers never fire
   and RISC OS waits for an interrupt for ever. Such a test defines
   rpcemu_test_virtual_ns() and overrides this weak default, which keeps every
   other test's behaviour exactly as it was. */
__attribute__((weak)) uint64_t rpcemu_test_virtual_ns(void) { return 0; }
uint64_t rpcemu_nsec_timer_ticks(void) { return rpcemu_test_virtual_ns(); }

/* Activity indicators */
void fdc_activity_increment(void) {}
void hostfs_activity_increment(void) {}
void ide_activity_increment(void) {}
void network_activity_increment(void) {}

/* Sound back-end */
void plt_sound_init(int samples) { (void) samples; }
void plt_sound_buffer_play(void) {}
void plt_sound_buffer_free(void) {}
void plt_sound_pause(void) {}
void plt_sound_restart(void) {}
void sound_thread_start(void) {}
void sound_thread_close(void) {}
void sound_thread_wakeup(void) {}

/* VIDC worker thread */
void vidcstartthread(void) {}
void vidcendthread(void) {}
void vidcwakeupthread(void) {}
void vidcreleasemutex(void) {}
int  vidctrymutex(void) { return 0; }

/*
 * Supplied by the front end: says whether host input or another command is
 * waiting, so the idle loop can stop and let the main loop act on it. Nothing in
 * a test posts any, and no test reaches rpcemu_idle() anyway.
 */
int
rpcemu_host_commands_pending(void)
{
	return 0;
}
