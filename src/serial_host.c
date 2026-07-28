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

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * serial_host.c - a serial port on the host, wired to an emulated UART.
 *
 * Structured like the TCP modem in peripheral_config.c, because the awkward
 * parts are the same: a device on the serial bus, non-blocking host I/O, and a
 * poll routine on the emulation thread. Only the transport differs.
 *
 * Two things are worth understanding before changing anything here.
 *
 * The emulated UART does not pace bytes by baud rate. It stores the divisor and
 * hands bytes over as fast as they arrive, so the guest's baud setting is not a
 * throttle: it is a request, which is passed to the host port so the wire speed
 * is right. Nothing here should try to reproduce bit timing, because the host's
 * own UART is doing the signalling.
 *
 * The guest's receive FIFO is 16 bytes and uart_rx_push() sets the overrun bit
 * and drops when it is full. So this reads at most as many bytes as
 * superio_serial_rx_space() reports free, leaving the rest in the host's buffer,
 * which is real backpressure rather than a bigger buffer of our own. Where the
 * kernel is doing RTS/CTS, that is what eventually stops the far end.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "rpcemu.h"
#include "serial.h"
#include "serial_host.h"
#include "superio.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* Bytes waiting to go out to the host. The guest can write faster than a real
   port can send, and dropping its output would be worse than buffering it. */
#define HOST_TX_RING	8192

typedef struct {
#ifdef _WIN32
	HANDLE handle;
#else
	int fd;
#endif
	char path[256];
	int is_open;		/* not a sentinel in fd/handle: this array starts
				   zeroed, and 0 is a valid descriptor */

	uint8_t tx[HOST_TX_RING];
	size_t tx_head;		/* next byte to send */
	size_t tx_tail;		/* next free slot */

	uint8_t ctrl;		/* last MCR seen from the guest */
	uint8_t status;		/* last line state read from the host */

	int no_modem_lines;	/* device cannot report CTS/DSR/DCD (e.g. a pty) */

	uint16_t divisor;	/* line settings last applied to the host */
	uint8_t lcr;
	int line_valid;

	unsigned long tx_dropped;	/* guest bytes lost to a full ring */
} HostSerial;

static HostSerial host_port[SERIAL_PORT_COUNT];

static const char *
port_name(SerialPortID port)
{
	return port == SERIAL_PORT_COM1 ? "COM1" : "COM2";
}

/* ========================================================================
 * Transmit ring
 * ======================================================================== */

static size_t
tx_used(const HostSerial *hs)
{
	return (hs->tx_tail - hs->tx_head + HOST_TX_RING) % HOST_TX_RING;
}

static void
tx_push(HostSerial *hs, uint8_t b)
{
	const size_t next = (hs->tx_tail + 1) % HOST_TX_RING;

	if (next == hs->tx_head) {
		/* Full. Count it rather than block the emulation thread or
		   silently pretend the byte went out. */
		hs->tx_dropped++;
		if (hs->tx_dropped == 1 || (hs->tx_dropped % 1000) == 0) {
			rpclog("serial_host: transmit buffer full, %lu byte(s) lost\n",
			       hs->tx_dropped);
		}
		return;
	}

	hs->tx[hs->tx_tail] = b;
	hs->tx_tail = next;
}

/* ========================================================================
 * Line settings
 *
 * baud = 115200 / divisor: the Risc PC's UART is clocked at 1.8432 MHz and
 * divides by 16. Divisor 12 is the 9600 the UART resets to.
 * ======================================================================== */

static unsigned
baud_from_divisor(uint16_t divisor)
{
	if (divisor == 0) {
		return 9600;	/* guest has not set one yet */
	}
	return 115200u / divisor;
}

#ifndef _WIN32

/* Nearest speed the host will accept, so an unusual divisor degrades rather
   than failing outright. */
