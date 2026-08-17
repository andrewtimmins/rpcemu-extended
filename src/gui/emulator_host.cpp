/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

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

#include "emulator_host.h"

extern "C" {
#include "perfprof.h"
}

#include "emulator_snapshot.h"

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>	/* pthread_set_qos_class_self_np - see MainEmuLoop() */
#endif

extern "C" {
#include "arm.h"
#include "hostclipboard.h"
#include "arm_disasm.h"
#include "cdrom-iso.h"
#include "cmos.h"
#include "debugcmd.h"
#include "hostcmd.h"
#include "usb_ohci.h"
#include "hostfs.h"
#include "ide.h"
#include "keyboard.h"
#include "mem.h"
#include "network.h"
#include "network-nat.h"
#include "podulerom.h"
#include "romload.h"
#include "savestate.h"
#include "sound.h"
#include "vidc20.h"
}

#ifdef RPCEMU_VNC
#include "vnc_server.h"
#endif

#if defined(__linux__)
extern "C" void ioctl_init(void);
#endif

std::atomic<int> instruction_count{0};
std::atomic<int> iomd_timer_count{0};
std::atomic<int> video_timer_count{0};
std::atomic<int> hostfs_activity{0};
std::atomic<int> network_activity{0};
std::atomic<int> ide_activity{0};
std::atomic<int> fdc_activity{0};

int mouse_captured = 0;
Config *pconfig_copy = nullptr;

static GuiBridge *g_gui_bridge = nullptr;
static EmulatorHost *g_emulator_host = nullptr;

/* How often the IOMD timers, the sound pacing and the podule timers are due,
   in nanoseconds. Shared by the running loop and the idle path so the two
   cannot drift apart. */
static const int64_t kIomdTimerIntervalNs = 2000000;

/* Set as soon as any fatal() is raised, on whatever thread. The raising thread
   then spins forever inside fatal(), so it can never again service commands or
   signal a condition variable. GUI-thread waits and joins check this flag so
   they can never deadlock on that spinning thread; the process is terminated
   cleanly by ShowFatal() (or OnClose) reaching std::_Exit. */
static std::atomic<bool> g_fatal_occurred{false};

bool EmulatorFatalOccurred()
{
	return g_fatal_occurred.load();
}

static pthread_t sound_thread;
static pthread_cond_t sound_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t sound_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t video_thread;
static bool video_thread_running = false;
static pthread_cond_t video_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *
sound_thread_function(void *p)
{
	NOT_USED(p);

	if (pthread_mutex_lock(&sound_mutex)) {
		fatal("Cannot lock mutex");
	}

	while (!quited) {
		if (pthread_cond_wait(&sound_cond, &sound_mutex)) {
			fatal("pthread_cond_wait failed");
		}
		if (!quited) {
			sound_buffer_update();
		}
	}

	pthread_mutex_unlock(&sound_mutex);
	return nullptr;
}

extern "C" void sound_thread_wakeup(void)
{
	if (pthread_mutex_lock(&sound_mutex)) {
		fatal("Cannot lock mutex");
	}
	if (pthread_cond_broadcast(&sound_cond)) {
		pthread_mutex_unlock(&sound_mutex);
		fatal("Couldn't signal sound thread");
	}
	pthread_mutex_unlock(&sound_mutex);
}

extern "C" void sound_thread_close(void)
{
	sound_thread_wakeup();
	pthread_join(sound_thread, nullptr);
}

extern "C" void sound_thread_start(void)
{
	if (pthread_create(&sound_thread, nullptr, sound_thread_function, nullptr)) {
		fatal("Couldn't create sound thread");
	}

	/* 2-arg (Linux/winpthreads) form; macOS's pthread_setname_np takes one arg
	   and names only the calling thread, so skip it there (names are cosmetic). */
#if defined(_GNU_SOURCE) && !defined(__APPLE__)
	pthread_setname_np(sound_thread, "rpcemu: sound");
#endif
}

static void vblupdate(void)
{
	drawscre++;
}

static void *
vidcthreadrunner(void *threadid)
{
	NOT_USED(threadid);

	if (pthread_mutex_lock(&video_mutex)) {
		fatal("Cannot lock mutex");
	}

	while (!quited) {
		if (pthread_cond_wait(&video_cond, &video_mutex)) {
			fatal("pthread_cond_wait failed");
		}
		if (!quited) {
			vidcthread();
		}
	}

	pthread_mutex_unlock(&video_mutex);
	return nullptr;
}

extern "C" void hostfs_activity_increment(void)
{
	hostfs_activity.fetch_add(1, std::memory_order_release);
}

extern "C" void network_activity_increment(void)
{
	network_activity.fetch_add(1, std::memory_order_release);
}

extern "C" void ide_activity_increment(void)
{
	ide_activity.fetch_add(1, std::memory_order_release);
}

extern "C" void fdc_activity_increment(void)
{
	fdc_activity.fetch_add(1, std::memory_order_release);
}

extern "C" void vidcstartthread(void)
{
	if (pthread_create(&video_thread, nullptr, vidcthreadrunner, nullptr)) {
		fatal("Couldn't create vidc thread");
	}
	video_thread_running = true;

#if defined(_GNU_SOURCE) && !defined(__APPLE__)
	pthread_setname_np(video_thread, "rpcemu: vidc");
#endif
}

extern "C" void vidcendthread(void)
{
	// Cleanly stop and join the VIDC thread so it can no longer touch
	// g_gui_bridge / g_vnc_server after teardown (previously this was a no-op,
	// leaving a use-after-free window during shutdown). quited is already set
	// by EmulatorHost::Stop() before we get here; wake the thread so it
	// observes it, then wait for it to exit. PostVideoUpdate() is now bounded
	// on quited, so the thread cannot be stuck blocking the join.
	if (!video_thread_running) {
		return;
	}
	vidcwakeupthread();
	pthread_join(video_thread, nullptr);
	video_thread_running = false;
}

extern "C" void vidcwakeupthread(void)
{
	if (pthread_cond_signal(&video_cond)) {
		fatal("Couldn't signal vidc thread");
	}
}

