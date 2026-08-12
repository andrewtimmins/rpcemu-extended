/*
 * The structure offsets the guest network driver hardcodes.
 *
 * riscos-progs/EtherRPCEm/etherrpcem.s is a hand-written assembler port of what
 * used to be C (c.Module, built inside a guest with the Acorn DDE). The C got
 * its structure layouts from Acorn's DCI4 headers by including them; assembler
 * cannot, so every offset is written out as an equate - and a wrong one does not
 * fail to assemble. It shows up as a data abort part way through a boot, or as
 * frames delivered to the wrong protocol module, which is a long way from the
 * mistake.
 *
 * So this reads the equates back out of the assembler source and checks them.
 * Two of the structures are checked against the emulator's own declarations in
 * src/network.h, which is the strongest check available: those two cross the
 * SWI boundary, so the host and the guest must agree about them or nothing
 * works. The rest are checked against models written out below, which were
 * verified field by field against Acorn's own DCI4 headers by compiling them
 * with `gcc -m32` - where a pointer, a long, a size_t and a ptrdiff_t are all
 * four bytes, as they are on the RISC OS target. That cannot be done here,
 * because this test has to compile on 64-bit hosts and on macOS, where -m32 does
 * not exist at all. Those headers used to sit in riscos-progs/EtherRPCEm/h/ and
 * were removed with the rest of the C once the port was done; they are in git
 * history at 9e44930 (`git show 9e44930:riscos-progs/EtherRPCEm/h/dcistructs`),
 * which is where to look if a model below is ever in doubt.
 *
 * Alignment is checked as well as position: the assembler clears the workspace,
 * a ClaimBuf, an mbctl and a struct stats a word at a time, so a size that is
 * not a multiple of four would run the loop past the end of the block.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host's view of the two structures that cross the private SWI. */
#include "network.h"

#define MAX_EQUATES 256

struct equate {
	char	name[64];
	long	value;
};

static struct equate equates[MAX_EQUATES];
static int equate_count;
static int failures;

/* ------------------------------------------------------------------------- */
/* Models of the guest-only structures. The h/ files named below are Acorn's */
/* DCI4 headers, in git history at 9e44930 (see the top of this file).       */
/* Every field is written with an explicit width so the offsets come out the */
/* same on any host: on the target these are all 32-bit.                     */
/* ------------------------------------------------------------------------- */

/* Dib, from h/dcistructs (dib_slot is a word of bit fields). */
struct model_dib {
	uint32_t	dib_swibase;
	uint32_t	dib_name;
	uint32_t	dib_unit;
	uint32_t	dib_address;
	uint32_t	dib_module;
	uint32_t	dib_location;
	uint32_t	dib_slot;
	uint32_t	dib_inquire;
};

/* ChDib, from h/dcistructs */
struct model_chdib {
	uint32_t	chd_next;
	uint32_t	chd_dib;
};

/* ClaimBuf, from h/Structs */
struct model_claim {
	uint32_t	flags;
	uint32_t	unit;
	uint32_t	frame_type;
	uint32_t	frame_level;
	uint32_t	address_level;
	uint32_t	error_level;
	uint32_t	handler;
	uint32_t	pwp;
	uint32_t	next;
	uint32_t	prev;
};

/* dci4_mbctl, from h/mbuf_c: twelve words the client and the manager fill in,
   then a table of twenty-two routine pointers. Only alloc_s and freem are
   called, but their offsets depend on every entry before them, so the whole
   table is modelled. */
struct model_mbctl {
	uint32_t	opaque;
	uint32_t	mbcsize;
	uint32_t	mbcvers;
	uint32_t	flags;
	uint32_t	advminubs;
	uint32_t	advmaxubs;
	uint32_t	mincontig;
	uint32_t	spare1;
	uint32_t	minubs;
	uint32_t	maxubs;
	uint32_t	maxcontig;
	uint32_t	spare2;
	uint32_t	alloc;
	uint32_t	alloc_g;
	uint32_t	alloc_u;
	uint32_t	alloc_s;
	uint32_t	alloc_c;
	uint32_t	ensure_safe;
	uint32_t	ensure_contig;
	uint32_t	free;
	uint32_t	freem;
	uint32_t	dtom_free;
	uint32_t	dtom_freem;
	uint32_t	dtom;
	uint32_t	any_unsafe;
	uint32_t	this_unsafe;
	uint32_t	count_bytes;
	uint32_t	cat;
	uint32_t	trim;
	uint32_t	copy;
	uint32_t	copy_p;
	uint32_t	copy_u;
	uint32_t	import;
	uint32_t	export;
};

