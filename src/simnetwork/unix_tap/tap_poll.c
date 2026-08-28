// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_sock.h"
#include "simnetwork/unix_tap/unix_tap.h"

/* TAP wait implementation */
int eth_wait_tap(eth_backend_t *backend, ETH_DEV *dev)
{
    return poll_eth_socket(backend, ETH_READER_POLL_TMO);
}

int eth_reader_tap(eth_backend_t *backend, ETH_DEV *dev)
{
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    (void)backend;
    len = read(backend->state.eth_socket, buf, sizeof(buf));
    if (len > 0) {
        eth_process_received_packet(dev, buf, len, len);
        return 1;
    }
    return (len < 0) ? -1 : 0;
}

int eth_writer_tap(ETH_DEV *dev, const ETH_PACK *packet)
{
    return (((int)packet->len == write(dev->backend->state.eth_socket, (void *)packet->msg, packet->len)) ? 0 : -1);
}
