// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>

#include "sim_ether.h"

/* Return whether a non-BPF receive path should deliver a packet to dev. */
bool eth_packet_matches_filter(ETH_DEV *dev, const uint8_t *data)
{
    bool to_me;
    bool from_me;

    eth_packet_filter_status(dev, data, &to_me, &from_me);
    return to_me && !from_me;
}

/* Return non-BPF address filter state for a received packet. */
void eth_packet_filter_status(ETH_DEV *dev, const uint8_t *data, bool *to_me, bool *from_me)
{
    int i;

    *to_me = false;
    *from_me = false;
    for (i = 0; i < dev->addr_count; i++) {
        *to_me = *to_me || (memcmp(data, dev->filter_address[i], sizeof(ETH_MAC)) == 0);
        *from_me = *from_me || (memcmp(&data[sizeof(ETH_MAC)], dev->filter_address[i], sizeof(ETH_MAC)) == 0);
    }

    /* all multicast mode and multicast frame? */
    if (dev->all_multicast && is_eth_groupmac(data))
        *to_me = true;

    /* promiscuous mode? */
    if (dev->promiscuous)
        *to_me = true;

    /* AUTODIN II hash mode? */
    if (dev->hash_filter && !*to_me && is_eth_groupmac(data))
        *to_me = eth_hash_lookup(dev->hash, data) != 0;
}

/* Ethernet multicast address hashing: */
int eth_hash_lookup(ETH_MULTIHASH hash, const u_char *data)
{
    int key = 0x3f & (eth_crc32(0, data, 6) >> 26);

    key ^= 0x3f;
    return (hash[key >> 3] & (1 << (key & 0x7)));
}
