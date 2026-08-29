/*
 * Settings that belong to the emulator rather than to a machine.
 *
 * The interesting behaviour is not "can it read a file" but the migration: a key
 * the file does not mention must leave the value that came from the legacy
 * per-machine location alone. Get that wrong and every existing installation
 * changes behaviour on upgrade, which is the kind of thing users notice and
 * maintainers do not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_settings.h"

static int failures;
static char dir[512];

static void
check(const char *what, int ok)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static void
write_settings(const char *body)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/rpcemu.cfg", dir);
	f = fopen(path, "w");
	if (f == NULL) {
		printf("  cannot write %s\n", path);
		exit(1);
	}
	fputs(body, f);
	fclose(f);
}

static void
remove_settings(void)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/rpcemu.cfg", dir);
	remove(path);
}

/** A config with recognisable values, standing in for what a machine file gave. */
static void
preset(Config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->vnc_enabled = 0;
	cfg->vnc_port = 5900;
	snprintf(cfg->vnc_password, sizeof(cfg->vnc_password), "from-machine");
	snprintf(cfg->vnc_password_readonly, sizeof(cfg->vnc_password_readonly),
	    "view-from-machine");
	cfg->hostcmd_enabled = 1;
	snprintf(cfg->hostcmd_socket, sizeof(cfg->hostcmd_socket), "machine.sock");
}

