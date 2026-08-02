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
	check("has() says no", app_settings_has(dir, "vnc_port") == 0);

	printf("\nkeys present in the file win\n");
	write_settings("vnc_enabled=1\nvnc_port=5905\nvnc_password=secret\n");
	preset(&cfg);
	check("three keys applied", app_settings_load(dir, &cfg) == 3);
	check("enabled overridden", cfg.vnc_enabled == 1);
	check("port overridden", cfg.vnc_port == 5905);
	check("password overridden", strcmp(cfg.vnc_password, "secret") == 0);
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

	printf("\nsave then load round-trips\n");
	remove_settings();
	preset(&cfg);
	cfg.vnc_enabled = 1;
	cfg.vnc_port = 5910;
	snprintf(cfg.vnc_password, sizeof(cfg.vnc_password), "round trip");
	cfg.hostcmd_enabled = 0;
	snprintf(cfg.hostcmd_socket, sizeof(cfg.hostcmd_socket), "/tmp/hc.sock");
	check("save succeeds", app_settings_save(dir, &cfg) == 0);
	{
		Config back;

		memset(&back, 0, sizeof(back));
		check("all five keys come back", app_settings_load(dir, &back) == 5);
		check("vnc_enabled", back.vnc_enabled == 1);
		check("vnc_port", back.vnc_port == 5910);
		check("vnc_password", strcmp(back.vnc_password, "round trip") == 0);
		check("hostcmd_enabled", back.hostcmd_enabled == 0);
		check("hostcmd_socket",
		    strcmp(back.hostcmd_socket, "/tmp/hc.sock") == 0);
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