extern "C" int vidctrymutex(void)
{
	const int ret = pthread_mutex_trylock(&video_mutex);
	if (ret == EBUSY) {
		return 0;
	}
	if (ret != 0) {
		fatal("Obtaining vidc mutex failed");
	}
	return 1;
}

extern "C" void vidcreleasemutex(void)
{
	if (pthread_mutex_unlock(&video_mutex)) {
		fatal("Releasing vidc mutex failed");
	}
}

extern "C" void rpcemu_video_update(const uint32_t *buffer, int xsize, int ysize,
                                    int yl, int yh, int double_size, int host_xsize, int host_ysize)
{
	if (buffer == nullptr || xsize <= 0 || ysize <= 0 || g_gui_bridge == nullptr) {
		return;
	}

	VideoUpdate update;
	update.buffer = buffer;
	update.xsize = xsize;
	update.ysize = ysize;
	update.yl = yl;
	update.yh = yh;
	update.double_size = double_size;
	update.host_xsize = host_xsize;
	update.host_ysize = host_ysize;

	g_gui_bridge->PostVideoUpdate(update);

#ifdef RPCEMU_VNC
	if (g_vnc_server && g_vnc_server->isRunning()) {
		g_vnc_server->updateFramebuffer(buffer, xsize, ysize, yl, yh);
	}
#endif
}

extern "C" void rpcemu_pointer_shape(const uint8_t *bits, int row_bytes,
                                     const uint32_t *palette, int width,
                                     int height, int hotspot_x, int hotspot_y,
                                     int visible, int host_side)
{
	if (g_gui_bridge == nullptr) {
		return;
	}

	PointerShape shape;

	shape.bits = bits;
	shape.row_bytes = row_bytes;
	if (palette != nullptr) {
		memcpy(shape.palette, palette, sizeof(shape.palette));
	}
	shape.width = width;
	shape.height = height;
	shape.hotspot_x = hotspot_x;
	shape.hotspot_y = hotspot_y;
	shape.visible = visible != 0;
	shape.host_side = host_side != 0;
	g_gui_bridge->PostPointerShape(shape);
}

/*
 * Whether anyone is actually watching over VNC.
 *
 * A remote viewer can only be sent what is in the frame, so while one is
 * connected the pointer has to stay composited and the host must not draw its
 * own. Deliberately the CLIENT COUNT and not isRunning(): the server is left
 * enabled in a machine's configuration far more often than it is used, and
 * gating on the server would have quietly switched the host-drawn pointer off
 * for everybody who has VNC turned on and nobody connected - which is most
 * people who have it turned on at all.
 *
 * A client arriving mid-session turns this true, the pointer goes back into the
 * frame on the next one, and the front end is told to stop drawing its own; a
 * client leaving reverses it. updateFramebuffer() already skips its work on the
 * same test, so the two agree about when a viewer matters.
 */
extern "C" int rpcemu_vnc_active(void)
{
#ifdef RPCEMU_VNC
	return (g_vnc_server && g_vnc_server->isRunning() &&
	        g_vnc_server->getClientCount() > 0) ? 1 : 0;
#else
	return 0;
#endif
}

extern "C" void rpcemu_move_host_mouse(uint16_t x, uint16_t y)
{
	if (g_gui_bridge == nullptr) {
		return;
	}

	MouseMoveUpdate update;
	update.x = static_cast<int16_t>(x);
	update.y = static_cast<int16_t>(y);
	g_gui_bridge->PostMoveHostMouse(update);
}

extern "C" void rpcemu_set_host_clipboard(const char *utf8, unsigned int len)
{
	if (g_gui_bridge != nullptr && utf8 != nullptr) {
		g_gui_bridge->PostSetHostClipboard(std::string(utf8, len));
	}
}

extern "C" void rpcemu_set_host_clipboard_image(int file_type, const void *data,
                                                unsigned int len)
{
	if (g_gui_bridge != nullptr && data != nullptr) {
		g_gui_bridge->PostSetHostClipboardImage(file_type,
		    std::string(static_cast<const char *>(data), len));
	}
}

extern "C" void rpcemu_request_poweroff(void)
{
	/* Guest soft power-off: ask the active front-end to quit. Fall back to
	   stopping the emulation loop directly if no bridge is attached. */
	if (g_gui_bridge != nullptr) {
		g_gui_bridge->PostQuit();
	} else {
		quited = 1;
	}
}

extern "C" void rpcemu_idle_process_events(void)
{
	if (g_emulator_host != nullptr) {
		g_emulator_host->IdleProcessEvents();
	}
}

extern "C" uint64_t rpcemu_nsec_timer_ticks(void)
{
	if (g_emulator_host != nullptr) {
		return static_cast<uint64_t>(g_emulator_host->GetElapsedTimerNs());
	}
	return 0;
}

