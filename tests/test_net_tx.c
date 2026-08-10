/*
 * network_nat_tx() against mbuf chains a guest can actually build.
 *
 * The transmit path walks a chain of mbufs that live in emulated RAM, so every
 * field in them - including each segment's length and the link to the next -
 * is guest data. Two things follow, and neither was handled:
 *
 *   1. The running total was a uint32_t. A segment length near 2^32 wraps it
 *      back to a small number, so the "packet too large" check passes, and
 *      memcpytohost() is then asked to copy that length into a fixed 2048-byte
 *      buffer. Checking the total is not enough on its own either: it does not
 *      bound the copy that follows it, so each segment has to be bounded in its
 *      own right.
 *
 *   2. The chain was followed to its end, and a guest can make a chain with no
 *      end. A cycle of zero-length segments never grows the total, so the size
 *      check cannot break out of it: the emulator span for ever with nothing in
 *      the log.
 *
 * The guest address space here is a plain array, which is enough to build
 * chains in. The stub for memcpytohost() copies what it is asked for, clamped
 * only to the size of that array - deliberately, because a stub that silently
 * bounded the copy to the destination would hide the very defect being tested.
 *
 * Note that the sanitisers do not report the overrun on their own, which is
 * part of why it went unnoticed: tx_buffer is a member of the file-static
 * struct that holds the whole NAT state, so writing past it stays inside that
 * one object and trips no redzone. What it lands in is the rest of that
 * struct - first the capture file handle, and then the received-packet queue.
 * A guest that can choose how far the copy runs can therefore put a value of
 * its choosing into a FILE * that the transmit path later hands to fwrite().
 *
 * So the assertions here are on behaviour, not on a crash: an over-long
 * segment must be refused and nothing must go out. Before the fix the frame
 * was accepted and transmitted, and the circular chain did not return at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rpcemu.h"
#include "network.h"
#include "network-nat.h"

/* ---- a guest address space ------------------------------------------- */

#define GUEST_RAM_SIZE	65536u

static uint8_t guest_ram[GUEST_RAM_SIZE];

/* The error buffer the guest passes in, as a guest address. */
#define ERRBUF_ADDR	0x100u
/* Where mbufs are built. */
#define MBUF_BASE	0x1000u

static int failures;

static void
check(int cond, const char *what)
{
	printf("%s: %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) {
		failures++;
	}
}

/* ---- the emulator side, stubbed -------------------------------------- */

void
memcpytohost(void *dest, uint32_t src, uint32_t len)
{
	size_t n = len;

	/* Clamp to the guest RAM we have, not to the destination: see the note
	   at the top. An over-long request still writes past the destination,
	   which is the behaviour under test. */
	if (src >= GUEST_RAM_SIZE) {
		return;
	}
	if (n > GUEST_RAM_SIZE - src) {
		n = GUEST_RAM_SIZE - src;
	}
	memcpy(dest, guest_ram + src, n);
}

void
memcpyfromhost(uint32_t dest, const void *source, uint32_t len)
{
	if (dest >= GUEST_RAM_SIZE) {
		return;
	}
	if (len > GUEST_RAM_SIZE - dest) {
		len = GUEST_RAM_SIZE - dest;
	}
	memcpy(guest_ram + dest, source, len);
}

void
strcpyfromhost(uint32_t dest, const char *source)
{
	memcpyfromhost(dest, source, (uint32_t) strlen(source) + 1);
}

/* Nothing below this point participates in the behaviour under test. */
void rpclog(const char *fmt, ...) { (void) fmt; }
void savestate_write_u32(FILE *f, uint32_t v) { (void) f; (void) v; }
uint32_t savestate_read_u32(FILE *f) { (void) f; return 0; }
Config config;
PortForwardRule port_forward_rules[MAX_PORT_FORWARDS];
unsigned char network_hwaddr[6] = { 0x00, 0x02, 0x07, 0x06, 0x00, 0x00 };
podule *network_poduleinfo;
void network_irq_raise(void) { }
void network_irq_lower(void) { }
int network_macaddress_parse(const char *s, uint8_t *m) { (void) s; (void) m; return 0; }
int net_slot_acquire(void) { return 0; }
void net_slot_release(void) { }
int app_settings_relay_enabled(void) { return 0; }
int broadcast_relay_init(void) { return 0; }
void broadcast_relay_close(void) { }
void broadcast_relay_poll(void) { }

/* Returns zero: "not taken", so the real path would hand the frame to SLiRP.
   The SLiRP stub below records it instead. */
int broadcast_relay_tx(const uint8_t *pkt, int pkt_len) { (void) pkt; (void) pkt_len; return 0; }

static size_t sent_len;
static int    sent_count;

void
slirp_input(void *slirp, const uint8_t *pkt, int pkt_len)
{
	(void) slirp;
	(void) pkt;
	sent_len = (size_t) pkt_len;
	sent_count++;
}

