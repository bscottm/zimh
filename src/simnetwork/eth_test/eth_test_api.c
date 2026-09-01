// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "simnetwork/eth_test/eth_test.h"


/* Test Ethernet emulation select/poll wait API. */
int eth_wait_test(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms)
{
    /* Test API doesn't wait, always return immediately */
    (void)backend;
    (void)dev;
    (void)timeout_ms;
    return 1;
}

/* Read one accepted packet from a test backend into the supplied packet. */
int eth_test_read(ETH_DEV *dev, ETH_PACK *packet, ETH_PCALLBACK routine)
{
    ETH_TEST_BACKEND *backend = dev->backend->state.test_backend;

    if (backend == NULL)
        return 0;

    while (backend->rx_to_guest.count > 0) {
        ETH_ITEM *item = &backend->rx_to_guest.item[backend->rx_to_guest.head];
        ETH_PACK *source = &item->packet;
        const uint8_t *data = source->oversize ? source->oversize : source->msg;

        if (source->len >= 2 * sizeof(ETH_MAC) &&
            eth_packet_matches_filter(dev, data)) {
            size_t copy_len = MAX(source->len, source->crc_len);

            packet->len = source->len;
            packet->crc_len = source->crc_len;
            packet->used = 0;
            packet->status = source->status;
            packet->oversize = NULL;
            memcpy(packet->msg, data, copy_len);
            if (packet->len < ETH_MIN_PACKET) {
                memset(&packet->msg[packet->len], 0,
                       ETH_MIN_PACKET - packet->len);
                packet->len = ETH_MIN_PACKET;
            }
            if ((packet->crc_len == 0) && dev->need_crc)
                packet->crc_len = eth_test_append_crc(packet->msg, packet->len);
            ++dev->packets_received;
            ethq_remove(&backend->rx_to_guest);
            if (routine)
                routine(packet->status);
            return 1;
        }

        ethq_remove(&backend->rx_to_guest);
    }

    return 0;
}

/* Capture one packet written by a device attached to a test backend. */
t_stat eth_test_write(ETH_DEV *dev, ETH_PACK *packet, ETH_PCALLBACK routine)
{
    ETH_TEST_BACKEND *backend;
    int status;

    if (!dev || dev->backend->eth_api == ETH_API_NONE)
        return SCPE_UNATT;
    if (packet == NULL)
        return SCPE_ARG;
    if ((packet->len < ETH_MIN_PACKET) || (packet->len > ETH_MAX_PACKET)) {
        if (routine)
            routine(1);
        return SCPE_IOERR;
    }

    backend = dev->backend->state.test_backend;
    status = backend ? backend->write_status : 1;
    if (backend)
        ethq_insert_data(&backend->tx_from_guest, ETH_ITM_NORMAL, packet->msg, 0, packet->len, packet->crc_len, NULL, status);

    ++dev->packets_sent;
    if (status != 0)
        ++dev->transmit_packet_errors;
    if (routine)
        routine(status);

    return status == 0 ? SCPE_OK : SCPE_IOERR;
}
