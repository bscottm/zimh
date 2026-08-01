// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_VDE_POLL_H)
#    define SIM_VDE_POLL_H

#    include <libvdeplug.h>

#    include "sim_defs.h"
#    include "sim_sock.h"
#    include "sim_ether.h"
#    include "sim_ether_internal.h"
#    include "simnetwork/eth_backends.h"

/* VDE wait implementation */
int eth_wait_vde(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms);
/* VDE reader */
int eth_reader_vde(eth_backend_t *backend, ETH_DEV *dev);
/* VDE writer */
int eth_writer_vde(ETH_DEV *dev, const ETH_PACK *packet);
#endif