void *slirp_init(int a, uint32_t b, uint32_t c, uint32_t d, const char *e,
                 uint32_t f, uint32_t g, void *h)
{
	(void) a; (void) b; (void) c; (void) d; (void) e; (void) f; (void) g; (void) h;
	return NULL;
}
void slirp_select_fill(void *s, int *n, void *r, void *w, void *x) { (void) s; (void) n; (void) r; (void) w; (void) x; }
void slirp_select_poll(void *s, void *r, void *w, void *x, int p) { (void) s; (void) r; (void) w; (void) x; (void) p; }
int slirp_add_hostfwd(void *s, int a, uint32_t b, int c, uint32_t d, int e) { (void) s; (void) a; (void) b; (void) c; (void) d; (void) e; return 0; }
int slirp_remove_hostfwd(void *s, int a, uint32_t b, int c) { (void) s; (void) a; (void) b; (void) c; return 0; }

/* ---- building chains -------------------------------------------------- */

/*
 * An mbuf as the guest lays it out; the fields the transmit path reads are
 * m_next, m_off and m_len. Written little-endian, as the emulator reads them.
 */
static void
put_mbuf(uint32_t addr, uint32_t next, uint32_t off, uint32_t len)
{
	struct ro_mbuf_part m;

	memset(&m, 0, sizeof(m));
	m.m_next = next;
	m.m_off = off;
	m.m_len = len;
	memcpy(guest_ram + addr, &m, sizeof(m));
}

/* Was an error reported? The guest gets the address of its own buffer back. */
static int
reported_error(uint32_t r)
{
	return r == ERRBUF_ADDR && guest_ram[ERRBUF_ADDR] != '\0';
}

static const char *
error_text(void)
{
	return (const char *) (guest_ram + ERRBUF_ADDR);
}

static void
reset(void)
{
	memset(guest_ram, 0, sizeof(guest_ram));
	sent_len = 0;
	sent_count = 0;
}

int
main(void)
{
	uint32_t r;

	/* One of the cases below used to hang rather than fail. Unbuffered, so
	   the checks that did run are on record even if the process has to be
	   killed. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* --- an ordinary two-segment packet still goes out ---------------- */
	reset();
	/* Payload bytes for the two segments, at offsets from the mbuf address. */
	memset(guest_ram + MBUF_BASE + 0x40, 'A', 32);
	memset(guest_ram + MBUF_BASE + 0x200 + 0x40, 'B', 16);
	put_mbuf(MBUF_BASE,         MBUF_BASE + 0x200, 0x40, 32);
	put_mbuf(MBUF_BASE + 0x200, 0,                 0x40, 16);

	r = network_nat_tx(ERRBUF_ADDR, MBUF_BASE, 0, 0, 0x0800);
	check(r == 0, "an ordinary chain is accepted");
	check(sent_count == 1, "and is handed on once");
	check(sent_len == 14u + 32u + 16u,
	      "with the header and both segments in it");

	/* --- a segment length that wraps a 32-bit accumulator -------------- */
	reset();
	/* 14 + 0xfffffff8 wraps to 6, which is under any buffer size, so a
	   check on the running total alone lets this through. */
	put_mbuf(MBUF_BASE, 0, 0x40, 0xfffffff8u);

	r = network_nat_tx(ERRBUF_ADDR, MBUF_BASE, 0, 0, 0x0800);
	check(reported_error(r), "a segment length that wraps the total is rejected");
	printf("    (reported: %s)\n", reported_error(r) ? error_text() : "nothing");
	check(sent_count == 0, "and nothing is transmitted");

	/* --- a segment that is merely too big ------------------------------ */
	reset();
	put_mbuf(MBUF_BASE, 0, 0x40, 40000u);

	r = network_nat_tx(ERRBUF_ADDR, MBUF_BASE, 0, 0, 0x0800);
	check(reported_error(r), "an oversized segment is rejected");
	check(sent_count == 0, "and nothing is transmitted");

	/* --- a total that is too big, in segments that each fit ------------ */
	reset();
	put_mbuf(MBUF_BASE,         MBUF_BASE + 0x200, 0x40, 1500);
	put_mbuf(MBUF_BASE + 0x200, 0,                 0x40, 1500);

	r = network_nat_tx(ERRBUF_ADDR, MBUF_BASE, 0, 0, 0x0800);
	check(reported_error(r), "segments that fit individually but not together are rejected");
	check(sent_count == 0, "and nothing is transmitted");

	/* --- a circular chain ---------------------------------------------
	 * Zero-length segments, so the running total never grows and the size
	 * check can never break the cycle. Without a hop cap this call does not
	 * return, so reaching the next line at all is the result.
	 */
	reset();
	put_mbuf(MBUF_BASE,         MBUF_BASE + 0x200, 0x40, 0);
	put_mbuf(MBUF_BASE + 0x200, MBUF_BASE,         0x40, 0);

	r = network_nat_tx(ERRBUF_ADDR, MBUF_BASE, 0, 0, 0x0800);
	check(reported_error(r), "a circular chain is reported rather than followed for ever");
	printf("    (reported: %s)\n", reported_error(r) ? error_text() : "nothing");
	check(sent_count == 0, "and nothing is transmitted");

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
