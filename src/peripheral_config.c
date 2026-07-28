/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/types.h>

#include "serial_host.h"
#include "socket-compat.h"

#include "rpcemu.h"
#include "serial.h"
#include "superio.h"
#include "parallel.h"
#include "printer.h"
#include "peripheral_config.h"

PeripheralConfig peripheral_config;

#define SERIAL_LOG_DEVICE_NAME "Serial Log"
#define SERIAL_MODEM_DEVICE_NAME "TCP Modem"
#define PARALLEL_LOG_DEVICE_NAME "Parallel Log"

typedef struct {
	FILE *logfile;
	char path[512];
	int saw_tx;
} SerialLogState;

/*
 * TCP "modem": a Hayes-AT-command front end over a real TCP socket, with a
 * telnet client layer so it works with telnet BBSes and stays 8-bit clean for
 * X/Y/ZMODEM file transfers. All socket I/O is non-blocking and driven from
 * serial_modem_poll(), which runs on the emulation thread alongside the UART,
 * so no locking is needed.
 */

#define MODEM_RING_SIZE   16384  /* guest<->socket byte rings */
#define MODEM_LINE_MAX    256    /* AT command line buffer */
#define MODEM_GUARD_MS    1000   /* +++ escape guard time (Hayes) */
#define MODEM_CONNECT_TIMEOUT_MS 30000

/* Telnet protocol bytes (RFC 854) */
#define TEL_SE   240
#define TEL_SB   250
#define TEL_WILL 251
#define TEL_WONT 252
#define TEL_DO   253
#define TEL_DONT 254
#define TEL_IAC  255
#define TELOPT_BINARY 0
#define TELOPT_ECHO   1
#define TELOPT_SGA    3

/* Hayes numeric result codes */
#define RC_OK         0
#define RC_CONNECT    1
#define RC_NO_CARRIER 3
#define RC_ERROR      4

typedef enum {
	MODEM_OFFLINE = 0,    /* command mode, not connected */
	MODEM_CONNECTING,     /* non-blocking connect in progress */
	MODEM_ONLINE,         /* data mode, connected */
	MODEM_COMMAND_ONLINE  /* command mode (after +++) but still connected */
} ModemConnState;

typedef enum {
	TNS_DATA = 0,
	TNS_IAC,    /* saw IAC */
	TNS_OPT,    /* saw IAC WILL/WONT/DO/DONT, expecting option */
	TNS_SB,     /* inside subnegotiation */
	TNS_SB_IAC  /* IAC inside subnegotiation */
} TelnetParseState;

typedef struct {
	ModemConnState state;
	int fd;                  /* socket, or -1 */

	char line[MODEM_LINE_MAX];
	size_t line_len;

	/* bytes destined for the guest (result codes, echo, incoming data) */
	uint8_t rx[MODEM_RING_SIZE];
	size_t rx_head, rx_tail, rx_count;

	/* bytes destined for the socket (already telnet-escaped) */
	uint8_t tx[MODEM_RING_SIZE];
	size_t tx_head, tx_tail, tx_count;

	/* telnet parser */
	int telnet;
	TelnetParseState tstate;
	uint8_t topt_cmd;

	/* +++ escape detection (guard-timed so binary data is never mistaken) */
	int plus_count;
	int64_t last_tx_ms;
	int64_t last_plus_ms;

	/* settings */
	int echo;     /* ATE */
	int verbose;  /* ATV: 1 = text result codes, 0 = numeric */
	int quiet;    /* ATQ: 1 = suppress result codes */
	int prev_dtr; /* for DTR-drop hangup */

	/* pending connect address list */
	struct addrinfo *ai;
	struct addrinfo *ai_next;
	int64_t connect_deadline_ms;
} ModemState;

typedef struct {
	FILE *logfile;
	char path[512];
} ParallelLogState;

static SerialLogState serial_log_state[SERIAL_PORT_COUNT];
static ModemState modem_state[SERIAL_PORT_COUNT];

/*
 * Mark every modem's socket as closed.
 *
 * This matters more than it looks: the array above is zero-initialised, so
 * without this m->fd is 0, and modem_close()'s "fd >= 0" test then closes
 * descriptor 0 - the process's standard input, or whatever else has since been
 * given that slot. It cost an afternoon: a serial device opened on fd 0 was
 * closed underneath us by the second port's reconfigure.
 */
