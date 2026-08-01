// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "sim_defs.h"
#include "sim_atomic.h"
#include "sim_tailq.h"
#include "sim_ether.h"
#include "sim_ether_internal.h"

/* sim_tailq adapter functions for ethernet packet queue
 * sim_tailq stores generic pointers, we need to wrap eth_item structures
 */

void ethq_item_free(sim_tailq_item_t item)
{
    struct eth_item *eth_itm = (struct eth_item *)item;

    if (eth_itm != NULL) {
        free(eth_itm->packet.oversize);
        free(eth_itm);
    }
}

t_stat eth_tailq_init(sim_tailq_t *que, int max)
{
    (void)max; /* sim_tailq grows dynamically, no fixed max */
    /* sim_tailq_init returns 1 on success, 0 on failure */
    return (sim_tailq_init(que) == 0) ? SCPE_MEM : SCPE_OK;
}

void eth_tailq_destroy(sim_tailq_t *que)
{
    sim_tailq_destroy(que, ethq_item_free);
}

void eth_tailq_clear(sim_tailq_t *que)
{
    /* Dequeue and free all items */
    sim_tailq_item_t item;
    while ((item = sim_tailq_dequeue(que)) != NULL) {
        ethq_item_free(item);
    }
}

void eth_tailq_remove(sim_tailq_t *que)
{
    sim_tailq_item_t item = sim_tailq_dequeue(que);
    if (item != NULL)
        ethq_item_free(item);
}

void eth_tailq_insert_data(sim_tailq_t *que, int32_t type, const uint8_t *data, int used, size_t len, size_t crc_len,
                           const uint8_t *crc_data, int32_t status)
{
    struct eth_item *item = (struct eth_item *)calloc(1, sizeof(struct eth_item));
    if (item == NULL)
        return; /* Allocation failure - packet dropped */

    item->type = type;
    item->packet.len = (uint32_t)len;
    item->packet.used = (uint32_t)used;
    item->packet.crc_len = (uint32_t)crc_len;

    if (MAX(len, crc_len) <= sizeof(item->packet.msg)) {
        memcpy(item->packet.msg, data, ((len > crc_len) ? len : crc_len));
        if (crc_data && (crc_len > len))
            memcpy(&item->packet.msg[len], crc_data, ETH_CRC_SIZE);
    } else {
        item->packet.oversize = (uint8_t *)malloc(((len > crc_len) ? len : crc_len));
        if (item->packet.oversize != NULL) {
            memcpy(item->packet.oversize, data, ((len > crc_len) ? len : crc_len));
            if (crc_data && (crc_len > len))
                memcpy(&item->packet.oversize[len], crc_data, ETH_CRC_SIZE);
        } else {
            free(item);
            return; /* Allocation failure - packet dropped */
        }
    }
    item->packet.status = status;

    sim_tailq_enqueue(que, (sim_tailq_item_t)item);
}

void eth_tailq_insert(sim_tailq_t *que, int32_t type, ETH_PACK *pack, int32_t status)
{
    eth_tailq_insert_data(que, type, pack->oversize ? pack->oversize : pack->msg, pack->used, pack->len, pack->crc_len,
                          NULL, status);
}

/* Get queue depth - for compatibility with old API */
static SIM_UNUSED_FUNC int ethq_count(sim_tailq_t *que)
{
    return (int)sim_tailq_count(que);
}
