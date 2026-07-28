/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2024-2025 Andy Timmins

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
 * Virtual Printer Device Implementation
 *
 * This module implements a virtual printer as a parallel bus device.
 * It captures data sent via the parallel port and saves to files.
 *
 * The printer behaves like a simple Centronics-compatible printer:
 * - Accepts data on strobe
 * - Always ready (never busy)
 * - Generates ACK after each byte
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "rpcemu.h"
#include "rpcemu-win.h"
#include "printer.h"
#include "parallel.h"
#include "print_convert.h"

/* Buffer size for print data (1MB) */
#define PRINTER_BUFFER_SIZE (1024 * 1024)

/* Flush buffered data if no bytes arrive for this long (milliseconds) */
#define PRINTER_IDLE_FLUSH_MS          500
#define PRINTER_IDLE_FLUSH_LONG_MS     5000

/* ========================================================================
 * Printer State
 * ======================================================================== */

static PrinterOutputMode output_mode = PrinterOutput_Disabled;

/* Device path or print queue name for PrinterOutput_Host. */
static char host_target[512] = "";
static char output_path[512] = "";

static uint8_t *print_buffer = NULL;
static size_t buffer_pos = 0;
static int job_number = 1;

static int attached_port = -1;  /* Which port we're attached to, or -1 */
static uint64_t last_write_ms = 0;
static uint8_t last_ctrl = PARALLEL_CTRL_INIT | PARALLEL_CTRL_SELECT;
static int auto_pdf_enabled = 0;

static int
printer_buffer_looks_complete(void)
{
    static const char ps_eof[] = "%%EOF";
    size_t eof_len = sizeof(ps_eof) - 1;

    if (buffer_pos < eof_len) {
        return 0;
    }

    if (memcmp(print_buffer, "%!PS", 4) != 0) {
        return 0;
    }

    return (memcmp(print_buffer + buffer_pos - eof_len, ps_eof, eof_len) == 0) ? 1 : 0;
}

static uint64_t
printer_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static const char *
printer_effective_output_path(void)
{
    static char default_path[512];

    if (output_path[0] != '\0') {
        return output_path;
    }

    snprintf(default_path, sizeof(default_path), "%sprintjobs",
             rpcemu_get_machine_datadir());
    return default_path;
}

static void
printer_ensure_output_dir(void)
{
    const char *path = printer_effective_output_path();

    mkdir(path, 0777);
}

/* ========================================================================
 * Parallel Device Callbacks
 * ======================================================================== */

/**
 * Called when host strobes data to the printer.
 */
static void
printer_on_write(uint8_t data, void *userdata)
{
    (void)userdata;
    
    if (output_mode == PrinterOutput_Disabled || print_buffer == NULL) {
        return;
    }
    
    if (buffer_pos < PRINTER_BUFFER_SIZE) {
        print_buffer[buffer_pos++] = data;
        
        /* Log first 10 bytes and periodic updates */
        if (buffer_pos <= 10 || (buffer_pos % 1000) == 0) {
            rpclog("Printer: Captured byte %zu: 0x%02X '%c'\n", 
                   buffer_pos, data, (data >= 32 && data < 127) ? data : '.');
        }
    } else {
        /* Buffer full - auto flush */
        printer_flush();
        print_buffer[buffer_pos++] = data;
    }

    last_write_ms = printer_now_ms();
    
    /* Signal ACK to the host (byte accepted) */
    if (attached_port >= 0) {
        parallel_bus_device_ack((ParallelPortID)attached_port);
    }
}

/**
 * Called when host changes control signals.
 */
static void
printer_on_ctrl(uint8_t ctrl, void *userdata)
{
    (void)userdata;

    /* RISC OS drivers often leave SELECT asserted, but flush if they deselect */
    if ((last_ctrl & PARALLEL_CTRL_SELECT) && !(ctrl & PARALLEL_CTRL_SELECT)) {
        if (buffer_pos > 0) {
            rpclog("Printer: Flush on deselect\n");
            printer_flush();
        }
    }

    last_ctrl = ctrl;
}

/**
 * Get printer status.
 */
static uint8_t
printer_get_status(void *userdata)
{
    (void)userdata;
    
    /* Always ready: not busy, ACK idle, paper present, online, no error */
    return PARALLEL_STAT_BUSY | PARALLEL_STAT_ACK | 
           PARALLEL_STAT_SELECT | PARALLEL_STAT_ERROR;
}

/**
 * Called when printer is reset via nInit signal.
 */
static void
printer_on_reset(void *userdata)
{
    (void)userdata;
    
    rpclog("Printer: Reset via parallel bus\n");
    
    /* Flush any pending data */
    if (buffer_pos > 0) {
        printer_flush();
    }
}

