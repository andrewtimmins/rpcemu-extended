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

#ifndef PERIPHERAL_CONFIG_H
#define PERIPHERAL_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PeripheralSerial_Disabled = 0,
	PeripheralSerial_LogToFile = 1,
	PeripheralSerial_TcpModem = 2,
	PeripheralSerial_PhysicalDevice = 3
} PeripheralSerialMode;

typedef enum {
	PeripheralParallel_Disabled = 0,
	PeripheralParallel_LogToFile = 1,
	PeripheralParallel_VirtualPrinter = 2,
	/* 3 was "physical device", which implied pin-level access to a real
	   parallel port. That is not what this does: the print stream is handed
	   to a printer on the host. The value is unchanged so existing machine
	   configurations keep working. */
	PeripheralParallel_HostPrinter = 3
} PeripheralParallelMode;

typedef struct {
	PeripheralSerialMode com1_mode;
	char com1_log_path[512];
	char com1_device[256];

	PeripheralSerialMode com2_mode;
	char com2_log_path[512];
	char com2_device[256];

	PeripheralParallelMode parallel_mode;
	char parallel_log_path[512];
	char parallel_device[256];
	char printer_output_path[512];
	int printer_auto_pdf;
} PeripheralConfig;

extern PeripheralConfig peripheral_config;

void peripheral_config_set_defaults(void);
void peripheral_config_apply(void);
void peripheral_config_shutdown(void);
void serial_modem_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PERIPHERAL_CONFIG_H */