/* struct stats, from h/dcistructs: three bytes of general information and a
   pad, then counters. */
struct model_stats {
	uint8_t		st_interface_type;
	uint8_t		st_link_status;
	uint8_t		st_link_polarity;
	uint8_t		st_blank1;
	uint32_t	st_link_failures;
	uint32_t	st_network_collisions;
	uint32_t	st_collisions;
	uint32_t	st_excess_collisions;
	uint32_t	st_heartbeat_failures;
	uint32_t	st_not_listening;
	uint32_t	st_tx_frames;
	uint32_t	st_tx_bytes;
	uint32_t	st_tx_general_errors;
	uint8_t		st_last_dest_addr[8];
	uint32_t	st_crc_failures;
	uint32_t	st_frame_alignment_errors;
	uint32_t	st_dropped_frames;
	uint32_t	st_runt_frames;
	uint32_t	st_overlong_frames;
	uint32_t	st_jabbers;
	uint32_t	st_late_events;
	uint32_t	st_unwanted_frames;
	uint32_t	st_rx_frames;
	uint32_t	st_rx_bytes;
	uint32_t	st_rx_general_errors;
	uint8_t		st_last_src_addr[8];
};

/* struct mbuf, from h/mbuf_c. The first six words are src/network.h's
   ro_mbuf_part, which is what the host reads and writes; m_type and the
   manager's three private bytes are only the guest's business. */
struct model_mbuf {
	uint32_t	m_next;
	uint32_t	m_list;
	uint32_t	m_off;
	uint32_t	m_len;
	uint32_t	m_inioff;
	uint32_t	m_inilen;
	uint8_t		m_type;
	uint8_t		sys[3];
};

/* ------------------------------------------------------------------------- */
/* Reading the equates out of the assembler source                          */
/* ------------------------------------------------------------------------- */

static void
check(const char *what, int ok)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

static const struct equate *
find_equate(const char *name)
{
	int i;

	for (i = 0; i < equate_count; i++) {
		if (strcmp(equates[i].name, name) == 0) {
			return &equates[i];
		}
	}
	return NULL;
}

/*
 * Evaluate the right-hand side of an equate.
 *
 * The source writes offsets as plain numbers but sizes and flag words as
 * expressions - "WS_ERRMESS + ERRMESS_SIZE", "1 << 15", "A | B". Rather than
 * duplicate those values here, where they could drift, the few operators the
 * source actually uses are evaluated: + - | << and parentheses, over decimal
 * and hex literals and equates already seen. Anything else is reported rather
 * than guessed at, so a new kind of expression cannot silently read as zero.
 */
static int parse_error;

/* Set when an expression refers to something lower case, which in this source
   means an assembler label rather than a constant - DIB_NAME_LEN is the length
   of a string, measured as the distance between two labels. Only the assembler
   can work those out, so such an equate is passed over rather than failed,
   while an unknown SHOUTED name stays a failure: that is the case where a
   constant this test is meant to be checking has been renamed or lost. */
static int parse_saw_label;

static long parse_expr(const char **p);

static void
skip_spaces(const char **p)
{
	while (**p == ' ' || **p == '\t') {
		(*p)++;
	}
}