/* Printer device descriptor */
static const ParallelDevice printer_device = {
    .name = "Virtual Printer",
    .on_write = printer_on_write,
    .on_ctrl = printer_on_ctrl,
    .get_status = printer_get_status,
    .on_reset = printer_on_reset,
    .userdata = NULL
};

/* ========================================================================
 * Printer API
 * ======================================================================== */

void
printer_init(void)
{
    if (print_buffer == NULL) {
        print_buffer = malloc(PRINTER_BUFFER_SIZE);
        if (print_buffer == NULL) {
            rpclog("Printer: Failed to allocate buffer\n");
            return;
        }
    }
    
    buffer_pos = 0;
    attached_port = -1;
    
    rpclog("Printer: Initialized\n");
}

void
printer_reset(void)
{
    if (buffer_pos > 0) {
        printer_flush();
    }
    buffer_pos = 0;
}

void
printer_shutdown(void)
{
    if (buffer_pos > 0) {
        printer_flush();
    }
    
    printer_detach();
    
    if (print_buffer != NULL) {
        free(print_buffer);
        print_buffer = NULL;
    }
    
    rpclog("Printer: Shutdown\n");
}

int
printer_attach(ParallelPortID port)
{
    if (attached_port >= 0) {
        printer_detach();
    }
    
    if (parallel_bus_attach(port, &printer_device) < 0) {
        rpclog("Printer: Failed to attach to LPT%d\n", port + 1);
        return -1;
    }
    
    attached_port = (int)port;
    rpclog("Printer: Attached to LPT%d\n", port + 1);
    return 0;
}

void
printer_detach(void)
{
    if (attached_port >= 0) {
        parallel_bus_detach((ParallelPortID)attached_port);
        rpclog("Printer: Detached from LPT%d\n", attached_port + 1);
        attached_port = -1;
    }
}

int
printer_get_port(void)
{
    return attached_port;
}

static void
generate_filename(char *filename, size_t size)
{
    time_t now;
    struct tm *tm_info;
    char timestamp[64];
    
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
    
    snprintf(filename, size, "%s/printjob_%s_%03d.prn",
             printer_effective_output_path(), timestamp, job_number);
}

void
printer_set_host_target(const char *target)
{
    if (target == NULL) {
        host_target[0] = '\0';
        return;
    }
    strncpy(host_target, target, sizeof(host_target) - 1);
    host_target[sizeof(host_target) - 1] = '\0';
}

/** True if the host target names a filesystem object rather than a queue. */
static int
host_target_is_path(const char *target)
{
#ifdef _WIN32
    return target[0] == '\\' || (target[1] == ':' && target[2] == '\\') ||
           (target[1] == ':' && target[2] == '/');
#else
    return target[0] == '/';
#endif
}

/*
 * Hand a finished job to the host.
 *
 * A device path is opened and written to. A queue name is spooled through the
 * host's own print system as raw data, because the guest has already produced
 * whatever its RISC OS printer driver emits (PCL, ESC/P, PostScript) and nothing
 * here should try to interpret or convert it.
 *
 * Returns 0 on success.
 */
static int
printer_send_to_host(const uint8_t *data, size_t len)
{
    FILE *f;
    size_t written;

    if (host_target[0] == '\0') {
        rpclog("Printer: no host printer set, discarding %zu bytes\n", len);
        return -1;
    }

    if (host_target_is_path(host_target)) {
        f = fopen(host_target, "wb");
        if (f == NULL) {
            rpclog("Printer: could not open '%s': %s\n", host_target,
                   strerror(errno));
            return -1;
        }
        written = fwrite(data, 1, len, f);
        if (fclose(f) != 0 || written != len) {
            rpclog("Printer: writing to '%s' failed after %zu of %zu bytes\n",
                   host_target, written, len);
            return -1;
        }
        rpclog("Printer: sent %zu bytes to '%s'\n", len, host_target);
        return 0;
    }

#ifdef _WIN32
    /* Spooling by queue name needs the Windows print API, which is not wired up
       here. A device path (\\.\LPT1, or a file) does work, and the virtual
       printer covers the rest. */
    rpclog("Printer: '%s' looks like a print queue; on Windows give a device "
           "path instead, or use the virtual printer\n", host_target);
    return -1;
#else
    {
        char command[640];
        int status;

        /* -o raw: pass the bytes through untouched. The queue name is quoted so
           a name with spaces cannot become extra arguments. */
        snprintf(command, sizeof(command), "lp -d '%s' -o raw -", host_target);

        f = popen(command, "w");
        if (f == NULL) {
            rpclog("Printer: could not run lp: %s\n", strerror(errno));
            return -1;
        }
        written = fwrite(data, 1, len, f);
        status = pclose(f);
        if (status != 0 || written != len) {
            rpclog("Printer: lp -d '%s' failed (status %d, %zu of %zu bytes)\n",
                   host_target, status, written, len);
            return -1;
        }
        rpclog("Printer: spooled %zu bytes to queue '%s'\n", len, host_target);
        return 0;
    }
#endif
}