static void
modem_state_init(void)
{
	static int done = 0;
	int port;

	if (done) {
		return;
	}
	for (port = 0; port < SERIAL_PORT_COUNT; port++) {
		modem_state[port].fd = -1;
	}
	done = 1;
}
static ParallelLogState parallel_log_state;

static void
serial_log_close(SerialPortID port)
{
	if (serial_log_state[port].logfile != NULL) {
		fclose(serial_log_state[port].logfile);
		serial_log_state[port].logfile = NULL;
	}
	serial_log_state[port].saw_tx = 0;
}

static void
serial_log_write_banner(SerialPortID port)
{
	FILE *logfile = serial_log_state[port].logfile;

	if (logfile == NULL) {
		return;
	}

	fprintf(logfile, "\n# RPCEmu COM%d serial log session started\n", (int) port + 1);
	fflush(logfile);
}

static void
serial_log_on_write(uint8_t data, void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;
	FILE *logfile = serial_log_state[port].logfile;

	if (logfile != NULL) {
		if (!serial_log_state[port].saw_tx) {
			serial_log_state[port].saw_tx = 1;
			rpclog("Peripherals: COM%d first TX byte 0x%02X\n",
			       (int) port + 1, data);
		}
		fputc(data, logfile);
		fflush(logfile);
	}

	/* Local loopback so blocking reads can complete in log-to-file mode */
	superio_serial_rx(port, data);
}

static void
serial_log_on_reset(void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;

	serial_log_close(port);
	if (serial_log_state[port].path[0] != '\0') {
		serial_log_state[port].logfile = fopen(serial_log_state[port].path, "ab");
		if (serial_log_state[port].logfile == NULL) {
			rpclog("Peripherals: failed to open serial log '%s'\n",
			       serial_log_state[port].path);
		} else {
			serial_log_write_banner(port);
		}
	}
}

static uint8_t
serial_log_get_status(void *userdata)
{
	(void)userdata;
	return SERIAL_STAT_CTS | SERIAL_STAT_DSR | SERIAL_STAT_DCD;
}

static int
serial_attach_log(SerialPortID port, const char *path)
{
	SerialDevice device;

	if (path == NULL || path[0] == '\0') {
		rpclog("Peripherals: COM%d log mode requires a file path\n", (int) port + 1);
		return -1;
	}

	serial_log_close(port);
	strncpy(serial_log_state[port].path, path, sizeof(serial_log_state[port].path) - 1);
	serial_log_state[port].path[sizeof(serial_log_state[port].path) - 1] = '\0';
	serial_log_state[port].logfile = fopen(serial_log_state[port].path, "ab");
	if (serial_log_state[port].logfile == NULL) {
		rpclog("Peripherals: failed to open serial log '%s'\n", path);
		return -1;
	}
	serial_log_write_banner(port);

	memset(&device, 0, sizeof(device));
	device.name = SERIAL_LOG_DEVICE_NAME;
	device.on_write = serial_log_on_write;
	device.on_ctrl = NULL;
	device.get_status = serial_log_get_status;
	device.on_reset = serial_log_on_reset;
	device.userdata = (void *) (uintptr_t) port;

	if (serial_bus_attach(port, &device) < 0) {
		serial_log_close(port);
		return -1;
	}

	rpclog("Peripherals: COM%d logging to '%s'\n", (int) port + 1, path);
	return 0;
}

