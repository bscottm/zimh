/* sim_ether_test.c: deterministic Ethernet backend for unit tests */
// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sim_ether_test.h"
#include "sim_ether_internal.h"
#include "sim_ether_test_internal.h"
#include "sim_sock.h"
#include "simnetwork/eth_test/eth_test.h"

/* Append an Ethernet CRC in the same byte order used by the polled backend. */
static uint32_t eth_test_append_crc(uint8_t *msg, uint32_t len)
{
    uint32_t crc = eth_crc32(0, msg, len);
    uint32_t ncrc = htonl(crc);

    memcpy(&msg[len], &ncrc, sizeof(ncrc));
    return len + (uint32_t)sizeof(ncrc);
}

/* Queue one packet into a backend receive or transmit queue. */
static t_stat eth_test_queue_packet(ETH_QUE *queue, const uint8_t *data,
                                    size_t len, size_t crc_len, int32_t status)
{
    if (!queue || !data || (len > ETH_FRAME_SIZE) ||
        (crc_len > ETH_FRAME_SIZE) || ((crc_len != 0) && (crc_len < len)))
        return SCPE_ARG;

    ethq_insert_data(queue, ETH_ITM_NORMAL, data, 0, len, crc_len, NULL,
                     status);
    return SCPE_OK;
}

/* Clear all queued packets and failure state for a named test backend. */
t_stat eth_test_clear(const char *name)
{
    ETH_TEST_BACKEND *backend = eth_test_find_backend(name);

    if (!backend)
        return SCPE_UNATT;

    ethq_clear(&backend->rx_to_guest);
    ethq_clear(&backend->tx_from_guest);
    backend->write_status = 0;
    return SCPE_OK;
}

/* Queue one wire packet for delivery to a device attached to test:name. */
t_stat eth_test_inject(const char *name, const uint8_t *data, size_t len)
{
    return eth_test_inject_ex(name, data, len, 0, 0);
}

/* Queue one wire packet with explicit CRC length and callback status. */
t_stat eth_test_inject_ex(const char *name, const uint8_t *data, size_t len,
                          size_t crc_len, int32_t status)
{
    ETH_TEST_BACKEND *backend;
    t_stat stat = eth_test_get_backend(name, &backend);

    if (stat != SCPE_OK)
        return stat;

    return eth_test_queue_packet(&backend->rx_to_guest, data, len, crc_len,
                                 status);
}

/* Pop the oldest packet transmitted by a device attached to test:name. */
t_stat eth_test_pop_tx(const char *name, ETH_PACK *packet)
{
    ETH_TEST_BACKEND *backend = eth_test_find_backend(name);
    ETH_ITEM *item;
    ETH_PACK copy;

    if (!packet)
        return SCPE_ARG;
    if (!backend)
        return SCPE_UNATT;
    if (backend->tx_from_guest.count == 0)
        return SCPE_EOF;

    item = &backend->tx_from_guest.item[backend->tx_from_guest.head];
    copy = item->packet;
    if (item->packet.oversize) {
        copy.oversize =
            (uint8_t *)malloc(MAX(item->packet.len, item->packet.crc_len));
        if (!copy.oversize)
            return SCPE_MEM;
        memcpy(copy.oversize, item->packet.oversize,
               MAX(item->packet.len, item->packet.crc_len));
    }
    *packet = copy;
    ethq_remove(&backend->tx_from_guest);
    return SCPE_OK;
}

/* Return the number of receive packets queued for a named test backend. */
int eth_test_rx_count(const char *name)
{
    ETH_TEST_BACKEND *backend = eth_test_find_backend(name);

    return backend ? backend->rx_to_guest.count : 0;
}

/* Return the number of transmit packets captured for a named test backend. */
int eth_test_tx_count(const char *name)
{
    ETH_TEST_BACKEND *backend = eth_test_find_backend(name);

    return backend ? backend->tx_from_guest.count : 0;
}

/* Configure the status returned by future writes through a test backend. */
t_stat eth_test_set_write_status(const char *name, int status)
{
    ETH_TEST_BACKEND *backend;
    t_stat stat = eth_test_get_backend(name, &backend);

    if (stat != SCPE_OK)
        return stat;

    backend->write_status = status;
    return SCPE_OK;
}
