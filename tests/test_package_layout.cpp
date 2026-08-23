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
 * test_package_layout.cpp - where the package database goes, and what happens to
 * one that is already there.
 *
 * Worth testing on real directories rather than by reading the code, because
 * every mistake here is destructive in a way that looks like success: a database
 * put where RISC OS cannot see it still installs packages, a merge that drops a
 * record leaves files on the disc that nothing will ever remove, and a logical
 * path resolved wrongly installs a working-looking package whose modules are in
 * a directory the OS does not load from.
 *
 * Argument 1 is the source tree, so the bundled template can be found.
 */

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/utils.h>
#include <wx/string.h>

#include "package_layout.h"

#include <set>

#include <cstdio>
#include <cstdarg>

static int failures;
static wxString resource_dir;		/* what the stub below reports */

/* package_layout.cpp calls both of these. Stubbed rather than linked from the
   core, so this test needs neither the emulator nor a configuration. */
extern "C" void rpclog(const char *format, ...)
{
	(void) format;
}

extern "C" const char *rpcemu_get_resourcedir(void)
{
	static std::string held;

	held = std::string(resource_dir.utf8_str());
	return held.c_str();
}

static void
check(const char *what, bool ok)
{
	printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static wxString
Sep()
{
	return wxString(wxFileName::GetPathSeparator());
}

/** A path from '/' separated components, so the tests read like RISC OS paths. */
static wxString
At(const wxString &base, const wxString &relative)
{
	wxString rel = relative;

	rel.Replace("/", Sep());
	return base + Sep() + rel;
}

static void
MakeDir(const wxString &path)
{
	wxFileName::Mkdir(path, 0755, wxPATH_MKDIR_FULL);
}

static void
WriteFile(const wxString &path, const wxString &text)
{
	wxFFile file;

	MakeDir(path.BeforeLast(wxFileName::GetPathSeparator()));
	file.Open(path, "wb");
	file.Write(text);
}

static wxString
ReadFile(const wxString &path)
{
	wxFFile file(path, "rb");
	wxString text;

	if (file.IsOpened()) {
		file.ReadAll(&text);
	}

	return text;
}

/** A fresh empty directory to play a machine's disc. */
static wxString
FreshDisc(const char *name)
{
	const wxString dir = wxFileName::GetTempDir() + Sep() +
	    wxString::Format("rpcemu-pkglayout-%s-%d", name, (int) wxGetProcessId());

	if (wxDirExists(dir)) {
		wxFileName::Rmdir(dir, wxPATH_RMDIR_RECURSIVE);
	}
	MakeDir(dir);

	return dir;
}

/* ------------------------------------------------------------------ resolve */

static void
test_resolve(void)
{
	printf("resolve: which of the two locations\n");

	{
		const wxString disc = FreshDisc("res-nolboot");
		const PackagesLocation loc = PackagesResolve(disc);

		check("a disc with no !Boot falls back to $.!Packages",
		      loc.layout == PackagesLayout::Legacy &&
		      loc.dir == At(disc, "!Packages"));
		check("...and reports it does not exist yet", !loc.existed);
	}

	{
		const wxString disc = FreshDisc("res-resources");

		MakeDir(At(disc, "!Boot/Resources"));

		const PackagesLocation loc = PackagesResolve(disc);

		check("a disc with !Boot.Resources chooses it",
		      loc.layout == PackagesLayout::BootResources &&
		      loc.dir == At(disc, "!Boot/Resources/!Packages"));
		check("...and reports it does not exist yet", !loc.existed);
	}

	{
		const wxString disc = FreshDisc("res-existing");

		MakeDir(At(disc, "!Boot/Resources/!Packages"));

		const PackagesLocation loc = PackagesResolve(disc);

		check("an existing !Boot.Resources.!Packages is used as it is",
		      loc.layout == PackagesLayout::BootResources && loc.existed);
	}

	{
		/* Both present is the state every machine upgrading to this build is
		   in. The RISC OS one has to win, or nothing changes. */
		const wxString disc = FreshDisc("res-both");

		MakeDir(At(disc, "!Boot/Resources/!Packages"));
		MakeDir(At(disc, "!Packages"));

		const PackagesLocation loc = PackagesResolve(disc);

		check("with both present the RISC OS location wins",
		      loc.dir == At(disc, "!Boot/Resources/!Packages"));
	}

	{
		/* A !Boot with no Resources is not a layout to guess at. */
		const wxString disc = FreshDisc("res-bootonly");

		MakeDir(At(disc, "!Boot"));

		const PackagesLocation loc = PackagesResolve(disc);

		check("a !Boot with no Resources is left to the legacy location",
		      loc.layout == PackagesLayout::Legacy);
	}
}

/* ------------------------------------------------------------------ prepare */

static void
test_prepare_creates(void)
{
	const wxString disc = FreshDisc("prep-create");
	PackagesLocation loc;

	printf("prepare: creating a database from the bundled template\n");

	MakeDir(At(disc, "!Boot/Resources"));
	loc = PackagesPrepare(disc);

	check("the database is created in !Boot.Resources",
	      loc.dir == At(disc, "!Boot/Resources/!Packages") &&
	      wxDirExists(loc.dir));

	/* Without !Boot there is no Packages$Dir, and then it is a database no
	   RISC OS tool can find - which is the entire point of moving it. */
	check("!Boot is laid down, so Packages$Dir gets published",
	      wxFileExists(At(loc.dir, "!Boot,feb")));
	check("!Boot sets Packages$Dir",
	      ReadFile(At(loc.dir, "!Boot,feb")).Contains("Set Packages$Dir"));
	check("!Run is laid down", wxFileExists(At(loc.dir, "!Run,feb")));

	check("all three sprite files are laid down",
	      wxFileExists(At(loc.dir, "!Sprites,ff9")) &&
	      wxFileExists(At(loc.dir, "!Sprites11,ff9")) &&
	      wxFileExists(At(loc.dir, "!Sprites22,ff9")));

	check("Version says 2", ReadFile(At(loc.dir, "Version")).Contains("2"));
	check("Status exists and is empty",
	      wxFileExists(At(loc.dir, "Status")) &&
	      ReadFile(At(loc.dir, "Status")).empty());
	check("Paths is laid down",
	      ReadFile(At(loc.dir, "Paths")).Contains("System = <System$Dir>"));
	check("Info exists, so it looks like a database",
	      wxDirExists(At(loc.dir, "Info")));

	/* Called on every operation, so it has to be safe to call repeatedly. */
	WriteFile(At(loc.dir, "Status"), "Zool\t1.61-2\tinstalled\t\t\n");
	PackagesPrepare(disc);
	check("preparing again does not overwrite Status",
	      ReadFile(At(loc.dir, "Status")).Contains("Zool"));
}

static void
test_prepare_legacy_topup(void)
{
	const wxString disc = FreshDisc("prep-topup");
	PackagesLocation loc;

	printf("prepare: a disc with no !Boot still gets a proper !Packages\n");

	/* What an earlier build of ours left behind: a database with none of the
	   RISC OS side. */
	WriteFile(At(disc, "!Packages/Status"), "Nebulus\t0-3\tinstalled\t\t\n");
	WriteFile(At(disc, "!Packages/Version"), "2\n");

	loc = PackagesPrepare(disc);

	check("it stays where it is, there being no !Boot to move it into",
	      loc.layout == PackagesLayout::Legacy &&
	      loc.dir == At(disc, "!Packages"));
	check("the missing RISC OS side is added",
	      wxFileExists(At(loc.dir, "!Boot,feb")) &&
	      wxFileExists(At(loc.dir, "!Sprites11,ff9")) &&
	      wxFileExists(At(loc.dir, "Paths")));
	check("the existing Status is left exactly as it was",
	      ReadFile(At(loc.dir, "Status")) ==
	      "Nebulus\t0-3\tinstalled\t\t\n");
}

/* -------------------------------------------------------------------- merge */

static void
test_merge(void)
{
	const wxString disc = FreshDisc("merge");
	PackagesLocation loc;

	printf("prepare: merging a legacy database into PackMan's\n");

	/* PackMan's, with its own record. Deliberately with NO trailing newline,
	   because that is how a real PackMan writes it, and appending straight onto
	   such a file glues the first migrated line to PackMan's own record. */
	WriteFile(At(disc, "!Boot/Resources/!Packages/Status"),
	          "PackMan\t0.9.8-1\tinstalled\t\tb");
	WriteFile(At(disc, "!Boot/Resources/!Packages/Version"), "2\n");
	WriteFile(At(disc, "!Boot/Resources/!Packages/Info/PackMan/Control"),
	          "Package: PackMan\n");

	/* Ours, with two of its own and one PackMan already has. */
	WriteFile(At(disc, "!Packages/Status"),
	          "Nebulus\t0-3\tinstalled\t\t\n"
	          "Zool\t1.61-2\tinstalled\t\t\n"
	          "PackMan\t0.0.1-1\tinstalled\t\t\n");
	WriteFile(At(disc, "!Packages/Version"), "2\n");
	WriteFile(At(disc, "!Packages/Info/Nebulus/Control"), "Package: Nebulus\n");
	WriteFile(At(disc, "!Packages/Info/Nebulus/Files"), "Apps.Games.!Nebulus\n");
	WriteFile(At(disc, "!Packages/Info/Zool/Control"), "Package: Zool\n");
	WriteFile(At(disc, "!Packages/Info/PackMan/Control"), "Package: WRONG\n");

	loc = PackagesPrepare(disc);

	const wxString status = ReadFile(At(loc.dir, "Status"));

	check("the merged database is PackMan's",
	      loc.dir == At(disc, "!Boot/Resources/!Packages"));
	check("PackMan's own record is still there", status.Contains("PackMan\t0.9.8-1"));
	check("a target Status with no trailing newline is not run into",
	      status.Contains("installed\t\tb\n"));
	check("every record is on a line of its own",
	      PackagesReadStatus(At(loc.dir, "Status")).size() == 3);
	check("our packages are now recorded there",
	      status.Contains("Nebulus\t0-3") && status.Contains("Zool\t1.61-2"));
	check("their Info directories came across",
	      wxFileExists(At(loc.dir, "Info/Nebulus/Control")) &&
	      wxFileExists(At(loc.dir, "Info/Nebulus/Files")) &&
	      wxFileExists(At(loc.dir, "Info/Zool/Control")));

	/* A name in both: the target installed the files, so the target's record is
	   the one that describes them. */
	check("a package recorded in both keeps the target's version",
	      !status.Contains("PackMan\t0.0.1-1"));
	check("...and the target's Info is not overwritten",
	      ReadFile(At(loc.dir, "Info/PackMan/Control")) == "Package: PackMan\n");

	check("the old database is renamed, not deleted",
	      !wxDirExists(At(disc, "!Packages")) &&
	      wxDirExists(At(disc, "!Packages-migrated")));
	check("its contents are still readable in the renamed copy",
	      ReadFile(At(disc, "!Packages-migrated/Info/Zool/Control")) ==
	      "Package: Zool\n");

	/* Once merged, doing it again must not duplicate the lines. */
	PackagesPrepare(disc);
	check("merging is not repeated on the next call",
	      ReadFile(At(loc.dir, "Status")) == status);
}

static void
test_version_guard(void)
{
	const wxString disc = FreshDisc("version");
	PackagesLocation loc;

	printf("prepare: a database in a format we do not know\n");

	WriteFile(At(disc, "!Boot/Resources/!Packages/Status"),
	          "PackMan\t9.9.9-1\tinstalled\t\tb\n");
	WriteFile(At(disc, "!Boot/Resources/!Packages/Version"), "3\n");
	WriteFile(At(disc, "!Packages/Status"), "Zool\t1.61-2\tinstalled\t\t\n");
	WriteFile(At(disc, "!Packages/Version"), "2\n");

	loc = PackagesPrepare(disc);

	check("a Version we do not recognise sends us back to the legacy location",
	      loc.layout == PackagesLayout::Legacy &&
	      loc.dir == At(disc, "!Packages"));
	check("...and that database is not touched at all",
	      ReadFile(At(disc, "!Boot/Resources/!Packages/Status")) ==
	      "PackMan\t9.9.9-1\tinstalled\t\tb\n");
	check("...and it is not renamed away",
	      wxDirExists(At(disc, "!Packages")));
}

/* -------------------------------------------------------------------- paths */

static void
test_paths_default(void)
{
	const wxString disc = FreshDisc("paths-default");
	PackagesPathMap paths;

	printf("paths: the built-in set, for a database with no Paths file\n");

	MakeDir(At(disc, "!Packages"));
	paths = PackagesReadPaths(disc, At(disc, "!Packages"));

	check("Apps is at the root, as it always was", paths["Apps"] == "Apps");
	check("Manuals likewise", paths["Manuals"] == "Manuals");
	check("Boot is !Boot, not Boot", paths["Boot"] == "!Boot");
	check("Resources is inside !Boot", paths["Resources"] == "!Boot/Resources");
	check("System is !Boot.Resources.!System",
	      paths["System"] == "!Boot/Resources/!System");
	check("ToBeLoaded is the PreDesk directory",
	      paths["ToBeLoaded"] == "!Boot/Choices/Boot/PreDesk");
	check("ToBeTasks is the Tasks directory",
	      paths["ToBeTasks"] == "!Boot/Choices/Boot/Tasks");
	check("Bootloader is inside !Boot", paths["Bootloader"] == "!Boot/Loader");
	check("Packages$Dir expands to wherever the database is",
	      paths["Sprites"] == "!Packages/Sprites");
}

static void
test_paths_from_file(void)
{
	const wxString disc = FreshDisc("paths-file");
	PackagesPathMap paths;

	printf("paths: a Paths file on the disc overrides the built-in set\n");

	WriteFile(At(disc, "!Packages/Paths"),
	          "Apps = <Boot$Dir>.^.Programs\n"
	          "Odd = <Nonsense$Dir>.Somewhere\n"
	          "Deep = <Boot$Dir>.^.a.b.^.c\n");

	paths = PackagesReadPaths(disc, At(disc, "!Packages"));

	check("a disc that puts Apps elsewhere is honoured",
	      paths["Apps"] == "Programs");
	check("an entry naming a variable we do not know is dropped",
	      paths.find("Odd") == paths.end());
	check("a ^ in the middle pops the component before it",
	      paths["Deep"] == "a/c");
}

static void
test_apply_paths(void)
{
	PackagesPathMap paths;

	printf("paths: applying them to a member's stored path\n");

	paths["Apps"] = "Apps";
	paths["Apps.Admin.!PackMan"] = "Apps/!PackMan";
	paths["System"] = "!Boot/Resources/!System";
	paths["Boot"] = "!Boot";

	check("an ordinary app is unchanged",
	      PackagesApplyPaths(paths, "Apps/Games/!Nebulus") ==
	      "Apps/Games/!Nebulus");

	/* The one that was broken: a module went to "$.System.310.Modules", which
	   the OS does not load from, and the package looked installed. */
	check("a system module lands inside !Boot.Resources.!System",
	      PackagesApplyPaths(paths, "System/310/Modules") ==
	      "!Boot/Resources/!System/310/Modules");

	check("a boot fragment lands inside !Boot",
	      PackagesApplyPaths(paths, "Boot/Choices") == "!Boot/Choices");

	/* Longest match, or everything inside !PackMan would go to Apps/Admin. */
	check("the longest logical name wins over a shorter one",
	      PackagesApplyPaths(paths, "Apps/Admin/!PackMan/Sub") ==
	      "Apps/!PackMan/Sub");

	check("a root we have never heard of is left exactly as it is",
	      PackagesApplyPaths(paths, "Whatever/Deep/Thing") ==
	      "Whatever/Deep/Thing");

	check("a logical name on its own resolves to the directory itself",
	      PackagesApplyPaths(paths, "System") == "!Boot/Resources/!System");
}

static void
test_riscos_path(void)
{
	const wxString disc = At("/tmp", "disc");

	printf("paths: a host path as RISC OS names it\n");

	/* A trigger is an Obey file run inside the guest and told where it lives, so
	   this has to be the path RISC OS sees, not the host's. */
	check("a database path under !Boot becomes a RISC OS path",
	      PackagesRiscosPath(disc,
	          At(disc, "!Boot/Resources/!Packages/Info/Zool/Triggers")) ==
	      "$.!Boot.Resources.!Packages.Info.Zool.Triggers");

	check("the legacy location works the same way",
	      PackagesRiscosPath(disc, At(disc, "!Packages/Info/Zool")) ==
	      "$.!Packages.Info.Zool");

	/* HostFS carries a RISC OS '.' inside a name as a '/', so the two exchange
	   per component rather than every '.' becoming a separator. */
	check("a dot inside a component becomes a slash, not a separator",
	      PackagesRiscosPath(disc, At(disc, "!Packages/Info/Some.Name")) ==
	      "$.!Packages.Info.Some/Name");

	check("the disc's own root is just $",
	      PackagesRiscosPath(disc, disc) == "$.");
}

static void
test_disc_owned(void)
{
	const wxString disc = FreshDisc("owned");
	std::set<wxString> owned;

	printf("prune floor: which directories the disc owns\n");

	MakeDir(At(disc, "!Packages"));
	owned = PackagesDiscOwnedDirs(disc, PackagesReadPaths(disc,
	    At(disc, "!Packages")));

	/* Each of these is a directory the disc is laid out with. Tidying an empty
	   one away after a removal takes something the OS put there. */
	check("$.Apps is protected", owned.count(At(disc, "Apps")) == 1);
	check("$.!Boot is protected", owned.count(At(disc, "!Boot")) == 1);
	check("$.!Boot.Resources is protected",
	      owned.count(At(disc, "!Boot/Resources")) == 1);
	check("the PreDesk directory is protected",
	      owned.count(At(disc, "!Boot/Choices/Boot/PreDesk")) == 1);
	check("!System is protected",
	      owned.count(At(disc, "!Boot/Resources/!System")) == 1);
	check("a directory a package made is NOT protected",
	      owned.count(At(disc, "Apps/Games/!Nebulus")) == 0);
}

/* --------------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
	wxInitializer init;

	if (!init.IsOk()) {
		fprintf(stderr, "wxWidgets could not be initialised\n");
		return 1;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: %s <source-tree>\n", argv[0]);
		return 1;
	}

	/* rpcemu_get_resourcedir() reports a directory with a trailing separator,
	   which is what the code under test appends "resources" to. */
	resource_dir = wxString::FromUTF8(argv[1]) + Sep();

	if (!wxDirExists(resource_dir + "resources" + Sep() + "ro5" + Sep() +
	                 "packages")) {
		fprintf(stderr, "the !Packages template is not in %s\n",
		        (const char *) resource_dir.utf8_str());
		return 1;
	}

	test_resolve();
	test_prepare_creates();
	test_prepare_legacy_topup();
	test_merge();
	test_version_guard();
	test_paths_default();
	test_paths_from_file();
	test_apply_paths();
	test_riscos_path();
	test_disc_owned();

	printf("\n%s\n", failures ? "FAILED" : "all checks passed");
	return failures ? 1 : 0;
}