extern "C" void error(const char *format, ...)
{
	char buf[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	rpclog("ERROR: %s\n", buf);
	fprintf(stderr, "RPCEmu error: %s\n", buf);

	if (g_gui_bridge == nullptr) {
		return;
	}

	if (g_gui_bridge->IsGuiThread()) {
		g_gui_bridge->ShowError(buf);
	} else {
		g_gui_bridge->PostError(buf);
	}
}

extern "C" void fatal(const char *format, ...)
{
	char buf[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	rpclog("FATAL: %s\n", buf);
	fprintf(stderr, "RPCEmu fatal error: %s\n", buf);

	/* Only once the CPU has run: before that the registers are sixteen
	   zeroes, and most of these calls are a failed allocation or a ROM that
	   would not load. Reads only scalars and fixed-size arrays, with the one
	   variable index bounds-checked, so it cannot fail however corrupt
	   things are. */
	if (inscount != 0) {
		rpclog("FATAL: PC=%08x mode=%u model=%s mem=%uMB event=%08x\n",
		       arm.reg[15], (unsigned) arm.mode,
		       machine.model < Model_MAX ? models[machine.model].name_config : "?",
		       config.mem_size, (unsigned) arm.event);
		rpclog("  regs r0-r7:  %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       arm.reg[0], arm.reg[1], arm.reg[2], arm.reg[3],
		       arm.reg[4], arm.reg[5], arm.reg[6], arm.reg[7]);
		rpclog("  regs r8-r15: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       arm.reg[8], arm.reg[9], arm.reg[10], arm.reg[11],
		       arm.reg[12], arm.reg[13], arm.reg[14], arm.reg[15]);
	}

	/* Record the fatal before anything else so any GUI-thread wait/join can
	   observe it and stop blocking on this (about to spin) thread. A cv.wait
	   only re-evaluates its predicate when notified, so also wake every request
	   condition variable — otherwise a GUI thread already parked in one of them
	   would sleep forever, since this thread is about to stop signalling. */
	g_fatal_occurred.store(true);
	if (g_emulator_host != nullptr) {
		g_emulator_host->WakeAllWaiters();
	}

	if (g_gui_bridge == nullptr) {
		exit(EXIT_FAILURE);
	}

	if (g_gui_bridge->IsGuiThread()) {
		g_gui_bridge->ShowFatal(buf);
	} else {
		g_gui_bridge->PostFatal(buf);
	}

	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

extern "C" void rpcemu_log_platform(void)
{
	rpclog("RPCEmu wxWidgets host %s\n", VERSION);
}

EmulatorHost *emulator_host_instance()
{
	return g_emulator_host;
}

EmulatorHost::EmulatorHost(GuiBridge *gui_bridge)
	: gui_bridge_(gui_bridge)
{
	g_gui_bridge = gui_bridge;
	g_emulator_host = this;
	clipboard_set_host_setter(rpcemu_set_host_clipboard);
	clipboard_set_host_image_setter(rpcemu_set_host_clipboard_image);
	start_time_ = std::chrono::steady_clock::now();
}

EmulatorHost::~EmulatorHost()
{
	Stop();
	Join();
	if (g_emulator_host == this) {
		g_emulator_host = nullptr;
	}
	if (g_gui_bridge == gui_bridge_) {
		g_gui_bridge = nullptr;
	}
}

void EmulatorHost::Start()
{
	emu_thread_ = std::thread([this]() { MainEmuLoop(); });
}

void EmulatorHost::Stop()
{
	quited = 1;
	sound_thread_wakeup();
	vidcwakeupthread();
}

void EmulatorHost::Join()
{
	if (g_fatal_occurred.load()) {
		/* The emulator thread is spinning forever inside fatal() and will
		   never return; joining it would deadlock. The process is terminated
		   via ShowFatal()/OnClose reaching std::_Exit instead. */
		return;
	}
	if (emu_thread_.joinable()) {
		emu_thread_.join();
	}
}

void EmulatorHost::WakeAllWaiters()
{
	/* Notify without holding the per-request mutexes: the predicates already
	   test g_fatal_occurred (set before this is called), so a waiter that wakes
	   spuriously here still sees the fatal and returns. */
	trace_cv_.notify_all();
	snapshot_cv_.notify_all();
	state_cv_.notify_all();
	memory_cv_.notify_all();
	disasm_cv_.notify_all();
}

/**
 * Set while the queue has anything in it, cleared as the queue is taken.
 *
 * The emulator thread only acts on commands at the top of MainEmuLoop, because
 * that is the one place it is safe to: Reset, SwitchMachine and Exit cannot be
 * run from inside a SWI half way through an instruction. Under "Reduce CPU
 * usage" it takes a long time to get back there. It sits in rpcemu_idle()
 * sleeping a millisecond at a time and only returns once the 20000-cycle budget
 * is spent, about 35 times a second measured, and kEmuBatch runs 32 of those
 * before draining - so the queue was being taken once or twice a second.
 *
 * Every mouse move and key press waited that long, which is issue #36. So did
 * the flyback interrupt, which is worse than it sounds: RISC OS applies pointer
 * and palette changes on vsync, and the interrupt that says a frame has gone out
 * is posted from the video thread through this same queue.
 *
 * This flag lets rpcemu_idle() stop as soon as there is something to do, so the
 * main loop reaches the drain in the ordinary way. Nothing is executed from the
 * idle path, so the reason the drain lives where it does is undisturbed.
 */
static std::atomic<int> g_commands_pending{0};

void EmulatorHost::PostCommand(EmuCommand command)
{
	{
		std::lock_guard<std::mutex> lock(command_mutex_);
		commands_.push(std::move(command));
	}

	/* After the push, so the emulator thread never sees the flag set with an
	   empty queue. */
	g_commands_pending.store(1, std::memory_order_release);
}

/**
 * Whether the host has queued anything for the emulator thread.
 *
 * Called from rpcemu_idle() on the emulator thread. Deliberately a plain flag
 * rather than a lock: this is read in the idle loop's condition, and taking the
 * command mutex there would put the queue's lock on the path of every idle
 * iteration for no benefit.
 */
extern "C" int rpcemu_host_commands_pending(void)
{
	return g_commands_pending.load(std::memory_order_acquire);
}

static EmuCommand MakeCommand(EmuCommandType type)
{
	EmuCommand cmd;
	cmd.type = type;
	return cmd;
}

int64_t EmulatorHost::GetElapsedTimerNs() const
{
	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time_).count();
}

void EmulatorHost::DrainCommands()
{
	std::queue<EmuCommand> pending;

	/* Cleared before the queue is taken, so a command posted while this is
	   running sets it again rather than being left unnoticed until the next
	   one arrives. */
	g_commands_pending.store(0, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lock(command_mutex_);
		pending.swap(commands_);
	}

	while (!pending.empty()) {
		HandleCommand(pending.front());
		pending.pop();
	}
}

void EmulatorHost::KeyPress(unsigned scan_code)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::KeyPress;
	cmd.scan_code = scan_code;
	PostCommand(cmd);
}

void EmulatorHost::KeyRelease(unsigned scan_code)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::KeyRelease;
	cmd.scan_code = scan_code;
	PostCommand(cmd);
}

void EmulatorHost::MouseMove(int x, int y)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::MouseMove;
	cmd.arg1 = x;
	cmd.arg2 = y;
	PostCommand(cmd);
}

