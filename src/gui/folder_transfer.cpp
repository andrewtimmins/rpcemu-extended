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
 * folder_transfer.cpp - bringing somebody's files across to a new folder.
 *
 * The properties this has to have are in folder_transfer.h. The short version:
 * verify before deleting, and never touch the configuration until the files have
 * arrived.
 */

#include "folder_transfer.h"

#include <wx/wx.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/progdlg.h>
#include <wx/stattext.h>

#include <cstdio>
#include <memory>
#include <vector>

extern "C" {
#include "md5.h"
#include "rpcemu.h"
}

namespace {

/* Files and directories under a root, relative to it, directories first so they
   can be created before the files that go in them. */
struct TreeListing {
	std::vector<wxString> dirs;
	std::vector<wxString> files;
	unsigned long long bytes = 0;
	bool ok = false;
};

void
ListTree(const wxString &root, const wxString &prefix, TreeListing &out)
{
	const wxChar sep = wxFileName::GetPathSeparator();
	wxDir dir(root + (prefix.empty() ? "" : wxString(sep) + prefix));

	if (!dir.IsOpened()) {
		out.ok = false;
		return;
	}

	wxString name;
	bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES | wxDIR_HIDDEN);
	while (more) {
		const wxString rel = prefix.empty() ? name : prefix + sep + name;

		out.files.push_back(rel);
		out.bytes += (unsigned long long) wxFileName::GetSize(
		    root + sep + rel).GetValue();
		more = dir.GetNext(&name);
	}

	more = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS | wxDIR_HIDDEN);
	while (more) {
		const wxString rel = prefix.empty() ? name : prefix + sep + name;

		out.dirs.push_back(rel);
		ListTree(root, rel, out);
		if (!out.ok) {
			return;
		}
		more = dir.GetNext(&name);
	}
	out.ok = true;
}

TreeListing
ListTree(const wxString &root)
{
	TreeListing out;

	out.ok = true;
	ListTree(root, wxEmptyString, out);
	return out;
}

/* Is a directory empty? A directory holding only empty directories counts as
   empty for this purpose: there is nothing in it to lose. */
bool
DirIsEmpty(const wxString &path)
{
	const TreeListing listing = ListTree(path);

	return listing.ok && listing.files.empty();
}

/* Can we write here? Asked by writing, because every other answer (permissions,
   read-only mounts, Windows ACLs) is a guess. */
bool
CanWriteIn(const wxString &dir)
{
	const wxString probe = dir + wxFileName::GetPathSeparator() +
	    ".rpcemu-write-test";

	if (!wxDirExists(dir)) {
		return false;
	}
	{
		wxFile f;

		if (!f.Create(probe, true)) {
			return false;
		}
	}
	wxRemoveFile(probe);
	return true;
}

/*
 * The nearest existing directory at or above `path`.
 *
 * Used to ask about writability and free space for a destination that does not
 * exist yet: the questions have to be put to something that is there.
 */
wxString
NearestExisting(const wxString &path)
{
	wxFileName fn;

	fn.AssignDir(path);
	while (fn.GetDirCount() > 0 && !wxDirExists(fn.GetPath())) {
		fn.RemoveLastDir();
	}
	return fn.GetPath();
}

/* Same file content, checked properly. A size comparison would pass for a copy
   truncated by a full disc, which is exactly the failure this exists to catch. */
bool
SameContent(const wxString &a, const wxString &b)
{
	char hex_a[33];
	char hex_b[33];

	if (wxFileName::GetSize(a) != wxFileName::GetSize(b)) {
		return false;
	}
	if (md5_file_hex(a.utf8_str().data(), hex_a) != 0) {
		return false;
	}
	if (md5_file_hex(b.utf8_str().data(), hex_b) != 0) {
		return false;
	}
	return memcmp(hex_a, hex_b, 32) == 0;
}

/*
 * Remove a tree we created ourselves.
 *
 * Only ever called on a destination that folder_move_decide() confirmed was empty
 * or absent before the transfer started, so everything in it was put there by us.
 * That is the entire justification for a recursive delete living in this file.
 */
void
RemoveOurTree(const wxString &root, bool remove_root)
{
	const TreeListing listing = ListTree(root);
	const wxChar sep = wxFileName::GetPathSeparator();

	if (!listing.ok) {
		return;
	}
	for (const wxString &f : listing.files) {
		wxRemoveFile(root + sep + f);
	}
	/* Deepest first, or a parent will not be empty when its turn comes. */
	for (size_t i = listing.dirs.size(); i > 0; i--) {
		wxRmdir(root + sep + listing.dirs[i - 1]);
	}
	if (remove_root) {
		wxRmdir(root);
	}
}

