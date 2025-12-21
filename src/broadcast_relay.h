/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

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
 * broadcast_relay.h - UDP broadcast relay for Access+/ShareFS support
 *
 * Provides Access+ file sharing support over RPCEmu's NAT networking.
 * Access+ discovery uses UDP broadcasts which don't traverse NAT, so
 * this module bridges traffic between the guest's virtual network
 * (10.10.10.x) and the host's physical network.
 *
 * UDP ports handled:
 *   32770 - Discovery and share announcements (broadcast)
 *   32771 - Share management
 *   49171 - File operations
 *
 * Outgoing broadcasts are relayed to the host network; incoming
 * responses are repackaged and injected into the guest. Payloads
 * exceeding Ethernet MTU are delivered via IP fragmentation.
 */

#ifndef BROADCAST_RELAY_H
#define BROADCAST_RELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the broadcast relay.
 * Call from network_nat_init() after SLiRP is initialized.
 *
 * @return 0 on success, -1 on failure (relay disabled, emulator continues)
 */
int broadcast_relay_init(void);

/**
 * Shutdown the broadcast relay and release resources.
 * Call from network cleanup.
 */
void broadcast_relay_close(void);

/**
 * Poll for incoming broadcasts from the host network.
 * Call from network_nat_poll() on each iteration.
 * This is non-blocking - returns immediately if no data.
 */
void broadcast_relay_poll(void);

/**
 * Check if an outgoing packet from the guest is a broadcast
 * that should be relayed to the host network.
 *
 * @param pkt     Complete Ethernet frame from guest
 * @param pkt_len Length of frame in bytes
 *
 * @return 1 if packet was relayed (still pass to SLiRP too), 0 otherwise
 */
int broadcast_relay_tx(const uint8_t *pkt, int pkt_len);

/**
 * Get relay statistics for debugging.
 *
 * @param tx_count   Packets relayed guest -> host
 * @param rx_count   Packets relayed host -> guest
 * @param dropped    Packets dropped (rate limit, errors)
 */
void broadcast_relay_stats(uint32_t *tx_count, uint32_t *rx_count, uint32_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* BROADCAST_RELAY_H */
