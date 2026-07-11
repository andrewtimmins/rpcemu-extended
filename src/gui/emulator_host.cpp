#include "emulator_host.h"

#include "emulator_snapshot.h"

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <pthread.h>

extern "C" {
#include "arm.h"
#include "arm_disasm.h"
#include "cdrom-iso.h"
#include "cmos.h"
#include "debugcmd.h"
#include "hostfs.h"
#include "ide.h"
#include "keyboard.h"
#include "mem.h"
#include "network.h"
#include "network-nat.h"
#include "podulerom.h"
#include "romload.h"
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

	if (g_emulator_host != nullptr) {
		g_emulator_host->PostVideoFlyback();
	}

#ifdef RPCEMU_VNC
	if (g_vnc_server && g_vnc_server->isRunning()) {
		g_vnc_server->updateFramebuffer(buffer, xsize, ysize, yl, yh);
	}
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

extern "C" void rpcemu_send_nat_rule_to_gui(PortForwardRule rule)
{
	if (g_emulator_host != nullptr) {
		g_emulator_host->StoreNatRuleForGui(rule);
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
	if (emu_thread_.joinable()) {
		emu_thread_.join();
	}
}

void EmulatorHost::PostCommand(EmuCommand command)
{
	std::lock_guard<std::mutex> lock(command_mutex_);
	commands_.push(std::move(command));
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
	case EmuCommandType::Exit:
		quited = 1;
		break;
	case EmuCommandType::VideoFlyback:
		VideoFlyback();
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
	case EmuCommandType::CpuIdle:
		config.cpu_idle ^= 1;
		config_save(&config);
		resetrpc();
		break;
	case EmuCommandType::IntegerScaling:
		config.integer_scaling ^= 1;
		config_save(&config);
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

void EmulatorHost::SwitchMachine(const std::string &config_path)
{
	EmuCommand cmd;
	cmd.type = EmuCommandType::SwitchMachine;
	cmd.string_path = config_path;
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

	trace_cv_.wait(lock, [this]() { return trace_ready_; });

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

	snapshot_cv_.wait(lock, [this]() { return snapshot_ready_; });
	return snapshot_result_;
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

	memory_cv_.wait(lock, [this]() { return memory_ready_; });

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

	disasm_cv_.wait(lock, [this]() { return disasm_ready_; });
	return disasm_result_;
}

void EmulatorHost::StoreNatRuleForGui(PortForwardRule rule)
{
	{
		std::lock_guard<std::mutex> lock(nat_rules_mutex_);
		pending_nat_rules_.push_back(rule);
	}

	if (gui_bridge_ != nullptr) {
		gui_bridge_->PostNatRule(rule);
	}
}

std::vector<PortForwardRule> EmulatorHost::TakePendingNatRules()
{
	std::lock_guard<std::mutex> lock(nat_rules_mutex_);
	std::vector<PortForwardRule> rules(pending_nat_rules_.begin(), pending_nat_rules_.end());
	pending_nat_rules_.clear();
	return rules;
}

void EmulatorHost::NotifyDebuggerStateChanged()
{
	if (gui_bridge_ != nullptr) {
		gui_bridge_->PostDebuggerStateChanged();
	}
}

void EmulatorHost::PostVideoFlyback()
{
	if (flyback_pending_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}

	EmuCommand cmd;
	cmd.type = EmuCommandType::VideoFlyback;
	PostCommand(cmd);
}

void EmulatorHost::VideoFlyback()
{
	flyback_pending_.store(false, std::memory_order_release);
	iomd_flyback(1);
}

void EmulatorHost::IdleProcessEvents()
{
	const int32_t iomd_timer_interval = 2000000;
	const int64_t elapsed = GetElapsedTimerNs();

	if (elapsed >= iomd_timer_next_) {
		iomd_timer_count.fetch_add(1, std::memory_order_release);
		gentimerirq(elapsed);
		iomd_timer_next_ += iomd_timer_interval;
	}

	if (elapsed >= video_timer_next_) {
		video_timer_count.fetch_add(1, std::memory_order_release);
		vblupdate();
		video_timer_next_ += video_timer_interval_;
	}
}

void EmulatorHost::MainEmuLoop()
{
	const int32_t iomd_timer_interval = 2000000;
	const int refresh_hz = config.refresh > 0 ? config.refresh : 60;
	video_timer_interval_ = 1000000000LL / refresh_hz;

	iomd_timer_next_ = iomd_timer_interval;
	video_timer_next_ = video_timer_interval_;

	unsigned network_nat_rate = 0;
	bool last_paused = debugger_is_paused() != 0;

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
		}

		if (debugger_is_paused()) {
			if (!last_paused) {
				NotifyDebuggerStateChanged();
				last_paused = true;
			}
			continue;
		}

		const int64_t elapsed = GetElapsedTimerNs();

		if (elapsed >= iomd_timer_next_) {
			iomd_timer_count.fetch_add(1, std::memory_order_release);
			gentimerirq(elapsed);
			iomd_timer_next_ += iomd_timer_interval;
		}

		if (elapsed >= video_timer_next_) {
			video_timer_count.fetch_add(1, std::memory_order_release);
			vblupdate();
			video_timer_next_ += video_timer_interval_;
		}

		if (config.network_type == NetworkType_NAT) {
			network_nat_rate++;
			if ((network_nat_rate & 0x3u) == 0u) {
				network_nat_poll();
			}
		}
	}

	endrpcemu();
}