wxString
Pretty(unsigned long long bytes)
{
	if (bytes >= 1024ull * 1024 * 1024) {
		return wxString::Format("%.1f GB",
		    (double) bytes / (1024.0 * 1024.0 * 1024.0));
	}
	if (bytes >= 1024ull * 1024) {
		return wxString::Format("%.0f MB", (double) bytes / (1024.0 * 1024.0));
	}
	if (bytes >= 1024) {
		return wxString::Format("%.0f KB", (double) bytes / 1024.0);
	}
	return wxString::Format("%llu bytes", bytes);
}

} /* namespace */

folder_move_facts
FolderTransferGatherFacts(const wxString &from, const wxString &to, bool in_use,
                          unsigned long long *bytes)
{
	folder_move_facts f;

	memset(&f, 0, sizeof(f));
	f.in_use = in_use ? 1 : 0;

	/* Normalised, so the same folder spelled two ways is recognised as the same
	   folder and the user is told there is nothing to do. */
	{
		wxFileName a;
		wxFileName b;

		a.AssignDir(from);
		b.AssignDir(to);
		a.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS | wxPATH_NORM_TILDE);
		b.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS | wxPATH_NORM_TILDE);
		f.same_place = (a.GetPath() == b.GetPath()) ? 1 : 0;
	}

	f.source_exists = wxDirExists(from) || wxFileExists(from);
	f.source_is_dir = wxDirExists(from) ? 1 : 0;
	f.source_empty = f.source_is_dir ? (DirIsEmpty(from) ? 1 : 0) : 1;
	f.source_writable = f.source_is_dir ? (CanWriteIn(from) ? 1 : 0) : 0;

	f.dest_exists = (wxDirExists(to) || wxFileExists(to)) ? 1 : 0;
	f.dest_is_dir = wxDirExists(to) ? 1 : 0;
	f.dest_empty = f.dest_is_dir ? (DirIsEmpty(to) ? 1 : 0) : 1;

	{
		const wxString anchor = f.dest_is_dir ? to : NearestExisting(to);

		f.dest_parent_writable = (!anchor.empty() && CanWriteIn(anchor)) ? 1 : 0;

		if (f.source_is_dir) {
			const TreeListing listing = ListTree(from);

			f.bytes_needed = listing.ok ? listing.bytes : 0;
		}
		/*
		 * Free space where the files are going. Left at zero when it cannot be
		 * had: folder_move_space_is_enough() treats a pair of zeroes as "not
		 * measured" and does not refuse, which is deliberate - a filesystem we
		 * cannot size should not cost the user the feature.
		 */
		wxLongLong free_bytes;
		if (!anchor.empty() && wxGetDiskSpace(anchor, NULL, &free_bytes)) {
			f.bytes_free = (unsigned long long) free_bytes.GetValue();
		} else {
			f.bytes_needed = 0;
			f.bytes_free = 0;
		}
	}

	if (bytes != NULL) {
		*bytes = f.source_is_dir ? ListTree(from).bytes : 0;
	}
	return f;
}