static speed_t
termios_speed(unsigned baud, unsigned *actual)
{
	static const struct { unsigned baud; speed_t speed; } table[] = {
		{ 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 },
		{ 150, B150 }, { 200, B200 }, { 300, B300 }, { 600, B600 },
		{ 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 },
		{ 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
		{ 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
	};
	size_t best = 0;
	unsigned best_diff = ~0u;
	size_t i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		const unsigned diff = baud > table[i].baud
		    ? baud - table[i].baud : table[i].baud - baud;

		if (diff < best_diff) {
			best_diff = diff;
			best = i;
		}
	}

	*actual = table[best].baud;
	return table[best].speed;
}

/*
 * Ask the host port for the speed and framing the guest programmed.
 *
 * The port is put in raw mode: no echo, no signal characters, no newline
 * translation. Anything else would corrupt a binary stream, which is what a
 * modem or a file transfer sends.
 */
static void
apply_line_settings(HostSerial *hs, SerialPortID port, uint16_t divisor,
    uint8_t lcr)
{
	struct termios tio;
	unsigned baud = baud_from_divisor(divisor);
	unsigned actual = baud;
	speed_t speed;

	if (hs->fd < 0 || tcgetattr(hs->fd, &tio) != 0) {
		return;
	}

	cfmakeraw(&tio);
	speed = termios_speed(baud, &actual);
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	/* Word length, LCR bits 0-1.
	   An LCR of zero is the value the UART resets to, not a considered
	   choice, and it would ask the host for 5 data bits. Until the guest
	   configures the port, use 8N1, which is what anything on the other end
	   is likely to expect. */
	if (lcr == 0) {
		lcr = 0x03;
	}

	tio.c_cflag &= ~(tcflag_t) CSIZE;
	switch (lcr & 0x03) {
	case 0:  tio.c_cflag |= CS5; break;
	case 1:  tio.c_cflag |= CS6; break;
	case 2:  tio.c_cflag |= CS7; break;
	default: tio.c_cflag |= CS8; break;
	}

	/* Stop bits, LCR bit 2 */
	if (lcr & 0x04) {
		tio.c_cflag |= CSTOPB;
	} else {
		tio.c_cflag &= ~(tcflag_t) CSTOPB;
	}

	/* Parity, LCR bits 3 (enable) and 4 (even). Stick parity, bit 5, has no
	   termios equivalent and is left as no parity rather than guessed at. */
	tio.c_cflag &= ~(tcflag_t) (PARENB | PARODD);
	if ((lcr & 0x08) && !(lcr & 0x20)) {
		tio.c_cflag |= PARENB;
		if (!(lcr & 0x10)) {
			tio.c_cflag |= PARODD;
		}
	}

	/* Let the kernel handle RTS/CTS. With the emulator's speed depending on
	   the host's load, this is what stops a fast sender overrunning a guest
	   that has fallen behind. */
#ifdef CRTSCTS
	tio.c_cflag |= CRTSCTS;
#endif
	tio.c_cflag |= CLOCAL | CREAD;

	/* VMIN=1 rather than 0, deliberately. The descriptor is O_NONBLOCK, so an
	   empty port gives EAGAIN and a closed one gives 0, which is the only way
	   to tell "nothing to read yet" from "the other end has gone". With
	   VMIN=0 a tty returns 0 for both, and an idle port would look like an
	   unplugged one on the first quiet poll. */
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(hs->fd, TCSANOW, &tio) != 0) {
		rpclog("serial_host: %s: could not set %u baud: %s\n",
		       port_name(port), baud, strerror(errno));
		return;
	}

	if (actual != baud) {
		rpclog("serial_host: %s: guest asked for %u baud, host set to %u\n",
		       port_name(port), baud, actual);
	} else {
		rpclog("serial_host: %s: %u baud, %d data, %s parity, %d stop\n",
		       port_name(port), baud, 5 + (lcr & 3),
		       (lcr & 0x08) ? ((lcr & 0x10) ? "even" : "odd") : "no",
		       (lcr & 0x04) ? 2 : 1);
	}
}

