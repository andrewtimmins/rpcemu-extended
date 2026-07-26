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

#include "headless_main.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "headless_bridge.h"
#include "emulator_host.h"
#ifdef RPCEMU_VNC
#include "vnc_server.h"
#endif

extern "C" {
#include "rpcemu.h"
#include "savestate.h"
}

namespace {

/*
 * Set from a signal handler to request an orderly shutdown. The handler only
 * touches a sig_atomic_t (async-signal-safe); the actual teardown, which is not
 * async-safe, runs back on the main thread once it observes the flag.
 */
volatile sig_atomic_t g_headless_stop = 0;

void HeadlessSignalHandler(int /*signum*/)
{
	g_headless_stop = 1;
}

bool DirExists(const std::string &path)
{
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FileExists(const std::string &path)
{
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string WithSep(std::string dir)
{
	if (!dir.empty() && dir.back() != '/') {
		dir += '/';
	}
	return dir;
}

std::string ExeDir()
{
#ifdef _WIN32
	char buf[MAX_PATH];
	const DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf)) {
		return {};
	}
	const std::string path(buf, n);
	const size_t slash = path.find_last_of("/\\");
#else
	char buf[PATH_MAX];
	const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) {
		return {};
	}
	buf[n] = '\0';
	const std::string path(buf);
	const size_t slash = path.find_last_of('/');
#endif
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string CwdDir()
{
	char buf[PATH_MAX];
	return getcwd(buf, sizeof(buf)) != nullptr ? std::string(buf) : std::string();
}

bool HasConfigs(const std::string &dir)
{
	return DirExists(WithSep(dir) + "configs");
}

/* Writable per-user data folder, matching the GUI (~/RPCEmu). */
std::string HomeRpcemu()
{
	const char *home = getenv("HOME");
	if (home == nullptr || home[0] == '\0') {
		return {};
	}
	return WithSep(home) + "RPCEmu";
}

/*
 * Resolve the data and resource directories and hand them to the core. Mirrors
 * the precedence used by the GUI's wx-based resolver (env -> exe dir -> cwd ->
 * install prefix -> /usr/share), but with no wxWidgets dependency. Returns
 * false if no directory containing a configs/ subdirectory can be found.
 */
bool InitHeadlessPaths()
{
	const char *env_data = getenv("RPCEMU_DATADIR");
	const char *env_res = getenv("RPCEMU_RESOURCE_DIR");

	const std::string exe = ExeDir();
	const std::string cwd = CwdDir();
	const std::string install = RPCEMU_INSTALL_DATADIR;

	/* Shared, read-only resources (ROM/config/podule seed data). */
	std::string resourcedir;
	if (env_res != nullptr && env_res[0] != '\0') {
		resourcedir = env_res;
	} else if (HasConfigs(exe)) {
		resourcedir = exe;
	} else if (HasConfigs(cwd)) {
		resourcedir = cwd;
	} else if (HasConfigs(install)) {
		resourcedir = install;
	} else if (DirExists("/usr/share/rpcemu/configs")) {
		resourcedir = "/usr/share/rpcemu";
	} else {
		return false;
	}

	/* Writable per-user data. Portable/dev builds keep everything beside the
	   binary (configs found in exe/cwd); an installed build uses ~/RPCEmu, which
	   the GUI seeds on first run (or override with RPCEMU_DATADIR). */
	std::string datadir;
	if (env_data != nullptr && env_data[0] != '\0') {
		datadir = env_data;
	} else if (HasConfigs(exe)) {
		datadir = exe;
	} else if (HasConfigs(cwd)) {
		datadir = cwd;
	} else {
		datadir = HomeRpcemu();
		if (datadir.empty()) {
			datadir = resourcedir;
		}
	}

	/* The core appends a trailing separator itself, so pass the paths as-is. */
	rpcemu_set_datadir(datadir.c_str());
	rpcemu_set_resourcedir(resourcedir.c_str());
	return true;
}

void PrintNoDataError()
{
	fprintf(stderr, "error: could not locate RPCEmu data (no 'configs' directory found).\n");
	fprintf(stderr, "       Run from a directory containing 'configs/', or point\n");
	fprintf(stderr, "       RPCEMU_DATADIR at your data directory.\n");
}

/* Resolve a machine name to a config path, or empty if it does not exist. */
std::string ResolveMachineConfig(const char *name)
{
	std::string leaf = name;
	const bool has_suffix = leaf.size() >= 4 &&
	                        strcasecmp(leaf.c_str() + leaf.size() - 4, ".cfg") == 0;
	if (!has_suffix) {
		leaf += ".cfg";
	}

	std::string path;
	if (!leaf.empty() && leaf[0] == '/') {
		path = leaf; /* absolute path given */
	} else {
		path = std::string(rpcemu_get_datadir()) + "configs/" + leaf;
	}

	return FileExists(path) ? path : std::string();
}

} // namespace