void
printer_flush(void)
{
    char filename[1024];
    FILE *f;
    
    if (buffer_pos == 0) {
        return;
    }
    
    if (output_mode == PrinterOutput_Disabled) {
        rpclog("Printer: Discarding %zu bytes (disabled)\n", buffer_pos);
        buffer_pos = 0;
        return;
    }
    
    if (print_buffer == NULL) {
        buffer_pos = 0;
        return;
    }
    
    if (output_mode == PrinterOutput_Host) {
        (void) printer_send_to_host(print_buffer, buffer_pos);
        job_number++;
        buffer_pos = 0;
        return;
    }

    generate_filename(filename, sizeof(filename));

    printer_ensure_output_dir();

    f = fopen(filename, "wb");
    if (f == NULL) {
        rpclog("Printer: Failed to open '%s'\n", filename);
        buffer_pos = 0;
        return;
    }
    
    fwrite(print_buffer, 1, buffer_pos, f);
    fclose(f);
    
    rpclog("Printer: Wrote %zu bytes to '%s'\n", buffer_pos, filename);

    if (auto_pdf_enabled && print_convert_available()) {
        char pdf_filename[1024];
        char errbuf[256];
        PrintConvertResult result;

        print_convert_prn_to_pdf_path(filename, pdf_filename, sizeof(pdf_filename));
        result = print_convert_prn_to_pdf(filename, pdf_filename, errbuf, sizeof(errbuf));
        if (result != PrintConvert_OK) {
            rpclog("Printer: PDF conversion failed for '%s': %s\n",
                   filename, errbuf[0] != '\0' ? errbuf : "unknown error");
        }
    }
    
    job_number++;
    buffer_pos = 0;
}

void
printer_set_output_mode(PrinterOutputMode mode)
{
    if (mode != output_mode && buffer_pos > 0) {
        printer_flush();
    }
    output_mode = mode;
    rpclog("Printer: Output mode = %d\n", mode);
}

PrinterOutputMode
printer_get_output_mode(void)
{
    return output_mode;
}

void
printer_set_output_path(const char *path)
{
    if (path != NULL) {
        strncpy(output_path, path, sizeof(output_path) - 1);
        output_path[sizeof(output_path) - 1] = '\0';
    } else {
        output_path[0] = '\0';
    }
    rpclog("Printer: Output path = '%s'\n",
           path != NULL && path[0] != '\0' ? output_path : printer_effective_output_path());
}

const char *
printer_get_output_path(void)
{
    return output_path;
}

size_t
printer_get_buffer_size(void)
{
    return buffer_pos;
}

int
printer_has_pending_data(void)
{
    return (buffer_pos > 0) ? 1 : 0;
}

void
printer_poll(void)
{
    uint64_t now;
    uint64_t idle_ms;
    uint64_t required_idle_ms;

    if (buffer_pos == 0 || output_mode == PrinterOutput_Disabled ||
        last_write_ms == 0) {
        return;
    }

    now = printer_now_ms();
    idle_ms = now - last_write_ms;

    if (printer_buffer_looks_complete()) {
        required_idle_ms = PRINTER_IDLE_FLUSH_MS;
    } else {
        required_idle_ms = PRINTER_IDLE_FLUSH_LONG_MS;
    }

    if (idle_ms >= required_idle_ms) {
        rpclog("Printer: Flush after idle timeout (%llums, %zu bytes)\n",
               (unsigned long long)idle_ms, buffer_pos);
        printer_flush();
        last_write_ms = 0;
    }
}

void
printer_set_auto_pdf(int enable)
{
    auto_pdf_enabled = (enable != 0) ? 1 : 0;
    rpclog("Printer: Auto PDF conversion = %d\n", auto_pdf_enabled);
}

int
printer_get_auto_pdf(void)
{
    return auto_pdf_enabled;
}

/* ========================================================================
 * Legacy API (for backward compatibility)
 * These are stubs - the parallel bus is now the primary interface
 * ======================================================================== */

void
printer_write_data(uint8_t data)
{
    /* Legacy: directly write to printer (bypasses parallel bus) */
    printer_on_write(data, NULL);
}

void
printer_set_strobe(int strobe)
{
    /* Legacy: strobe handling now done by parallel bus */
    (void)strobe;
}

int
printer_is_busy(void)
{
    /* Never busy */
    return 0;
}
