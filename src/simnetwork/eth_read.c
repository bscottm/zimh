// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "simnetwork/eth_backends.h"

/* Read a packet from the Ethernet device, simulator side.
 *
 * Returns:
 * > 0: Packet read successfully, queued for simulator device.
 *   0: No packet read (not an error, safe to retry)
 * < 0: Error reading packet.
 */
int eth_read(ETH_DEV *dev, ETH_PACK *packet, ETH_PCALLBACK routine)
{
    int status;

    /* Make sure device exists, the backend is legitimate and the packet is
     * also valid. */
    if (dev == NULL || dev->backend->eth_api == ETH_API_NONE || packet == NULL)
        return 0;

    status = 0;
    packet->len = 0;

    if (!dev->asynch_io) {
        /* Polled/direct read. */
        dev->read_packet = packet;
        dev->read_callback = routine;

        eth_backend_t *backend = dev->backend;

        /* Check if packet waiting. Note the zero (0) timeout -- immediate return whether
         * packet data is available or not. */
        status = backend->eth_funcs->packet_wait(backend, dev, 0);

        /* Packet available? */
        if (status > 0) {
            /* Have backend deliver it. Note that the backend's packet_read will
             * enqueue the packet onto the device's read queue. */
            status = backend->eth_funcs->packet_read(backend, dev);
        }
    }

    /* Dequeue from reader's queue. Lock-free dequeue - sim_tailq_t is SPSC safe */
    if (status > 0 && !sim_tailq_empty(&dev->read_queue)) {
        struct eth_item *item = (struct eth_item *)sim_tailq_dequeue(&dev->read_queue);
        if (item) {
            const uint8_t *src_data = item->packet.oversize ? item->packet.oversize : item->packet.msg;
            packet->len = item->packet.len;
            packet->crc_len = item->packet.crc_len;
            memcpy(packet->msg, src_data, ((packet->len > packet->crc_len) ? packet->len : packet->crc_len));
            status = 1;
            ethq_item_free((sim_tailq_item_t)item);
        }
    }

    if (status > 0 && dev->asynch_io && routine != NULL) {
        routine(0);
    } else if (status < 0 && !dev->asynch_io) {
        /* Polled/direct read: deal with the error. */
        ++dev->receive_packet_errors;
        eth_error(dev, "eth_read");
    }

    return status;
}