FolderTransferChoice
FolderTransferAsk(wxWindow *parent, const wxString &what, const wxString &from,
                  const wxString &to, folder_move_offer offer,
                  folder_move_reason why, unsigned long long bytes)
{
	wxDialog dlg(parent, wxID_ANY, wxString::Format("Move your %s?", what));
	wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);

	wxString text;

	text << "Your files are in:\n    " << from << "\n\nThe new folder is:\n    "
	     << to << "\n\n";

	if (offer == FOLDER_MOVE_OFFER_NOTHING) {
		text << folder_move_reason_text(why) << "\n\n"
		     << "You can still change the setting and move the files across "
		        "yourself.";
	} else {
		if (bytes > 0) {
			text << "There is " << Pretty(bytes) << " to bring across.\n\n";
		}
		text << "Move - the files are copied and checked, and only then removed "
		        "from the old folder.\n";
		if (offer == FOLDER_MOVE_OFFER_BOTH) {
			text << "Copy - the old folder is left exactly as it is, so it "
			        "doubles as a backup. Needs room for both.\n";
		} else {
			text << "\n" << folder_move_reason_text(why) << "\n";
		}
		text << "\nEither way, please make sure you have a backup first: this is "
		        "your "
		     << what
		     << ". Nothing is deleted until every file has been copied and "
		        "checked, but an automatic move is not a substitute for a backup.";
	}

	root->Add(new wxStaticText(&dlg, wxID_ANY, text), 0, wxALL, 12);

	wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
	const int ID_MOVE = wxID_HIGHEST + 1;
	const int ID_COPY = wxID_HIGHEST + 2;
	const int ID_MANUAL = wxID_HIGHEST + 3;

	if (offer != FOLDER_MOVE_OFFER_NOTHING) {
		if (offer == FOLDER_MOVE_OFFER_BOTH) {
			buttons->Add(new wxButton(&dlg, ID_MOVE, "Move my files"), 0,
			    wxRIGHT, 8);
		}
		buttons->Add(new wxButton(&dlg, ID_COPY, "Copy my files"), 0, wxRIGHT, 8);
	}
	buttons->Add(new wxButton(&dlg, ID_MANUAL, "I'll do it myself"), 0,
	    wxRIGHT, 8);
	buttons->Add(new wxButton(&dlg, wxID_CANCEL, "Cancel"));
	root->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 12);

	FolderTransferChoice choice = FolderTransferChoice::Cancel;
	auto pick = [&dlg, &choice](FolderTransferChoice c) {
		choice = c;
		dlg.EndModal(wxID_OK);
	};
	dlg.Bind(wxEVT_BUTTON, [&pick](wxCommandEvent &) {
		pick(FolderTransferChoice::Move);
	}, ID_MOVE);
	dlg.Bind(wxEVT_BUTTON, [&pick](wxCommandEvent &) {
		pick(FolderTransferChoice::Copy);
	}, ID_COPY);
	dlg.Bind(wxEVT_BUTTON, [&pick](wxCommandEvent &) {
		pick(FolderTransferChoice::Manual);
	}, ID_MANUAL);

	dlg.SetSizerAndFit(root);
	dlg.Centre();
	if (dlg.ShowModal() != wxID_OK) {
		return FolderTransferChoice::Cancel;
	}
	return choice;
}