static int64_t
modem_now_ms(void)
{
#ifdef _WIN32
	/* GetTickCount64() is a monotonic millisecond clock since boot. */
	return (int64_t) GetTickCount64();
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void
modem_rx_put(ModemState *m, uint8_t b)
{
	if (m->rx_count >= MODEM_RING_SIZE) {
		return; /* guest not draining fast enough; drop */
	}
	m->rx[m->rx_head] = b;
	m->rx_head = (m->rx_head + 1) % MODEM_RING_SIZE;
	m->rx_count++;
}

static void
modem_tx_put(ModemState *m, uint8_t b)
{
	if (m->tx_count >= MODEM_RING_SIZE) {
		return;
	}
	m->tx[m->tx_head] = b;
	m->tx_head = (m->tx_head + 1) % MODEM_RING_SIZE;
	m->tx_count++;
}

static void
modem_to_guest_str(ModemState *m, const char *s)
{
	while (*s != '\0') {
		modem_rx_put(m, (uint8_t) *s++);
	}
}

/* Queue a byte for the socket, telnet-escaping IAC so the stream stays clean. */
static void
modem_to_socket(ModemState *m, uint8_t b)
{
	modem_tx_put(m, b);
	if (m->telnet && b == TEL_IAC) {
		modem_tx_put(m, TEL_IAC);
	}
}

static void
modem_send_telnet(ModemState *m, uint8_t cmd, uint8_t opt)
{
	modem_tx_put(m, TEL_IAC);
	modem_tx_put(m, cmd);
	modem_tx_put(m, opt);
}

static void
modem_result(ModemState *m, int code, const char *text)
{
	if (m->quiet) {
		return;
	}
	if (m->verbose) {
		modem_to_guest_str(m, "\r\n");
		modem_to_guest_str(m, text);
		modem_to_guest_str(m, "\r\n");
	} else {
		char buf[8];
		snprintf(buf, sizeof(buf), "%d\r", code);
		modem_to_guest_str(m, buf);
	}
}

static uint8_t
modem_status_bits(const ModemState *m)
{
	uint8_t s = SERIAL_STAT_CTS | SERIAL_STAT_DSR;

	if (m->fd >= 0) {
		s |= SERIAL_STAT_DCD; /* carrier present while connected */
	}
	return s;
}

static void
modem_close(ModemState *m)
{
	if (m->fd >= 0) {
		closesocket(m->fd);
		m->fd = -1;
	}
	if (m->ai != NULL) {
		freeaddrinfo(m->ai);
		m->ai = NULL;
		m->ai_next = NULL;
	}
	m->state = MODEM_OFFLINE;
	m->plus_count = 0;
	m->tstate = TNS_DATA;
}

/* Reset modem settings + buffers (called when (re)attaching the backend). */
static void
modem_reset_state(ModemState *m)
{
	modem_close(m);
	m->line_len = 0;
	m->rx_head = m->rx_tail = m->rx_count = 0;
	m->tx_head = m->tx_tail = m->tx_count = 0;
	m->tstate = TNS_DATA;
	m->plus_count = 0;
	m->echo = 1;
	m->verbose = 1;
	m->quiet = 0;
	m->prev_dtr = 0;
	m->telnet = 1;
}

static void
modem_on_connected(ModemState *m, SerialPortID port)
{
	if (m->ai != NULL) {
		freeaddrinfo(m->ai);
		m->ai = NULL;
		m->ai_next = NULL;
	}
	m->state = MODEM_ONLINE;
	m->tstate = TNS_DATA;
	m->plus_count = 0;
	m->last_tx_ms = modem_now_ms();
	rpclog("Peripherals: COM%d modem CONNECT\n", (int) port + 1);
	superio_serial_update_msr(port, modem_status_bits(m)); /* raise DCD */
	modem_result(m, RC_CONNECT, "CONNECT");
}

static void
modem_try_next_address(ModemState *m, SerialPortID port)
{
	while (m->ai_next != NULL) {
		struct addrinfo *ai = m->ai_next;
		int fd, one = 1, r;

		m->ai_next = ai->ai_next;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		socket_set_nonblocking(fd);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));

		r = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (r == 0) {
			m->fd = fd;
			modem_on_connected(m, port);
			return;
		}
		if (r < 0 && (sock_errno() == SOCK_EINPROGRESS || sock_errno() == SOCK_EINTR)) {
			m->fd = fd;
			m->state = MODEM_CONNECTING;
			m->connect_deadline_ms = modem_now_ms() + MODEM_CONNECT_TIMEOUT_MS;
			return;
		}
		closesocket(fd);
	}

	/* No addresses left to try. */
	if (m->ai != NULL) {
		freeaddrinfo(m->ai);
		m->ai = NULL;
		m->ai_next = NULL;
	}
	m->state = MODEM_OFFLINE;
	modem_result(m, RC_NO_CARRIER, "NO CARRIER");
}

