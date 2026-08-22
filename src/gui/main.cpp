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
#include <wx/evtloop.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef __WXMSW__
#include <winsock2.h>	/* before windows.h, or the old winsock.h clashes */
#include <windows.h>
#endif

#include "data_paths.h"
#include "config_paths.h"
#include "data_dir_dialog.h"
#include "gui_preferences.h"
#include "manager_frame.h"
#include "gui_preferences.h"
#include "main_frame.h"
#include "headless_main.h"
#include "http_transfer.h"	/* RPCEMU_HAVE_HTTP */
#include "package_index.h"
#include "package_sources.h"
#include "package_install.h"
#include "riscos_fetch.h"

extern "C" {
#include "openbus_coproc.h"
#include "openbus_stub.h"
#include "rpcemu.h"
#include "savestate.h"
#include "app_settings.h"
#include "machine_lock.h"
}

class RpcemuApp : public wxApp {
public:
	bool OnInit() override;
	void OnInitCmdLine(wxCmdLineParser &parser) override;

	/*
	 * wxAppConsoleBase::ExitMainLoop() only acts while the main loop is the
	 * active one, and IsRunning() means exactly that - GetActive() == this. A
	 * request arriving from anywhere else is dropped without a word, which is
	 * how closing the machine window left the application running.
	 *
	 * ScheduleExit() is wx's answer for that case: it marks the loop to finish
	 * without needing it to be the active one. It does require the loop to have
	 * been started, so fall back to the base version when there is none.
	 */
	void ExitMainLoop() override
	{
		wxEventLoopBase *loop = GetMainLoop();

		if (loop != nullptr && !loop->IsRunning()) {
			loop->ScheduleExit(0);
			return;
		}
		wxApp::ExitMainLoop();
	}

private:
	void InstallSignalHandlers();
};

#ifndef __WXMSW__
/*
 * Ctrl-C, or a "please stop" from the system.
 *
 * Started from a terminal, the window is only half the story: the shell can
 * still interrupt the process, and until now that killed it outright, losing
 * the CMOS and any disc images written since the machine started. The same
 * goes for the SIGTERM sent at logout or shutdown.
 *
 * Not a signal handler in the usual sense, despite the name. wxWidgets catches
 * the signal itself, notes it, and calls this from the event loop afterwards,
 * so it runs on the main thread with nothing to avoid - it can take locks,
 * allocate and wait for the emulator thread, none of which a real handler
 * could do.
 */
void HandleQuitSignal(int signum)
{
	auto *frame = dynamic_cast<MainFrame *>(wxTheApp->GetTopWindow());

	/* Worth a line in the log: an exit nobody asked for looks the same as a
	   crash afterwards, and which signal arrived says whether somebody pressed
	   Ctrl-C or the machine was being shut down around it. */
	rpclog("RPCEmu: %s received, shutting down\n",
	       signum == SIGINT ? "SIGINT" : "SIGTERM");

	if (frame != nullptr) {
		frame->CloseForSignal();
		return;
	}

	/* No machine window yet, so this is the selector or one of the dialogues
	   that can precede it. Those run their own nested event loop, which
	   ExitMainLoop() does not reach - the signal would be swallowed and the
	   emulator would appear to ignore it. End the dialogue first, which
	   unwinds that loop and lets the startup path fall through to its "no
	   machine chosen" exit. */
	/* The iterator is tested directly rather than against nullptr. wxWidgets
	   defines wxWindowList::compatibility_iterator two different ways: with
	   wxUSE_STL it is a class offering operator bool() and no comparison with
	   nullptr_t, and without it a wrapper that converts to a node pointer. Only
	   the second form compiles against nullptr, so a build with the STL
	   containers enabled - which is what Homebrew's wx gives on macOS - failed
	   here with "invalid operands to binary expression". Truth-testing works in
	   both. Reported by Septercius, issue #30. */
	for (auto node = wxTopLevelWindows.GetFirst(); node;
	     node = node->GetNext()) {
		auto *dialog = dynamic_cast<wxDialog *>(node->GetData());

		if (dialog != nullptr && dialog->IsModal()) {
			dialog->EndModal(wxID_CANCEL);
		}
	}

	wxTheApp->ExitMainLoop();
}

/*
 * SIGUSR1: reset the machine, as the Reset item on the File menu does.
 *
 * A way to restart a guest from a script or another terminal without going
 * near the window.
 *
 * Unlike the two signals above this does not end the process, so a machine
 * that has not been started yet - the selector is still open, or the window is
 * already closing - simply has nothing to reset, and the signal is noted and
 * ignored rather than being allowed to kill the emulator by its default
 * action.
 */
