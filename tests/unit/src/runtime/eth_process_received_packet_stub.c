/* Stub implementation of eth_process_received_packet for test_sim_slirp
 * This file should be linked with the test instead of the real implementation
 * from eth_callback.c to avoid multiple definition errors.
 */

#include <stdint.h>
#include <string.h>
#include "sim_ether.h"

/* This stub routes packets directly to a test callback stored in dev->read_callback */
void eth_process_received_packet(ETH_DEV *dev, const uint8_t *data, uint32_t len, uint32_t caplen)
{
    (void)caplen;  /* Unused in stub */

    /* If a read_callback is set, call it with the packet data */
    if (dev && dev->read_callback) {
        /* Create a minimal packet structure for the callback */
        ETH_PACK packet;
        memset(&packet, 0, sizeof(packet));

        if (len <= sizeof(packet.msg)) {
            memcpy(packet.msg, data, len);
            packet.oversize = NULL;
        } else {
            /* For oversized packets, just point to the original data */
            packet.oversize = (uint8_t *)data;
        }
        packet.len = len;
        packet.used = len;
        packet.crc_len = len;

        dev->read_callback(dev, &packet, 0);
    }
}
