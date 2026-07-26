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

#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/display.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include "data_paths.h"
#include "config_paths.h"
#include "config_selector_dialog.h"
#include "main_frame.h"
#include "headless_main.h"

extern "C" {
#include "rpcemu.h"
#include "savestate.h"
}

class RpcemuApp : public wxApp {
public:
	bool OnInit() override;
	void OnInitCmdLine(wxCmdLineParser &parser) override;
};

/*
 * RPCEmu uses long options ("--machine") on every platform, so that a command
 * line or a script works unchanged on Linux, macOS and Windows.
 *
 * wxWidgets otherwise accepts DOS-style "/switch" on Windows only, and answers
 * an unrecognised one such as "/H" with its own usage dialog listing wx's
 * options rather than RPCEmu's. Restricting the switch character to "-" turns
 * those into ordinary arguments and keeps the accepted option set identical
 * across platforms.
 */
void RpcemuApp::OnInitCmdLine(wxCmdLineParser &parser)
{
	wxApp::OnInitCmdLine(parser);
	parser.SetSwitchChars("-");
}

/*
 * Machine named by --machine, or null when the selector should be shown, plus
 * the state selected by --resume/--state (mutually exclusive, both requiring
 * --machine). Set by main() before wxEntry(), and only ever read on the GUI
 * thread in OnInit(). The pointers are into argv, which outlives the app.
 */
static const char *g_startup_machine = nullptr;
static bool g_startup_resume = false;
static const char *g_startup_state_file = nullptr;

#ifdef __WXMSW__
/*
 * Windows collects console output into one dialog rather than showing a box per
 * line. Held as plain globals: this is only ever touched from main(), before
 * any thread or wxApp exists.
 */
static std::string g_msg_text;
static bool g_msg_is_error = false;

static void ConsoleMessageAppend(bool is_error, const char *text)
{
	g_msg_text += text;
	if (is_error) {
		g_msg_is_error = true; /* any error makes the whole dialog an error */
	}
}

/*
 * Show whatever ConsoleMessage() accumulated, if anything. Safe to call more
 * than once. Uses the plain Win32 message box rather than wxMessageBox because
 * these paths run before wxEntry(), so no wxApp exists yet.
 */
static void ConsoleMessageFlush(void)
{
	if (g_msg_text.empty()) {
		return;
	}

	/* A modal dialog blocks until someone clicks OK, which is fatal to a script
	   or a CI job running without an interactive desktop. RPCEMU_NO_GUI_MESSAGES
	   forces the text back onto the standard streams: it still goes nowhere in a
	   plain double-click, but a caller that redirects them (a pipe or a file,
	   both of which give a GUI-subsystem process a valid handle) gets it. */
	const char *quiet = getenv("RPCEMU_NO_GUI_MESSAGES");

	if (quiet != nullptr && quiet[0] != '\0' && strcmp(quiet, "0") != 0) {
		fputs(g_msg_text.c_str(), g_msg_is_error ? stderr : stdout);
		fflush(g_msg_is_error ? stderr : stdout);
	} else {
		MessageBoxA(nullptr, g_msg_text.c_str(), "RPCEmu",
		            MB_OK | (g_msg_is_error ? MB_ICONERROR : MB_ICONINFORMATION));
	}

	g_msg_text.clear();
	g_msg_is_error = false;
}
#else
static void ConsoleMessageFlush(void) { }
#endif

/*
 * Report a console message.
 *
 * On Linux and macOS this writes to stdout/stderr exactly as any command-line
 * program does. On Windows the emulator is linked as a GUI-subsystem binary so
 * that double-clicking it never flashes up a console window - and such a binary
 * has no terminal to write to. Printing there is not merely invisible: the
 * shell does not wait for a GUI-subsystem child, so it has already redrawn its
 * prompt by the time any output could appear, which is why borrowing the
 * parent's console (AttachConsole) produced text welded onto the prompt.
 *
 * Windows therefore shows the same text in a message box instead. The option
 * set, the exit codes and the behaviour are identical on all three platforms;
 * only the presentation of these messages differs.
 *
 * Note this makes --list-machines a dialog rather than pipeable output on
 * Windows. That is the accepted trade-off for a single GUI-subsystem binary.
 */