/* Mirror the guest's DTR and RTS onto the host port. */
static void
apply_control_lines(HostSerial *hs, uint8_t ctrl)
{
	int bits = 0;

	if (hs->fd < 0 || hs->no_modem_lines) {
		return;
	}

	if (ioctl(hs->fd, TIOCMGET, &bits) != 0) {
		return;
	}

	if (ctrl & SERIAL_CTRL_DTR) {
		bits |= TIOCM_DTR;
	} else {
		bits &= ~TIOCM_DTR;
	}
	if (ctrl & SERIAL_CTRL_RTS) {
		bits |= TIOCM_RTS;
	} else {
		bits &= ~TIOCM_RTS;
	}

	(void) ioctl(hs->fd, TIOCMSET, &bits);
}

/*
 * The host's CTS, DSR, RI and DCD, in the bus's terms.
 *
 * Some devices have no modem lines to report: a pseudo terminal answers
 * TIOCMGET with ENOTTY, and so do some USB adapters for lines they do not wire.
 * Reporting those as low is not neutral, it is a stop signal: RISC OS waits for
 * CTS before transmitting and hangs indefinitely, which is exactly what happened
 * the first time this was tried down a pty. So a device that cannot tell us
 * asserts CTS, DSR and DCD instead, on the grounds that no handshake at all is
 * closer to the truth than a permanently blocked one.
 */
static uint8_t
read_status_lines(HostSerial *hs)
{
	int bits = 0;
	uint8_t status = 0;

	if (hs->fd < 0) {
		return hs->status;
	}

	if (hs->no_modem_lines) {
		return SERIAL_STAT_CTS | SERIAL_STAT_DSR | SERIAL_STAT_DCD;
	}

	if (ioctl(hs->fd, TIOCMGET, &bits) != 0) {
		if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS) {
			hs->no_modem_lines = 1;
			rpclog("serial_host: %s cannot report its modem lines, "
			       "treating CTS/DSR/DCD as asserted\n", hs->path);
			return SERIAL_STAT_CTS | SERIAL_STAT_DSR |
			    SERIAL_STAT_DCD;
		}
		return hs->status;
	}

	if (bits & TIOCM_CTS) { status |= SERIAL_STAT_CTS; }
	if (bits & TIOCM_DSR) { status |= SERIAL_STAT_DSR; }
	if (bits & TIOCM_RNG) { status |= SERIAL_STAT_RI; }
	if (bits & TIOCM_CAR) { status |= SERIAL_STAT_DCD; }

	return status;
}

#else /* _WIN32 */

static void
apply_line_settings(HostSerial *hs, SerialPortID port, uint16_t divisor,
    uint8_t lcr)
{
	DCB dcb;
	COMMTIMEOUTS timeouts;
	const unsigned baud = baud_from_divisor(divisor);

	if (hs->handle == INVALID_HANDLE_VALUE) {
		return;
	}

	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(hs->handle, &dcb)) {
		return;
	}

	dcb.BaudRate = baud;
	dcb.ByteSize = (BYTE) (5 + (lcr & 0x03));
	dcb.StopBits = (lcr & 0x04) ? TWOSTOPBITS : ONESTOPBIT;
	if ((lcr & 0x08) && !(lcr & 0x20)) {
		dcb.fParity = TRUE;
		dcb.Parity = (lcr & 0x10) ? EVENPARITY : ODDPARITY;
	} else {
		dcb.fParity = FALSE;
		dcb.Parity = NOPARITY;
	}
	/* As on the other platforms, let the driver do RTS/CTS. */
	dcb.fOutxCtsFlow = TRUE;
	dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
	dcb.fBinary = TRUE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;

	if (!SetCommState(hs->handle, &dcb)) {
		rpclog("serial_host: %s: could not set %u baud (error %lu)\n",
		       port_name(port), baud, (unsigned long) GetLastError());
		return;
	}

	/* All zero: reads and writes return at once with whatever is ready,
	   which is what a poll on the emulation thread needs. */
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = MAXDWORD;
	(void) SetCommTimeouts(hs->handle, &timeouts);

	rpclog("serial_host: %s: %u baud, %d data, %s parity, %d stop\n",
	       port_name(port), baud, 5 + (lcr & 3),
	       (lcr & 0x08) ? ((lcr & 0x10) ? "even" : "odd") : "no",
	       (lcr & 0x04) ? 2 : 1);
}

