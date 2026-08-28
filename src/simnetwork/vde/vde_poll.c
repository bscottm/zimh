// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_sock.h"
#include "simnetwork/vde/sim_vde.h"

/* VDE wait implementation */
int eth_wait_vde(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)dev;

    return poll_eth_socket(backend, ETH_READER_POLL_TMO);
}

int eth_reader_vde(eth_backend_t *backend, ETH_DEV *dev)
{
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    len = vde_recv(dev->backend->state.vde, buf, sizeof(buf), 0);
    if (len > 0) {
        eth_process_received_packet(dev, buf, len, len);
        return 1;
    }
    return (len < 0) ? -1 : 0;
}

int eth_writer_vde(ETH_DEV *dev, const ETH_PACK *packet)
{
    int status = vde_send(dev->backend->state.vde, (void *)packet->msg, packet->len, 0);
    if ((status == (int)packet->len) || (status == 0))
        return 0;
    if ((status == -1) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
        return 0;
    return 1;
}