void EmulatorHost::MouseMoveRelative(int dx, int dy)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::MouseMoveRelative;
	cmd.arg1 = dx;
	cmd.arg2 = dy;
	PostCommand(cmd);
}

void EmulatorHost::MousePress(int buttons)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::MousePress;
	cmd.arg1 = buttons;
	PostCommand(cmd);
}

void EmulatorHost::MouseRelease(int buttons)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::MouseRelease;
	cmd.arg1 = buttons;
	PostCommand(cmd);
}

void EmulatorHost::MouseWheel(int dy)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::MouseWheel;
	cmd.arg1 = dy;
	PostCommand(cmd);
}

void EmulatorHost::Reset()
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::Reset;
	PostCommand(cmd);
}

void EmulatorResetForSignal(EmulatorHost *emulator)
{
	/* The menu asks before resetting, which a signal cannot do; past that this
	   is the same reset. */
	if (emulator == nullptr || !emulator->IsRunning()) {
		rpclog("RPCEmu: SIGUSR1 received, but no machine is running\n");
		return;
	}

	rpclog("RPCEmu: SIGUSR1 received, resetting the machine\n");
	emulator->Reset();
}

void EmulatorHost::ReloadIdeImages()
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::ReloadIdeImages;
	PostCommand(cmd);
}

void EmulatorHost::HandleCommand(const EmuCommand &command)
{
	switch (command.type) {
	case EmuCommandType::KeyPress: {
		const uint8_t *scan_codes = keyboard_map_key(command.scan_code);
		if (scan_codes == nullptr) {
			rpclog("Unknown keyboard mapping for host keycode 0x%08x\n", command.scan_code);
		}
		keyboard_key_press(scan_codes);
		break;
	}
	case EmuCommandType::KeyRelease: {
		const uint8_t *scan_codes = keyboard_map_key(command.scan_code);
		if (scan_codes && scan_codes[0] == 0xe1) {
			break;
		}
		keyboard_key_release(scan_codes);
		break;
	}
	case EmuCommandType::MouseMove:
		mouse_mouse_move(command.arg1, command.arg2);
		break;
	case EmuCommandType::MouseMoveRelative:
		mouse_mouse_move_relative(command.arg1, command.arg2);
		break;
	case EmuCommandType::MousePress:
		mouse_mouse_press(command.arg1);
		break;
	case EmuCommandType::MouseRelease:
		mouse_mouse_release(command.arg1);
		break;
	case EmuCommandType::MouseWheel:
		podulerom_mouse_wheel_change(command.arg1);
		break;
	case EmuCommandType::Reset:
		pthread_mutex_lock(&video_mutex);
		pthread_mutex_unlock(&video_mutex);
		resetrpc();
		break;
	case EmuCommandType::SaveState: {
		int r;

		/* Quiesce the video thread while the snapshot is taken */
		pthread_mutex_lock(&video_mutex);
		r = state_save(command.string_path.c_str());
		pthread_mutex_unlock(&video_mutex);

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			state_ok_ = (r == 0);
			state_error_.clear();
			state_ready_ = true;
		}
		state_cv_.notify_one();
		break;
	}
	case EmuCommandType::LoadState: {
		char errbuf[256];
		bool ok;
		std::string err;

		/* Validate against the running configuration first, so a
		   mismatched or non-snapshot file leaves the session untouched */
		if (state_check(command.string_path.c_str(), errbuf, sizeof(errbuf)) != 0) {
			ok = false;
			err = errbuf;
		} else {
			int r;

			pthread_mutex_lock(&video_mutex);
			r = state_load(command.string_path.c_str());
			pthread_mutex_unlock(&video_mutex);

			ok = (r == 0);
			if (!ok) {
				err = "Failed to load the machine state; the machine has been reset.";
			}
		}

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			state_ok_ = ok;
			state_error_ = err;
			state_ready_ = true;
		}
		state_cv_.notify_one();
		break;
	}
	case EmuCommandType::Exit:
		quited = 1;
		break;
	case EmuCommandType::ConfigUpdated:
		if (command.config_ptr != nullptr) {
			rpcemu_config_apply_new_settings(command.config_ptr, command.model);
			free(command.config_ptr);
		}
		break;
	case EmuCommandType::LoadDisc0:
	case EmuCommandType::LoadDisc1: {
		const int drive = (command.type == EmuCommandType::LoadDisc0) ? 0 : 1;
		rpcemu_floppy_load(drive, command.string_path.c_str());
		break;
	}
	case EmuCommandType::EjectDisc0:
		rpcemu_floppy_eject(0);
		break;
	case EmuCommandType::EjectDisc1:
		rpcemu_floppy_eject(1);
		break;
	case EmuCommandType::ReloadIdeImages:
		ide_reload_images();
		break;
	case EmuCommandType::SwitchMachine: {
		char old_rom_dir[512];

		rpclog("RPCEmu: Switching machine to: %s\n", command.string_path.c_str());

		savecmos();

		strncpy(old_rom_dir, config.rom_dir, sizeof(old_rom_dir) - 1);
		old_rom_dir[sizeof(old_rom_dir) - 1] = '\0';

		config_set_path(command.string_path.c_str());
		config_load(&config);

		hostfs_init();
		cmos_init();

		if (strcmp(old_rom_dir, config.rom_dir) != 0) {
			rpclog("RPCEmu: ROM directory changed, reloading ROMs\n");
			loadroms();
		}

		resetrpc();

		if (gui_bridge_ != nullptr) {
			gui_bridge_->PostMachineSwitched(config.name);
		}

		rpclog("RPCEmu: Machine switch complete\n");
		break;
	}
	case EmuCommandType::GuestCommand: {
		/*
		 * Hand it to hostcmd, which is on this thread. The completion callback
		 * is called here too, so it only forwards the result GUI-ward.
		 *
		 * The token is passed as the opaque pointer rather than allocating
		 * anything: it is a small integer and this way there is nothing to
		 * free if the machine is reset before the command finishes.
		 */
		const unsigned token = command.token;

		if (hostcmd_internal_submit(command.string_path.c_str(),
		        [](uint32_t rc, const char *output, size_t len, void *opaque) {
			        if (g_emulator_host != nullptr &&
			            g_emulator_host->gui_bridge_ != nullptr) {
				        g_emulator_host->gui_bridge_->PostGuestCommandResult(
				            static_cast<unsigned>(
				                reinterpret_cast<uintptr_t>(opaque)),
				            rc, std::string(output, len), true);
			        }
		        },
		        reinterpret_cast<void *>(
		            static_cast<uintptr_t>(token))) != 0) {
			/* Refused: say so at once rather than leaving the caller to time
			   out, which is the difference between "the guest is busy" and
			   "the guest is not answering". */
			if (gui_bridge_ != nullptr) {
				gui_bridge_->PostGuestCommandResult(token, 0, std::string(),
				                                    false);
			}
		}
		break;
	}

	case EmuCommandType::GuestCommandAbandon:
		hostcmd_internal_abandon();
		break;

	case EmuCommandType::UsbApplyConfig:
		usb_ohci_apply_config();
		break;

	case EmuCommandType::Restart: {
		rpclog("RPCEmu: Restarting machine\n");

		/* The guest writes its CMOS out as it changes it (see the checksum
		   handling in cmos.c), so this is only here to catch a change that had
		   not reached the checksum byte yet. */
		savecmos();

		/* Re-read the machine's own configuration file, so a setting changed
		   in the machine editor since it started is picked up. */
		config_load(&config);

		hostfs_init();
		cmos_init();

		/* Unconditionally, unlike SwitchMachine: the machine editor has already
		   written the new ROM directory into the live config by this point, so
		   comparing the old value against the new one would never differ and
		   the ROM would never be reloaded - which is the whole reason a reset
		   was not enough. */
		loadroms();

		resetrpc();

		if (gui_bridge_ != nullptr) {
			gui_bridge_->PostMachineSwitched(config.name);
		}

		rpclog("RPCEmu: Restart complete\n");
		break;
	}
	case EmuCommandType::CpuIdle:
		config.cpu_idle ^= 1;
		config_save(&config);
		resetrpc();
		break;
	case EmuCommandType::IntegerScaling:
		config.integer_scaling ^= 1;
		config_save(&config);
		break;
	case EmuCommandType::FitToWindow:
		config.fit_to_window ^= 1;
		config_save(&config);
		break;
	case EmuCommandType::FollowHostDisplay:
		config.follow_host_display ^= 1;
		config_save(&config);
		break;
	case EmuCommandType::ClipboardEnabled:
		config.clipboard_enabled ^= 1;
		config_save(&config);
		if (!config.clipboard_enabled) {
			clipboard_reset();
		}
		break;
	case EmuCommandType::HostClipboardChanged:
		/* Runs on the emulator thread, so setting the guest's pollword and
		   touching the stored clipboard are safe here. */
		clipboard_host_changed(command.arg1, command.string_path.data(),
		                       static_cast<unsigned int>(command.string_path.size()));
		break;
	case EmuCommandType::CdromDisabled:
		if (config.cdromenabled) {
			config.cdromenabled = 0;
			config_save(&config);
			resetrpc();
		}
		break;
	case EmuCommandType::CdromEmpty:
		if (!config.cdromenabled) {
			config.cdromenabled = 1;
			config_save(&config);
			resetrpc();
		}
		atapi->exit();
		iso_init();
		break;
	case EmuCommandType::CdromLoadIso:
		if (!config.cdromenabled) {
			config.cdromenabled = 1;
			config_save(&config);
			resetrpc();
		}
		if (snprintf(config.isoname, sizeof(config.isoname), "%s",
		             command.string_path.c_str()) >= (int)sizeof(config.isoname)) {
			error("ISO image disk path '%s' too long", command.string_path.c_str());
			break;
		}
		atapi->exit();
		iso_open(config.isoname);
		break;
	case EmuCommandType::CdromIoctl:
#if defined(__linux__)
		if (!config.cdromenabled) {
			config.cdromenabled = 1;
			config_save(&config);
			resetrpc();
		}
		atapi->exit();
		ioctl_init();
#else
		/* Real-drive CD-ROM (ioctl backend) is Linux-only; use an ISO image. */
		rpclog("CD-ROM: real-drive access is not supported on this platform\n");
#endif
		break;
	case EmuCommandType::MouseHack:
		config.mousehackon ^= 1;
		config_save(&config);
		break;
	case EmuCommandType::MouseTwobutton:
		config.mousetwobutton ^= 1;
		config_save(&config);
		break;
	case EmuCommandType::ShowFullscreenMessageOff:
		config.show_fullscreen_message = 0;
		config_save(&config);
		break;
	case EmuCommandType::NatRuleAdd:
		network_nat_forward_add(command.nat_rule);
		rpcemu_nat_forward_add(command.nat_rule);
		config_save(&config);
		break;
	case EmuCommandType::NatRuleEdit:
		network_nat_forward_edit(command.nat_rule_old, command.nat_rule);
		rpcemu_nat_forward_remove(command.nat_rule_old);
		rpcemu_nat_forward_add(command.nat_rule);
		config_save(&config);
		break;
	case EmuCommandType::NatRuleRemove:
		network_nat_forward_remove(command.nat_rule);
		rpcemu_nat_forward_remove(command.nat_rule);
		config_save(&config);
		break;
	case EmuCommandType::DebuggerPause:
		debugger_request_pause(DebugPauseReason_User);
		NotifyDebuggerStateChanged();
		break;
	case EmuCommandType::DebuggerResume:
		debugger_resume();
		NotifyDebuggerStateChanged();
		break;
	case EmuCommandType::DebuggerStep:
		debugger_single_step(1);
		NotifyDebuggerStateChanged();
		break;
	case EmuCommandType::DebuggerStepN: {
		uint32_t count = command.debug_count;
		if (count == 0) {
			count = 1;
		}
		debugger_single_step(count);
		NotifyDebuggerStateChanged();
		break;
	}
	case EmuCommandType::DebuggerAddBreakpoint:
		if (debugger_add_breakpoint(command.debug_address)) {
			NotifyDebuggerStateChanged();
		}
		break;
	case EmuCommandType::DebuggerRemoveBreakpoint:
		if (debugger_remove_breakpoint(command.debug_address)) {
			NotifyDebuggerStateChanged();
		}
		break;
	case EmuCommandType::DebuggerClearBreakpoints:
		debugger_clear_breakpoints();
		NotifyDebuggerStateChanged();
		break;
	case EmuCommandType::DebuggerAddWatchpoint:
		if (debugger_add_watchpoint(command.debug_address, command.debug_size,
		                            command.debug_on_read ? 1 : 0,
		                            command.debug_on_write ? 1 : 0,
		                            command.debug_log_only ? 1 : 0)) {
			NotifyDebuggerStateChanged();
		}
		break;
	case EmuCommandType::DebuggerRemoveWatchpoint:
		if (debugger_remove_watchpoint(command.debug_address, command.debug_size,
		                               command.debug_on_read ? 1 : 0,
		                               command.debug_on_write ? 1 : 0)) {
			NotifyDebuggerStateChanged();
		}
		break;
	case EmuCommandType::DebuggerClearWatchpoints:
		debugger_clear_watchpoints();
		NotifyDebuggerStateChanged();
		break;
	case EmuCommandType::TakeSnapshot: {
		MachineSnapshot snapshot = emulator_take_snapshot();
		{
			std::lock_guard<std::mutex> lock(snapshot_mutex_);
			snapshot_result_ = snapshot;
			snapshot_ready_ = true;
		}
		snapshot_cv_.notify_one();
		break;
	}
	case EmuCommandType::ReadMemory: {
		std::vector<uint8_t> data =
		    emulator_read_memory(command.debug_address, command.debug_count);
		{
			std::lock_guard<std::mutex> lock(memory_mutex_);
			memory_result_ = std::move(data);
			memory_ready_ = true;
		}
		memory_cv_.notify_one();
		break;
	}
	case EmuCommandType::Disassemble: {
		std::string lines = emulator_disassemble_at(command.debug_address, command.arg1);
		{
			std::lock_guard<std::mutex> lock(disasm_mutex_);
			disasm_result_ = std::move(lines);
			disasm_ready_ = true;
		}
		disasm_cv_.notify_one();
		break;
	}
	case EmuCommandType::SetDebugTraceConfig:
		debugger_set_trace_config(&command.debug_trace_config);
		break;
	case EmuCommandType::DrainTraceEvents: {
		uint32_t max = command.debug_count ? command.debug_count : 256;
		std::vector<DebugTraceEvent> events(max);
		uint32_t dropped = 0;
		uint32_t n = debugger_drain_trace_events(events.data(), max, &dropped);
		events.resize(n);
		{
			std::lock_guard<std::mutex> lock(trace_mutex_);
			trace_result_ = std::move(events);
			trace_dropped_ = dropped;
			trace_ready_ = true;
		}
		trace_cv_.notify_one();
		break;
	}
	}
}