void HandleResetSignal(int /*signum*/)
{
	auto *frame = dynamic_cast<MainFrame *>(wxTheApp->GetTopWindow());

	if (frame != nullptr) {
		frame->ResetForSignal();
	} else {
		/* Selector still open, so there is no machine and nothing to do - said
		   through the same helper so the log reads the same either way. */
		EmulatorResetForSignal(nullptr);
	}
}
#endif

/*
 * Answer the signals a console can send, on the platforms that have them.
 *
 * Windows has no equivalent of these two, and wxWidgets does not offer
 * SetSignalHandler() there, so it keeps the behaviour it always had.
 */
void RpcemuApp::InstallSignalHandlers()
{
#ifndef __WXMSW__
	if (!SetSignalHandler(SIGINT, HandleQuitSignal) ||
	    !SetSignalHandler(SIGTERM, HandleQuitSignal) ||
	    !SetSignalHandler(SIGUSR1, HandleResetSignal)) {
		rpclog("main: could not install the interrupt handlers\n");
	}
#endif
}

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

/*
 * --managed: this process is a machine started by the Manager window (see
 * manager_frame.h), not a standalone launch. It boots exactly like --machine
 * (indeed it requires --machine, to say which one), except the window is
 * never shown - the Manager displays it instead, via the shared-memory
 * framebuffer and control channel MainFrame::EnableManagedMode() sets up.
 */
static bool g_startup_managed = false;

/*
 * --fetch-riscos and its modifiers. Unlike the options above, this one is not
 * handled before wxEntry(): the transfer needs wxWidgets and an event loop. It
 * runs under RiscosFetchApp below rather than the GUI application.
 */
static bool g_fetch_riscos = false;
static bool g_fetch_nightly = false;
static bool g_fetch_disc = true;
static bool g_fetch_accept_licence = false;
/*
 * --datadir: where machines, ROMs and settings live, for this run only. Outranks
 * both RPCEMU_DATADIR and whatever was chosen on a first run, and is deliberately
 * NOT remembered, so a scripted run with an unusual directory cannot quietly
 * become the default for every later interactive one. See data_dir_choice.h.
 */
static const char *g_datadir = nullptr;

/** --datadir as a wxString, empty when not given. */
static wxString DataDirFromCommandLine()
{
	return g_datadir != nullptr ? wxString::FromUTF8(g_datadir) : wxString();
}

static bool g_pkg_list = false;
static bool g_pkg_sources = false;
static const char *g_pkg_filter = NULL;
static const char *g_pkg_info = NULL;
static const char *g_pkg_install = NULL;
static const char *g_pkg_remove = NULL;
static const char *g_pkg_machine = NULL;

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
		MessageBoxA(nullptr, g_msg_text.c_str(), "RPCEmu Extended",
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
	/* stdout is buffered and stderr is not, so a message written to one can
	   otherwise appear before text already written to the other. Several
	   callers here mix the two in one report, where the order is the meaning. */
	if (is_error) {
		fflush(stdout);
	}
	fputs(buf, is_error ? stderr : stdout);
	if (is_error) {
		fflush(stderr);
	}
#endif
}

/*
 * Progress for --fetch-riscos, on the console rather than in a dialog.
 *
 * Percentages are only reported as they change, and only every ten points, so
 * that a redirected log does not fill up with them. There is no way to cancel:
 * Ctrl-C is the answer, and it leaves nothing behind because the fetch stages
 * everything before moving it into place.
 */
class CommandLineFetchReporter : public RiscosFetchReporter {
public:
	bool Stage(const wxString &description) override
	{
		ConsoleMessage(false, "%s\n",
		               static_cast<const char *>(description.utf8_str()));
		last_decile_ = -1;
		return true;
	}

	bool Progress(long long done, long long total) override
	{
		if (total > 0) {
			const int decile = static_cast<int>((done * 10) / total);

			if (decile != last_decile_) {
				last_decile_ = decile;
				ConsoleMessage(false, "  %d%%\n", decile * 10);
			}
		}
		return true;
	}

private:
	int last_decile_ = -1;
};

/*
 * The application object for --pkg-list.
 *
 * A wxAppConsole for the same reason as the fetch below: this needs wxWidgets
 * for the transfer and its event loop but has no window, and it exists partly
 * so the catalogue can be read in a test or a container.
 */
class PackageListApp : public wxAppConsole {
public:
	bool OnInit() override { return true; }

