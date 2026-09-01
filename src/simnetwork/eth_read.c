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

    packet->len = 0;

    if (dev->asynch_io) {
#if ETH_THREADING_AVAILABLE
        /* Async mode: dequeue from reader thread's queue */
        status = 0;
        /* Lock-free dequeue - sim_tailq_t is SPSC safe */
        if (!sim_tailq_empty(&dev->read_queue)) {
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
        if (status && routine != NULL)
            routine(0);
#else
        status = 0; /* Should never reach here */
#endif
    } else {
        /* Sync mode: poll backend directly */
        dev->read_packet = packet;
        dev->read_callback = routine;

        /* dispatch read request to receive a filtered packet or timeout */
        do {
            status = 0;

            switch (dev->backend->eth_api) {
            case ETH_API_PCAP:
                status = eth_reader_pcap(dev->backend, dev);
                break;

            case ETH_API_TAP:
#ifdef HAVE_TAP_NETWORK
            {
                int len;
                u_char buf[ETH_MAX_JUMBO_FRAME];

                len = read(dev->backend->state.eth_socket, buf, sizeof(buf));
                if (len > 0) {
                    status = 1;
                    eth_process_received_packet(dev, buf, len, len);
                } else {
                    status = (len < 0) ? -1 : 0;
                }
            }
#endif /* HAVE_TAP_NETWORK */
            break;

            case ETH_API_NAT:
#ifdef HAVE_SLIRP_NETWORK
                status = eth_reader_nat(dev->backend, dev);
#else
                status = -1;
                sim_messagef(SCPE_IERR, "NAT network API not available");
#endif
                break;

            case ETH_API_VDE:
#ifdef HAVE_VDE_NETWORK
            {
                int len;
                u_char buf[ETH_MAX_JUMBO_FRAME];

                len = vde_recv(dev->backend->state.vde, buf, sizeof(buf), 0);
                if (len > 0) {
                    status = 1;
                    eth_process_received_packet(dev, buf, len, len);
                } else {
                    status = (len < 0) ? -1 : 0;
                }
            }
#endif /* HAVE_VDE_NETWORK */
            break;

            case ETH_API_UDP: {
                int len;
                u_char buf[ETH_MAX_JUMBO_FRAME];

                len = (int)sim_read_sock(dev->backend->state.eth_socket, (char *)buf, (int32_t)sizeof(buf));
                if (len > 0) {
                    status = 1;
                    eth_process_received_packet(dev, buf, len, len);
                } else {
                    status = (len < 0) ? -1 : 0;
                }
            } break;

            case ETH_API_TEST:
                status = eth_test_read(dev, packet, routine);
                break;

            case ETH_API_NONE:
            case ETH_API_COUNT:
                status = -1;
                break;
            }
        } while ((status > 0) && (0 == packet->len));

        if (status < 0) {
            ++dev->receive_packet_errors;
            _eth_error(dev, "eth_reader");
        }
    }

    return status;
}
