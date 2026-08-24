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
 * support_install.c - putting the embedded guest files into the data directory
 *
 * The filesystem half of support_files.c. What to copy is decided there, from
 * stated inputs and without touching a disc; this carries the answer out.
 *
 * Written so it can run before any window exists, because on a first run it
 * has to: the machine selector cannot offer a machine whose expansion cards
 * have no modules to load.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rpcemu.h"
#include "rpcemu-win.h"
#include "support_files.h"
#include "support_payload.h"

#define MANIFEST_NAME	"manifest.txt"

/* Enough for every file a release has ever carried, with room to grow. Sized
   rather than allocated because this runs before anything else is up, and a
   fixed ceiling that logs when it is reached is easier to reason about at that
   point than an allocation that could fail. */
#define MAX_ENTRIES	256

/**
 * Make one directory, ignoring "it is already there".
 *
 * @return non-zero on success
 */
static int
make_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0) {
		return S_ISDIR(st.st_mode) ? 1 : 0;
	}

	return mkdir(path, 0755) == 0;
}

/**
 * Make the directories a relative path needs, underneath the data directory.
 *
 * Only the components of `rel` are created; the data directory itself is
 * assumed to exist, because something has already had to write a config into
 * it to get this far.
 *
 * Walking the whole absolute path instead is the obvious way to write this and
 * it is wrong on Windows: the first separator there falls after the drive
 * letter, so the first directory it tries to create is "D:", which cannot be
 * created and stopped every file from being written. Building up from the data
 * directory cannot meet a drive letter or a root at all.
 *
 * @param datadir Data directory, with a trailing separator
 * @param rel     Relative path of the file, '/' separated
 * @return non-zero on success
 */
static int
make_parents(const char *datadir, const char *rel)
{
	char path[1024];
	size_t base_len;
	const char *p;

	if (snprintf(path, sizeof(path), "%s", datadir) >= (int) sizeof(path)) {
		return 0;
	}
	base_len = strlen(path);

	for (p = rel; *p != '\0'; p++) {
		if (*p != '/') {
			continue;
		}

		/* The component so far, appended to the data directory. */
		if (base_len + (size_t) (p - rel) >= sizeof(path)) {
			return 0;
		}
		memcpy(path + base_len, rel, (size_t) (p - rel));
		path[base_len + (size_t) (p - rel)] = '\0';

		if (!make_dir(path)) {
			return 0;
		}
	}

	return 1;
}

/**
 * Read a whole file into a NUL terminated buffer the caller frees.
 *
 * @return The contents, or NULL if it could not be read - which includes the
 *         ordinary case of it not being there at all.
 */
static char *
read_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;
	char *buf;

	if (f == NULL) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0 || len > (1 << 20)) {
		fclose(f);
		return NULL;
	}

	buf = malloc((size_t) len + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t) len, f) != (size_t) len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

