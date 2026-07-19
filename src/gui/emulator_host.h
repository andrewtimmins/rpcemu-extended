#ifndef EMULATOR_HOST_H
#define EMULATOR_HOST_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "gui_bridge.h"
#include "machine_snapshot.h"

extern "C" {
#include "rpcemu.h"
}

enum class EmuCommandType {
	KeyPress,
	KeyRelease,
	MouseMove,
	MouseMoveRelative,
	MousePress,
	MouseRelease,
	MouseWheel,
	Reset,
	Exit,
	VideoFlyback,
	ConfigUpdated,
	LoadDisc0,
	LoadDisc1,
	EjectDisc0,
	EjectDisc1,
	CdromDisabled,
	CdromEmpty,
	CdromLoadIso,
	CdromIoctl,
	MouseHack,
	MouseTwobutton,
	CpuIdle,
	IntegerScaling,
	ReloadIdeImages,
	SwitchMachine,
	NatRuleAdd,
	NatRuleEdit,
	NatRuleRemove,
	ShowFullscreenMessageOff,
	DebuggerPause,
	DebuggerResume,
	DebuggerStep,
	DebuggerStepN,
	DebuggerAddBreakpoint,
	DebuggerRemoveBreakpoint,
	DebuggerClearBreakpoints,
	DebuggerAddWatchpoint,
	DebuggerRemoveWatchpoint,
	DebuggerClearWatchpoints,
	TakeSnapshot,
	ReadMemory,
	Disassemble,
	SetDebugTraceConfig,
	DrainTraceEvents,
	SaveState,
	LoadState,
};

struct EmuCommand {
	EmuCommandType type = EmuCommandType::Exit;
	unsigned scan_code = 0;
	int arg1 = 0;
	int arg2 = 0;
	Config *config_ptr = nullptr;
	Model model = Model_RPCARM710;
	std::string string_path;
	char drive_letter = 0;
	PortForwardRule nat_rule{};
	PortForwardRule nat_rule_old{};
	uint32_t debug_address = 0;
	uint32_t debug_count = 0;
	uint32_t debug_size = 0;
	bool debug_on_read = false;
	bool debug_on_write = false;
	bool debug_log_only = false;
	DebugTraceConfig debug_trace_config{};
};

class EmulatorHost {
public:
	explicit EmulatorHost(GuiBridge *gui_bridge);
	~EmulatorHost();

	void Start();
	void Stop();
	void Join();

	bool IsRunning() const { return emu_thread_.joinable(); }

	void PostCommand(EmuCommand command);

	void KeyPress(unsigned scan_code);
	void KeyRelease(unsigned scan_code);
	void MouseMove(int x, int y);
	void MouseMoveRelative(int dx, int dy);
	void MousePress(int buttons);
	void MouseRelease(int buttons);
	void MouseWheel(int dy);
	void Reset();
	void ReloadIdeImages();
	void RequestExit();
	void ApplyConfig(Config *new_config, Model new_model);

	void LoadDisc(int drive, const std::string &disc_path);
	void EjectDisc(int drive);
	void CdromDisabled();
	void CdromEmpty();
	void CdromLoadIso(const std::string &iso_path);
	void CdromIoctl();
	void MouseHack();
	void MouseTwobutton();
	void CpuIdle();
	void IntegerScaling();
	void SwitchMachine(const std::string &config_path);
	void NatRuleAdd(const PortForwardRule &rule);
	void NatRuleEdit(const PortForwardRule &old_rule, const PortForwardRule &new_rule);
	void NatRuleRemove(const PortForwardRule &rule);
	void ShowFullscreenMessageOff();
	void DebuggerPause();
	void DebuggerResume();
	void DebuggerStep();
	void DebuggerStepN(uint32_t instruction_count);
	void DebuggerAddBreakpoint(uint32_t address);
	void DebuggerRemoveBreakpoint(uint32_t address);
	void DebuggerClearBreakpoints();
	void DebuggerAddWatchpoint(uint32_t address, uint32_t size, bool on_read, bool on_write, bool log_only);
	void DebuggerRemoveWatchpoint(uint32_t address, uint32_t size, bool on_read, bool on_write);
	void DebuggerClearWatchpoints();
	void SetDebugTraceConfig(const DebugTraceConfig &cfg);
	std::vector<DebugTraceEvent> DrainTraceEvents(uint32_t max, uint32_t *dropped_out);
	MachineSnapshot TakeSnapshot();
	size_t ReadMemory(uint32_t address, uint32_t length, uint8_t *buffer, size_t buffer_size);
	std::string DisassembleAt(uint32_t address, int count);

	/* Machine save-state (suspend/resume). Both block until the emulator
	   thread has completed the operation at an instruction boundary. */
	bool SaveState(const std::string &path);
	bool LoadState(const std::string &path, std::string *error_out);

	void StoreNatRuleForGui(PortForwardRule rule);
	std::vector<PortForwardRule> TakePendingNatRules();

	void IdleProcessEvents();
	int64_t GetElapsedTimerNs() const;

	void PostVideoFlyback();

private:
	void MainEmuLoop();
	void DrainCommands();
	void HandleCommand(const EmuCommand &command);
	void VideoFlyback();
	void NotifyDebuggerStateChanged();

	GuiBridge *gui_bridge_;
	std::thread emu_thread_;
	std::mutex command_mutex_;
	std::queue<EmuCommand> commands_;
	std::chrono::steady_clock::time_point start_time_;
	int64_t iomd_timer_next_ = 0;
	int64_t video_timer_next_ = 0;
	int64_t video_timer_interval_ = 0;

	std::mutex snapshot_mutex_;
	std::condition_variable snapshot_cv_;
	bool snapshot_ready_ = false;
	MachineSnapshot snapshot_result_{};

	std::mutex state_mutex_;
	std::condition_variable state_cv_;
	bool state_ready_ = false;
	bool state_ok_ = false;
	std::string state_error_;

	std::mutex memory_mutex_;
	std::condition_variable memory_cv_;
	bool memory_ready_ = false;
	std::vector<uint8_t> memory_result_{};

	std::mutex disasm_mutex_;
	std::condition_variable disasm_cv_;
	bool disasm_ready_ = false;
	std::string disasm_result_{};

	std::mutex trace_mutex_;
	std::condition_variable trace_cv_;
	bool trace_ready_ = false;
	std::vector<DebugTraceEvent> trace_result_{};
	uint32_t trace_dropped_ = 0;

	std::mutex nat_rules_mutex_;
	std::deque<PortForwardRule> pending_nat_rules_;

	std::atomic<bool> flyback_pending_{false};
};

extern std::atomic<int> instruction_count;
extern std::atomic<int> iomd_timer_count;
extern std::atomic<int> video_timer_count;
extern std::atomic<int> hostfs_activity;
extern std::atomic<int> network_activity;
extern std::atomic<int> ide_activity;
extern std::atomic<int> fdc_activity;

extern int mouse_captured;
extern Config *pconfig_copy;

EmulatorHost *emulator_host_instance();

#endif
