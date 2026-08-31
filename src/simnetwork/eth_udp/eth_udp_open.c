// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_sock.h"
#include "simnetwork/eth_udp/eth_udp.h"

#include "simnetwork/eth_backends.h"

t_stat eth_udp_open(const char *devname, ETH_DEV *dev, char *savname, size_t savname_size)
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
    backend->packet_wait = eth_wait_udp;
    backend->packet_read = eth_reader_udp;
    backend->before_packet_write = NULL;
    backend->write_packet = eth_writer_udp;
    backend->after_packet_write = NULL;
    backend->reader_shutdown = NULL;
    backend->writer_shutdown = NULL;
    backend->state.eth_socket = eth_socket;

    return SCPE_OK;
}