/** Does the data directory hold this file? Answers support_files_plan(). */
static int
data_file_exists(const char *rel, void *ctx)
{
	const char *datadir = (const char *) ctx;
	char full[1024];
	struct stat st;

	if (snprintf(full, sizeof(full), "%s%s", datadir, rel) >= (int) sizeof(full)) {
		return 1;	/* Cannot name it, so do not claim it is missing */
	}

	return stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Write one embedded file into the data directory.
 *
 * Through a temporary file and a rename, so that a run interrupted half way -
 * the machine losing power, the user closing the window - leaves either the old
 * file or the new one and never half of a module an expansion card will then
 * try to load.
 */
static int
write_payload_file(const char *datadir, const SupportPayloadFile *f)
{
	char full[1024];
	char tmp[1024];
	FILE *out;

	if (snprintf(full, sizeof(full), "%s%s", datadir, f->path) >= (int) sizeof(full)) {
		return 0;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.new", full) >= (int) sizeof(tmp)) {
		return 0;
	}
	if (!make_parents(datadir, f->path)) {
		return 0;
	}

	out = fopen(tmp, "wb");
	if (out == NULL) {
		return 0;
	}
	if (f->size > 0 && fwrite(f->data, 1, f->size, out) != f->size) {
		fclose(out);
		remove(tmp);
		return 0;
	}
	if (fclose(out) != 0) {
		remove(tmp);
		return 0;
	}

	/* Windows will not rename onto an existing file. */
	remove(full);
	if (rename(tmp, full) != 0) {
		remove(tmp);
		return 0;
	}

	return 1;
}

/** Record what the data directory now holds, so the next run can compare. */
static int
write_manifest(const char *datadir)
{
	char path[1024];
	char tmp[1024];
	FILE *f;
	int i;

	if (snprintf(path, sizeof(path), "%s" MANIFEST_NAME, datadir) >= (int) sizeof(path)) {
		return 0;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int) sizeof(tmp)) {
		return 0;
	}

	f = fopen(tmp, "wb");
	if (f == NULL) {
		return 0;
	}

	fprintf(f, "# RPCEmu support files - written by the emulator, do not edit.\n");
	fprintf(f, "# Records what this data directory was last seeded with, so an\n");
	fprintf(f, "# upgrade can tell which of them it needs to bring over.\n");
	fprintf(f, "version 1\n");
	fprintf(f, "algorithm md5\n");
	for (i = 0; i < support_payload_count; i++) {
		fprintf(f, "%s  %s\n", support_payload[i].hash, support_payload[i].path);
	}

	if (fclose(f) != 0) {
		remove(tmp);
		return 0;
	}

	remove(path);
	if (rename(tmp, path) != 0) {
		remove(tmp);
		return 0;
	}

	return 1;
}

const char *
support_root_for(const char *subdir)
{
	static char root[1024];
	char probe[1024];
	struct stat st;

	/*
	 * RPCEMU_RESOURCE_DIR first, when it is set AND actually holds this
	 * directory. That is somebody saying outright where the payload is, and it
	 * already outranks every other route to it - see InitRpcemuPaths().
	 * Preferring an extracted copy over it would quietly undo that, and would
	 * do so for everybody, because after a first run the data directory always
	 * has one.
	 *
	 * The "and actually holds it" is not caution for its own sake. The macOS
	 * boot test sets the variable to the bundle's Resources, which is where the
	 * payload used to be shipped and no longer is; honouring that blindly sent
	 * every lookup somewhere empty, so no HostFS loaded, !Boot never ran, and
	 * the machine came up to a black screen. A variable naming a directory
	 * without the files in it is not an instruction worth following.
	 */
	if (getenv("RPCEMU_RESOURCE_DIR") != NULL &&
	    getenv("RPCEMU_RESOURCE_DIR")[0] != '\0' &&
	    snprintf(probe, sizeof(probe), "%s%s", rpcemu_get_resourcedir(), subdir)
	        < (int) sizeof(probe) &&
	    stat(probe, &st) == 0 && S_ISDIR(st.st_mode))
	{
		snprintf(root, sizeof(root), "%s", rpcemu_get_resourcedir());
		return root;
	}

	/*
	 * Otherwise the data directory when it holds this payload directory, and
	 * the resource directory when it does not.
	 *
	 * The fallback is what keeps a failed extraction from being fatal. These
	 * files are what an expansion card loads, so a machine whose poduleroms
	 * never arrived has no HostFS, does not run !Boot, and comes up to a black
	 * screen - which is precisely what happened on Windows the first time this
	 * shipped, because the directories could not be created there. Falling back
	 * to where the files have always been means the worst case is the behaviour
	 * we had before, not a machine that will not start.
	 */
	if (snprintf(probe, sizeof(probe), "%s%s", rpcemu_get_datadir(), subdir)
	    < (int) sizeof(probe) &&
	    stat(probe, &st) == 0 && S_ISDIR(st.st_mode))
	{
		snprintf(root, sizeof(root), "%s", rpcemu_get_datadir());
		return root;
	}

	snprintf(root, sizeof(root), "%s", rpcemu_get_resourcedir());
	return root;
}