void EmulatorHost::ApplyConfig(Config *new_config, Model new_model)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::ConfigUpdated;
	cmd.config_ptr = new_config;
	cmd.model = new_model;
	PostCommand(cmd);
}

void EmulatorHost::RequestExit()
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::Exit;
	PostCommand(cmd);
}

void EmulatorHost::LoadDisc(int drive, const std::string &disc_path)
{
	EmuCommand cmd;
	cmd.type = (drive == 0) ? EmuCommandType::LoadDisc0 : EmuCommandType::LoadDisc1;
	cmd.string_path = disc_path;
	PostCommand(cmd);
}

void EmulatorHost::EjectDisc(int drive)
{
	EmuCommand cmd;
	cmd.type = (drive == 0) ? EmuCommandType::EjectDisc0 : EmuCommandType::EjectDisc1;
	PostCommand(cmd);
}

void EmulatorHost::CdromDisabled()
{
	PostCommand(MakeCommand(EmuCommandType::CdromDisabled));
}

void EmulatorHost::CdromEmpty()
{
	PostCommand(MakeCommand(EmuCommandType::CdromEmpty));
}

void EmulatorHost::CdromLoadIso(const std::string &iso_path)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::CdromLoadIso;
	cmd.string_path = iso_path;
	PostCommand(cmd);
}

void EmulatorHost::CdromIoctl()
{
	PostCommand(MakeCommand(EmuCommandType::CdromIoctl));
}

void EmulatorHost::MouseHack()
{
	PostCommand(MakeCommand(EmuCommandType::MouseHack));
}

void EmulatorHost::MouseTwobutton()
{
	PostCommand(MakeCommand(EmuCommandType::MouseTwobutton));
}

void EmulatorHost::CpuIdle()
{
	PostCommand(MakeCommand(EmuCommandType::CpuIdle));
}

void EmulatorHost::IntegerScaling()
{
	PostCommand(MakeCommand(EmuCommandType::IntegerScaling));
}

void EmulatorHost::FitToWindow()
{
	PostCommand(MakeCommand(EmuCommandType::FitToWindow));
}

void EmulatorHost::FollowHostDisplay()
{
	PostCommand(MakeCommand(EmuCommandType::FollowHostDisplay));
}

void EmulatorHost::SetClipboardEnabled()
{
	PostCommand(MakeCommand(EmuCommandType::ClipboardEnabled));
}

void EmulatorHost::HostClipboardChanged(int file_type, const std::string &data)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::HostClipboardChanged;
	cmd.arg1 = file_type;
	cmd.string_path = data;	/* bytes, not necessarily text */
	PostCommand(cmd);
}

void EmulatorHost::SwitchMachine(const std::string &config_path)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::SwitchMachine;
	cmd.string_path = config_path;
	PostCommand(cmd);
}

void EmulatorHost::RunGuestCommand(const std::string &command, unsigned token)
{
	EmuCommand cmd;

	cmd.type = EmuCommandType::GuestCommand;
	cmd.string_path = command;
	cmd.token = token;
	PostCommand(cmd);
}

void EmulatorHost::AbandonGuestCommand()
{
	EmuCommand cmd;

	cmd.type = EmuCommandType::GuestCommandAbandon;
	PostCommand(cmd);
}