static void
apply_control_lines(HostSerial *hs, uint8_t ctrl)
{
	if (hs->handle == INVALID_HANDLE_VALUE) {
		return;
	}

	(void) EscapeCommFunction(hs->handle,
	    (ctrl & SERIAL_CTRL_DTR) ? SETDTR : CLRDTR);
	(void) EscapeCommFunction(hs->handle,
	    (ctrl & SERIAL_CTRL_RTS) ? SETRTS : CLRRTS);
}

static uint8_t
read_status_lines(HostSerial *hs)
{
	DWORD bits = 0;
	uint8_t status = 0;

	if (hs->handle == INVALID_HANDLE_VALUE ||
	    !GetCommModemStatus(hs->handle, &bits)) {
		return hs->status;
	}

	if (bits & MS_CTS_ON)  { status |= SERIAL_STAT_CTS; }
	if (bits & MS_DSR_ON)  { status |= SERIAL_STAT_DSR; }
	if (bits & MS_RING_ON) { status |= SERIAL_STAT_RI; }
	if (bits & MS_RLSD_ON) { status |= SERIAL_STAT_DCD; }

	return status;
}

#endif /* _WIN32 */

/* ========================================================================
 * Serial bus device
 * ======================================================================== */

/* Which port a callback belongs to, passed as userdata. */
static SerialPortID
userdata_port(void *userdata)
{
	return (SerialPortID) (uintptr_t) userdata;
}

static void
host_on_write(uint8_t data, void *userdata)
{
	tx_push(&host_port[userdata_port(userdata)], data);
}

static void
host_on_ctrl(uint8_t ctrl, void *userdata)
{
	HostSerial *hs = &host_port[userdata_port(userdata)];

	if (ctrl == hs->ctrl) {
		return;
	}
	hs->ctrl = ctrl;
	apply_control_lines(hs, ctrl);
}

static uint8_t
host_get_status(void *userdata)
{
	return host_port[userdata_port(userdata)].status;
}

static void
host_on_reset(void *userdata)
{
	HostSerial *hs = &host_port[userdata_port(userdata)];

	hs->tx_head = 0;
	hs->tx_tail = 0;
	hs->line_valid = 0;	/* re-apply settings on the next poll */
}

/* ========================================================================
 * Attach and detach
 * ======================================================================== */

static int
host_is_open(const HostSerial *hs)
{
	return hs->is_open;
}

int
serial_host_is_open(SerialPortID port)
{
	if (port >= SERIAL_PORT_COUNT) {
		return 0;
	}
	return host_is_open(&host_port[port]);
}

int
serial_host_attach(SerialPortID port, const char *device, char *error,
    size_t errlen)
{
	HostSerial *hs;
	SerialDevice dev;

	if (port >= SERIAL_PORT_COUNT) {
		return -1;
	}
	if (device == NULL || device[0] == '\0') {
		snprintf(error, errlen, "No serial device was given.");
		return -1;
	}

	serial_host_close(port);
	hs = &host_port[port];

	memset(hs, 0, sizeof(*hs));
#ifdef _WIN32
	hs->handle = INVALID_HANDLE_VALUE;
#else
	hs->fd = -1;
#endif
	strncpy(hs->path, device, sizeof(hs->path) - 1);

#ifdef _WIN32
	{
		char name[300];

		/* COM10 and above need the \\.\ form, and it is valid for the
		   low-numbered ones too, so always use it. */
		if (strncmp(device, "\\\\", 2) == 0) {
			snprintf(name, sizeof(name), "%s", device);
		} else {
			snprintf(name, sizeof(name), "\\\\.\\%s", device);
		}

		hs->handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0,
		    NULL, OPEN_EXISTING, 0, NULL);
		if (hs->handle == INVALID_HANDLE_VALUE) {
			const DWORD err = GetLastError();

			if (err == ERROR_FILE_NOT_FOUND) {
				snprintf(error, errlen,
				    "There is no serial port called %s.", device);
			} else if (err == ERROR_ACCESS_DENIED) {
				snprintf(error, errlen,
				    "%s is in use by another program.", device);
			} else {
				snprintf(error, errlen,
				    "%s could not be opened (error %lu).",
				    device, (unsigned long) err);
			}
			return -1;
		}
	}
