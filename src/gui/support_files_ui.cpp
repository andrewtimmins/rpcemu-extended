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
 */

/*
 * support_files_ui.cpp - saying so when the guest files are being brought over
 *
 * The work itself is support_install.c's; this decides whether it is worth
 * telling anybody about, and shows a window while it happens if it is.
 *
 * Almost every start has nothing to do, so the question is asked before any
 * window is made: a progress dialogue that flashes up and vanishes on every
 * launch is worse than no dialogue at all. One appears on a first run and after
 * an upgrade, which are the two occasions somebody would otherwise be looking
 * at a still screen wondering whether it had hung.
 */

#include <wx/wx.h>
#include <wx/progdlg.h>

#include "support_files_ui.h"

extern "C" {
#include "rpcemu.h"
#include "support_files.h"
}

namespace {

/* Carried through the C callback, which takes a void *. */
struct ProgressState {
	wxProgressDialog *dialog = nullptr;
};

void OnProgress(int done, int total, const char *path, void *ctx)
{
	auto *state = static_cast<ProgressState *>(ctx);

	if (state == nullptr || state->dialog == nullptr) {
		return;
	}

	/* The final call has no path and says the run is over. */
	if (path == nullptr) {
		state->dialog->Update(total);
		return;
	}

	/* The name of each file, because "Updating support files" on its own for
	   several seconds says nothing about whether anything is happening. Not
	   cancellable: stopping half way through would leave a machine whose
	   expansion cards have some modules from this release and some from the
	   last, which is the state this exists to prevent. */
	state->dialog->Update(done, wxString::Format("Updating support files...\n%s",
	                                             wxString::FromUTF8(path)));
	wxYield();
}

} /* namespace */

int SupportFilesEnsure(wxWindow *parent)
{
	/* The core's own idea of the data directory, which already carries the
	   trailing separator the paths are built from. */
	const char *const datadir = rpcemu_get_datadir();

	if (datadir == NULL || datadir[0] == '\0') {
		return 0;
	}

	const int pending = support_install_pending(datadir);

	if (pending <= 0) {
		return 0;
	}

	rpclog("support: %d guest support file%s to bring into the data directory\n",
	       pending, pending == 1 ? "" : "s");

	/*
	 * Silently where there is no toolkit to show anything with: the console
	 * entry points, and headless, which reaches here before wx is up at all -
	 * so wxTheApp is checked for NULL rather than assumed. The work still
	 * happens, because a machine started from a script needs its expansion card
	 * modules just as much as one started from the Manager.
	 *
	 * A null parent is NOT the test. The graphical route calls this before any
	 * window exists, which is the whole point of it - the files have to be in
	 * place before the Manager can offer a machine - and a progress dialogue
	 * with no parent is an ordinary top-level window.
	 */
	if (wxTheApp == nullptr || !wxTheApp->IsGUI()) {
		return support_install_run(datadir, nullptr, nullptr);
	}

	ProgressState state;
	wxProgressDialog dialog("RPCEmu",
	    "Updating support files...", pending, parent,
	    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);

	state.dialog = &dialog;

	const int done = support_install_run(datadir, OnProgress, &state);

	return done;
}
