/*
 * The lock that stops one machine being run twice at once.
 *
 * Two emulators sharing a machine directory share its hard disc image, its CMOS
 * and its HostFS tree, and they will corrupt each other's writes to all three.
 * That is data loss on a user's own files, so the interesting cases here are the
 * ones where the lock has to hold: a second attempt on a locked machine must
 * fail, and a directory that cannot be locked at all must still be allowed to
 * run rather than being refused over a lock we could not take.
 *
 * The exclusion is tested with the platform's own primitives rather than by
 * calling machine_lock_acquire() twice, because that would overwrite the single
 * handle the unit keeps and the second call would be testing nothing. Both
 * flock() and a zero share-mode CreateFile conflict between two open handles in
 * one process, so a second process is not needed.
 *
 * See src/machine_lock.h and docs/multi-machine.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine_lock.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir((p), 0755)
#endif

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

/**
 * Can another handle take this machine's lock?
 *
 * Deliberately duplicating the unit's own locking calls rather than reusing it:
 * this is the second process's point of view, which is the thing being tested.
 *
 * @return 1 if the lock was free (and it is given straight back).
 */
static int
lock_is_free(const char *machine_dir)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/running.lock", machine_dir);

#ifdef _WIN32
	{
		HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if (h == INVALID_HANDLE_VALUE) {
			return 0;
		}
		CloseHandle(h);
		return 1;
	}
#else
	{
		int fd = open(path, O_RDWR | O_CREAT, 0644);

		if (fd < 0) {
			return 0;
		}
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			close(fd);
			return 0;
		}
		close(fd);	/* releases it again */
		return 1;
	}
#endif
}

int
main(int argc, char *argv[])
{
	char machine[1024];
	char other[1024];
	long pid = 0;
	int port = 0;

	snprintf(dir, sizeof(dir), "%s", (argc > 1) ? argv[1] : ".");
	snprintf(machine, sizeof(machine), "%s/lock-test-machine", dir);
	snprintf(other, sizeof(other), "%s/lock-test-other", dir);
	make_dir(machine);
	make_dir(other);
	{
		char path[1200];

		snprintf(path, sizeof(path), "%s/running.lock", machine);
		remove(path);
		snprintf(path, sizeof(path), "%s/running.lock", other);
		remove(path);
	}

	printf("an unlocked machine can be claimed\n");
	check("the lock starts free", lock_is_free(machine) == 1);
	check("acquire succeeds", machine_lock_acquire(machine, 5900) == 1);

	printf("\nand is then held against anyone else\n");
	/* The whole point of the exercise. */
	check("a second claim is refused", lock_is_free(machine) == 0);
	check("a different machine is unaffected", lock_is_free(other) == 1);

	printf("\nthe lock file says who has it\n");
	/* Only a hint - the lock is what enforces anything - but it is what the
	   machine selector shows and what tells a user which window already has
	   this machine open. */
	check("the owner can be read back",
	    machine_lock_read_owner(machine, &pid, &port) == 1);
	check("the pid is this process", pid == (long)
#ifdef _WIN32
	    GetCurrentProcessId()
#else
	    getpid()
#endif
	    );
	check("the VNC port is the one passed in", port == 5900);

	printf("\nthe port can be corrected once it is really listening\n");
	/* The port is not known until the server has bound it, so the lock is
	   written first with an intention and updated with the fact. A machine
	   advertising a port nothing is listening on sends people to a dead end. */
	machine_lock_set_vnc_port(5921);
	pid = 0;
	port = 0;
	check("the new port is in the file",
	    machine_lock_read_owner(machine, &pid, &port) == 1 && port == 5921);
	check("...and the pid survived the rewrite", pid != 0);

	/* A shorter number must not leave the tail of the longer one behind: the
	   file is truncated and rewritten, not overwritten in place. */
	machine_lock_set_vnc_port(590);
	port = 0;
	(void) machine_lock_read_owner(machine, NULL, &port);
	check("a shorter port does not leave digits behind", port == 590);

	printf("\nreleasing hands it back\n");
	machine_lock_release();
	check("the lock is free again", lock_is_free(machine) == 1);
	/* Releasing twice is what an error path will do on the way out. */
	machine_lock_release();
	check("releasing again is harmless", lock_is_free(machine) == 1);

	printf("\nreading an owner that is not there\n");
	{
		char empty[1200];

		snprintf(empty, sizeof(empty), "%s/lock-test-no-such-dir", dir);
		pid = 12345;
		port = 999;
		check("a machine with no lock file reports no owner",
		    machine_lock_read_owner(empty, &pid, &port) == 0);
		check("...and clears what it was given", pid == 0 && port == 0);
	}

	printf("\na machine directory that cannot be locked still runs\n");
	/* Refusing to start because a lock could not be created would make a
	   read-only machine directory unusable, which is worse than the race the
	   lock prevents. POSIX only: the unit returns 1 there deliberately, while
	   the Windows branch has no equivalent path. */
#ifndef _WIN32
	check("a nonexistent directory does not stop the machine",
	    machine_lock_acquire("/nonexistent-directory-for-a-test", 5900) == 1);
	machine_lock_release();
#else
	printf("  (POSIX only - skipped)\n");
#endif

	printf("\nthe lock file lands in the right place\n");
	/* lock_path() is static, so this checks it through its effect: the file has
	   to appear in the machine's own directory whether or not the path it was
	   given ended in a separator. */
	{
		char with_slash[1200];
		char path[1400];

		snprintf(with_slash, sizeof(with_slash), "%s/", machine);
		check("acquire with a trailing slash",
		    machine_lock_acquire(with_slash, 5901) == 1);
		snprintf(path, sizeof(path), "%s/running.lock", machine);
		{
			FILE *f = fopen(path, "r");

			check("the file is in the machine directory, not beside it",
			    f != NULL);
			if (f != NULL) {
				fclose(f);
			}
		}
		port = 0;
		check("and it is the same file the reader finds",
		    machine_lock_read_owner(machine, NULL, &port) == 1 &&
		    port == 5901);
		machine_lock_release();
	}

	/*
	 * Whether the process named by a lock file is still there.
	 *
	 * The lock file outlives its holder: it is written on acquire and nothing
	 * removes it if the process is killed or crashes. Everything that reads it
	 * to decide "is this machine running" therefore has to ask about the pid
	 * as well, and the Manager did not - it attached to machines that had been
	 * dead for hours and then reported that they had failed to start.
	 */
	printf("\nis the recorded owner still running?\n");
	{
		long self = (long) getpid();

		check("this process is alive", machine_lock_owner_alive(self) == 1);

		/* A pid that cannot be running: 0 and negatives are not processes,
		   and kill(0) would mean the whole process group. */
		check("pid 0 is not a process", machine_lock_owner_alive(0) == 0);
		check("a negative pid is not a process", machine_lock_owner_alive(-1) == 0);

		/*
		 * A pid that is almost certainly gone. Picked far above what a
		 * freshly booted system will have issued, and the test only says
		 * "not alive" - if the number ever were in use the check would
		 * fail loudly rather than pass by accident.
		 */
		check("a pid that was never issued is not alive",
		    machine_lock_owner_alive(4194303) == 0);
	}

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
