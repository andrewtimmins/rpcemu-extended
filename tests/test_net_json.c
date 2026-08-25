/*
 * The JSON wire format, shared with every other emulator that speaks it.
 *
 * WHY THIS EXISTS. This is an interoperability format owned by somebody else:
 * https://github.com/gerph/tuntap-json-server decides what it is, and if we
 * disagree with it there is nobody to notice on our side. A frame goes out, the
 * server accepts the line, and the other end quietly makes something different
 * of it - or drops it, which looks exactly like a machine that is not there.
 *
 * The part most easily got wrong is that "data" is the PAYLOAD and not the
 * frame. The server lifts the destination, source and type out of the Ethernet
 * header and sends them as their own fields (tap_jsonserver.py:370-373), so a
 * base64 of the whole frame would be accepted by the JSON parser at the far end
 * and produce a frame with fourteen bytes of rubbish in front of the payload.
 *
 * These check the encoding against that layout byte for byte, and check that a
 * line survives the round trip unchanged.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "socket-compat.h"

#include "net_json.h"

#include "rpcemu.h"

/*
 * The emulator the transport would otherwise reach. Only the encoding and
 * decoding are under test here, so nothing below is called; they exist to let
 * net_json.c link on its own, which is the point - the format can be checked
 * with no emulator, no guest and no server.
 */
Config config;
unsigned char network_hwaddr[6] = { 0x06, 0x02, 0x03, 0x04, 0x05, 0x06 };

int
network_nat_inject_packet(const uint8_t *pkt, int pkt_len)
{
	(void) pkt;
	(void) pkt_len;
	return 0;
}

int
broadcast_relay_tx(const uint8_t *pkt, int pkt_len)
{
	(void) pkt;
	(void) pkt_len;
	return 0;
}

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* dst ff:ff:ff:ff:ff:ff, src 06:02:03:04:05:06, type 0x0800, then a payload. */
static int
build_frame(uint8_t *frame, const char *payload, int payload_len)
{
	memset(frame, 0xff, 6);
	frame[6] = 0x06; frame[7] = 0x02; frame[8] = 0x03;
	frame[9] = 0x04; frame[10] = 0x05; frame[11] = 0x06;
	frame[12] = 0x08;
	frame[13] = 0x00;
	memcpy(frame + 14, payload, (size_t) payload_len);
	return 14 + payload_len;
}

static void
test_encoding(void)
{
	uint8_t frame[1522];
	char line[4096];
	int frame_len, len;

	printf("what goes on the wire\n");

	frame_len = build_frame(frame, "hello", 5);
	len = net_json_encode(frame, frame_len, line, sizeof(line));
	check("a frame encodes", len > 0);
	line[len > 0 ? len - 1 : 0] = '\0';	/* drop the newline for comparison */

	/* The type is decimal, as the server's json.dumps writes it: 0x0800. */
	check("frame_type is the EtherType, in decimal",
	    strstr(line, "\"frame_type\": 2048") != NULL);
	check("src is six integers, source first",
	    strstr(line, "\"src\": [6, 2, 3, 4, 5, 6]") != NULL);
	check("dst is six integers",
	    strstr(line, "\"dst\": [255, 255, 255, 255, 255, 255]") != NULL);

	/* base64("hello") is aGVsbG8=. If this were the whole frame it would begin
	   with the header's bytes and look nothing like it. */
	check("data is the PAYLOAD in base64, not the whole frame",
	    strstr(line, "\"data\": \"aGVsbG8=\"") != NULL);

	check("the line ends with a newline",
	    len > 0 && net_json_encode(frame, frame_len, line, sizeof(line)) == len &&
	    line[len - 1] == '\n');
}

static void
test_round_trip(void)
{
	uint8_t frame[1522], back[1522];
	char line[4096];
	int frame_len, len, back_len;
	int i;

	printf("\nand what comes back off it\n");

	/* Every byte value, so a base64 table mistake in the top or bottom half
	   cannot hide behind printable text. */
	{
		uint8_t payload[256];

		for (i = 0; i < 256; i++) {
			payload[i] = (uint8_t) i;
		}
		frame_len = build_frame(frame, (const char *) payload, 256);
	}

	len = net_json_encode(frame, frame_len, line, sizeof(line));
	check("a frame with every byte value encodes", len > 0);

	back_len = net_json_decode(line, back, sizeof(back));
	check("it decodes to the same length", back_len == frame_len);
	check("and to the same bytes",
	    back_len == frame_len && memcmp(frame, back, (size_t) frame_len) == 0);

	/* Payload lengths either side of a base64 group, where the padding differs. */
	for (i = 1; i <= 4; i++) {
		char what[80];

		frame_len = build_frame(frame, "abcd", i);
		len = net_json_encode(frame, frame_len, line, sizeof(line));
		back_len = net_json_decode(line, back, sizeof(back));
		snprintf(what, sizeof(what), "a %d-byte payload survives the round trip", i);
		check(what, back_len == frame_len &&
		    memcmp(frame, back, (size_t) frame_len) == 0);
	}
}