int
support_install_pending(const char *datadir)
{
	static SupportFile shipped[MAX_ENTRIES];
	static SupportFile recorded[MAX_ENTRIES];
	char manifest_path[1024];
	char *text;
	int recorded_count = 0;
	int i;
	int n;

	if (datadir == NULL || support_payload_count == 0) {
		return 0;
	}

	for (i = 0; i < support_payload_count && i < MAX_ENTRIES; i++) {
		snprintf(shipped[i].path, sizeof(shipped[i].path), "%s",
		         support_payload[i].path);
		snprintf(shipped[i].hash, sizeof(shipped[i].hash), "%s",
		         support_payload[i].hash);
	}
	n = (support_payload_count < MAX_ENTRIES) ? support_payload_count : MAX_ENTRIES;

	if (snprintf(manifest_path, sizeof(manifest_path), "%s" MANIFEST_NAME,
	             datadir) < (int) sizeof(manifest_path))
	{
		text = read_text(manifest_path);
		if (text != NULL) {
			const int parsed = support_manifest_parse(text, recorded, MAX_ENTRIES);

			/* A manifest that will not parse is treated as no manifest: the
			   files are then all copied, which is the safe way to be wrong. */
			recorded_count = (parsed > 0) ? parsed : 0;
			if (recorded_count > MAX_ENTRIES) {
				recorded_count = MAX_ENTRIES;
			}
			free(text);
		}
	}

	return support_files_plan(shipped, n, recorded, recorded_count,
	                          data_file_exists, (void *) datadir, NULL, 0);
}

int
support_install_run(const char *datadir, SupportInstallProgress progress, void *ctx)
{
	static SupportFile shipped[MAX_ENTRIES];
	static SupportFile recorded[MAX_ENTRIES];
	static const SupportFile *plan[MAX_ENTRIES];
	char manifest_path[1024];
	char *text;
	int recorded_count = 0;
	int i;
	int n;
	int planned;
	int done = 0;
	int failed = 0;

	if (datadir == NULL || support_payload_count == 0) {
		return 0;
	}

	for (i = 0; i < support_payload_count && i < MAX_ENTRIES; i++) {
		snprintf(shipped[i].path, sizeof(shipped[i].path), "%s",
		         support_payload[i].path);
		snprintf(shipped[i].hash, sizeof(shipped[i].hash), "%s",
		         support_payload[i].hash);
	}
	n = (support_payload_count < MAX_ENTRIES) ? support_payload_count : MAX_ENTRIES;

	if (snprintf(manifest_path, sizeof(manifest_path), "%s" MANIFEST_NAME,
	             datadir) < (int) sizeof(manifest_path))
	{
		text = read_text(manifest_path);
		if (text != NULL) {
			const int parsed = support_manifest_parse(text, recorded, MAX_ENTRIES);

			recorded_count = (parsed > 0) ? parsed : 0;
			if (recorded_count > MAX_ENTRIES) {
				recorded_count = MAX_ENTRIES;
			}
			free(text);
		}
	}

	planned = support_files_plan(shipped, n, recorded, recorded_count,
	                             data_file_exists, (void *) datadir,
	                             plan, MAX_ENTRIES);
	if (planned <= 0) {
		return 0;
	}
	if (planned > MAX_ENTRIES) {
		planned = MAX_ENTRIES;
	}

	for (i = 0; i < planned; i++) {
		const SupportPayloadFile *src = NULL;
		int j;

		for (j = 0; j < support_payload_count; j++) {
			if (strcmp(support_payload[j].path, plan[i]->path) == 0) {
				src = &support_payload[j];
				break;
			}
		}
		if (src == NULL) {
			continue;
		}

		if (progress != NULL) {
			progress(i, planned, src->path, ctx);
		}

		if (write_payload_file(datadir, src)) {
			done++;
		} else {
			failed++;
			rpclog("support: could not write '%s' into the data directory\n",
			       src->path);
		}
	}

	if (progress != NULL) {
		progress(planned, planned, NULL, ctx);
	}

	/*
	 * Only record the manifest when every file went in. A partial run that
	 * recorded success would leave the missing ones looking up to date, and
	 * they would never be retried - the failure would become permanent and
	 * silent, which is the shape of the fault this whole thing exists to fix.
	 */
	if (failed == 0) {
		if (!write_manifest(datadir)) {
			rpclog("support: files updated but the manifest could not be "
			       "written, so this will run again next time\n");
		}
	}

	rpclog("support: %d guest support file%s brought up to date%s\n",
	       done, done == 1 ? "" : "s",
	       failed ? " (some could not be written)" : "");

	return done;
}