/* Public wrappers: the GUI path needs these to validate --machine before
   wxEntry(), but the implementations above stay internal to this file. */
std::string HeadlessResolveMachineConfig(const char *machine_name)
{
	if (machine_name == nullptr || machine_name[0] == '\0') {
		return std::string();
	}
	return ResolveMachineConfig(machine_name);
}

bool HeadlessInitPaths(void)
{
	return InitHeadlessPaths();
}

void HeadlessPrintNoDataError(void)
{
	PrintNoDataError();
}

/* Output sink for the two GUI-reachable listings; null means print to stdout. */
static void (*g_output_sink)(const char *text) = nullptr;

void HeadlessSetOutputSink(void (*sink)(const char *text))
{
	g_output_sink = sink;
}

/* printf() for the two functions below, honouring the sink when one is set. */
static void HeadlessOutput(const char *fmt, ...)
{
	char buf[8192];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (g_output_sink != nullptr) {
		g_output_sink(buf);
	} else {
		fputs(buf, stdout);
	}
}

void HeadlessPrintUsage(const char *argv0)
{
	const char *name = (argv0 != nullptr && argv0[0] != '\0') ? argv0 : "rpcemu";
	HeadlessOutput(
	    "Usage: %s [options]\n"
	    "\n"
	    "With no options the graphical machine selector is shown.\n"
	    "\n"
	    "Options:\n"
	    "  --machine <name>      Machine to run (config name in the configs dir,\n"
	    "                        with or without the .cfg suffix). On its own it\n"
	    "                        starts the GUI directly on that machine, skipping\n"
	    "                        the machine selector. Required by --headless.\n"
	    "  --resume              Resume the machine's saved state, as the selector's\n"
	    "                        Resume does. The snapshot is consumed (kept as\n"
	    "                        .bak). Requires --machine.\n"
	    "  --state <file>        Load an explicit state file, as Load State does.\n"
	    "                        The file is left in place. Requires --machine, and\n"
	    "                        cannot be combined with --resume.\n"
	    "  --headless            Run a machine without the GUI window; access it\n"
	    "                        over the built-in VNC server. Requires --machine\n"
	    "                        and a machine config with VNC enabled. Needs no\n"
	    "                        display or desktop session on any platform.\n"
	    "  --list-machines       List available machine configs and exit.\n"
	    "  -h, --help            Show this help and exit.\n"
	    "\n"
	    "Only long options are accepted, and there are no positional arguments, so an\n"
	    "unrecognised option or a stray argument is reported rather than ignored.\n"
	    "Exit status is 0 on success and 2 for a usage error.\n"
#ifdef _WIN32
	    "\n"
	    "This is a GUI application, so it has no console to write to: these messages\n"
	    "are shown in a message box. Set RPCEMU_NO_GUI_MESSAGES=1 to send them to\n"
	    "stdout/stderr instead, and redirect the output, when scripting.\n"
#endif
	    "\n"
	    "Data is located via $RPCEMU_DATADIR, else the executable directory or the\n"
	    "current directory if it contains a 'configs/' folder, else the install prefix.\n",
	    name);
}

int HeadlessListMachines(void)
{
	if (!InitHeadlessPaths()) {
		PrintNoDataError();
		return 2;
	}

	const std::string configs = std::string(rpcemu_get_datadir()) + "configs";

	DIR *dir = opendir(configs.c_str());
	if (dir == nullptr) {
		HeadlessOutput("No machines found in %s\n", configs.c_str());
		return 0;
	}

	std::vector<std::string> names;
	for (struct dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
		const std::string n = entry->d_name;
		if (n.size() > 4 && n.compare(n.size() - 4, 4, ".cfg") == 0) {
			names.push_back(n.substr(0, n.size() - 4));
		}
	}
	closedir(dir);

	if (names.empty()) {
		HeadlessOutput("No machines found in %s\n", configs.c_str());
		return 0;
	}

	std::sort(names.begin(), names.end());
	HeadlessOutput("Available machines (in %s):\n", configs.c_str());
	for (const std::string &n : names) {
		HeadlessOutput("  %s\n", n.c_str());
	}
	return 0;
}