FolderTransferResult
FolderTransferRun(wxWindow *parent, const wxString &from, const wxString &to,
                  FolderTransferChoice choice)
{
	FolderTransferResult result;
	const wxChar sep = wxFileName::GetPathSeparator();
	const bool moving = (choice == FolderTransferChoice::Move);
	const bool dest_existed = wxDirExists(to);

	if (choice != FolderTransferChoice::Move &&
	    choice != FolderTransferChoice::Copy) {
		result.message = "Nothing was moved.";
		return result;
	}

	const TreeListing listing = ListTree(from);

	if (!listing.ok) {
		result.message = wxString::Format("Could not read '%s', so nothing was "
		    "moved. The setting has been left as it was.", from);
		return result;
	}

	/*
	 * ★ REFUSED HERE AS WELL AS IN folder_move_decide(), not instead of it.
	 *
	 * The cleanup after a failure removes everything at the destination, and its
	 * only justification is that the destination was empty beforehand, so
	 * everything in it is ours. That justification is the caller's to establish -
	 * and a destructive function that trusts its caller for the one fact keeping
	 * it safe is one refactor away from deleting somebody's files.
	 *
	 * Found by a test that called this directly with an occupied destination, to
	 * check "this must be safe on its own". It was not: the pre-existing file was
	 * deleted during cleanup.
	 */
	if (dest_existed && !DirIsEmpty(to)) {
		result.message = wxString::Format(
		    "'%s' already has files in it, so nothing was moved. RPCEmu will not "
		    "mix two sets of files together.", to);
		return result;
	}

	/*
	 * A move within one filesystem is a rename: instant whatever the size, and
	 * nothing is copied so nothing can be half-copied. Tried first, and if it
	 * fails for any reason (a different filesystem being the usual one) the copy
	 * path below runs instead.
	 *
	 * An existing empty destination is removed first, because renaming onto an
	 * existing directory fails on both Windows and Unix. Safe only because it was
	 * checked to be empty; see folder_move_decide().
	 */
	if (moving) {
		bool cleared = true;

		if (dest_existed && !wxRmdir(to)) {
			cleared = false;
		}
		if (cleared && wxRenameFile(from, to, false)) {
			result.ok = true;
			result.files = (unsigned long) listing.files.size();
			result.message = wxString::Format(
			    "Moved %lu file%s to '%s'.", result.files,
			    result.files == 1 ? "" : "s", to);
			return result;
		}
		if (cleared && dest_existed && !wxDirExists(to)) {
			/* Put back what we removed, so a failure leaves things as they
			   were. */
			wxDir::Make(to, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
		}
	}

	if (!wxDirExists(to) && !wxDir::Make(to, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
		result.message = wxString::Format("Could not create '%s', so nothing was "
		    "moved. The setting has been left as it was.", to);
		return result;
	}

	{
		/*
		 * ★ The progress dialogue is optional so that this function can be tested.
		 *
		 * wxProgressDialog needs a GUI toolkit and a display; everything else here
		 * is wxBase and needs neither. Creating it only when there is a GUI lets
		 * tests/test_folder_transfer.cpp drive the real copy, verify and delete
		 * code - which is the destructive part, and the part worth testing - on a
		 * console build with no display, including in CI.
		 */
		/*
		 * ★ wxAppConsole::GetInstance(), NOT wxTheApp.
		 *
		 * wxTheApp is a macro that DOWNCASTS the application object to wxApp*,
		 * and in a console program - which is exactly what the test is - the
		 * object is a wxAppConsole and never was a wxApp. The cast is undefined
		 * behaviour even though the pointer is only tested and IsGUI() is virtual
		 * on the base, and UBSan says so:
		 *
		 *   runtime error: downcast of address 0x... which does not point to an
		 *   object of type 'wxApp'
		 *
		 * Caught by the sanitiser job, in the first version of this file. Asking
		 * the base class needs no cast and answers the same question.
		 */
		const wxAppConsole *const app = wxAppConsole::GetInstance();
		const bool have_gui = (app != nullptr && app->IsGUI());
		std::unique_ptr<wxProgressDialog> progress;

		const int total = (int) listing.files.size();

		if (have_gui) {
			progress.reset(new wxProgressDialog(
			    moving ? "Moving files" : "Copying files",
			    /* Sized for the widest message it will show later, or the dialogue
			       grows as it goes and the text jumps about. */
			    wxString::Format("Copying %d files to\n%s", total, to),
			    total + 1, parent,
			    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
			    wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME | wxPD_ESTIMATED_TIME));
		}
		/*
		 * Named files and a running count, not just a bar: a disc with a thousand
		 * files on it takes long enough that "is it stuck?" is a fair question, and
		 * the answer should be on screen. Each file is copied AND verified before
		 * the next update, so the name shown is genuinely the one being worked on.
		 */
		auto keep_going = [&progress, total](int done, const wxString &label) {
			if (!progress) {
				return true;
			}
			return progress->Update(done,
			    wxString::Format("File %d of %d\n%s", done + 1, total, label));
		};

		for (const wxString &d : listing.dirs) {
			if (!wxDirExists(to + sep + d) &&
			    !wxDir::Make(to + sep + d, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
				RemoveOurTree(to, !dest_existed);
				result.message = wxString::Format(
				    "Could not create '%s'. Nothing has been moved and the "
				    "setting has been left as it was.", to + sep + d);
				return result;
			}
		}

		int done = 0;
		for (const wxString &f : listing.files) {
			const wxString src = from + sep + f;
			const wxString dst = to + sep + f;

			if (!keep_going(done++, f)) {
				/* Cancelled. The destination was empty before we started, so
				   everything in it is ours to remove. */
				RemoveOurTree(to, !dest_existed);
				result.message = "Cancelled. Your files have not been moved and "
				    "the setting has been left as it was.";
				return result;
			}

			if (!wxCopyFile(src, dst, true) || !SameContent(src, dst)) {
				/*
				 * ★ The failure that matters, and why nothing is deleted until
				 * the whole copy has been verified. A disc that fills up half way
				 * leaves a truncated file, which a size check would pass and an
				 * MD5 does not.
				 */
				RemoveOurTree(to, !dest_existed);
				result.message = wxString::Format(
				    "Could not copy '%s' (there may not be enough room). Your "
				    "files are untouched in '%s' and the setting has been left "
				    "as it was.", f, from);
				return result;
			}
			result.files++;
		}
		keep_going(total, "Finishing off...");
	}

	/* Every file is at the destination and verified. Only now is the old folder
	   touched, and only for a move. */
	if (moving) {
		RemoveOurTree(from, false);
		if (!DirIsEmpty(from)) {
			result.source_left_behind = true;
		}
	}

	result.ok = true;
	if (moving && result.source_left_behind) {
		result.message = wxString::Format(
		    "Copied and checked %lu file%s into '%s'. Some of the originals in "
		    "'%s' could not be removed, so they are still there - your machine "
		    "is using the new folder.",
		    result.files, result.files == 1 ? "" : "s", to, from);
	} else if (moving) {
		result.message = wxString::Format(
		    "Moved %lu file%s to '%s', checking each one.", result.files,
		    result.files == 1 ? "" : "s", to);
	} else {
		result.message = wxString::Format(
		    "Copied %lu file%s to '%s', checking each one. The old folder in "
		    "'%s' has been left exactly as it was.", result.files,
		    result.files == 1 ? "" : "s", to, from);
	}
	rpclog("Folder transfer: %s\n", result.message.utf8_str().data());
	return result;
}