static long
parse_primary(const char **p)
{
	skip_spaces(p);

	if (**p == '(') {
		long v;

		(*p)++;
		v = parse_expr(p);
		skip_spaces(p);
		if (**p == ')') {
			(*p)++;
		} else {
			parse_error = 1;
		}
		return v;
	}

	if ((**p >= '0' && **p <= '9')) {
		char *end;
		long v = strtol(*p, &end, 0);

		*p = end;
		return v;
	}

	if ((**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z') || **p == '_') {
		char name[64];
		size_t n = 0;
		const struct equate *e;

		while (((**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z') ||
		        (**p >= '0' && **p <= '9') || **p == '_') && n < sizeof(name) - 1)
		{
			name[n++] = *(*p)++;
		}
		name[n] = '\0';

		e = find_equate(name);
		if (e == NULL) {
			if (name[0] >= 'a' && name[0] <= 'z') {
				parse_saw_label = 1;
			} else {
				parse_error = 1;
			}
			return 0;
		}
		return e->value;
	}

	parse_error = 1;
	return 0;
}

static long
parse_shift(const char **p)
{
	long v = parse_primary(p);

	for (;;) {
		skip_spaces(p);
		if ((*p)[0] == '<' && (*p)[1] == '<') {
			*p += 2;
			v = v << parse_primary(p);
		} else {
			return v;
		}
	}
}

static long
parse_expr(const char **p)
{
	long v = parse_shift(p);

	for (;;) {
		skip_spaces(p);
		if (**p == '+') {
			(*p)++;
			v += parse_shift(p);
		} else if (**p == '-') {
			(*p)++;
			v -= parse_shift(p);
		} else if (**p == '|') {
			(*p)++;
			v |= parse_shift(p);
		} else {
			return v;
		}
	}
}

/**
 * Read every "NAME = expression" line out of the assembler source.
 *
 * @param path etherrpcem.s
 * @return 0 on success, 1 if the file could not be read
 */
static int
load_equates(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		const char *p = line;
		size_t n = 0;
		char *comment;
		long value;

		/* Trim the assembler's comment, so "= 4 @ note" parses. */
		comment = strchr(line, '@');
		if (comment != NULL) {
			*comment = '\0';
		}

		skip_spaces(&p);
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')) {
			continue;
		}
		/* Mixed case, because some equates are not shouted: Net_SWI and the
		   X-form SWI names among them. Labels and instructions are read as
		   names too and then dropped below, having no "=" after them. */
		while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		        (*p >= '0' && *p <= '9') || *p == '_') && n < sizeof(name) - 1)
		{
			name[n++] = *p++;
		}
		name[n] = '\0';

		skip_spaces(&p);
		if (*p != '=') {
			continue;
		}
		p++;

		parse_error = 0;
		parse_saw_label = 0;
		value = parse_expr(&p);
		skip_spaces(&p);
		if (parse_saw_label) {
			continue;	/* measured between labels; see parse_saw_label */
		}
		if (parse_error || (*p != '\0' && *p != '\n' && *p != '\r')) {
			/* An expression this test cannot read is a failure, not
			   something to skip: skipping it would quietly stop checking
			   whatever it defines. */
			printf("  cannot evaluate the equate for %-33s FAIL\n", name);
			failures++;
			continue;
		}

		if (equate_count == MAX_EQUATES) {
			printf("  more than %d equates                          FAIL\n",
			       MAX_EQUATES);
			failures++;
			break;
		}
		strcpy(equates[equate_count].name, name);
		equates[equate_count].value = value;
		equate_count++;
	}

	fclose(f);
	return 0;
}

/**
 * Check one equate against the offset it is supposed to be.
 */
static void
check_offset(const char *name, size_t want)
{
	const struct equate *e = find_equate(name);
	char what[128];

	if (e == NULL) {
		snprintf(what, sizeof(what), "%s is defined", name);
		check(what, 0);
		return;
	}

	snprintf(what, sizeof(what), "%s = %ld (want %zu)", name, e->value, want);
	check(what, e->value == (long) want);
}

static long
value_of(const char *name)
{
	const struct equate *e = find_equate(name);

	return (e == NULL) ? -1 : e->value;
}

