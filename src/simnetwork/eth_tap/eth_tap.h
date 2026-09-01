// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_UNIX_TAP_H)
#    define SIM_UNIX_TAP_H

#    if defined(__linux) || defined(__linux__)
#        include <sys/ioctl.h>
#        include <net/if.h>
#        include <linux/if_tun.h>
#    elif defined(HAVE_BSDTUNTAP)
#        include <sys/types.h>
#        include <net/if_types.h>
#        include <net/if.h>
#    else /* We don't know how to do this on the current platform */
#        undef HAVE_TAP_NETWORK
#    endif

#    include "sim_defs.h"
#    include "sim_ether.h"
#    include "simnetwork/eth_backends.h"

/* TAP wait implementation */
int eth_wait_tap(eth_backend_t *backend, ETH_DEV *dev, int timeout_ms);
/* TAP reader */
int eth_reader_tap(eth_backend_t *backend, ETH_DEV *dev);
/* TAP writer */
int eth_writer_tap(ETH_DEV *dev, const ETH_PACK *packet);
#endif