#else
	/* O_NOCTTY so a tty cannot become this process's controlling terminal,
	   and O_NONBLOCK so neither open() nor a later read() can stall the
	   emulation thread on a port whose carrier is absent. */
	hs->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (hs->fd < 0) {
		const int err = errno;

		if (err == EACCES) {
			snprintf(error, errlen,
			    "No permission to use %s. On Linux, serial ports "
			    "usually need your user to be in the 'dialout' "
			    "group; you will need to log out and in again after "
			    "being added to it.", device);
		} else if (err == ENOENT) {
			snprintf(error, errlen,
			    "There is no device at %s. Check that the adapter "
			    "is plugged in.", device);
		} else if (err == EBUSY) {
			snprintf(error, errlen,
			    "%s is in use by another program.", device);
		} else {
			snprintf(error, errlen, "%s could not be opened: %s.",
			    device, strerror(err));
		}
		return -1;
	}

	/* Move off stdin/stdout/stderr if we landed on one. Code elsewhere that
	   closes a descriptor it believes it owns would otherwise take ours with
	   it, which is exactly what a zero-initialised socket field in the modem
	   used to do. */
	if (hs->fd <= 2) {
		const int high = fcntl(hs->fd, F_DUPFD, 3);

		if (high >= 0) {
			close(hs->fd);
			hs->fd = high;
		}
	}
#endif

	hs->is_open = 1;

	memset(&dev, 0, sizeof(dev));
	dev.name = "Host serial port";
	dev.on_write = host_on_write;
	dev.on_ctrl = host_on_ctrl;
	dev.get_status = host_get_status;
	dev.on_reset = host_on_reset;
	dev.userdata = (void *) (uintptr_t) port;

	if (serial_bus_attach(port, &dev) != 0) {
		snprintf(error, errlen,
		    "%s already has something attached to it.", port_name(port));
		serial_host_close(port);
		return -1;
	}

	/* Settings are applied on the first poll, from whatever the guest has
	   programmed by then, rather than guessed at now. */
	hs->line_valid = 0;
	hs->status = read_status_lines(hs);
	serial_bus_device_status(port, hs->status);

	rpclog("serial_host: %s attached to %s\n", port_name(port), device);
	return 0;
}

void
serial_host_close(SerialPortID port)
{
	HostSerial *hs;

	if (port >= SERIAL_PORT_COUNT) {
		return;
	}
	hs = &host_port[port];

	if (!host_is_open(hs)) {
		return;
	}

	if (hs->tx_dropped > 0) {
		rpclog("serial_host: %s: %lu transmitted byte(s) were lost to a "
		       "full buffer\n", port_name(port), hs->tx_dropped);
	}

	hs->is_open = 0;
#ifdef _WIN32
	CloseHandle(hs->handle);
	hs->handle = INVALID_HANDLE_VALUE;
#else
	close(hs->fd);
	hs->fd = -1;
#endif

	serial_bus_detach(port);
	rpclog("serial_host: %s detached from %s\n", port_name(port), hs->path);
}

/*
 * The device has gone: unplugged, or the far end of a pseudo terminal closed.
 * Drop it rather than logging the same error every poll, and let the guest see
 * the status lines go away, which is what would happen if a real cable were
 * pulled out.
 */