	int OnRun() override
	{
		std::vector<PackageRecord> packages;
		CommandLineFetchReporter reporter;
		PackageIndexResult result;

		/* No prompt argument: a console entry point must never raise a
		   dialogue, whatever else is true. --datadir still applies. */
		InitRpcemuPaths(DataDirFromCommandLine());

		/* Listing the sources touches no network, so it happens before the
		   catalogue is fetched rather than after: somebody checking why a
		   repository is not showing up should not have to wait for the
		   repositories to be read to find out it is turned off. */
		if (g_pkg_sources) {
			std::vector<wxString> warnings;
			const std::vector<PackageSource> sources =
			    PackageSourcesLoad(&warnings);

			ConsoleMessage(false, "%s\n\n",
			    static_cast<const char *>(PackageSourcesPath().utf8_str()));

			for (const auto &source : sources) {
				ConsoleMessage(false, "%-14s %-4s %s\n",
				    static_cast<const char *>(source.name.utf8_str()),
				    source.enabled ? "on" : "off",
				    static_cast<const char *>(source.url.utf8_str()));
				if (!source.description.empty()) {
					ConsoleMessage(false, "%-19s %s\n", "",
					    static_cast<const char *>(
					        source.description.utf8_str()));
				}
			}

			for (const auto &warning : warnings) {
				ConsoleMessage(true, "warning: %s\n",
				    static_cast<const char *>(warning.utf8_str()));
			}

			ConsoleMessageFlush();
			return 0;
		}

		result = PackageIndexRefresh(packages, reporter,
		    [this]() { return CreateMainLoop(); });

		if (!result.ok) {
			ConsoleMessage(true, "error: %s\n",
			               static_cast<const char *>(result.message.utf8_str()));
			ConsoleMessageFlush();
			return 1;
		}

		/* Install or remove, against a named machine's disc. */
		if (g_pkg_install != NULL || g_pkg_remove != NULL) {
			const wxString machine = wxString::FromUTF8(g_pkg_machine);
			const wxString hostfs = ConfigPathsMachinesDir() +
			    wxFileName::GetPathSeparator() + machine +
			    wxFileName::GetPathSeparator() + "hostfs";

			if (!wxDirExists(hostfs)) {
				ConsoleMessage(true, "error: machine '%s' has no hard disc at "
				               "%s\n", g_pkg_machine,
				               static_cast<const char *>(hostfs.utf8_str()));
				ConsoleMessageFlush();
				return 1;
			}

			if (g_pkg_remove != NULL) {
				const PackageActionResult r = PackageRemove(
				    wxString::FromUTF8(g_pkg_remove), hostfs);

				ConsoleMessage(r.ok ? false : true, "%s\n",
				    static_cast<const char *>(r.ok
				        ? wxString::Format("Removed %s (%d file(s)).",
				              g_pkg_remove, r.files).utf8_str()
				        : r.message.utf8_str()));
				ConsoleMessageFlush();
				return r.ok ? 0 : 1;
			}

			{
				const wxString wanted =
				    wxString::FromUTF8(g_pkg_install).Lower();
				bool adffs_noted = false;
				const PackageRecord *found = nullptr;

				for (const auto &pkg : packages) {
					if (pkg.name.Lower() == wanted) {
						found = &pkg;
						break;
					}
				}
				if (found == nullptr) {
					ConsoleMessage(true, "error: no package called '%s'.\n",
					               g_pkg_install);
					ConsoleMessageFlush();
					return 1;
				}
				if (!found->RunsHere()) {
					ConsoleMessage(true, "error: %s is built for another kind "
					               "of ARM (Environment: %s).\n",
					    g_pkg_install,
					    static_cast<const char *>(found->environment.utf8_str()));
					ConsoleMessageFlush();
					return 1;
				}

				PackageInstalledMap installed = PackageInstalledList(hostfs);
				std::vector<wxString> missing;
				std::vector<PackageRecord> deps = PackageResolveDepends(
				    *found, packages, installed, missing);

				for (const auto &name : missing) {
					ConsoleMessage(false, "warning: %s needs %s, which is not "
					               "in the catalogue\n", g_pkg_install,
					    static_cast<const char *>(name.utf8_str()));
				}

				deps.push_back(*found);
				for (const auto &pkg : deps) {
					const PackageActionResult r = PackageInstall(pkg, hostfs,
					    reporter, [this]() { return CreateMainLoop(); });

					if (!r.ok) {
						ConsoleMessage(true, "error: %s\n",
						    static_cast<const char *>(r.message.utf8_str()));
						ConsoleMessageFlush();
						return 1;
					}
					ConsoleMessage(false, "Installed %s %s (%d file(s)).\n",
					    static_cast<const char *>(pkg.name.utf8_str()),
					    static_cast<const char *>(pkg.version.utf8_str()),
					    r.files);

					/* No machine is running here, so its graphics card
					   setting cannot be read - say it unconditionally rather
					   than not at all, and only once however many packages
					   the dependency resolution pulled in. */
					if (PackageNeedsAdffs(pkg) && !adffs_noted) {
						adffs_noted = true;
						ConsoleMessage(false,
						    "  note: ADFFS writes straight to screen memory, "
						    "which the graphics card moves into its own "
						    "framestore. Set gfxcard_enabled=0 for this machine "
						    "or games will abort as soon as they draw.\n");
					}
				}
			}

			ConsoleMessageFlush();
			return 0;
		}

		/* One package in full, which is also how the resolved download URL
		   can be seen: the indexes do not all give absolute ones. */
		if (g_pkg_info != NULL) {
			const wxString wanted = wxString::FromUTF8(g_pkg_info).Lower();

			for (const auto &pkg : packages) {
				if (pkg.name.Lower() != wanted) {
					continue;
				}
				for (const auto &field : pkg.fields) {
					ConsoleMessage(false, "%-16s %s\n",
					    static_cast<const char *>(field.first.utf8_str()),
					    static_cast<const char *>(field.second.utf8_str()));
				}
				ConsoleMessage(false, "%-16s %s\n", "(download)",
				    static_cast<const char *>(pkg.url.utf8_str()));
				ConsoleMessage(false, "%-16s %s\n", "(runs here)",
				    pkg.RunsHere() ? "yes" : "no, built for another ARM");
				ConsoleMessageFlush();
				return 0;
			}
			ConsoleMessage(true, "error: no package called '%s'.\n", g_pkg_info);
			ConsoleMessageFlush();
			return 1;
		}

		{
			const wxString filter = (g_pkg_filter != NULL)
			    ? wxString::FromUTF8(g_pkg_filter).Lower() : wxString();
			int shown = 0, unsuitable = 0;

			ConsoleMessage(false, "%d package(s) from %d source(s)\n\n",
			               result.packages, result.sources_read);

			for (const auto &pkg : packages) {
				if (!pkg.RunsHere()) {
					unsuitable++;
					continue;
				}
				/* Matches the licence as well, so "non free" narrows the
				   list to what a user might want to think twice about -
				   the same search the window offers. */
				if (!filter.empty() &&
				    !pkg.name.Lower().Contains(filter) &&
				    !pkg.section.Lower().Contains(filter) &&
				    !pkg.licence.Lower().Contains(filter) &&
				    !pkg.description.Lower().Contains(filter)) {
					continue;
				}

				/* A record stating no licence says so, rather than leaving a
				   gap that reads as "nothing to worry about". */
				ConsoleMessage(false, "%-24s %-14s %-16s %-16s %s\n",
				    static_cast<const char *>(pkg.name.utf8_str()),
				    static_cast<const char *>(pkg.version.utf8_str()),
				    static_cast<const char *>(pkg.section.utf8_str()),
				    static_cast<const char *>(pkg.licence.empty()
				        ? wxString("not stated").utf8_str()
				        : pkg.licence.utf8_str()),
				    static_cast<const char *>(pkg.description.utf8_str()));
				shown++;
			}

			ConsoleMessage(false, "\n%d shown", shown);
			if (unsuitable > 0) {
				ConsoleMessage(false, ", %d skipped as built for another "
				               "kind of ARM", unsuitable);
			}
			ConsoleMessage(false, "\n");
		}

		ConsoleMessageFlush();
		return 0;
	}
};

