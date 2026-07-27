/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2021 RiscOS Cloverleaf
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
 * Shared clipboard: the host half.
 *
 * The interface here - the SWI number, its four reason codes, and the pollword
 * the host sets when its clipboard changes - is RiscOS Cloverleaf's, from their
 * RPCEmu fork at https://github.com/riscoscloverleaf/rpcemu, and this file is
 * derived from their src/hostclipboard.c. It is kept identical on purpose, so
 * their guest module and ours are interchangeable. Two details of the design are
 * theirs and worth naming: the guest hands over RISC OS's own UCS conversion
 * table at setup, so text is converted through the alphabet the machine is
 * actually configured for rather than an assumed one; and the host announces a
 * change by setting a pollword in guest memory, which the guest's Wimp task
 * waits on, so nothing has to poll.
 *
 * Text and images. The filetype travels with the data, which is what let images
 * be added without changing the interface: Cloverleaf's guest module already
 * asks for PNG and JPEG alongside text when it goes looking for something to
 * paste, and passes the type straight through, so only this side had to learn
 * about them.
 *
 * Images are carried as the file the host and the guest already agree on - a
 * PNG or a JPEG - and are never converted here. RISC OS sprites are not offered:
 * that would mean a sprite encoder and decoder, and every RISC OS application
 * worth pasting into reads PNG.
 */

#ifndef HOSTCLIPBOARD_H
#define HOSTCLIPBOARD_H

#include <stdint.h>

#include "hostfs.h"

/* Reason codes in R0, as Cloverleaf defined them. */
#define ARCEM_SWI_CLIPBOARD_SETUP	1
#define ARCEM_SWI_CLIPBOARD_HOST_SET	2
#define ARCEM_SWI_CLIPBOARD_HOST_GET	3
#define ARCEM_SWI_CLIPBOARD_HOST_CHECK	4

/* Their SWI number, kept so a Cloverleaf-built guest module works here. */
#define ARCEM_SWI_CLIPBOARD	(ARCEM_SWI_CHUNK + 0x10)

/* Bit the host sets in the guest's pollword. */
#define CLIPBOARD_POLLWORD_HOST_CHANGED	1

/* RISC OS filetypes carried. The two image types are the ones Cloverleaf's guest
   module asks for, so a RISC OS application offering either is understood. */
#define CLIPBOARD_TYPE_TEXT	0xfff
#define CLIPBOARD_TYPE_PNG	0xb60
#define CLIPBOARD_TYPE_JPEG	0xc85

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle the clipboard SWI issued by the guest module.
 */
extern void clipboard_swi(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                          uint32_t r4, uint32_t r5, uint32_t *retr0, uint32_t *retr1);

/**
 * Tell the guest the host's clipboard has changed. Called from the GUI thread
 * with text in the host's own encoding (UTF-8 as wxWidgets gives it).
 */
extern void clipboard_host_changed(int file_type, const char *data, unsigned int data_len);

/**
 * How the front end is handed text the guest has copied. Registered by the GUI;
 * left unset in builds with no clipboard of their own (headless, the tests), so
 * the core does not depend on the front end at link time.
 */
typedef void (*clipboard_host_setter)(const char *utf8, unsigned int len);

extern void clipboard_set_host_setter(clipboard_host_setter setter);

/**
 * The same for an image the guest has copied, handed over as the encoded file
 * (CLIPBOARD_TYPE_PNG or CLIPBOARD_TYPE_JPEG) rather than as pixels.
 */
typedef void (*clipboard_host_image_setter)(int file_type, const void *data,
                                            unsigned int len);

extern void clipboard_set_host_image_setter(clipboard_host_image_setter setter);

/** Is this one of the image types carried? */
extern int clipboard_type_is_image(int file_type);

/**
 * Forget everything, e.g. on reset: the guest's pollword address is no longer
 * valid and its module will set up again.
 */
extern void clipboard_reset(void);

#ifdef __cplusplus
}
#endif

#endif
