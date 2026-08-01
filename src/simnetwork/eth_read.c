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
    if (dev->backend->eth_api == ETH_API_TEST)
        return eth_test_read(dev, packet, routine);

#if !defined(USE_READER_THREAD)
    /* set read packet */
    dev->read_packet = packet;

    /* set optional callback routine */
    dev->read_callback = routine;

    /* dispatch read request to either receive a filtered packet or timeout */
    do {
        status = 0;

        switch (dev->backend->eth_api) {
        case ETH_API_PCAP:
            status = eth_reader_pcap(&dev->backend, dev);
            break;

        case ETH_API_TAP:
#    ifdef HAVE_TAP_NETWORK
        {
            int len;
            u_char buf[ETH_MAX_JUMBO_FRAME];

            len = read(dev->backend->state.eth_socket, buf, sizeof(buf));
            if (len > 0) {
                status = 1;
                eth_process_received_packet(dev, buf, len, len);
            } else {
                if (len < 0)
                    status = -1;
                else
                    status = 0;
            }
        }
#    endif /* HAVE_TAP_NETWORK */
        break;

        case ETH_API_NAT:
#    ifdef HAVE_SLIRP_NETWORK
            status = -1;
            sim_messagef(SCPE_IERR, "USE_READER_THREAD must be defined to use the SLIRP network API");
#    endif /* HAVE_SLIRP_NETWORK */
            break;

        case ETH_API_VDE:
#    ifdef HAVE_VDE_NETWORK
        {
            int len;
            u_char buf[ETH_MAX_JUMBO_FRAME];

            len = vde_recv(dev->backend->state.vde, buf, sizeof(buf), 0);
            if (len > 0) {
                status = 1;
                eth_process_received_packet(dev, buf, len, len);
            } else {
                if (len < 0)
                    status = -1;
                else
                    status = 0;
            }
        }
#    endif /* HAVE_VDE_NETWORK */
        break;

        case ETH_API_UDP: {
            int len;
            u_char buf[ETH_MAX_JUMBO_FRAME];

            len = (int)sim_read_sock(dev->backend->state.eth_socket, (char *)buf, (int32_t)sizeof(buf));
            if (len > 0) {
                status = 1;
                eth_process_received_packet(dev, buf, len, len);
            } else {
                if (len < 0)
                    status = -1;
                else
                    status = 0;
            }
        } break;
        case ETH_API_NONE:
        case ETH_API_TEST:
        case ETH_API_COUNT:
            /* Should never reach here - these are not valid runtime API types */
            status = -1;
            break;
        }
    } while ((status > 0) && (0 == packet->len));
    if (status < 0) {
        ++dev->receive_packet_errors;
        _eth_error(dev, "eth_reader");
    }

#else /* USE_READER_THREAD */

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
#endif

    return status;
}
