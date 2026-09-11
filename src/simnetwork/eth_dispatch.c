// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet reader/writer dispatch functions - one per API type */

#include "sim_ether.h"
#include "sim_ether_internal.h"
#include "sim_sock.h"
#include "simnetwork/eth_backends.h"
#include "simnetwork/eth_dispatch.h"

/*============================================================================*/
/*                         Writer Dispatch Functions                          */
/*============================================================================*/

int eth_writer_none(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return -1; /* Error: no API configured */
}

int eth_writer_test(ETH_DEV *dev, const ETH_PACK *packet)
{
    (void)dev;
    (void)packet;
    return 0; /* Test API handles writes differently */
}

/*============================================================================*/
/*                         Reader Dispatch Functions                          */
/*============================================================================*/

int eth_reader_none(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)backend;
    (void)dev;
    return -1; /* No backend. */
}

int eth_reader_test(eth_backend_t *backend, ETH_DEV *dev)
{
    (void)backend;
    (void)dev;
    return 0; /* Test API handles reads differently */
}