/*
 * The application object for --fetch-riscos.
 *
 * Deliberately a wxAppConsole and not a wxApp: the fetch needs wxWidgets for
 * the transfer and its event loop, but it has no window, and a wxApp would
 * connect to the display and fail on a machine that has none. That would rule
 * out exactly the uses this option exists for - scripted installs, containers
 * and automated testing.
 */
class RiscosFetchApp : public wxAppConsole {
public:
	bool OnInit() override { return true; }

	int OnRun() override
	{
		RiscosFetchRequest request;
		CommandLineFetchReporter reporter;
		RiscosFetchOutcome outcome;

		/* As above: no prompt, because there is no GUI here. */
		InitRpcemuPaths(DataDirFromCommandLine());

		/* The graphical routes put this in a dialogue with an Agree button.
		   A script cannot click one, so the acknowledgement is the option:
		   printed either way, and required before anything is fetched. */
		ConsoleMessage(false, "%s\n\n",
		               static_cast<const char *>(
		                   RiscosFetchLicenceText().utf8_str()));
		if (!g_fetch_accept_licence) {
			ConsoleMessage(true,
			               "error: pass --accept-licence to confirm you accept "
			               "the terms above, and the download will proceed.\n");
			ConsoleMessageFlush();
			return 1;
		}

		request.release = g_fetch_nightly ? RiscosRelease::Nightly
		                                  : RiscosRelease::Stable;
		request.include_disc = g_fetch_disc;
		request.create_machine = true;

		/* CreateMainLoop() is the application's own choice of event
		   loop, and is only reachable from a subclass, which is why
		   the fetch takes it as a parameter. Here it yields the
		   console loop, with no display connection. */
		outcome = RiscosFetchPerform(request, reporter,
		                             [this]() { return CreateMainLoop(); });

		if (outcome.ok) {
			ConsoleMessage(false, "RISC OS %s installed as '%s'.\n",
			               static_cast<const char *>(outcome.version.utf8_str()),
			               static_cast<const char *>(outcome.rom_name.utf8_str()));
			if (!outcome.machine_name.empty()) {
				ConsoleMessage(false, "Machine '%s' created",
				               static_cast<const char *>(outcome.machine_name.utf8_str()));
				if (outcome.disc_files > 0) {
					ConsoleMessage(false, " with %d files on its hard disc",
					               outcome.disc_files);
				}
				ConsoleMessage(false, ".\n");
			}
		} else if (outcome.cancelled) {
			ConsoleMessage(true, "error: cancelled.\n");
		} else {
			ConsoleMessage(true, "error: %s\n",
			               static_cast<const char *>(outcome.message.utf8_str()));
		}

		ConsoleMessageFlush();
		return outcome.ok ? 0 : 1;
	}
};

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
	SetAppName("RPCEmu Extended");

	if (!wxApp::OnInit()) {
		return false;
	}

	/* Before the selector rather than after the window, so the signals are
	   answered for the whole life of the process. SIGUSR1 in particular kills
	   by default, and choosing a machine is no reason to be killed. */
	InstallSignalHandlers();

	// Register image handlers (PNG etc.). wxGTK pulls these in implicitly, but
	// wxMSW does not; without this, PNG bitmaps (toolbar icons, app icon) fail
	// to load.
	wxInitAllImageHandlers();

	/*
	 * The only call that passes a prompt, so the first-run question can only
	 * ever appear here - in a real GUI application with an event loop. Whether
	 * it actually appears is data_dir_decide()'s business, and it says no for
	 * an existing user, a portable layout, an explicit --datadir or variable,
	 * and anything headless.
	 */
	InitRpcemuPaths(DataDirFromCommandLine(),
	    [](const wxString &suggested, wxString *chosen) {
		    return AskForDataDir(nullptr, suggested, chosen);
	    });

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
		/* A machine can be marked as the one to open without asking. Three
		   things override that, so it can never be a trap:
		     - the configuration file having gone, in which case there is
		       nothing to open and the selector is the only sensible answer;
		     - holding Shift while starting, which is the way back to the
		       selector when the default machine itself is the problem;
		     - --machine, handled above.
		   It can also be turned off from inside the machine, through
		   Settings > Open This Machine Automatically. */
		const wxString default_machine =
		    wxString::FromUTF8(GetDefaultMachine());
		bool used_default = false;

		if (!default_machine.empty() && !wxGetKeyState(WXK_SHIFT)) {
			const wxString candidate = default_machine + ".cfg";

			if (wxFileExists(ConfigPathsAbsoluteConfigPath(candidate))) {
				config_path = candidate;
				used_default = true;
			} else {
				ClearDefaultMachine();
			}
		}

		if (!used_default) {
			/*
			 * The Manager window is the ordinary entry point: it lists every
			 * configured machine and can start any number of them at once,
			 * each as its own "--managed" process (see manager_frame.h and
			 * MainFrame::EnableManagedMode()), switching the single display
			 * area between whichever ones are running - the way VirtualBox
			 * and VMware Workstation's own manager windows do.
			 *
			 * Unlike --machine or the default-machine path just above, this
			 * process's job stops here: it does not go on to load a config or
			 * run an emulator itself, only to start/stop/display others that
			 * do. A scripted or remote launch that wants exactly one machine
			 * and nothing else still has --machine for that.
			 */
#ifdef __WXMSW__
			/* This process stops short of rpcemu_prestart(), which is what
			   starts Winsock, and it still opens a socket to every machine it
			   shows. Without this those connections failed with
			   WSANOTINITIALISED and no machine ever appeared. */
			{
				WSADATA wsadata;

				if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
					wxMessageBox("Could not start Windows Sockets.",
					    "RPCEmu Extended Manager", wxOK | wxICON_ERROR);
					return false;
				}
			}