static void
test_bad_input(void)
{
	uint8_t frame[1522], out[1522];
	char line[4096];

	printf("\nwhat is refused\n");

	/* Anything arriving on this socket came from another program, and a line
	   that cannot be understood must be dropped rather than turned into a
	   frame with whatever happened to be in the buffer. */
	check("a line that is not JSON is refused",
	    net_json_decode("not json at all", out, sizeof(out)) < 0);
	check("a missing data field is refused",
	    net_json_decode("{\"frame_type\": 2048, \"src\": [1,2,3,4,5,6], "
	        "\"dst\": [1,2,3,4,5,6]}", out, sizeof(out)) < 0);
	check("a short MAC is refused",
	    net_json_decode("{\"frame_type\": 2048, \"src\": [1,2,3], "
	        "\"dst\": [1,2,3,4,5,6], \"data\": \"\"}", out, sizeof(out)) < 0);
	check("a MAC byte out of range is refused",
	    net_json_decode("{\"frame_type\": 2048, \"src\": [1,2,3,4,5,300], "
	        "\"dst\": [1,2,3,4,5,6], \"data\": \"\"}", out, sizeof(out)) < 0);
	check("base64 that is not base64 is refused",
	    net_json_decode("{\"frame_type\": 2048, \"src\": [1,2,3,4,5,6], "
	        "\"dst\": [1,2,3,4,5,6], \"data\": \"!!!!\"}", out, sizeof(out)) < 0);

	/* A frame with no payload at all is legitimate: the header is the frame. */
	check("a header with no payload is accepted",
	    net_json_decode("{\"frame_type\": 2048, \"src\": [1,2,3,4,5,6], "
	        "\"dst\": [1,2,3,4,5,6], \"data\": \"\"}", out, sizeof(out)) == 14);

	check("a frame shorter than an Ethernet header will not encode",
	    net_json_encode(frame, 13, line, sizeof(line)) < 0);

	{
		char tiny[16];
		const int frame_len = build_frame(frame, "hello", 5);

		check("encoding refuses a buffer it would overrun",
		    net_json_encode(frame, frame_len, tiny, sizeof(tiny)) < 0);
	}
}

/*
 * Whitespace. json.dumps in the server writes ", " between items, but nothing
 * in the format promises that, and a different client may not. The reader must
 * not depend on it.
 */
static void
test_whitespace(void)
{
	uint8_t out[1522];

	printf("\nspacing the format does not promise\n");

	check("no spaces after the colons",
	    net_json_decode("{\"frame_type\":2048,\"src\":[1,2,3,4,5,6],"
	        "\"dst\":[9,9,9,9,9,9],\"data\":\"aGVsbG8=\"}", out, sizeof(out)) == 19);
	check("and the fields in another order",
	    net_json_decode("{\"data\": \"aGVsbG8=\", \"dst\": [9,9,9,9,9,9], "
	        "\"src\": [1,2,3,4,5,6], \"frame_type\": 2048}", out, sizeof(out)) == 19);
}

/*
 * Reconnecting.
 *
 * A machine used to give up for good the moment the server was unavailable, so
 * one started before its server, or left running across a server restart, was
 * off the network until somebody restarted it.
 *
 * A real listening socket rather than a mock: what is under test is the
 * connect/close/reconnect sequence against a real TCP stack, and the retry
 * interval is deliberately not waited out - the point is that the attempt is
 * made at all, and that the machine stays on the JSON wire while it waits.
 */
static int
open_listener(unsigned short *port_out)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int fd = (int) socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;	/* any free port */

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
	    listen(fd, 4) != 0 ||
	    getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
		closesocket(fd);
		return -1;
	}
	*port_out = ntohs(addr.sin_port);
	return fd;
}

static void
test_reconnect(void)
{
	unsigned short port = 0;
	int listener;

	printf("reconnecting\n");

	listener = open_listener(&port);
	if (listener < 0) {
		check("a listening socket could be opened", 0);
		return;
	}

	/* A server that is not there: the port is one nothing is listening on,
	   which is the machine-started-before-its-server case. */
	closesocket(listener);

	config.json_net_enabled = 1;
	snprintf(config.json_net_host, sizeof(config.json_net_host), "127.0.0.1");
	config.json_net_port = (int) port;

	check("init succeeds even though the server is not there",
	    net_json_init() == 0);
	check("and the machine is not connected", !net_json_is_connected());
	/* ★ The point of the whole change: not connected, but still on this wire,
	   so network-nat.c must not put it on the loopback wire instead. */
	check("but it does still want the connection", net_json_wants_connection());

	/* The server appears. Reopening the same port is what a server restart
	   looks like from here. */
	listener = open_listener(&port);
	if (listener < 0) {
		check("the listener could be reopened", 0);
		return;
	}
	config.json_net_port = (int) port;

	/* The retry is due immediately after a failed init only once the interval
	   has passed, so this drives the attempt directly rather than sleeping. */
	check("a fresh init connects once the server is up",
	    net_json_init() == 0 && net_json_is_connected());

	/* The server goes away underneath a working connection. */
	closesocket(listener);
	net_json_close();
	check("closing gives up the connection", !net_json_is_connected());
	check("and stops wanting one", !net_json_wants_connection());

	config.json_net_enabled = 0;
	config.json_net_host[0] = '\0';
	check("no server configured is not wanting one",
	    net_json_init() == -1 && !net_json_wants_connection());
}

int
main(void)
{
#ifdef _WIN32
	/* Sockets do nothing at all before this on Windows, and net_json.c is
	   normally reached long after rpcemu.c has done it. */
	WSADATA wsadata;

	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		printf("FAIL: WSAStartup\n");
		return 1;
	}
#endif

	test_encoding();
	test_round_trip();
	test_bad_input();
	test_whitespace();
	test_reconnect();

	printf("\n%s\n", failures ? "FAILED" : "All tests passed");
	return failures != 0;
}