static void ConsoleMessage(bool is_error, const char *fmt, ...)
{
	char buf[4096];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

#ifdef __WXMSW__
	/* Accumulate into one dialog: several of the callers below emit a message
	   in two or three parts, and a box per part would be unusable. The buffer
	   is flushed by ConsoleMessageFlush() before the process exits. */
	ConsoleMessageAppend(is_error, buf);
#else
	fputs(buf, is_error ? stderr : stdout);
#endif
}

/* Plain existence check, usable before wxWidgets is initialised. */
static bool FileIsReadable(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == nullptr) {
		return false;
	}
	fclose(f);
	return true;
}

/*
 * No wxIMPLEMENT_APP here: we provide our own main() so that headless mode can
 * run without ever constructing a wxApp (and therefore without gtk_init() and a
 * display connection). The GUI path is reached via wxEntry() below.
 */
wxIMPLEMENT_APP_NO_MAIN(RpcemuApp);

bool RpcemuApp::OnInit()
{
	if (!wxApp::OnInit()) {
		return false;
	}

	// Register image handlers (PNG etc.). wxGTK pulls these in implicitly, but
	// wxMSW does not; without this, PNG bitmaps (toolbar icons, app icon) fail
	// to load.
	wxInitAllImageHandlers();

	InitRpcemuPaths();

	wxString config_path;
	bool resume_requested = false;
	wxString state_file;

	if (g_startup_machine != nullptr) {
		// --machine: skip the selector and boot this machine directly. main()
		// has already checked the config exists, but the resolution there used
		// the non-wx path logic, so verify again through the wx resolver that
		// the rest of OnInit() uses.
		config_path = wxString::FromUTF8(g_startup_machine);
		if (!wxFileExists(ConfigPathsAbsoluteConfigPath(config_path))) {
			config_path = wxString::FromUTF8(g_startup_machine) + ".cfg";
		}
		if (!wxFileExists(ConfigPathsAbsoluteConfigPath(config_path))) {
			ConsoleMessage(true, "error: machine '%s' not found.\n",
			               g_startup_machine);
			ConsoleMessageFlush();
			return false;
		}
	} else {
		ConfigSelectorDialog selector(nullptr);
		if (selector.ShowModal() != wxID_OK) {
			return false;
		}

		config_path = selector.GetSelectedConfigPath();
		resume_requested = selector.ShouldResume();
		state_file = selector.GetStateFileToLoad();
	}
	// The machine's own snapshot is "consumed" (renamed to .bak) on resume;
	// a state file the user opened explicitly via Load State is left in place.
	const wxString own_snapshot = ConfigPathsSnapshotForConfig(
	    ConfigPathsAbsoluteConfigPath(config_path));

	// --resume / --state are the command-line equivalents of the selector's
	// Resume and Load State buttons, and feed the same machinery below:
	// --resume targets this machine's own snapshot (so it is consumed to .bak),
	// --state targets an explicit file (so it is left in place). Without either,
	// --machine does a plain boot and any snapshot is left untouched.
	if (g_startup_machine != nullptr) {
		if (g_startup_resume) {
			if (!wxFileExists(own_snapshot)) {
				ConsoleMessage(true,
				               "error: machine '%s' has no saved state to resume.\n",
				               g_startup_machine);
				ConsoleMessageFlush();
				return false;
			}
			resume_requested = true;
			state_file = own_snapshot;
		} else if (g_startup_state_file != nullptr) {
			resume_requested = true;
			state_file = wxString::FromUTF8(g_startup_state_file);
		}
	}
	config_set_path(ConfigPathsAbsoluteConfigPath(config_path).utf8_str().data());
	rpcemu_prestart();

	auto *frame = new MainFrame();
	frame->Show(true);
	SetTopWindow(frame);

	// Tell the core the size of the display the window opened on, before
	// rpcemu_start() loads the ROMs: the synthesised monitor EDID is patched in
	// during loadroms(), and without this it falls back to a fixed 1920x1080 and
	// the guest's "Auto" monitor detection learns the wrong native mode.
	//
	// The monitor's full geometry is used rather than its client area, because
	// that is what an EDID's preferred timing means - the display's native mode,
	// not the space left over beside a taskbar. rom_patch.c clamps it.
	{
		// Show() is asynchronous on some platforms, so the frame may not be
		// mapped yet and GetFromWindow() can answer wxNOT_FOUND. Fall back to
		// the primary display rather than constructing an invalid wxDisplay.
		int index = wxDisplay::GetFromWindow(frame);

		if (index == wxNOT_FOUND) {
			index = 0;
		}

		const wxDisplay display((unsigned) index);
		const wxRect geom = display.GetGeometry();

		if (geom.width > 0 && geom.height > 0) {
			const wxVideoMode mode = display.GetCurrentMode();

			rpcemu_set_host_display((unsigned) geom.width, (unsigned) geom.height,
			                        mode.refresh > 0 ? (unsigned) mode.refresh : 0);
			rpclog("main: host display %dx%d (display %d of %u)\n",
			       geom.width, geom.height, index, wxDisplay::GetCount());
		}
	}

	rpcemu_start();

	// If the user chose Resume in the machine selector, load this machine's
	// snapshot. This runs before the emulator thread starts, so state_load()
	// operates single-threaded. The snapshot is renamed to .bak on success so
	// it is "consumed" (the session is now live) yet recoverable; a Restart /
	// Start leaves the snapshot untouched.
	if (resume_requested) {
		const std::string state_utf8 = state_file.utf8_str().data();
		char errbuf[256];

		if (state_check(state_utf8.c_str(), errbuf, sizeof(errbuf)) == 0 &&
		    state_load(state_utf8.c_str()) == 0) {
			/* Only the machine's own snapshot is consumed to .bak; a file
			   opened explicitly via Load State is left where it is. */
			if (state_file == own_snapshot) {
				const wxString bak = own_snapshot + ".bak";
				if (wxFileExists(bak)) {
					wxRemoveFile(bak);
				}
				wxRenameFile(own_snapshot, bak);
			}
		} else {
			rpclog("main: could not load state '%s': %s\n",
			       state_utf8.c_str(), errbuf);
			wxMessageBox(
			    wxString::Format("Could not load the machine state:\n%s\n\n"
			                     "Performing a normal boot instead.",
			                     wxString::FromUTF8(errbuf)),
			    "RPCEmu", wxOK | wxICON_WARNING, frame);
		}
	}

	// Start the emulator (which spawns the CPU and VIDC worker threads) only
	// once the GUI event loop is actually running. If it starts here - before
	// OnInit returns and the loop begins pumping - the VIDC thread can post its
	// first frame while no event loop is servicing CallAfter. PostVideoUpdate()
	// then blocks that thread (holding video_mutex) on a CallAfter that cannot
	// run, deadlocking startup. Deferring via CallAfter guarantees the loop is
	// live first. (Symptom: window opens with menus but no toolbar/display and
	// "Not Responding"; timing-dependent, seen on Windows.)
	frame->CallAfter([frame]() { frame->StartEmulator(); });
	return true;
}