int RunHeadless(const char *machine_name, bool resume, const char *state_file)
{
#ifndef RPCEMU_VNC
	(void)machine_name;
	(void)resume;
	(void)state_file;
	fprintf(stderr,
	        "error: this build was compiled without VNC support, so --headless\n"
	        "       has no way to expose the machine. Rebuild with RPCEMU_ENABLE_VNC=ON.\n");
	return 1;
#else
	if (machine_name == nullptr || machine_name[0] == '\0') {
		fprintf(stderr,
		        "error: --headless requires --machine <name> (there is no\n"
		        "       interactive selector in headless mode).\n"
		        "       Use --list-machines to see available machines.\n");
		return 2;
	}

	if (!InitHeadlessPaths()) {
		PrintNoDataError();
		return 2;
	}

	const std::string config_path = ResolveMachineConfig(machine_name);
	if (config_path.empty()) {
		fprintf(stderr, "error: machine '%s' not found in %sconfigs\n", machine_name,
		        rpcemu_get_datadir());
		fprintf(stderr, "       Use --list-machines to see available machines.\n");
		return 2;
	}

	config_set_path(config_path.c_str());
	rpcemu_prestart(); /* loads the selected config into the global `config` */

	/* Resolve the state to load, if any. config_load() has just pointed the
	   machine data dir at this machine, so the machine's own snapshot sits
	   beside its cmos.ram - the same file the GUI selector's Resume offers. */
	std::string state_to_load;
	bool consume_snapshot = false;
	const std::string own_snapshot =
	    std::string(rpcemu_get_machine_datadir()) + "suspend.state";

	if (resume) {
		if (!FileExists(own_snapshot)) {
			fprintf(stderr, "error: machine '%s' has no saved state to resume.\n",
			        config.name);
			return 2;
		}
		state_to_load = own_snapshot;
		consume_snapshot = true; /* the session is now live; keep a .bak */
	} else if (state_file != nullptr && state_file[0] != '\0') {
		if (!FileExists(state_file)) {
			fprintf(stderr, "error: state file '%s' does not exist.\n", state_file);
			return 2;
		}
		/* An explicitly named file is left in place, matching Load State. */
		state_to_load = state_file;
	}

	/* The whole point of headless mode is VNC access, so refuse to start a
	   machine that has no way to be reached. */
	if (!config.vnc_enabled) {
		fprintf(stderr, "error: machine '%s' has the VNC server disabled.\n", config.name);
		fprintf(stderr,
		        "       Headless mode is only reachable over VNC. Enable the VNC server\n"
		        "       in this machine's configuration (vnc_enabled=1) and try again.\n");
		return 1;
	}

	HeadlessBridge bridge;
	auto emulator = std::make_unique<EmulatorHost>(&bridge);

	auto vnc = std::make_unique<VncServer>(emulator.get());
	g_vnc_server = vnc.get();
	if (!vnc->start(config.vnc_port, std::string(config.vnc_password))) {
		fprintf(stderr, "error: failed to start the VNC server on port %d.\n", config.vnc_port);
		g_vnc_server = nullptr;
		return 1;
	}

	printf("RPCEmu headless: machine '%s' running.\n", config.name);
	printf("VNC server listening on port %d%s.\n", config.vnc_port,
	       config.vnc_password[0] == '\0' ? " (no password set)" : "");
	printf("Press Ctrl-C to shut down.\n");
	fflush(stdout);

	/* Handle Ctrl-C / SIGTERM so CMOS, disc images and config are saved on exit. */
	std::signal(SIGINT, HeadlessSignalHandler);
	std::signal(SIGTERM, HeadlessSignalHandler);

	rpcemu_start();

	/* Load the requested state before the emulator thread starts, so state_load()
	   runs single-threaded (as it does on the GUI path). A failure here is not
	   fatal: report it and continue with the normal boot already set up. */
	if (!state_to_load.empty()) {
		char errbuf[256];

		if (state_check(state_to_load.c_str(), errbuf, sizeof(errbuf)) == 0 &&
		    state_load(state_to_load.c_str()) == 0) {
			printf("Loaded machine state '%s'.\n", state_to_load.c_str());
			if (consume_snapshot) {
				/* Consume the snapshot to .bak: recoverable, but not
				   re-resumed on next launch. Mirrors the GUI. */
				const std::string bak = own_snapshot + ".bak";
				remove(bak.c_str());
				if (rename(own_snapshot.c_str(), bak.c_str()) != 0) {
					rpclog("headless: could not rename '%s' to .bak\n",
					       own_snapshot.c_str());
				}
			}
		} else {
			fprintf(stderr,
			        "warning: could not load the machine state '%s': %s\n"
			        "         Performing a normal boot instead.\n",
			        state_to_load.c_str(), errbuf);
		}
	}

	emulator->Start();

	/* Park the main thread until a signal arrives or the guest powers off
	   (which sets `quited`). The blocking teardown runs here, off the handler. */
	while (g_headless_stop == 0 && quited == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	printf("\nRPCEmu headless: shutting down...\n");
	fflush(stdout);

	emulator->RequestExit();
	emulator->Stop();
	emulator->Join(); /* MainEmuLoop runs endrpcemu(): saves CMOS/discs/config */

	vnc->stop();
	g_vnc_server = nullptr;
	emulator.reset();

	return 0;
#endif
}
