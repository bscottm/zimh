// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_sock.h"
#include "simnetwork/eth_backends.h"
#include "simnetwork/eth_udp/eth_udp.h"

// Close and clean up.
static void eth_udp_close(eth_backend_t *self);

/* UDP tunnel Ethernet emulation functions */
static const eth_api_funcs_t udp_eth_funcs = {
    .packet_wait = eth_wait_udp,
    .packet_read = eth_reader_udp,
    .before_packet_write = NULL,
    .write_packet = eth_writer_udp,
    .after_packet_write = NULL,
    .reader_shutdown = NULL,
    .writer_shutdown = NULL,
    .close = eth_udp_close
};

t_stat eth_udp_open(const char *devname, ETH_DEV *dev)
{
    char localport[CBUFSIZE], host[CBUFSIZE], port[CBUFSIZE];
    char hostport[2 * CBUFSIZE];

    if (!strcmp(devname, "udp:sourceport:remotehost:remoteport"))
        return sim_messagef(SCPE_OPENERR, "Eth: Must specify actual udp host and ports(i.e. udp:1224:somehost.com:2234)\n");

    devname = devname + 4;
    while (isspace(*devname))
        ++devname;
    if (SCPE_OK != sim_parse_addr_ex(devname, host, sizeof(host), "localhost", port, sizeof(port), localport,
                                     sizeof(localport), NULL))
        return sim_messagef(SCPE_OPENERR, "Eth: Error parsing UDP address (%s)\n", devname);

    if (localport[0] == '\0')
        strlcpy(localport, port, sizeof(localport));

    snprintf(hostport, sizeof(hostport), "%s:%s", host, port);

    if ((SCPE_OK == sim_parse_addr(hostport, NULL, 0, NULL, NULL, 0, NULL, "localhost")) &&
        (0 == strcmp(localport, port)))
        return sim_messagef(SCPE_OPENERR, "Eth: Must specify different udp localhost ports\n");

    SOCKET eth_socket = sim_connect_sock_ex(localport, hostport, NULL, NULL, SIM_SOCK_OPT_DATAGRAM);
    if (INVALID_SOCKET == eth_socket) {
        return sim_messagef(SCPE_OPENERR, "Eth: Error creating UDP socket to %s:%s - %s\n", host, port,
                            sim_get_err_sock("connect"));
    }

    eth_backend_t *backend;

    if ((backend = (eth_backend_t *)calloc(1, sizeof(*backend))) == NULL)
        return sim_messagef(SCPE_MEM, "Eth: Error allocating memory for eth_backend_t\n");

    backend->eth_api = ETH_API_UDP;
    backend->state.eth_socket = eth_socket;
    backend->eth_funcs = &udp_eth_funcs;

    return SCPE_OK;
}

// Close and clean up.
void eth_udp_close(eth_backend_t *self)
{
    sim_close_sock(self->state.eth_socket);
}
