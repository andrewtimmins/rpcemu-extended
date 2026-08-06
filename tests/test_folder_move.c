/*
 * Whether moving somebody's files can be offered.
 *
 * Copying a hard disc is the most destructive thing RPCEmu does to the host's
 * filesystem, and every condition that makes it unsafe is awkward to arrange for
 * real: a destination with files already in it, a full disc, a machine still
 * running from the folder. So the rules are a pure function over facts somebody
 * else gathered, and this puts the combinations to them.
 *
 * The cases that matter most are the refusals. An offer that should not have been
 * made is how people lose data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "folder_move.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-68s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* The ordinary case: files here, empty folder there, room for them. */
static folder_move_facts good(void)
{
	folder_move_facts f;

	memset(&f, 0, sizeof(f));
	f.source_exists = 1;
	f.source_is_dir = 1;
	f.source_writable = 1;
	f.source_empty = 0;
	f.dest_exists = 1;
	f.dest_is_dir = 1;
	f.dest_empty = 1;
	f.dest_parent_writable = 1;
	f.bytes_needed = 100ull * 1024 * 1024;
	f.bytes_free = 900ull * 1024 * 1024;
	return f;
}

static void
test_the_ordinary_case(void)
{
	folder_move_facts f = good();
	folder_move_reason why = FOLDER_MOVE_NO_SPACE;

	puts("Files to bring across and an empty folder to put them in:");

	check("both a move and a copy are offered",
	    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_BOTH);
	check("and there is nothing to explain", why == FOLDER_MOVE_OK);

	/* A destination that does not exist yet is normal: it gets created. */
	f.dest_exists = 0;
	f.dest_is_dir = 0;
	f.dest_empty = 1;
	check("a destination that does not exist yet is fine, it will be created",
	    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_BOTH);
}