int main(int argc, char **argv)
{
	bool headless = false;
	bool list_machines = false;
	bool show_help = false;
	bool resume = false;
	const char *machine_name = nullptr;
	const char *state_file = nullptr;

	/* wxApp::OnInit() runs wxWidgets' own command-line parser over argv and
	   rejects any option it does not know, reporting it with wx's error and
	   usage dialogs - which list wx's options, not RPCEmu's. So this loop owns
	   the option set completely: it consumes what it knows and rejects the rest
	   itself, and wx only ever sees argv[0] plus non-option arguments. That
	   keeps the accepted options and the error text identical on all platforms.

	   Since every argument is either consumed or rejected, wx is handed just
	   argv[0]. */

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--headless") == 0) {
			headless = true;
		} else if (strcmp(arg, "--list-machines") == 0) {
			list_machines = true;
		} else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			show_help = true;
		} else if (strcmp(arg, "--resume") == 0) {
			resume = true;
		} else if (strcmp(arg, "--machine") == 0) {
			if (i + 1 < argc) {
				machine_name = argv[++i];
			} else {
				ConsoleMessage(true, "error: --machine requires a machine name.\n");
				ConsoleMessageFlush();
				return 2;
			}
		} else if (strncmp(arg, "--machine=", 10) == 0) {
			machine_name = arg + 10;
		} else if (strcmp(arg, "--state") == 0) {
			if (i + 1 < argc) {
				state_file = argv[++i];
			} else {
				ConsoleMessage(true, "error: --state requires a state file path.\n");
				ConsoleMessageFlush();
				return 2;
			}
		} else if (strncmp(arg, "--state=", 8) == 0) {
			state_file = arg + 8;
		} else if ((arg[0] == '-'
#ifdef __WXMSW__
		            /* On Windows "/H" is a switch attempt, not a path, so report
		               it rather than passing it through silently. Elsewhere a
		               leading slash is an ordinary absolute path. */
		            || arg[0] == '/'
#endif
		            ) && arg[1] != '\0') {
			/* An option we do not know. Reject it here rather than letting wx
			   answer with its own error and usage dialogs, which describe wx's
			   options rather than RPCEmu's. */
			ConsoleMessage(true, "error: unknown option '%s'.\n", arg);
			ConsoleMessage(true, "       Use --help to see the available options.\n");
			ConsoleMessageFlush();
			return 2;
		} else {
			/* RPCEmu takes no positional arguments: every option above is a
			   flag, and a machine is named with --machine. A stray argument is
			   therefore a mistake - most often a mistyped option, or a path
			   that a shell rewrote. Reject it rather than ignoring it and
			   launching the GUI, which on a machine with no display just
			   hangs. */
			ConsoleMessage(true, "error: unexpected argument '%s'.\n", arg);
			ConsoleMessage(true, "       Use --help to see the available options.\n");
			ConsoleMessageFlush();
			return 2;
		}
	}
	/* wx sees only the program name: NULL-terminated, as argv must be. */
	char *wx_argv[] = { argv[0], nullptr };
	int wx_argc = 1;

	/* These paths run entirely without wxWidgets, so they never initialise GTK
	   and work on a system with no display present. They are handled before the
	   validation below so that --help still prints usage alongside any options.

	   On Windows they are routed into a message box, since a GUI-subsystem
	   binary has no console; elsewhere they print to stdout as usual. Headless
	   mode deliberately keeps its console output either way. */
	if (show_help || list_machines) {
#ifdef __WXMSW__
		if (!headless) {
			HeadlessSetOutputSink([](const char *text) {
				ConsoleMessageAppend(false, text);
			});
		}
#endif
		int rc = 0;

		if (show_help) {
			HeadlessPrintUsage(argv[0]);
		} else {
			rc = HeadlessListMachines();
		}

		ConsoleMessageFlush();
		return rc;
	}

	/* --resume and --state name two different snapshots; honouring both is
	   ambiguous, so reject it rather than silently picking one. */
	if (resume && state_file != nullptr) {
		ConsoleMessage(true, "error: --resume and --state are mutually exclusive.\n");
		ConsoleMessageFlush();
		return 2;
	}
	/* Both select a state for a specific machine, so both need one naming it. */
	if ((resume || state_file != nullptr) && machine_name == nullptr) {
		ConsoleMessage(true, "error: %s requires --machine <name>.\n",
		               resume ? "--resume" : "--state");
		ConsoleMessageFlush();
		return 2;
	}

	if (headless) {
		return RunHeadless(machine_name, resume, state_file);
	}

	/* --machine without --headless: start the GUI directly on that machine,
	   skipping the selector. Resolve it here, before wxEntry() constructs a
	   wxApp, so a bad name is reported on the console and exits cleanly rather
	   than reaching config_set_path()/rpcemu_prestart() with a path that does
	   not exist. Uses the non-wx resolver; OnInit() re-resolves through the
	   wx path logic once the toolkit is up. */
	if (machine_name != nullptr) {
		if (!HeadlessInitPaths()) {
			HeadlessPrintNoDataError();
			return 2;
		}
		if (HeadlessResolveMachineConfig(machine_name).empty()) {
			ConsoleMessage(true, "error: machine '%s' not found in %sconfigs\n",
			               machine_name, rpcemu_get_datadir());
			ConsoleMessage(true, "       Use --list-machines to see available machines.\n");
			ConsoleMessageFlush();
			return 2;
		}
		if (state_file != nullptr && !FileIsReadable(state_file)) {
			ConsoleMessage(true, "error: state file '%s' does not exist.\n",
			               state_file);
			ConsoleMessageFlush();
			return 2;
		}
		g_startup_machine = machine_name;
		g_startup_resume = resume;
		g_startup_state_file = state_file;
	}

	/* Normal graphical launch, with the options consumed above removed. */
	return wxEntry(wx_argc, wx_argv);
}