static void
modem_dial(ModemState *m, SerialPortID port, const char *number)
{
	char host[256];
	char service[32];
	size_t i;
	struct addrinfo hints, *res = NULL;
	int gai;

	/* Skip dial modifiers / punctuation (T, P, W, spaces, parens, dashes). */
	while (*number != '\0' && (*number == ' ' || *number == '\t' ||
	       toupper((unsigned char) *number) == 'T' ||
	       toupper((unsigned char) *number) == 'P' ||
	       toupper((unsigned char) *number) == 'W' ||
	       *number == '(' || *number == ')' || *number == '-' || *number == ',')) {
		number++;
	}

	/* Host: up to ':' or whitespace. */
	i = 0;
	while (*number != '\0' && *number != ':' && *number != ' ' &&
	       *number != '\t' && i < sizeof(host) - 1) {
		host[i++] = *number++;
	}
	host[i] = '\0';

	/* Optional port after ':' or whitespace; default to telnet (23). */
	service[0] = '\0';
	if (*number == ':' || *number == ' ' || *number == '\t') {
		number++;
		while (*number == ' ' || *number == '\t') {
			number++;
		}
		i = 0;
		while (*number != '\0' && *number != ' ' && *number != '\t' &&
		       i < sizeof(service) - 1) {
			service[i++] = *number++;
		}
		service[i] = '\0';
	}
	if (service[0] == '\0') {
		strcpy(service, "23");
	}
	if (host[0] == '\0') {
		modem_result(m, RC_ERROR, "ERROR");
		return;
	}

	/* Drop any existing connection before dialling. */
	modem_close(m);

	rpclog("Peripherals: COM%d modem dialling %s:%s\n", (int) port + 1, host, service);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	gai = getaddrinfo(host, service, &hints, &res); /* note: DNS lookup is synchronous */
	if (gai != 0 || res == NULL) {
		rpclog("Peripherals: COM%d modem cannot resolve %s (%s)\n",
		       (int) port + 1, host, gai_strerror(gai));
		modem_result(m, RC_NO_CARRIER, "NO CARRIER");
		return;
	}

	m->ai = res;
	m->ai_next = res;
	m->telnet = 1;
	modem_try_next_address(m, port);
}

static void
modem_process_command(ModemState *m, SerialPortID port)
{
	char *line = m->line;
	char *s;
	size_t len = m->line_len;
	char first;

	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
		line[--len] = '\0';
	}
	s = line;
	while (*s == ' ' || *s == '\t') {
		s++;
	}

	if (*s == '\0') {
		return; /* blank line: no response */
	}
	if (!((s[0] == 'A' || s[0] == 'a') && (s[1] == 'T' || s[1] == 't'))) {
		modem_result(m, RC_ERROR, "ERROR");
		return;
	}
	s += 2;

	rpclog("Peripherals: COM%d modem command: AT%s\n", (int) port + 1, s);

	if (*s == '\0') {
		modem_result(m, RC_OK, "OK"); /* bare "AT" */
		return;
	}

	/* Dial: 'D' anywhere; the remainder is the host[:port]. */
	{
		char *d;
		for (d = s; *d != '\0'; d++) {
			if (toupper((unsigned char) *d) == 'D') {
				modem_dial(m, port, d + 1);
				return;
			}
		}
	}

	first = toupper((unsigned char) *s);
	if (first == 'H') { /* hang up */
		if (m->fd >= 0) {
			modem_close(m);
			superio_serial_update_msr(port, modem_status_bits(m));
			modem_result(m, RC_NO_CARRIER, "NO CARRIER");
		} else {
			modem_result(m, RC_OK, "OK");
		}
		return;
	}
	if (first == 'O') { /* return online */
		if (m->fd >= 0) {
			m->state = MODEM_ONLINE;
			m->last_tx_ms = modem_now_ms();
			modem_result(m, RC_CONNECT, "CONNECT");
		} else {
			modem_result(m, RC_NO_CARRIER, "NO CARRIER");
		}
		return;
	}

	/* Otherwise treat as a configuration/init string. Honour the common
	 * toggles (E echo, V verbose, Q quiet) and acknowledge everything else. */
	{
		char *p;
		for (p = s; *p != '\0'; p++) {
			char c = toupper((unsigned char) *p);
			char n = p[1];
			if (c == 'E') {
				m->echo = (n == '0') ? 0 : 1;
			} else if (c == 'V') {
				m->verbose = (n == '0') ? 0 : 1;
			} else if (c == 'Q') {
				m->quiet = (n == '1') ? 1 : 0;
			}
		}
	}
	if (strncasecmp(s, "I", 1) == 0) {
		modem_to_guest_str(m, "\r\nRPCEmu TCP modem\r\n");
	}
	modem_result(m, RC_OK, "OK");
}

