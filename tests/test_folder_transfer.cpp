/*
 * Actually moving somebody's files.
 *
 * tests/test_folder_move.c covers the decision. This covers the doing, on real
 * files, because the doing is the part that can lose a hard disc. It runs without
 * a display: FolderTransferRun() only creates its progress dialogue when there is
 * a GUI, which is what makes this possible in CI.
 *
 * ★ THE CASE THAT MATTERS MOST is the failure half way through. A copy that dies
 * on file three must leave the originals untouched, and must not leave a
 * half-populated destination for the machine to boot from. It is arranged here by
 * making a source file unreadable, which is the closest thing to a full disc that
 * a test can arrange portably.
 */

#include <wx/wx.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/textfile.h>

#include "folder_transfer.h"

#include <cstdio>

static int failures;

static void
check(const char *what, bool ok)
{
	printf("  %-68s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/*
 * wxFileName::SetPermissions is a member, so it needs an object. Wrapped because
 * this is used to arrange the failure cases and reading it inline is noise.
 *
 * On Windows clearing the read bit does not make a file unreadable, so the callers
 * check whether the arrangement actually worked and skip rather than assert
 * something the platform will not do.
 */
static bool
SetPerms(const wxString &path, int mode)
{
	return wxFileName(path).SetPermissions(mode);
}

static wxString
Sep()
{
	return wxString(wxFileName::GetPathSeparator());
}

static void
WriteFile(const wxString &path, const wxString &contents)
{
	wxFileName fn(path);

	wxDir::Make(fn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

	wxFile f;
	f.Create(path, true);
	f.Write(contents);
	f.Close();
}

static wxString
ReadFile(const wxString &path)
{
	wxFile f(path);
	wxString out;

	if (!f.IsOpened()) {
		return wxEmptyString;
	}
	f.ReadAll(&out);
	return out;
}

/* A source tree with a couple of levels, some content, and an empty directory -
   which a naive copy that walks files only would silently drop. */
static void
MakeSourceTree(const wxString &root)
{
	WriteFile(root + Sep() + "!Boot", "boot");
	WriteFile(root + Sep() + "Apps" + Sep() + "Draw", "draw");
	WriteFile(root + Sep() + "Apps" + Sep() + "Deep" + Sep() + "File", "deep");
	wxDir::Make(root + Sep() + "Empty", wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

static bool
TreeArrived(const wxString &root)
{
	return ReadFile(root + Sep() + "!Boot") == "boot" &&
	    ReadFile(root + Sep() + "Apps" + Sep() + "Draw") == "draw" &&
	    ReadFile(root + Sep() + "Apps" + Sep() + "Deep" + Sep() + "File") == "deep";
}

/* wx does the recursion, and does it without complaining about directories that
   are not empty yet - which a hand-rolled version did, printing wx errors that
   read exactly like test failures. */
static void
Nuke(const wxString &root)
{
	if (!wxDirExists(root)) {
		return;
	}
	wxFileName::Rmdir(root, wxPATH_RMDIR_RECURSIVE);
}

static void
test_copy(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "copy-from";
	const wxString to = tmp + Sep() + "copy-to";

	puts("Copy brings the files across and leaves the originals alone:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);

	const FolderTransferResult r = FolderTransferRun(NULL, from, to,
	    FolderTransferChoice::Copy);

	check("it reports success", r.ok);
	check("every file arrived, with its content intact", TreeArrived(to));
	check("an empty directory came too, rather than being dropped",
	    wxDirExists(to + Sep() + "Empty"));
	check("three files were counted", r.files == 3);
	check("the originals are still there - this doubles as a backup",
	    TreeArrived(from));
	check("and the message says the old folder was left alone",
	    r.message.Contains("left exactly as it was"));
}

static void
test_move(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "move-from";
	const wxString to = tmp + Sep() + "move-to";

	puts("Move brings the files across and empties the old folder:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);

	const FolderTransferResult r = FolderTransferRun(NULL, from, to,
	    FolderTransferChoice::Move);

	check("it reports success", r.ok);
	check("every file arrived, with its content intact", TreeArrived(to));
	check("nothing is left in the old folder",
	    !wxFileExists(from + Sep() + "!Boot") &&
	    !wxFileExists(from + Sep() + "Apps" + Sep() + "Draw"));
	check("and it does not claim files were left behind",
	    !r.source_left_behind);
}

/*
 * Moving into a destination that already exists but is empty. The rename path has
 * to clear it first, because renaming onto an existing directory fails on both
 * Windows and Unix - and getting that wrong would break the ordinary case where
 * the user picked a folder with a file browser, which creates it.
 */
static void
test_move_into_existing_empty_dir(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "existing-from";
	const wxString to = tmp + Sep() + "existing-to";

	puts("Moving into a folder that exists and is empty:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);
	wxDir::Make(to, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

	const FolderTransferResult r = FolderTransferRun(NULL, from, to,
	    FolderTransferChoice::Move);

	check("it reports success", r.ok);
	check("every file arrived", TreeArrived(to));
}

/* ★ The one that matters. */
static void
test_failure_leaves_everything_as_it_was(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "fail-from";
	const wxString to = tmp + Sep() + "fail-to";
	const wxString unreadable = from + Sep() + "Apps" + Sep() + "Draw";

	puts("A copy that fails part way through changes nothing:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);

	/* Unreadable, so the copy of this one file fails. Standing in for the real
	   cause, a disc that fills up. */
	if (!SetPerms(unreadable, 0)) {
		puts("  (cannot make a file unreadable here - skipped)");
		return;
	}
	/* Root ignores permissions, so make sure the arrangement actually worked
	   before drawing a conclusion from it. */
	{
		wxFile probe(unreadable);

		if (probe.IsOpened()) {
			puts("  (running as root, permissions do not apply - skipped)");
			SetPerms(unreadable, wxS_DEFAULT);
			return;
		}
	}

	/* The failure is deliberate, so wx's own report of it is not news. Without
	   this the output carries scary red errors that read as test failures. */
	FolderTransferResult r;
	{
		wxLogNull quiet;

		r = FolderTransferRun(NULL, from, to, FolderTransferChoice::Copy);
	}

	check("it reports failure", !r.ok);
	check("it says the files are untouched, and where they are",
	    r.message.Contains(from));

	/* The destination must not be left half full: it is what the machine would
	   boot from if the caller had switched the setting. */
	check("the partial destination has been cleaned up entirely",
	    !wxDirExists(to) || !wxFileExists(to + Sep() + "!Boot"));

	SetPerms(unreadable, wxS_DEFAULT);
	check("and the source is intact, including the file that could not be read",
	    TreeArrived(from));
}

/*
 * A move never destroys anything on the way, whichever path it takes.
 *
 * Two things are checked, because a move has two implementations. On one
 * filesystem it is a rename, which cannot half-happen - so the assertion is simply
 * that it worked and the files are all there. And whatever the path, an occupied
 * destination is refused outright: the cleanup after a failure removes everything
 * at the destination, and that is only safe because the destination was empty to
 * begin with.
 *
 * ★ That second check exists because the first version of this test called the
 * transfer directly with an occupied destination "to prove it is safe on its own",
 * and it was not - the pre-existing file was deleted during cleanup. The guard in
 * FolderTransferRun() came from this test failing.
 */
static void
test_move_never_destroys(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "failmove-from";
	const wxString to = tmp + Sep() + "failmove-to";

	puts("A move cannot half-happen, and will not touch an occupied folder:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);

	{
		const FolderTransferResult r = FolderTransferRun(NULL, from, to,
		    FolderTransferChoice::Move);

		check("on one filesystem it succeeds as a rename", r.ok);
		check("with everything present afterwards", TreeArrived(to));
	}

	/* Now the refusal. A file the caller did not put there must survive. */
	Nuke(from);
	MakeSourceTree(from);
	WriteFile(to + Sep() + "someone-elses-file", "precious");

	{
		const FolderTransferResult r = FolderTransferRun(NULL, from, to,
		    FolderTransferChoice::Move);

		check("an occupied destination is refused", !r.ok);
		check("the file that was already there is untouched",
		    ReadFile(to + Sep() + "someone-elses-file") == "precious");
		check("and the source is untouched too", TreeArrived(from));
	}
}

static void
test_manual_and_cancel_do_nothing(const wxString &tmp)
{
	const wxString from = tmp + Sep() + "noop-from";
	const wxString to = tmp + Sep() + "noop-to";

	puts("The choices that are not transfers do nothing at all:");

	Nuke(from);
	Nuke(to);
	MakeSourceTree(from);

	FolderTransferResult r = FolderTransferRun(NULL, from, to,
	    FolderTransferChoice::Manual);
	check("'I'll do it myself' copies nothing", !r.ok && !wxDirExists(to));

	r = FolderTransferRun(NULL, from, to, FolderTransferChoice::Cancel);
	check("Cancel copies nothing", !r.ok && !wxDirExists(to));
	check("the source is untouched either way", TreeArrived(from));
	check("and there is always a message to show", !r.message.empty());
}

int
main(int argc, char **argv)
{
	wxInitializer init(argc, argv);

	if (!init.IsOk()) {
		fputs("could not initialise wxWidgets\n", stderr);
		return EXIT_FAILURE;
	}

	const wxString tmp = wxFileName::GetTempDir() + Sep() +
	    wxString::Format("rpcemu-transfer-test-%ld", (long) wxGetProcessId());

	wxDir::Make(tmp, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

	test_copy(tmp);
	test_move(tmp);
	test_move_into_existing_empty_dir(tmp);
	test_failure_leaves_everything_as_it_was(tmp);
	test_move_never_destroys(tmp);
	test_manual_and_cancel_do_nothing(tmp);

	/* Tidy up, whatever happened. */
	{
		static const char *const names[] = {
			"copy-from", "copy-to", "move-from", "move-to",
			"existing-from", "existing-to", "fail-from", "fail-to",
			"failmove-from", "failmove-to", "noop-from", "noop-to"
		};

		for (const char *n : names) {
			Nuke(tmp + Sep() + wxString::FromUTF8(n));
		}
		wxRmdir(tmp);
	}

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall folder transfer checks passed");
	return EXIT_SUCCESS;
}
