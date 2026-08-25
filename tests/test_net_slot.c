/*
 * The slot that gives each running machine its own network address.
 *
 * Every instance used to run its own NAT with the same constants, so every guest
 * was handed 10.10.10.10 and the same hardcoded MAC address. Two machines were
 * then two hosts with one address, which no amount of carrying frames between
 * them can fix. A slot is a small number this process holds exclusively, and the
 * addresses are derived from it.
 *
 * The contract, and what is checked below, is that a slot is exclusive while it
 * is held and released when it is given up. That is deliberately phrased without
 * reference to which number comes out: the slots are claimed as loopback ports on
 * the host, so any other RPCEmu running on this machine - the user's own, during
 * a test run - legitimately holds some of them. A test asserting "the first slot
 * is 0" would pass alone and fail whenever the developer had a machine open,
 * which is the kind of test that gets deleted rather than fixed.
 *
 * net_slot.c is compiled in rather than linked, for the port base.
 *
 * See src/net_slot.h and docs/multi-machine.md.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Before net_slot.c, which expects the sockets layer to be in scope. */
#include "socket-compat.h"

/* The unit under test, for NET_SLOT_PORT_BASE. Its only other dependency is
   rpclog(), stubbed below. */
#include "net_slot.c"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* net_slot.c logs when it takes a slot above 0. */
void
rpclog(const char *format, ...)
{
	(void) format;
}

/**
 * Bind the loopback port belonging to a slot, as another instance would.
 *
 * Calls the unit's own claim_slot() rather than reimplementing the bind, which
 * matters more than it looks: a copy of the bind here can only test the copy. A
 * first draft of this test did exactly that, and when claim_slot() was
 * deliberately broken by giving it SO_REUSEADDR - the one mistake the comment in
 * net_slot.c warns about, and the one that let every instance believe it owned
 * the same slot - the test carried on passing, because the second binder in the
 * test had no SO_REUSEADDR and so still collided. Going through claim_slot()
 * means both binders are the code under test, and the mutation is caught.
 *
 * claim_slot() writes the module's own slot_socket, so it is saved and put back:
 * this must not disturb whatever the module is holding.
 *
 * @return 1 if the slot was free. It is given straight back.
 */
static int
slot_is_free(int candidate)
{
	slot_socket_t saved = slot_socket;
	int got;

	slot_socket = SLOT_INVALID_SOCKET;
	got = claim_slot(candidate);
	if (got) {
		slot_closesocket(slot_socket);
	}
	slot_socket = saved;
	return got;
}

/**
 * Hold a slot's port for as long as the caller wants, the way a second emulator
 * would. Returns the socket to close, or an invalid one if the slot was taken.
 */
static slot_socket_t
hold_slot(int candidate)
{
	slot_socket_t saved = slot_socket;
	slot_socket_t held = SLOT_INVALID_SOCKET;

	slot_socket = SLOT_INVALID_SOCKET;
	if (claim_slot(candidate)) {
		held = slot_socket;
	}
	slot_socket = saved;
	return held;
}

int
main(void)
{
	int first;
	int again;

#ifdef _WIN32
	{
		WSADATA wsadata;

		if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
			printf("WSAStartup failed\n");
			return 1;
		}
	}
#endif

	printf("nothing is claimed until it is asked for\n");
	check("net_slot_get() says -1 before acquiring", net_slot_get() == -1);

	printf("\nacquiring gives a slot in range\n");
	first = net_slot_acquire();
	printf("      (slot %d - any other RPCEmu on this host holds the ones below)\n",
	    first);
	check("the slot is within the range", first >= 0 && first < NET_SLOT_MAX);
	check("net_slot_get() agrees", net_slot_get() == first);

	printf("\nasking twice does not claim twice\n");
	/* Called from the MAC address code and the DHCP code, which both want the
	   same answer; a second claim would take a second slot and leave the guest
	   with a MAC and an IP address from different ones. */
	again = net_slot_acquire();
	check("the same slot comes back", again == first);
	check("net_slot_get() is unchanged", net_slot_get() == first);

	printf("\nwhile held, nobody else can have it\n");
	/* The reason there is no SO_REUSEADDR in the unit: the second binder has to
	   fail, or every instance believes it owns the slot. */
	check("a second claim of that slot fails", slot_is_free(first) == 0);

	printf("\nreleasing gives it back\n");
	net_slot_release();
	check("net_slot_get() says -1 again", net_slot_get() == -1);
	check("the slot can now be claimed by someone else",
	    slot_is_free(first) == 1);
	/* An error path on the way out may do this twice. */
	net_slot_release();
	check("releasing again is harmless", net_slot_get() == -1);

	printf("\na slot that is taken is stepped over\n");
	/* Hold the slot that was just released, then acquire: the unit must walk
	   past it rather than failing or handing out a slot it does not own. */
	{
		slot_socket_t held = hold_slot(first);
		int next;

		if (held == SLOT_INVALID_SOCKET) {
			printf("  cannot hold slot %d to test the walk\n", first);
			failures++;
		} else {
			next = net_slot_acquire();
			check("a different slot is returned", next != first);
			check("and it is still in range",
			    next >= 0 && next < NET_SLOT_MAX);
			/* It really owns the one it claimed, not just a number. */
			check("the new slot is genuinely held",
			    slot_is_free(next) == 0);
			net_slot_release();
			slot_closesocket(held);
		}
	}

	printf("\nthe addresses a slot produces stay inside their ranges\n");
	/* The derivation lives at its two call sites (network-nat.c for the NAT's
	   DHCP address and MAC), and both are
	   only safe because NET_SLOT_MAX is small. Raising it without looking would
	   take the guest address out of the /24 or carry the MAC's last byte
	   into the next one, so the bound is checked here where it is defined.
	   Written as run-time checks rather than #error so the numbers are visible
	   in the output. */
	check("the highest guest address stays inside the /24",
	    (0x0au + (unsigned) (NET_SLOT_MAX - 1)) <= 0xfeu);
	check("the highest MAC last byte does not carry",
	    (0x06u + (unsigned) (NET_SLOT_MAX - 1)) <= 0xffu);

#ifdef _WIN32
	WSACleanup();
#endif

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