/* Guest -> socket while in data mode, with guard-timed +++ escape detection. */
static void
modem_data_tx_byte(ModemState *m, uint8_t b)
{
	int64_t now = modem_now_ms();

	if (b == '+' && m->plus_count < 3) {
		if (m->plus_count == 0) {
			if (now - m->last_tx_ms >= MODEM_GUARD_MS) {
				m->plus_count = 1;
				m->last_plus_ms = now;
				return; /* buffer the '+', do not transmit yet */
			}
			/* no leading guard time: this '+' is ordinary data */
		} else {
			m->plus_count++;
			m->last_plus_ms = now;
			return;
		}
	}

	/* Not (part of) a pending escape: flush any buffered '+' as data first. */
	if (m->plus_count > 0) {
		int i;
		for (i = 0; i < m->plus_count; i++) {
			modem_to_socket(m, '+');
		}
		m->plus_count = 0;
	}
	modem_to_socket(m, b);
	m->last_tx_ms = now;
}

static void
modem_on_write(uint8_t data, void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;
	ModemState *m = &modem_state[port];

	if (m->state == MODEM_ONLINE) {
		modem_data_tx_byte(m, data);
		return;
	}

	/* Command mode (offline, connecting, or +++ command-online). */
	if (m->echo) {
		modem_rx_put(m, data);
		if (data == '\r') {
			modem_rx_put(m, '\n');
		}
	}
	if (data == '\r' || data == '\n') {
		if (m->line_len > 0) {
			m->line[m->line_len] = '\0';
			modem_process_command(m, port);
			m->line_len = 0;
		}
		return;
	}
	if (data == '\b' || data == 0x7f) {
		if (m->line_len > 0) {
			m->line_len--;
		}
		return;
	}
	if (m->line_len + 1 < sizeof(m->line)) {
		m->line[m->line_len++] = (char) data;
	}
}

static void
modem_on_ctrl(uint8_t ctrl, void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;
	ModemState *m = &modem_state[port];
	int dtr = (ctrl & SERIAL_CTRL_DTR) ? 1 : 0;

	/* Hang up when the guest drops DTR (Hayes &D2 behaviour). */
	if (!dtr && m->prev_dtr && m->fd >= 0) {
		rpclog("Peripherals: COM%d modem hangup (DTR dropped)\n", (int) port + 1);
		modem_close(m);
		superio_serial_update_msr(port, modem_status_bits(m));
		modem_result(m, RC_NO_CARRIER, "NO CARRIER");
	}
	m->prev_dtr = dtr;
}

static void
modem_on_reset(void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;

	modem_reset_state(&modem_state[port]);
}

static uint8_t
modem_get_status(void *userdata)
{
	SerialPortID port = (SerialPortID) (uintptr_t) userdata;

	return modem_status_bits(&modem_state[port]);
}

/* Feed incoming socket bytes through the telnet parser, delivering clean data
 * bytes to the guest and answering option negotiations to stay 8-bit clean. */