void EmulatorHost::ApplyUsbConfig()
{
	EmuCommand cmd;

	cmd.type = EmuCommandType::UsbApplyConfig;
	PostCommand(cmd);
}

void EmulatorHost::Restart()
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::Restart;
	PostCommand(cmd);
}

void EmulatorHost::NatRuleAdd(const PortForwardRule &rule)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::NatRuleAdd;
	cmd.nat_rule = rule;
	PostCommand(cmd);
}

void EmulatorHost::NatRuleEdit(const PortForwardRule &old_rule, const PortForwardRule &new_rule)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::NatRuleEdit;
	cmd.nat_rule_old = old_rule;
	cmd.nat_rule = new_rule;
	PostCommand(cmd);
}

void EmulatorHost::NatRuleRemove(const PortForwardRule &rule)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::NatRuleRemove;
	cmd.nat_rule = rule;
	PostCommand(cmd);
}

void EmulatorHost::ShowFullscreenMessageOff()
{
	PostCommand(MakeCommand(EmuCommandType::ShowFullscreenMessageOff));
}

void EmulatorHost::DebuggerPause()
{
	PostCommand(MakeCommand(EmuCommandType::DebuggerPause));
}

void EmulatorHost::DebuggerResume()
{
	PostCommand(MakeCommand(EmuCommandType::DebuggerResume));
}

void EmulatorHost::DebuggerStep()
{
	PostCommand(MakeCommand(EmuCommandType::DebuggerStep));
}

void EmulatorHost::DebuggerStepN(uint32_t instruction_count)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::DebuggerStepN;
	cmd.debug_count = instruction_count;
	PostCommand(cmd);
}

void EmulatorHost::DebuggerAddBreakpoint(uint32_t address)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::DebuggerAddBreakpoint;
	cmd.debug_address = address;
	PostCommand(cmd);
}

void EmulatorHost::DebuggerRemoveBreakpoint(uint32_t address)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::DebuggerRemoveBreakpoint;
	cmd.debug_address = address;
	PostCommand(cmd);
}

void EmulatorHost::DebuggerClearBreakpoints()
{
	PostCommand(MakeCommand(EmuCommandType::DebuggerClearBreakpoints));
}

void EmulatorHost::DebuggerAddWatchpoint(uint32_t address, uint32_t size, bool on_read, bool on_write, bool log_only)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::DebuggerAddWatchpoint;
	cmd.debug_address = address;
	cmd.debug_size = size;
	cmd.debug_on_read = on_read;
	cmd.debug_on_write = on_write;
	cmd.debug_log_only = log_only;
	PostCommand(cmd);
}

void EmulatorHost::DebuggerRemoveWatchpoint(uint32_t address, uint32_t size, bool on_read, bool on_write)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::DebuggerRemoveWatchpoint;
	cmd.debug_address = address;
	cmd.debug_size = size;
	cmd.debug_on_read = on_read;
	cmd.debug_on_write = on_write;
	PostCommand(cmd);
}

void EmulatorHost::DebuggerClearWatchpoints()
{
	PostCommand(MakeCommand(EmuCommandType::DebuggerClearWatchpoints));
}

void EmulatorHost::SetDebugTraceConfig(const DebugTraceConfig &cfg)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::SetDebugTraceConfig;
	cmd.debug_trace_config = cfg;
	PostCommand(cmd);
}

std::vector<DebugTraceEvent> EmulatorHost::DrainTraceEvents(uint32_t max, uint32_t *dropped_out)
{
	if (max == 0) {
		max = 256;
	}

	std::unique_lock<std::mutex> lock(trace_mutex_);
	trace_ready_ = false;

	EmuCommand cmd;
	cmd.type = EmuCommandType::DrainTraceEvents;
	cmd.debug_count = max;
	PostCommand(cmd);

	trace_cv_.wait(lock, [this]() { return trace_ready_ || g_fatal_occurred.load(); });

	if (dropped_out != nullptr) {
		*dropped_out = trace_dropped_;
	}
	return std::move(trace_result_);
}

MachineSnapshot EmulatorHost::TakeSnapshot()
{
	std::unique_lock<std::mutex> lock(snapshot_mutex_);
	snapshot_ready_ = false;

	PostCommand(MakeCommand(EmuCommandType::TakeSnapshot));

	snapshot_cv_.wait(lock, [this]() { return snapshot_ready_ || g_fatal_occurred.load(); });
	return snapshot_result_;
}

bool EmulatorHost::SaveState(const std::string &path)
{
	std::unique_lock<std::mutex> lock(state_mutex_);
	state_ready_ = false;

	EmuCommand command = MakeCommand(EmuCommandType::SaveState);
	command.string_path = path;
	PostCommand(command);

	state_cv_.wait(lock, [this]() { return state_ready_ || g_fatal_occurred.load(); });
	return state_ok_;
}

bool EmulatorHost::LoadState(const std::string &path, std::string *error_out)
{
	std::unique_lock<std::mutex> lock(state_mutex_);
	state_ready_ = false;

	EmuCommand command = MakeCommand(EmuCommandType::LoadState);
	command.string_path = path;
	PostCommand(command);

	state_cv_.wait(lock, [this]() { return state_ready_ || g_fatal_occurred.load(); });
	if (error_out != nullptr) {
		*error_out = state_error_;
	}
	return state_ok_;
}

size_t EmulatorHost::ReadMemory(uint32_t address, uint32_t length, uint8_t *buffer, size_t buffer_size)
{
	if (buffer == nullptr || buffer_size == 0) {
		return 0;
	}
	if (length > buffer_size) {
		length = static_cast<uint32_t>(buffer_size);
	}
	if (length > 4096) {
		length = 4096;
	}

	std::unique_lock<std::mutex> lock(memory_mutex_);
	memory_ready_ = false;

	EmuCommand cmd;
	cmd.type = EmuCommandType::ReadMemory;
	cmd.debug_address = address;
	cmd.debug_count = length;
	PostCommand(cmd);

	memory_cv_.wait(lock, [this]() { return memory_ready_ || g_fatal_occurred.load(); });

	const size_t to_copy = std::min(static_cast<size_t>(length), memory_result_.size());
	if (to_copy > 0) {
		memcpy(buffer, memory_result_.data(), to_copy);
	}
	return to_copy;
}

