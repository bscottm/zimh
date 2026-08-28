// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet emulation API functions for UDP point-to-point tunnels. */

#include "simnetwork/udp_tunnel/udp_tunnel.h"

/* UDP wait implementation */
int eth_wait_udp(eth_backend_t *backend, ETH_DEV *dev)
{
    return poll_eth_socket(backend, ETH_READER_POLL_TMO);
}

/* UDP packet writer */
int eth_writer_udp(ETH_DEV *dev, const ETH_PACK *packet)
{
    int n_written = sim_write_sock(dev->backend->state.eth_socket, (char *)packet->msg, (int32_t)packet->len);
    return (((int32_t)packet->len == n_written) ? 0 : -1);
}

/* UDP packet reader */
int eth_reader_udp(eth_backend_t *backend, ETH_DEV *dev)
{
#if defined(USE_READER_THREAD)
    int len;
    u_char buf[ETH_MAX_JUMBO_FRAME];

    (void)backend;
    len = (int)sim_read_sock(backend->state.eth_socket, (char *)buf, (int32_t)sizeof(buf));
    if (len > 0) {
        eth_process_received_packet(dev, buf, len, len);
        return 1;
    }
    return (len < 0) ? -1 : 0;
#else
    (void)backend;
    (void)dev;
    return 0;
#endif
}
