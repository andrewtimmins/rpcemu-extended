/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2024-2025 RPCEmu contributors

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "rpcemu.h"
#include "printer.h"
#include "parallel.h"

/* Buffer size for print data (1MB) */
#define PRINTER_BUFFER_SIZE (1024 * 1024)

/* ========================================================================
 * Printer State
 * ======================================================================== */

static PrinterOutputMode output_mode = PrinterOutput_Disabled;
static char output_path[512] = "";

static uint8_t *print_buffer = NULL;
static size_t buffer_pos = 0;
static int job_number = 1;

static int attached_port = -1;  /* Which port we're attached to, or -1 */

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
    (void)ctrl;
    /* We don't need to do anything special with control signals */
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
    
    if (output_path[0] != '\0') {
        snprintf(filename, size, "%s/printjob_%s_%03d.prn",
                 output_path, timestamp, job_number);
    } else {
        snprintf(filename, size, "printjob_%s_%03d.prn",
                 timestamp, job_number);
    }
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
    
    generate_filename(filename, sizeof(filename));
    
    f = fopen(filename, "wb");
    if (f == NULL) {
        rpclog("Printer: Failed to open '%s'\n", filename);
        buffer_pos = 0;
        return;
    }
    
    fwrite(print_buffer, 1, buffer_pos, f);
    fclose(f);
    
    rpclog("Printer: Wrote %zu bytes to '%s'\n", buffer_pos, filename);
    
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
    rpclog("Printer: Output path = '%s'\n", output_path);
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