#endif
			auto *manager = new ManagerFrame();
			manager->Show(true);
			SetTopWindow(manager);
			return true;
		}
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

	/*
	 * One emulator per machine. Two write the same cmos.ram and configuration on
	 * the way out, the later exit quietly discarding the earlier one's changes, and
	 * interleave sector writes into the same hard disc image. It has always been
	 * possible to do by hand; it becomes easy to do by accident once machines are
	 * started from a list, and the damage does not show until ADFS complains later.
	 */
	if (!machine_lock_acquire(rpcemu_get_machine_datadir(), 0)) {
		long pid = 0;
		int port = 0;
		wxString detail = wxString::Format(
		    "The machine '%s' is already running in another window or process.",
		    wxString::FromUTF8(config.name));

		if (machine_lock_read_owner(rpcemu_get_machine_datadir(), &pid, &port)) {
			detail += wxString::Format("\n\nIt is process %ld", pid);
			if (port > 0) {
				detail += wxString::Format(", reachable over VNC on port %d", port);
			}
			detail += ".";
		}
		detail += "\n\nRunning it twice would corrupt its hard disc and lose its "
		          "settings, so this copy will not start.";

		/* The console message goes first, deliberately. A modal box waits for a
		   click, and a machine started from a script or from a manager may have
		   nobody to click it, so the reason has to reach stderr whether or not
		   anyone is watching the screen. */
		ConsoleMessage(true, "error: machine '%s' is already running.\n",
		               config.name);
		ConsoleMessageFlush();
		wxMessageBox(detail, "Machine already running", wxOK | wxICON_ERROR);
		return false;
	}

	auto *frame = new MainFrame();
	if (g_startup_managed) {
		frame->EnableManagedMode();
	} else {
		frame->Show(true);
	}
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
			    "RPCEmu Extended", wxOK | wxICON_WARNING, frame);
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
	bool managed = false;
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
		} else if (strcmp(arg, "--fetch-riscos") == 0) {
			g_fetch_riscos = true;
		} else if (strncmp(arg, "--fetch-riscos=", 15) == 0) {
			const char *which = arg + 15;

			g_fetch_riscos = true;
			if (strcmp(which, "nightly") == 0) {
				g_fetch_nightly = true;
			} else if (strcmp(which, "stable") != 0) {
				ConsoleMessage(true, "error: --fetch-riscos takes "
				               "'stable' or 'nightly', not '%s'.\n", which);
				ConsoleMessageFlush();
				return 2;
			}
		} else if (strncmp(arg, "--pkg-install=", 14) == 0) {
			g_pkg_list = true;
			g_pkg_install = arg + 14;
		} else if (strncmp(arg, "--pkg-remove=", 13) == 0) {
			g_pkg_list = true;
			g_pkg_remove = arg + 13;
		} else if (strncmp(arg, "--pkg-machine=", 14) == 0) {
			g_pkg_machine = arg + 14;
		} else if (strncmp(arg, "--pkg-info=", 11) == 0) {
			g_pkg_list = true;
			g_pkg_info = arg + 11;
		} else if (strcmp(arg, "--pkg-sources") == 0) {
			g_pkg_list = true;
			g_pkg_sources = true;
		} else if (strcmp(arg, "--pkg-list") == 0) {
			g_pkg_list = true;
		} else if (strncmp(arg, "--pkg-list=", 11) == 0) {
			g_pkg_list = true;
			g_pkg_filter = arg + 11;
		} else if (strcmp(arg, "--datadir") == 0 || strncmp(arg, "--datadir=", 10) == 0) {
			const char *value = (arg[9] == '=') ? arg + 10
			                                    : (i + 1 < argc ? argv[++i] : nullptr);

			if (value == nullptr || value[0] == '\0') {
				ConsoleMessage(true, "error: --datadir needs a directory.\n");
				ConsoleMessageFlush();
				return 2;
			}
			g_datadir = value;
		} else if (strcmp(arg, "--openbus-stub") == 0) {
			/* Fits the OPEN Bus test card. Not a model of real hardware: it
			   exists so the second processor bus has something in it before
			   there is a processor to put on it. Note the debugger cannot read
			   the card - its accessor covers only ROM, VRAM and RAM, being
			   side-effect-free by design - so the decode is proved by
			   tests/test_openbus_decode.c instead. This is also the only
			   reference to openbus_stub.o, which would otherwise sit unused in
			   the static library. */
			openbus_stub_request();
		} else if (strcmp(arg, "--openbus-card") == 0 ||
		           strncmp(arg, "--openbus-card=", 15) == 0) {
			/* Fits a co-processor card to the second processor slot, with
			   the named core in it. This OVERRIDES the machine's own
			   Co-Processor Card setting for one run and is not written
			   back, which is the relationship every other --option here
			   has with the setting it shadows. The list of cores lives in
			   openbus_coproc.c, so the parser, the help text and the error
			   below all name the same set. */
			const char *value = (arg[14] == '=') ? arg + 15
			                                     : (i + 1 < argc ? argv[++i] : nullptr);

			if (value == nullptr) {
				ConsoleMessage(true, "error: --openbus-card requires a core name.\n");
				ConsoleMessageFlush();
				return 2;
			}
			if (openbus_coproc_request(value) != 0) {
				ConsoleMessage(true, "error: unknown co-processor core '%s'.\n",
				               value);
				/* Walked until the names run out rather than up to a
				   named last core, so adding one to the table is
				   enough and this cannot list a stale set. */
				ConsoleMessage(true, "Available cores:");
				for (int core = 0; ; core++) {
					const char *name = openbus_coproc_core_name(
					    (openbus_coproc_core) core);

					if (name == nullptr) {
						break;
					}
					ConsoleMessage(true, " %s", name);
				}
				ConsoleMessage(true, "\n");
				ConsoleMessageFlush();
				return 2;
			}
		} else if (strcmp(arg, "--no-disc") == 0) {
			g_fetch_disc = false;
		} else if (strcmp(arg, "--accept-licence") == 0 ||
		           strcmp(arg, "--accept-license") == 0) {
			g_fetch_accept_licence = true;
		} else if (strcmp(arg, "--vnc-port") == 0 || strncmp(arg, "--vnc-port=", 11) == 0) {
			const char *value = (arg[10] == '=') ? arg + 11
			                                     : (i + 1 < argc ? argv[++i] : nullptr);
			int port = value ? atoi(value) : 0;

			if (port <= 0 || port > 65535) {
				ConsoleMessage(true, "error: --vnc-port needs a port between 1 and 65535.\n");
				ConsoleMessageFlush();
				return 2;
			}
			app_settings_override_vnc_port(port);
		} else if (strcmp(arg, "--no-vnc") == 0) {
			app_settings_override_vnc_enabled(0);
		} else if (strcmp(arg, "--hostcmd-socket") == 0 ||
		           strncmp(arg, "--hostcmd-socket=", 17) == 0) {
			const char *value = (arg[16] == '=') ? arg + 17
			                                     : (i + 1 < argc ? argv[++i] : nullptr);

			if (value == nullptr) {
				ConsoleMessage(true, "error: --hostcmd-socket requires a path or port.\n");
				ConsoleMessageFlush();
				return 2;
			}
			app_settings_override_hostcmd_socket(value);
		} else if (strcmp(arg, "--debug-socket") == 0 ||
		           strncmp(arg, "--debug-socket=", 15) == 0) {
			const char *value = (arg[14] == '=') ? arg + 15
			                                     : (i + 1 < argc ? argv[++i] : nullptr);

			if (value == nullptr) {
				ConsoleMessage(true, "error: --debug-socket requires a path or port.\n");
				ConsoleMessageFlush();
				return 2;
			}
			app_settings_override_debug_socket(value);
		} else if (strcmp(arg, "--pyromaniac-net") == 0 ||
		           strncmp(arg, "--pyromaniac-net=", 17) == 0) {
			/* Charles Ferguson's JSON tun/tap server, which RISC OS
			   Pyromaniac speaks too - see docs/pyromaniac-networking.md.
			   Applies to the GUI and to --headless alike, this parsing
			   being shared. */
			const char *value = (arg[16] == '=') ? arg + 17
			                                     : (i + 1 < argc ? argv[++i] : nullptr);

			if (value == nullptr) {
				ConsoleMessage(true, "error: --pyromaniac-net requires host, host:port or off.\n");
				ConsoleMessageFlush();
				return 2;
			}
			app_settings_override_json_net(value);
		} else if (strcmp(arg, "--no-relay") == 0) {
			app_settings_override_relay(0);
		} else if (strcmp(arg, "--no-gl") == 0) {
			/* The escape hatch for a display where OpenGL works well enough to
			   start and badly enough to be a nuisance - which nothing here can
			   detect, so it has to be sayable. */
			SetHardwareAccelerationOverride(0);
		} else if (strcmp(arg, "--managed") == 0) {
			/* Internal: used by the Manager window to launch a machine it
			   will display and control itself. Not documented as a
			   user-facing option (HeadlessPrintUsage does not mention it),
			   the way no ordinary user needs to type --datadir's
			   RPCEMU_DATADIR equivalent by hand either. */
			managed = true;
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

	/* Before any path resolution, so that every entry point below - listing
	   machines, headless, the package tools, the GUI - honours --datadir. The
	   pointer is into argv, which outlives all of them. */
	HeadlessSetDataDir(g_datadir);

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
	/* --managed has no meaning without a machine to run, and headless mode
	   answers a different display path entirely (VNC only, no window and
	   hence nothing for a Manager to show). */
	if (managed && machine_name == nullptr) {
		ConsoleMessage(true, "error: --managed requires --machine <name>.\n");
		ConsoleMessageFlush();
		return 2;
	}
	if (managed && headless) {
		ConsoleMessage(true, "error: --managed and --headless are mutually exclusive.\n");
		ConsoleMessageFlush();
		return 2;
	}

	/*
	 * Every option below this point downloads something, and a build whose
	 * wxWidgets has no wxWebRequest cannot. Refused here, once, rather than
	 * each of them reaching the network layer and reporting the same thing in
	 * its own words.
	 */
	if (!RPCEMU_HAVE_HTTP &&
	    (g_fetch_riscos || g_pkg_list || g_pkg_sources || g_pkg_info != NULL ||
	     g_pkg_install != NULL || g_pkg_remove != NULL)) {
		ConsoleMessage(true,
		    "error: this build of RPCEmu cannot download anything, so the "
		    "package and RISC OS options are unavailable.\n"
		    "The wxWidgets it was built against has no wxWebRequest support. "
		    "Everything else works normally; see COMPILE.md.\n");
		ConsoleMessageFlush();
		return 2;
	}

	/* --fetch-riscos installs and exits, so combining it with options that
	   say which machine to run is a contradiction rather than a shortcut. */
	if (g_fetch_riscos && (headless || machine_name != nullptr || resume ||
	                       state_file != nullptr)) {
		ConsoleMessage(true, "error: --fetch-riscos cannot be combined with "
		               "--headless, --machine, --resume or --state.\n");
		ConsoleMessageFlush();
		return 2;
	}
	if ((g_pkg_install != NULL || g_pkg_remove != NULL) &&
	    g_pkg_machine == NULL) {
		ConsoleMessage(true, "error: --pkg-install and --pkg-remove need "
		               "--pkg-machine=<name> to say which machine's disc.\n");
		ConsoleMessageFlush();
		return 2;
	}
	if (g_pkg_list && (headless || machine_name != nullptr || resume ||
	                   state_file != nullptr || g_fetch_riscos)) {
		ConsoleMessage(true, "error: --pkg-list cannot be combined with "
		               "--fetch-riscos, --headless, --machine, --resume or "
		               "--state.\n");
		ConsoleMessageFlush();
		return 2;
	}
	if (!g_fetch_riscos && (!g_fetch_disc || g_fetch_accept_licence)) {
		ConsoleMessage(true, "error: %s only applies to --fetch-riscos.\n",
		               !g_fetch_disc ? "--no-disc" : "--accept-licence");
		ConsoleMessageFlush();
		return 2;
	}

	if (headless) {
		return RunHeadless(machine_name, resume, state_file);
	}

	/* Run the fetch under a console application rather than the GUI one, so
	   it works with no display present. Swapping the initializer function
	   makes wxEntry() construct that instead of RpcemuApp. */
	if (g_fetch_riscos) {
		wxAppConsole::SetInitializerFunction(
		    []() -> wxAppConsole * { return new RiscosFetchApp; });
		return wxEntry(wx_argc, wx_argv);
	}

	/* Reading the catalogue needs no display either. */
	if (g_pkg_list) {
		wxAppConsole::SetInitializerFunction(
		    []() -> wxAppConsole * { return new PackageListApp; });
		return wxEntry(wx_argc, wx_argv);
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
		g_startup_managed = managed;
	}

	/* Normal graphical launch, with the options consumed above removed. */
	return wxEntry(wx_argc, wx_argv);
}