int
main(int argc, char *argv[])
{
	Config cfg;

	snprintf(dir, sizeof(dir), "%s", (argc > 1) ? argv[1] : ".");

	printf("a missing file is not an error\n");
	remove_settings();
	preset(&cfg);
	check("load reports nothing applied", app_settings_load(dir, &cfg) == 0);
	check("the machine's value is untouched", cfg.vnc_port == 5900);
	check("...including strings",
	    strcmp(cfg.vnc_password, "from-machine") == 0);
	check("...including the read-only password",
	    strcmp(cfg.vnc_password_readonly, "view-from-machine") == 0);
	check("has() says no", app_settings_has(dir, "vnc_port") == 0);

	printf("\nkeys present in the file win\n");
	write_settings("vnc_enabled=1\nvnc_port=5905\nvnc_password=secret\n"
	               "vnc_password_readonly=watch\n");
	preset(&cfg);
	check("four keys applied", app_settings_load(dir, &cfg) == 4);
	check("enabled overridden", cfg.vnc_enabled == 1);
	check("port overridden", cfg.vnc_port == 5905);
	check("password overridden", strcmp(cfg.vnc_password, "secret") == 0);
	check("read-only password overridden",
	    strcmp(cfg.vnc_password_readonly, "watch") == 0);
	check("has() says yes", app_settings_has(dir, "vnc_port") == 1);

	printf("\nkeys absent from the file do NOT clobber what was there\n");
	/* This is what lets the file be a layer of defaults rather than the whole
	   answer: config_load() puts the built-in values in, overlays this file, and
	   then lets the machine's own configuration have the last word. A key this
	   file does not mention has to leave the value under it standing. */
	check("hostcmd_enabled left alone", cfg.hostcmd_enabled == 1);
	check("hostcmd_socket left alone",
	    strcmp(cfg.hostcmd_socket, "machine.sock") == 0);

	printf("\nformat tolerance\n");
	write_settings("# a comment\n"
	               "\n"
	               "  vnc_port  =  5906  \n"
	               "; another comment style\n"
	               "vnc_password=with spaces inside\n"
	               "a line with no equals sign\n"
	               "unknown_key=ignored\n");
	preset(&cfg);
	check("comments, blanks and blanks around = are handled",
	    app_settings_load(dir, &cfg) == 2);
	check("value parsed despite the spacing", cfg.vnc_port == 5906);
	check("spaces inside a value are kept",
	    strcmp(cfg.vnc_password, "with spaces inside") == 0);

	printf("\nrubbish is refused rather than believed\n");
	write_settings("vnc_port=99999\n");
	preset(&cfg);
	(void) app_settings_load(dir, &cfg);
	check("an impossible port leaves the previous value", cfg.vnc_port == 5900);
	write_settings("vnc_port=0\n");
	preset(&cfg);
	(void) app_settings_load(dir, &cfg);
	check("port 0 likewise", cfg.vnc_port == 5900);
	write_settings("vnc_port=notanumber\n");
	preset(&cfg);
	(void) app_settings_load(dir, &cfg);
	check("a non-numeric port likewise", cfg.vnc_port == 5900);

	printf("\nan empty password is a real setting, not an absent one\n");
	/* "no password" has to be expressible, or you cannot turn one off. */
	write_settings("vnc_password=\n");
	preset(&cfg);
	check("the empty value is applied", app_settings_load(dir, &cfg) == 1);
	check("...clearing the password", cfg.vnc_password[0] == '\0');
	write_settings("vnc_password_readonly=\n");
	preset(&cfg);
	check("an empty read-only password is applied",
	    app_settings_load(dir, &cfg) == 1);
	check("...disabling view-only access",
	    cfg.vnc_password_readonly[0] == '\0');

	printf("\nsave then load round-trips\n");
	remove_settings();
	preset(&cfg);
	cfg.vnc_enabled = 1;
	cfg.vnc_port = 5910;
	snprintf(cfg.vnc_password, sizeof(cfg.vnc_password), "round trip");
	snprintf(cfg.vnc_password_readonly, sizeof(cfg.vnc_password_readonly),
	    "watch round trip");
	cfg.hostcmd_enabled = 0;
	snprintf(cfg.hostcmd_socket, sizeof(cfg.hostcmd_socket), "/tmp/hc.sock");
	snprintf(cfg.relay_interface, sizeof(cfg.relay_interface), "Realtek");
	check("save succeeds", app_settings_save(dir, &cfg) == 0);
	{
		Config back;

		memset(&back, 0, sizeof(back));
		check("all seven keys come back", app_settings_load(dir, &back) == 7);
		check("vnc_enabled", back.vnc_enabled == 1);
		check("vnc_port", back.vnc_port == 5910);
		check("vnc_password", strcmp(back.vnc_password, "round trip") == 0);
		check("vnc_password_readonly",
		    strcmp(back.vnc_password_readonly, "watch round trip") == 0);
		check("hostcmd_enabled", back.hostcmd_enabled == 0);
		check("hostcmd_socket",
		    strcmp(back.hostcmd_socket, "/tmp/hc.sock") == 0);
		/* Issue #205. A name with a space in it is the normal case on Windows,
		   where the adapter is described as "Realtek RTL8852BE WiFi 6 ...", and
		   the parser keeps spaces inside a value - checked above. */
		check("relay_interface",
		    strcmp(back.relay_interface, "Realtek") == 0);
	}

	/*
	 * ★ A COMMAND-LINE OVERRIDE MUST NOT BECOME A SAVED SETTING.
	 *
	 * An override is applied to the live Config so everything downstream sees
	 * it, and config_save() then writes the Config out - so a per-run option was
	 * being recorded as though the user had chosen it. One run with
	 * --debug-socket left an absolute path in the machine's configuration that
	 * broke as soon as the data folder moved, and --vnc-port did the same to the
	 * port, which is worse than it sounds: two instances started on different
	 * ports permanently rewrote both machines' configured port.
	 *
	 * config_save() asks app_settings_is_overridden() before writing each of
	 * these, so this checks the question is answered correctly. It cannot check
	 * config_save() itself from here, since that needs wxWidgets.
	 */
	printf("\nan override is applied but never recorded as a setting\n");
	{
		Config cfg;

		app_settings_clear_overrides();

		check("nothing is overridden to begin with",
		    !app_settings_is_overridden(APP_SETTING_VNC_PORT) &&
		    !app_settings_is_overridden(APP_SETTING_VNC_ENABLED) &&
		    !app_settings_is_overridden(APP_SETTING_HOSTCMD_SOCKET) &&
		    !app_settings_is_overridden(APP_SETTING_DEBUG_SOCKET));

		memset(&cfg, 0, sizeof(cfg));
		cfg.vnc_port = 5900;
		snprintf(cfg.debug_socket, sizeof(cfg.debug_socket), "%s", "keep-me");

		/* Only --debug-socket given. */
		app_settings_override_debug_socket("/run/one-off.sock");
		app_settings_apply_overrides(&cfg);

		check("the override reaches the live config",
		    strcmp(cfg.debug_socket, "/run/one-off.sock") == 0);
		check("and is reported as overridden, so it will not be written",
		    app_settings_is_overridden(APP_SETTING_DEBUG_SOCKET));
		check("while the settings nobody overrode are still writable",
		    !app_settings_is_overridden(APP_SETTING_VNC_PORT) &&
		    !app_settings_is_overridden(APP_SETTING_VNC_ENABLED) &&
		    !app_settings_is_overridden(APP_SETTING_HOSTCMD_SOCKET));

		/* --vnc-port, the one that bit two instances at once. */
		app_settings_override_vnc_port(5911);
		app_settings_apply_overrides(&cfg);
		check("a port override reaches the config", cfg.vnc_port == 5911);
		check("and is reported as overridden",
		    app_settings_is_overridden(APP_SETTING_VNC_PORT));

		app_settings_override_vnc_enabled(0);
		app_settings_override_hostcmd_socket("/run/hc.sock");
		app_settings_apply_overrides(&cfg);
		check("every override is reported",
		    app_settings_is_overridden(APP_SETTING_VNC_ENABLED) &&
		    app_settings_is_overridden(APP_SETTING_HOSTCMD_SOCKET));

		/* The count is not an id and must answer no rather than run off the
		   end of the switch. */
		check("APP_SETTING_COUNT is not a setting",
		    !app_settings_is_overridden(APP_SETTING_COUNT));

		app_settings_clear_overrides();
		check("clearing forgets them all",
		    !app_settings_is_overridden(APP_SETTING_VNC_PORT) &&
		    !app_settings_is_overridden(APP_SETTING_DEBUG_SOCKET));

		/* Cleared overrides must not still be applied, or a later save would
		   write the value after all. */
		memset(&cfg, 0, sizeof(cfg));
		cfg.vnc_port = 5900;
		app_settings_apply_overrides(&cfg);
		check("and stops applying them", cfg.vnc_port == 5900);
	}

	printf("\nthe path is built sensibly\n");
	check("a trailing slash is not doubled",
	    strstr(app_settings_path("/tmp/"), "//") == NULL);
	check("a missing slash is added",
	    strstr(app_settings_path("/tmp"), "/rpcemu.cfg") != NULL);

	remove_settings();
	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
