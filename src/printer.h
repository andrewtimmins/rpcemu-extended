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
 * Virtual Printer Device
 *
 * This module implements a virtual printer that can be attached to
 * the parallel bus. It captures all data sent to it and saves to files.
 */

#ifndef PRINTER_H
#define PRINTER_H

#include <stdint.h>
#include <stddef.h>
#include "parallel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Printer Output Modes
 * ======================================================================== */

typedef enum {
    PrinterOutput_Disabled = 0,  /* Printer not attached */
    PrinterOutput_File = 1,      /* Output to raw file */
} PrinterOutputMode;

/* ========================================================================
 * Printer API
 * ======================================================================== */

/**
 * Initialize the printer subsystem.
 */
void printer_init(void);

/**
 * Reset the printer.
 */
void printer_reset(void);

/**
 * Shut down the printer, flushing any pending data.
 */
void printer_shutdown(void);

/**
 * Attach the printer to a parallel port.
 * @param port  Which port to attach to (LPT1 or LPT2)
 * @return 0 on success, -1 on failure
 */
int printer_attach(ParallelPortID port);

/**
 * Detach the printer from the parallel bus.
 */
void printer_detach(void);

/**
 * Get which port the printer is attached to.
 * @return Port ID, or -1 if not attached
 */
int printer_get_port(void);

/**
 * Flush the current print job to file.
 */
void printer_flush(void);

/**
 * Set the output mode.
 * @param mode  Output mode
 */
void printer_set_output_mode(PrinterOutputMode mode);

/**
 * Get the current output mode.
 * @return Current mode
 */
PrinterOutputMode printer_get_output_mode(void);

/**
 * Set the output directory path.
 * @param path  Directory path
 */
void printer_set_output_path(const char *path);

/**
 * Get the current output path.
 * @return Output path
 */
const char *printer_get_output_path(void);

/**
 * Get the current buffer size.
 * @return Bytes in buffer
 */
size_t printer_get_buffer_size(void);

/**
 * Check if there is pending data.
 * @return Non-zero if data pending
 */
int printer_has_pending_data(void);

/* ========================================================================
 * Legacy API (for backward compatibility with old superio.c)
 * ======================================================================== */

/**
 * Write data byte (legacy - use parallel bus instead).
 */
void printer_write_data(uint8_t data);

/**
 * Set strobe signal (legacy - use parallel bus instead).
 */
void printer_set_strobe(int strobe);

/**
 * Check if busy (legacy - use parallel bus instead).
 */
int printer_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* PRINTER_H */
