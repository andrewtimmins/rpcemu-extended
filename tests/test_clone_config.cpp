/*
 * What a cloned machine's configuration must stop sharing with the machine it
 * was copied from.
 *
 * A clone used to have only its `name` rewritten. Everything else was copied
 * verbatim, so a machine whose configuration spelled out a path shared it: the
 * same control socket, so a tool reached whichever machine bound it last; the
 * same capture file, so two machines wrote one pcap; the same HostFS, so two
 * guests wrote one filesystem. A machine with empty socket fields escaped only
 * by luck, because empty already means "under my own directory".
 *
 * See src/gui/config_paths.cpp and docs/multi-machine.md.
 */

#include <wx/app.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/init.h>
#include <wx/string.h>

#include <cstdio>

#include "config_paths.h"

static int failures;

static void
check(const char *what, bool ok)
{
	std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static wxString
read_key(const wxString &path, const char *key)
{
	wxFileConfig settings(wxEmptyString, wxEmptyString, path, wxEmptyString,
	                      wxCONFIG_USE_RELATIVE_PATH);
	wxString value;

	ConfigFileUseGeneralGroup(settings);
	settings.Read(key, &value);
	return value;
}

int
main(int argc, char **argv)
{
	wxInitializer initializer(argc, argv);
	const wxString tmp = wxFileName::GetTempDir() + wxFileName::GetPathSeparator()
	    + "rpcemu-clonetest";
	const wxString src_dir = tmp + wxFileName::GetPathSeparator() + "Alpha";
	const wxString dst_dir = tmp + wxFileName::GetPathSeparator() + "Beta";
	const wxString cfg = tmp + wxFileName::GetPathSeparator() + "Beta.cfg";

	if (!initializer.IsOk()) {
		std::printf("FAIL: wx would not initialise\n");
		return 1;
	}

	wxFileName::Rmdir(tmp, wxPATH_RMDIR_RECURSIVE);
	wxFileName::Mkdir(tmp, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

	/*
	 * A copy of Alpha's configuration, as wxCopyFile would leave it: every path
	 * still Alpha's. Written by hand rather than by the emulator so the awkward
	 * values are actually present - a config with empty socket fields would pass
	 * whatever this function did.
	 */
	{
		wxFileConfig settings(wxEmptyString, wxEmptyString, cfg, wxEmptyString,
		                      wxCONFIG_USE_RELATIVE_PATH);

		ConfigFileUseGeneralGroup(settings);
		settings.Write("name", "Alpha");
		settings.Write("hostcmd_socket", src_dir + "/hostcmd.sock");
		settings.Write("debug_socket", src_dir + "/debug.sock");
		settings.Write("netcap_socket", "/tmp/alpha-cap.sock");
		settings.Write("network_capture", "/tmp/alpha.pcap");
		settings.Write("serial_com1_log", "/tmp/alpha-serial.log");
		settings.Write("printer_output_path", "/tmp/alpha-print");
		settings.Write("hostfs_path", src_dir + "/hostfs");
		settings.Write("hd4_path", src_dir + "/hd4.hdf");
		settings.Write("cdrom_iso", "/srv/media/riscos.iso");
		settings.Write("mem_size", 512L);
		settings.Flush();
	}

	ConfigPathsPrepareClonedConfig(cfg, "Beta", src_dir, dst_dir);

	std::printf("a cloned configuration\n");
	check("the name becomes the clone's", read_key(cfg, "name") == "Beta");

	/* The three control channels: empty means the machine's own directory, so
	   clearing them is what makes the clone use its own. */
	check("the hostcmd socket is no longer the original's",
	    read_key(cfg, "hostcmd_socket").empty());
	check("the debug socket is no longer the original's",
	    read_key(cfg, "debug_socket").empty());
	check("the netcap socket is no longer the original's",
	    read_key(cfg, "netcap_socket").empty());

	/* Write targets: two machines appending to one file is never intended. */
	check("the capture file is not inherited",
	    read_key(cfg, "network_capture").empty());
	check("the serial log is not inherited",
	    read_key(cfg, "serial_com1_log").empty());
	check("the printer output path is not inherited",
	    read_key(cfg, "printer_output_path").empty());

	/* Paths inside the source machine's directory: the copy has just put the
	   files in the clone's directory, so that is where they should point. */
	check("HostFS follows the copy into the clone's directory",
	    read_key(cfg, "hostfs_path") == dst_dir + "/hostfs");
	check("the hard disc follows the copy too",
	    read_key(cfg, "hd4_path") == dst_dir + "/hd4.hdf");

	/* Anything outside it was a deliberate choice and is left alone. */
	check("a path outside the machine directory is left alone",
	    read_key(cfg, "cdrom_iso") == "/srv/media/riscos.iso");
	check("unrelated settings are untouched",
	    read_key(cfg, "mem_size") == "512");

	/*
	 * A machine whose fields were already empty must come out empty rather than
	 * gaining anything - this is the common case, and the one that looked
	 * correct while the bug was present.
	 */
	{
		const wxString plain = tmp + wxFileName::GetPathSeparator() + "Plain.cfg";
		wxFileConfig settings(wxEmptyString, wxEmptyString, plain, wxEmptyString,
		                      wxCONFIG_USE_RELATIVE_PATH);

		ConfigFileUseGeneralGroup(settings);
		settings.Write("name", "Alpha");
		settings.Write("hostcmd_socket", wxEmptyString);
		settings.Flush();

		ConfigPathsPrepareClonedConfig(plain, "Gamma", src_dir, dst_dir);
		std::printf("a configuration that named no paths\n");
		check("the name still becomes the clone's", read_key(plain, "name") == "Gamma");
		check("an already-empty socket stays empty",
		    read_key(plain, "hostcmd_socket").empty());
	}

	wxFileName::Rmdir(tmp, wxPATH_RMDIR_RECURSIVE);

	std::printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