static void
test_refusals(void)
{
	folder_move_reason why;

	puts("The refusals, which are the point of this:");

	/* ★ The dangerous one. Files are opened as the guest asks for them, so
	   moving them under a live machine damages whatever it reads next. */
	{
		folder_move_facts f = good();

		f.in_use = 1;
		check("a machine running from either folder refuses everything",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("and says so, since that is the reason worth hearing",
		    why == FOLDER_MOVE_IN_USE);
	}

	/* In use outranks everything else: it is the only reason that describes
	   damage rather than inconvenience. */
	{
		folder_move_facts f = good();

		f.in_use = 1;
		f.dest_empty = 0;
		f.bytes_free = 0;
		f.bytes_needed = 1;
		folder_move_decide(&f, &why);
		check("being in use is reported ahead of any other problem",
		    why == FOLDER_MOVE_IN_USE);
	}

	/* ★ Never merge. Two hard discs interleaved is not what anybody asked for. */
	{
		folder_move_facts f = good();

		f.dest_empty = 0;
		check("a destination with files in it refuses everything",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("and explains that files will not be mixed together",
		    why == FOLDER_MOVE_DEST_NOT_EMPTY);
	}

	{
		folder_move_facts f = good();

		f.dest_exists = 1;
		f.dest_is_dir = 0;
		check("a destination that is a file is refused",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_DEST_NOT_A_DIR);
	}

	{
		folder_move_facts f = good();

		f.dest_parent_writable = 0;
		check("a destination that cannot be written to is refused",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_DEST_READ_ONLY);
	}

	{
		folder_move_facts f = good();

		f.same_place = 1;
		check("the same folder twice is nothing to do",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_SAME_PLACE);
	}

	{
		folder_move_facts f = good();

		f.source_empty = 1;
		check("an empty source is nothing to do",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_SOURCE_EMPTY);
	}

	{
		folder_move_facts f = good();

		f.source_exists = 0;
		check("a source that is not there is nothing to do",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_SOURCE_MISSING);
	}

	{
		folder_move_facts f = good();

		/* A path that exists but is a file, not a directory. */
		f.source_is_dir = 0;
		check("a source that is a file is refused",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
	}

	check("NULL facts refuse rather than crash",
	    folder_move_decide(NULL, &why) == FOLDER_MOVE_OFFER_NOTHING);
	/* A caller that does not want the reason must not be a special case. */
	{
		folder_move_facts f = good();

		check("a NULL reason pointer is allowed",
		    folder_move_decide(&f, NULL) == FOLDER_MOVE_OFFER_BOTH);
	}
}

/*
 * A source that cannot be emptied afterwards gets the copy offered anyway. The
 * files end up where they are needed, which is the point; the leftovers are
 * visible rather than lost.
 */
static void
test_copy_only(void)
{
	folder_move_facts f = good();
	folder_move_reason why;

	puts("A source that cannot be emptied afterwards:");

	f.source_writable = 0;
	check("the copy is still offered",
	    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_COPY_ONLY);
	check("with the reason the move is not",
	    why == FOLDER_MOVE_SOURCE_READ_ONLY);
}

static void
test_space(void)
{
	folder_move_reason why;

	puts("Free space, with a margin, because filling a disc is its own failure:");

	check("plenty of room is enough",
	    folder_move_space_is_enough(100ull * 1024 * 1024,
	        900ull * 1024 * 1024));
	check("an exact fit is NOT enough - it would leave nothing spare",
	    !folder_move_space_is_enough(100ull * 1024 * 1024,
	        100ull * 1024 * 1024));
	check("nor is a hair over, below the 16 MB floor",
	    !folder_move_space_is_enough(100ull * 1024 * 1024,
	        100ull * 1024 * 1024 + 1024));
	check("5% clear of a large copy is enough",
	    folder_move_space_is_enough(10000ull * 1024 * 1024,
	        10600ull * 1024 * 1024));
	check("but 1% clear of a large copy is not",
	    !folder_move_space_is_enough(10000ull * 1024 * 1024,
	        10100ull * 1024 * 1024));

	/* The floor. A percentage of a small disc is no margin at all. */
	check("a small copy still needs the 16 MB floor",
	    !folder_move_space_is_enough(1024 * 1024, 2ull * 1024 * 1024));
	check("and passes once there is that much room",
	    folder_move_space_is_enough(1024 * 1024, 32ull * 1024 * 1024));

	/* ★ Unknown sizes must not refuse. A caller that cannot measure should not
	   have the feature taken away, and this is said in the header too. */
	check("both zero means 'not measured' and does not refuse",
	    folder_move_space_is_enough(0, 0));

	/* Overflow: needed + margin must not wrap and turn far-too-big into fine. */
	check("a size near the top of the range does not wrap into 'plenty'",
	    !folder_move_space_is_enough(0xffffffffffffffffull, 1024));

	{
		folder_move_facts f = good();

		f.bytes_needed = 500ull * 1024 * 1024;
		f.bytes_free = 100ull * 1024 * 1024;
		check("a full destination refuses the whole offer",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_NOTHING);
		check("named as such", why == FOLDER_MOVE_NO_SPACE);

		f.bytes_needed = 0;
		f.bytes_free = 0;
		check("and an unmeasured pair does not",
		    folder_move_decide(&f, &why) == FOLDER_MOVE_OFFER_BOTH);
	}
}

/*
 * Every reason must have something to show. A dialog that explains nothing is
 * how the original complaint started.
 */
static void
test_every_reason_says_something(void)
{
	static const folder_move_reason all[] = {
		FOLDER_MOVE_OK,
		FOLDER_MOVE_SAME_PLACE,
		FOLDER_MOVE_SOURCE_EMPTY,
		FOLDER_MOVE_SOURCE_MISSING,
		FOLDER_MOVE_DEST_NOT_EMPTY,
		FOLDER_MOVE_DEST_NOT_A_DIR,
		FOLDER_MOVE_NO_SPACE,
		FOLDER_MOVE_IN_USE,
		FOLDER_MOVE_DEST_READ_ONLY,
		FOLDER_MOVE_SOURCE_READ_ONLY,
	};
	size_t i;
	int all_have_text = 1;

	puts("Every reason can be shown to somebody:");

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const char *text = folder_move_reason_text(all[i]);

		if (text == NULL || text[0] == '\0') {
			all_have_text = 0;
		}
	}
	check("all ten reasons have wording", all_have_text);
	check("and so does a value that is not one of them, rather than NULL",
	    folder_move_reason_text((folder_move_reason) 999) != NULL);

	/* The two that must carry the specific warning, since they are the ones
	   people will meet. */
	check("the in-use reason says to close the machine first",
	    strstr(folder_move_reason_text(FOLDER_MOVE_IN_USE), "Close it") != NULL);
	check("the not-empty reason explains why files are not mixed",
	    strstr(folder_move_reason_text(FOLDER_MOVE_DEST_NOT_EMPTY),
	        "will not mix") != NULL);
}

int
main(void)
{
	test_the_ordinary_case();
	test_refusals();
	test_copy_only();
	test_space();
	test_every_reason_says_something();

	if (failures != 0) {
		printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
		return EXIT_FAILURE;
	}
	puts("\nall folder move checks passed");
	return EXIT_SUCCESS;
}