std::string EmulatorHost::DisassembleAt(uint32_t address, int count)
{
	std::unique_lock<std::mutex> lock(disasm_mutex_);
	disasm_ready_ = false;

	EmuCommand cmd;
	cmd.type = EmuCommandType::Disassemble;
	cmd.debug_address = address;
	cmd.arg1 = count;
	PostCommand(cmd);

	disasm_cv_.wait(lock, [this]() { return disasm_ready_ || g_fatal_occurred.load(); });
	return disasm_result_;
}

void EmulatorHost::NotifyDebuggerStateChanged()
{
	if (gui_bridge_ != nullptr) {
		gui_bridge_->PostDebuggerStateChanged();
	}
}

/**
 * Fire every IOMD and video tick that has fallen due, rather than one per call.
 *
 * A tick is owed every 2ms, and a frame every 16ms or so, but this is only
 * called as often as the host is willing to wake the emulator thread. That is
 * fine while the guest is busy, since the loop comes round in microseconds; it
 * is not fine while the guest is idling under "Reduce CPU Usage", where each
 * pass waits on a 1ms sleep that some hosts round up considerably.
 *
 * gentimerirq() corrects the two IOMD hardware timers from the absolute clock
 * itself, so those never drifted. The sound pacing and the podule timers inside
 * it advance a fixed step per call though, and each vblupdate() is one frame -
 * so serving a single tick per wake-up ran all three slow in exactly the case
 * where wake-ups are late (issues #26 and #36).
 *
 * The catch-up is bounded: after a long stall, such as a debugger pause or a
 * suspended laptop, there is nothing to be gained by delivering thousands of
 * backdated ticks, so what is left beyond the cap is dropped and the schedule
 * resynchronised to now.
 *
 * @param elapsed Nanoseconds on the emulator's monotonic timer
 */
void EmulatorHost::ServiceTimers(int64_t elapsed)
{
	/* Roughly 60ms of arrears each, which is enough to ride out a coarse host
	   timer without letting a real stall turn into a flood. */
	const int max_iomd_catch_up = 32;
	const int max_video_catch_up = 4;
	int n;

	for (n = 0; elapsed >= iomd_timer_next_ && n < max_iomd_catch_up; n++) {
		iomd_timer_count.fetch_add(1, std::memory_order_release);
		gentimerirq(elapsed);
		iomd_timer_next_ += kIomdTimerIntervalNs;
	}
	if (elapsed >= iomd_timer_next_) {
		iomd_timer_next_ = elapsed + kIomdTimerIntervalNs;
	}

	for (n = 0; elapsed >= video_timer_next_ && n < max_video_catch_up; n++) {
		video_timer_count.fetch_add(1, std::memory_order_release);
		vblupdate();
		video_timer_next_ += video_timer_interval_;
	}
	if (elapsed >= video_timer_next_) {
		video_timer_next_ = elapsed + video_timer_interval_;
	}
}

void EmulatorHost::IdleProcessEvents()
{
	ServiceTimers(GetElapsedTimerNs());
}

void EmulatorHost::MainEmuLoop()
{
	const int refresh_hz = config.refresh > 0 ? config.refresh : 60;
	video_timer_interval_ = 1000000000LL / refresh_hz;

	iomd_timer_next_ = kIomdTimerIntervalNs;
	video_timer_next_ = video_timer_interval_;

#ifdef __APPLE__
	/* macOS schedules by quality-of-service class and coalesces timer wake-ups
	   for threads that have not said they need to be prompt. A plain
	   std::thread carries no class of its own, and on Apple Silicon can be put
	   on the efficiency cores. This thread runs the guest, its timers and the
	   display, so user-interactive is what it is.

	   It matters most under "Reduce CPU Usage": rpcemu_idle() then paces the
	   whole machine off a 1ms sleep, and a sleep that returns late holds up the
	   vertical sync and with it the pointer (issue #36). The Windows build has
	   the same trouble for the same reason and answers it with
	   timeBeginPeriod(1) (issue #26). */
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

	unsigned network_nat_rate = 0;
	bool last_paused = debugger_is_paused() != 0;

	/* Nothing unless the build asked for it. Started here because the timer is
	   charged to this thread and delivered to it. */
	perfprof_start(10);

	while (!quited) {
		DrainCommands();

		const bool paused = debugger_is_paused() != 0;
		if (paused) {
			if (!last_paused) {
				NotifyDebuggerStateChanged();
				last_paused = true;
			}
			// Keep the debugger control socket serviced while the CPU is
			// paused (execrpcemu() isn't called here, so its debugcmd_poll()
			// doesn't run) — otherwise a client could never resume/inspect.
			debugcmd_poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (last_paused) {
			NotifyDebuggerStateChanged();
			last_paused = false;
		}

		// Run several instruction batches per loop to cut timer / mutex overhead.
		constexpr int kEmuBatch = 32;
		for (int batch = 0; batch < kEmuBatch && !quited; ++batch) {
			execrpcemu();

			if (debugger_is_paused()) {
				break;
			}

			if (inscount >= 0x10000) {
				instruction_count.fetch_add(static_cast<int>(inscount >> 16), std::memory_order_release);
				inscount &= 0xffff;
			}

			/* Inside the batch, not outside it. The one-in-four rate came
			   from the Qt front end, where the loop ran execrpcemu() once
			   per turn; batching 32 of them made the same test fire once
			   every 32 batches instead, and NAT throughput fell with it. */
			if (config.network_type == NetworkType_NAT) {
				network_nat_rate++;
				if ((network_nat_rate & 0x3u) == 0u) {
					network_nat_poll();
				}
			}
		}

		if (debugger_is_paused()) {
			if (!last_paused) {
				NotifyDebuggerStateChanged();
				last_paused = true;
			}
			continue;
		}

		ServiceTimers(GetElapsedTimerNs());
		perfprof_poll();
	}

	perfprof_stop();
	endrpcemu();
}