static void
modem_telnet_input(ModemState *m, const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t b = data[i];

		if (!m->telnet) {
			modem_rx_put(m, b);
			continue;
		}

		switch (m->tstate) {
		case TNS_DATA:
			if (b == TEL_IAC) {
				m->tstate = TNS_IAC;
			} else {
				modem_rx_put(m, b);
			}
			break;
		case TNS_IAC:
			if (b == TEL_IAC) {
				modem_rx_put(m, TEL_IAC); /* escaped 0xFF */
				m->tstate = TNS_DATA;
			} else if (b == TEL_WILL || b == TEL_WONT ||
			           b == TEL_DO || b == TEL_DONT) {
				m->topt_cmd = b;
				m->tstate = TNS_OPT;
			} else if (b == TEL_SB) {
				m->tstate = TNS_SB;
			} else {
				m->tstate = TNS_DATA; /* other 2-byte command: ignore */
			}
			break;
		case TNS_OPT:
			/* Reactive policy: enable BINARY/SGA (for clean transfers) and
			 * let the server echo; refuse everything else. */
			if (m->topt_cmd == TEL_DO) {
				modem_send_telnet(m,
				    (b == TELOPT_BINARY || b == TELOPT_SGA) ? TEL_WILL : TEL_WONT, b);
			} else if (m->topt_cmd == TEL_WILL) {
				modem_send_telnet(m,
				    (b == TELOPT_BINARY || b == TELOPT_SGA || b == TELOPT_ECHO) ? TEL_DO : TEL_DONT, b);
			}
			m->tstate = TNS_DATA;
			break;
		case TNS_SB:
			if (b == TEL_IAC) {
				m->tstate = TNS_SB_IAC;
			}
			break;
		case TNS_SB_IAC:
			m->tstate = (b == TEL_SE) ? TNS_DATA : TNS_SB;
			break;
		}
	}
}

static int
serial_attach_modem(SerialPortID port)
{
	SerialDevice device;

	modem_reset_state(&modem_state[port]);

	memset(&device, 0, sizeof(device));
	device.name = SERIAL_MODEM_DEVICE_NAME;
	device.on_write = modem_on_write;
	device.on_ctrl = modem_on_ctrl;
	device.get_status = modem_get_status;
	device.on_reset = modem_on_reset;
	device.userdata = (void *) (uintptr_t) port;

	if (serial_bus_attach(port, &device) < 0) {
		return -1;
	}

	rpclog("Peripherals: COM%d TCP modem ready (ATDT <host>[:port] to dial; telnet, default port 23)\n",
	       (int) port + 1);
	return 0;
}

static void
parallel_log_close(void)
{
	if (parallel_log_state.logfile != NULL) {
		fclose(parallel_log_state.logfile);
		parallel_log_state.logfile = NULL;
	}
}

static void
parallel_log_on_write(uint8_t data, void *userdata)
{
	(void)userdata;

	if (parallel_log_state.logfile != NULL) {
		fputc(data, parallel_log_state.logfile);
		fflush(parallel_log_state.logfile);
	}
}

static void
parallel_log_on_reset(void *userdata)
{
	(void)userdata;

	parallel_log_close();
	if (parallel_log_state.path[0] != '\0') {
		parallel_log_state.logfile = fopen(parallel_log_state.path, "ab");
		if (parallel_log_state.logfile == NULL) {
			rpclog("Peripherals: failed to open parallel log '%s'\n",
			       parallel_log_state.path);
		}
	}
}

static uint8_t
parallel_log_get_status(void *userdata)
{
	(void)userdata;
	return PARALLEL_STAT_DEFAULT;
}

static int
parallel_attach_log(const char *path)
{
	ParallelDevice device;

	if (path == NULL || path[0] == '\0') {
		rpclog("Peripherals: parallel log mode requires a file path\n");
		return -1;
	}

	parallel_log_close();
	strncpy(parallel_log_state.path, path, sizeof(parallel_log_state.path) - 1);
	parallel_log_state.path[sizeof(parallel_log_state.path) - 1] = '\0';
	parallel_log_state.logfile = fopen(parallel_log_state.path, "ab");
	if (parallel_log_state.logfile == NULL) {
		rpclog("Peripherals: failed to open parallel log '%s'\n", path);
		return -1;
	}

	memset(&device, 0, sizeof(device));
	device.name = PARALLEL_LOG_DEVICE_NAME;
	device.on_write = parallel_log_on_write;
	device.on_ctrl = NULL;
	device.get_status = parallel_log_get_status;
	device.on_reset = parallel_log_on_reset;
	device.userdata = NULL;

	if (parallel_bus_attach(PARALLEL_PORT_RISCPC, &device) < 0) {
		parallel_log_close();
		return -1;
	}

	rpclog("Peripherals: parallel port logging to '%s'\n", path);
	return 0;
}