static void
host_lost(SerialPortID port, const char *why)
{
	rpclog("serial_host: %s lost (%s)\n", port_name(port), why);
	serial_bus_device_status(port, 0);
	serial_host_close(port);
}

/* ========================================================================
 * Pump
 * ======================================================================== */

static void
poll_port(SerialPortID port)
{
	HostSerial *hs = &host_port[port];
	uint8_t buf[64];
	uint16_t divisor = 0;
	uint8_t lcr = 0;
	uint8_t status;
	int space;

	if (!host_is_open(hs)) {
		return;
	}

	/* Follow the guest's line settings, including the first time round.
	   DLAB (bit 7) is masked off: it selects whether the divisor latches are
	   visible, not a line setting, and the guest toggles it every time it
	   programs the speed. Comparing it would re-apply the host's termios
	   settings continuously, which is slow enough to be felt. */
	if (superio_serial_get_line(port, &divisor, &lcr) == 0 &&
	    ((lcr &= 0x7f), !hs->line_valid || divisor != hs->divisor ||
	     lcr != hs->lcr)) {
		apply_line_settings(hs, port, divisor, lcr);
		hs->divisor = divisor;
		hs->lcr = lcr;
		hs->line_valid = 1;
	}

	/* Guest to host. A partial write is normal and the rest waits. */
	while (tx_used(hs) > 0) {
		const size_t contiguous = hs->tx_head < hs->tx_tail
		    ? hs->tx_tail - hs->tx_head
		    : HOST_TX_RING - hs->tx_head;
#ifdef _WIN32
		DWORD written = 0;

		if (!WriteFile(hs->handle, hs->tx + hs->tx_head,
		        (DWORD) contiguous, &written, NULL)) {
			host_lost(port, "write failed");
			return;
		}
		if (written == 0) {
			break;		/* buffer full, try again next time */
		}
		hs->tx_head = (hs->tx_head + written) % HOST_TX_RING;
#else
		const ssize_t written = write(hs->fd, hs->tx + hs->tx_head,
		    contiguous);

		if (written < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EINTR) {
				break;	/* buffer full, try again next time */
			}
			host_lost(port, strerror(errno));
			return;
		}
		if (written == 0) {
			break;
		}
		hs->tx_head = (hs->tx_head + (size_t) written) % HOST_TX_RING;
#endif
	}

	/* Host to guest, but only as much as the guest can take. Leaving the
	   rest in the host's buffer is what makes flow control work. */
	space = superio_serial_rx_space(port);
	if (space > (int) sizeof(buf)) {
		space = (int) sizeof(buf);
	}
	if (space > 0) {
#ifdef _WIN32
		DWORD got = 0;

		if (!ReadFile(hs->handle, buf, (DWORD) space, &got, NULL)) {
			host_lost(port, "read failed");
			return;
		}
#else
		const ssize_t got = read(hs->fd, buf, (size_t) space);

		if (got < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				host_lost(port, strerror(errno));
				return;
			}
		} else if (got == 0) {
			/* End of file on a pseudo terminal means the program on
			   the other side has gone. A real port does not do this. */
			host_lost(port, "the other end closed");
			return;
		}
#endif
		if (got > 0) {
			int i;

			for (i = 0; i < (int) got; i++) {
				serial_bus_device_write_data(port, buf[i]);
			}
		}
	}

	/* Line states, reported only when they change so the guest is not given
	   a modem status interrupt on every poll. */
	status = read_status_lines(hs);
	if (status != hs->status) {
		hs->status = status;
		serial_bus_device_status(port, status);
	}
}

void
serial_host_poll(void)
{
	int port;

	for (port = 0; port < SERIAL_PORT_COUNT; port++) {
		poll_port((SerialPortID) port);
	}
}

void
serial_host_shutdown(void)
{
	int port;

	for (port = 0; port < SERIAL_PORT_COUNT; port++) {
		serial_host_close((SerialPortID) port);
	}
}