int
main(int argc, char **argv)
{
	const char *path;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path to etherrpcem.s>\n", argv[0]);
		return 1;
	}
	path = argv[1];

	printf("EtherRPCEm structure offsets, read from %s\n", path);
	if (load_equates(path) != 0) {
		return 1;
	}
	printf("  %d equates read\n", equate_count);
	if (equate_count == 0) {
		printf("  no equates found at all                                    FAIL\n");
		return 1;
	}

	/* --- the two structures the host and the guest share --- */
	printf("\nstruct mbuf, against src/network.h's ro_mbuf_part:\n");
	check_offset("MBUF_NEXT", offsetof(struct ro_mbuf_part, m_next));
	check_offset("MBUF_LIST", offsetof(struct ro_mbuf_part, m_list));
	check_offset("MBUF_OFF", offsetof(struct ro_mbuf_part, m_off));
	check_offset("MBUF_LEN", offsetof(struct ro_mbuf_part, m_len));
	check_offset("MBUF_INIOFF", offsetof(struct ro_mbuf_part, m_inioff));
	check_offset("MBUF_INILEN", offsetof(struct ro_mbuf_part, m_inilen));
	check_offset("MBUF_TYPE", offsetof(struct model_mbuf, m_type));
	check_offset("MBUF_SIZE", sizeof(struct model_mbuf));

	printf("\nRxHdr, against src/network.h's rx_hdr (the host fills it in):\n");
	check_offset("RXHDR_PTR", offsetof(struct rx_hdr, rx_ptr));
	check_offset("RXHDR_TAG", offsetof(struct rx_hdr, rx_tag));
	check_offset("RXHDR_SRC_ADDR", offsetof(struct rx_hdr, rx_src_addr));
	check_offset("RXHDR_DST_ADDR", offsetof(struct rx_hdr, rx_dst_addr));
	check_offset("RXHDR_FRAME_TYPE", offsetof(struct rx_hdr, rx_frame_type));
	check_offset("RXHDR_ERROR_LEVEL", offsetof(struct rx_hdr, rx_error_level));
	check_offset("RXHDR_CKSUM", offsetof(struct rx_hdr, rx_cksum));
	check_offset("RXHDR_SIZE", sizeof(struct rx_hdr));

	/* --- the guest-only structures --- */
	printf("\nDib, the block protocol modules are handed:\n");
	check_offset("DIB_SWIBASE", offsetof(struct model_dib, dib_swibase));
	check_offset("DIB_NAME", offsetof(struct model_dib, dib_name));
	check_offset("DIB_UNIT", offsetof(struct model_dib, dib_unit));
	check_offset("DIB_ADDRESS", offsetof(struct model_dib, dib_address));
	check_offset("DIB_MODULE", offsetof(struct model_dib, dib_module));
	check_offset("DIB_LOCATION", offsetof(struct model_dib, dib_location));
	check_offset("DIB_SLOT", offsetof(struct model_dib, dib_slot));
	check_offset("DIB_INQUIRE", offsetof(struct model_dib, dib_inquire));
	check_offset("DIB_SIZE", sizeof(struct model_dib));

	printf("\nChDib, one link of the enumeration chain:\n");
	check_offset("CHDIB_NEXT", offsetof(struct model_chdib, chd_next));
	check_offset("CHDIB_DIB", offsetof(struct model_chdib, chd_dib));
	check_offset("CHDIB_SIZE", sizeof(struct model_chdib));

	printf("\nClaimBuf, one protocol module's claim:\n");
	check_offset("CLAIM_FLAGS", offsetof(struct model_claim, flags));
	check_offset("CLAIM_UNIT", offsetof(struct model_claim, unit));
	check_offset("CLAIM_FRAME_TYPE", offsetof(struct model_claim, frame_type));
	check_offset("CLAIM_FRAME_LEVEL", offsetof(struct model_claim, frame_level));
	check_offset("CLAIM_ADDRESS_LEVEL", offsetof(struct model_claim, address_level));
	check_offset("CLAIM_ERROR_LEVEL", offsetof(struct model_claim, error_level));
	check_offset("CLAIM_HANDLER", offsetof(struct model_claim, handler));
	check_offset("CLAIM_PWP", offsetof(struct model_claim, pwp));
	check_offset("CLAIM_NEXT", offsetof(struct model_claim, next));
	check_offset("CLAIM_PREV", offsetof(struct model_claim, prev));
	check_offset("CLAIM_SIZE", sizeof(struct model_claim));

	printf("\ndci4_mbctl, and the two routines in it that are called:\n");
	check_offset("MBCTL_OPAQUE", offsetof(struct model_mbctl, opaque));
	check_offset("MBCTL_MBCSIZE", offsetof(struct model_mbctl, mbcsize));
	check_offset("MBCTL_MBCVERS", offsetof(struct model_mbctl, mbcvers));
	check_offset("MBCTL_FLAGS", offsetof(struct model_mbctl, flags));
	check_offset("MBCTL_ALLOC_S", offsetof(struct model_mbctl, alloc_s));
	check_offset("MBCTL_FREEM", offsetof(struct model_mbctl, freem));
	check_offset("MBCTL_SIZE", sizeof(struct model_mbctl));

	printf("\nstruct stats, as the Stats SWI returns it:\n");
	check_offset("ST_INTERFACE_TYPE", offsetof(struct model_stats, st_interface_type));
	check_offset("ST_LINK_STATUS", offsetof(struct model_stats, st_link_status));
	check_offset("ST_LINK_POLARITY", offsetof(struct model_stats, st_link_polarity));
	check_offset("ST_TX_FRAMES", offsetof(struct model_stats, st_tx_frames));
	check_offset("ST_UNWANTED_FRAMES", offsetof(struct model_stats, st_unwanted_frames));
	check_offset("ST_RX_FRAMES", offsetof(struct model_stats, st_rx_frames));
	check_offset("ST_SIZE", sizeof(struct model_stats));

	/*
	 * The workspace is the module's own, so there is nothing to check it
	 * against - but its fields must not overlap, and the blocks the assembler
	 * clears a word at a time must be whole numbers of words.
	 */
	printf("\nthe workspace, and what the word-at-a-time clears assume:\n");
	check("the Dib fits before the hardware address",
	      value_of("WS_DIB") + value_of("DIB_SIZE") <= value_of("WS_DEV_ADDR"));
	check("the hardware address fits before the flags",
	      value_of("WS_DEV_ADDR") + 6 <= value_of("WS_FLAGS"));
	check("the error number sits immediately before its text",
	      value_of("WS_ERRMESS") - value_of("WS_ERR") == 4);
	check("the error block is no larger than RISC OS allows",
	      value_of("ERRMESS_SIZE") + 4 <= 256);
	check("the mbctl block fits inside the workspace",
	      value_of("WS_MBCTL_BLK") + value_of("MBCTL_SIZE") <= value_of("WORKSPACE_SIZE"));
	check("the scratch space fits before the mbctl block",
	      value_of("WS_SCRATCH") + value_of("SCRATCH_SIZE") <= value_of("WS_MBCTL_BLK"));
	check("WORKSPACE_SIZE is a whole number of words",
	      value_of("WORKSPACE_SIZE") % 4 == 0);
	check("MBCTL_SIZE is a whole number of words",
	      value_of("MBCTL_SIZE") % 4 == 0);
	check("CLAIM_SIZE is a whole number of words",
	      value_of("CLAIM_SIZE") % 4 == 0);
	check("ST_SIZE is a whole number of words",
	      value_of("ST_SIZE") % 4 == 0);

	/*
	 * A couple of values that are not offsets but would be just as quiet if
	 * they were wrong: the SWI chunk is what the DIB advertises to the
	 * Internet stack, and the private SWI is the only way frames move.
	 */
	printf("\nthe numbers the host side agrees with:\n");
	check("the private network SWI is &56AC4", value_of("Net_SWI") == 0x56ac4);
	check("the SWI chunk is &58CC0", value_of("SWI_CHUNK") == 0x58cc0);
	check("the MTU is 1500", value_of("EY_MTU") == 1500);

	printf("\n%s\n", failures ? "FAILED" : "All offsets agree");
	return failures ? 1 : 0;
}