static int
serial_apply_port(SerialPortID port, PeripheralSerialMode mode,
                  const char *log_path, const char *device_path)
{
	serial_host_close((SerialPortID) port);	/* release a host device first: it
						   owns the bus attachment */
	serial_bus_detach(port);
	serial_log_close(port);
	modem_close(&modem_state[port]); /* drop any TCP connection on reconfigure */

	switch (mode) {
	case PeripheralSerial_LogToFile:
		return serial_attach_log(port, log_path);

	case PeripheralSerial_TcpModem:
		return serial_attach_modem(port);

	case PeripheralSerial_PhysicalDevice: {
		char why[512] = "";

		if (serial_host_attach((SerialPortID) port, device_path, why,
		        sizeof(why)) != 0) {
			rpclog("Peripherals: COM%d could not use %s: %s\n",
			       (int) port + 1,
			       (device_path != NULL) ? device_path : "(no device)",
			       why);
			return -1;
		}
		return 0;
	}

	case PeripheralSerial_Disabled:
	default:
		return 0;
	}
}

static int
parallel_apply(void)
{
	parallel_bus_detach(PARALLEL_PORT_LPT1);
	parallel_bus_detach(PARALLEL_PORT_LPT2);
	parallel_log_close();
	printer_detach();

	switch (peripheral_config.parallel_mode) {
	case PeripheralParallel_LogToFile:
		return parallel_attach_log(peripheral_config.parallel_log_path);

	case PeripheralParallel_VirtualPrinter:
		printer_set_output_path(peripheral_config.printer_output_path);
		printer_set_auto_pdf(peripheral_config.printer_auto_pdf);
		printer_set_output_mode(PrinterOutput_File);
		return printer_attach(PARALLEL_PORT_RISCPC);

	case PeripheralParallel_HostPrinter:
		if (peripheral_config.parallel_device[0] == '\0') {
			rpclog("Peripherals: no host printer set for the parallel port\n");
			return -1;
		}
		printer_set_host_target(peripheral_config.parallel_device);
		printer_set_auto_pdf(0);	/* the host printer prints it */
		printer_set_output_mode(PrinterOutput_Host);
		return printer_attach(PARALLEL_PORT_RISCPC);

	case PeripheralParallel_Disabled:
	default:
		printer_set_output_mode(PrinterOutput_Disabled);
		return 0;
	}
}

void
peripheral_config_set_defaults(void)
{
	int port;

	memset(&peripheral_config, 0, sizeof(peripheral_config));

	/* Sockets are zero-initialised statically, but fd 0 is a valid descriptor
	 * (stdin), so mark every modem as having no socket. */
	for (port = 0; port < SERIAL_PORT_COUNT; port++) {
		modem_state[port].fd = -1;
	}
}

void
peripheral_config_apply(void)
{
	modem_state_init();

	serial_apply_port(SERIAL_PORT_COM1,
	                  peripheral_config.com1_mode,
	                  peripheral_config.com1_log_path,
	                  peripheral_config.com1_device);
	serial_apply_port(SERIAL_PORT_COM2,
	                  peripheral_config.com2_mode,
	                  peripheral_config.com2_log_path,
	                  peripheral_config.com2_device);
	parallel_apply();
}

static void
modem_carrier_lost(ModemState *m, SerialPortID port)
{
	modem_close(m);
	superio_serial_update_msr(port, modem_status_bits(m));
	modem_result(m, RC_NO_CARRIER, "NO CARRIER");
}

void
serial_modem_poll(void)
{
	for (int port = 0; port < SERIAL_PORT_COUNT; port++) {
		ModemState *m = &modem_state[port];
		int64_t now;

		/* Complete a non-blocking connect. */
		if (m->state == MODEM_CONNECTING && m->fd >= 0) {
			struct pollfd pfd;
			pfd.fd = m->fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;

			if (poll(&pfd, 1, 0) > 0 &&
			    (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
				int err = 0;
				socklen_t elen = sizeof(err);
				getsockopt(m->fd, SOL_SOCKET, SO_ERROR, (char *) &err, &elen);
				if (err == 0) {
					modem_on_connected(m, (SerialPortID) port);
				} else {
					closesocket(m->fd);
					m->fd = -1;
					modem_try_next_address(m, (SerialPortID) port);
				}
			} else if (modem_now_ms() > m->connect_deadline_ms) {
				closesocket(m->fd);
				m->fd = -1;
				modem_try_next_address(m, (SerialPortID) port);
			}
		}

		/* Drain the socket into the guest RX path (telnet-decoded). */
		if (m->fd >= 0 &&
		    (m->state == MODEM_ONLINE || m->state == MODEM_COMMAND_ONLINE)) {
			for (;;) {
				uint8_t buf[2048];
				ssize_t n = recv(m->fd, (char *) buf, sizeof(buf), 0);
				if (n > 0) {
					modem_telnet_input(m, buf, (size_t) n);
					if ((size_t) n < sizeof(buf)) {
						break;
					}
				} else if (n == 0) {
					modem_carrier_lost(m, (SerialPortID) port);
					break;
				} else {
					if (sock_errno() == SOCK_EINTR) {
						continue;
					}
					if (sock_errno() != SOCK_EAGAIN && sock_errno() != SOCK_EWOULDBLOCK) {
						modem_carrier_lost(m, (SerialPortID) port);
					}
					break;
				}
			}
		}

		/* Flush queued bytes to the socket. */
		while (m->fd >= 0 && m->tx_count > 0) {
			size_t contig = MODEM_RING_SIZE - m->tx_tail;
			if (contig > m->tx_count) {
				contig = m->tx_count;
			}
			ssize_t n = send(m->fd, (const char *) &m->tx[m->tx_tail], contig, MSG_NOSIGNAL);
			if (n > 0) {
				m->tx_tail = (m->tx_tail + (size_t) n) % MODEM_RING_SIZE;
				m->tx_count -= (size_t) n;
			} else {
				if (n < 0 && sock_errno() == SOCK_EINTR) {
					continue;
				}
				if (n < 0 && (sock_errno() == SOCK_EAGAIN || sock_errno() == SOCK_EWOULDBLOCK)) {
					break; /* socket buffer full; try again next poll */
				}
				modem_carrier_lost(m, (SerialPortID) port);
				break;
			}
		}

		/* Resolve a pending +++ escape once its guard time has elapsed. */
		if (m->state == MODEM_ONLINE && m->plus_count > 0) {
			now = modem_now_ms();
			if (m->plus_count == 3 && now - m->last_plus_ms >= MODEM_GUARD_MS) {
				m->plus_count = 0;
				m->state = MODEM_COMMAND_ONLINE;
				modem_result(m, RC_OK, "OK");
			} else if (m->plus_count < 3 && now - m->last_plus_ms >= MODEM_GUARD_MS) {
				int i;
				for (i = 0; i < m->plus_count; i++) {
					modem_to_socket(m, '+'); /* were data after all */
				}
				m->plus_count = 0;
				m->last_tx_ms = now;
			}
		}

		/* Hand buffered bytes to the guest UART, paced by its RX FIFO. */
		if (m->rx_count > 0) {
			int space = superio_serial_rx_space((SerialPortID) port);
			while (space > 0 && m->rx_count > 0) {
				serial_bus_device_write_data((SerialPortID) port,
				                             m->rx[m->rx_tail]);
				m->rx_tail = (m->rx_tail + 1) % MODEM_RING_SIZE;
				m->rx_count--;
				space--;
			}
		}
	}
}

void
peripheral_config_shutdown(void)
{
	int port;

	modem_state_init();

	for (port = 0; port < SERIAL_PORT_COUNT; port++) {
		serial_host_close((SerialPortID) port);
		serial_bus_detach((SerialPortID) port);
		serial_log_close((SerialPortID) port);
		modem_close(&modem_state[(SerialPortID) port]);
	}

	parallel_bus_detach(PARALLEL_PORT_LPT1);
	parallel_bus_detach(PARALLEL_PORT_LPT2);
	parallel_log_close();
	printer_shutdown();
}
