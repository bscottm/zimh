// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_FUNCS_H)
#define SIM_ETH_FUNCS_H

#include <stdbool.h>

#include "simnetwork/eth_types.h"

/* Function declarations for Ethernet emulation */

/* Compute the Ethernet FCS (CRC-32/AUTODIN-II CRC) on data */
uint32_t crc32_fast    (const void* data, size_t length, uint32_t previousCrc32);

/* SIMH's API function to compute the Ethernet FCS. */
inline uint32_t eth_crc32(uint32_t crc, const void *vbuf, size_t len)
{
    return crc32_fast(vbuf, len, crc);
}

/* Return whether a non-BPF receive path should deliver a packet to dev. */
bool eth_packet_matches_filter(ETH_DEV *dev, const uint8_t *data);

/* Return non-BPF address filter state for a received packet. */
void eth_packet_filter_status(ETH_DEV *dev, const uint8_t *data, bool *to_me, bool *from_me);

/* Lookuup */
int eth_hash_lookup(ETH_MULTIHASH hash, const uint8_t *data);

/* Enumerate available network devices suitable for Ethernet emulation backends. */
int eth_devices(int max, ETH_LIST *list, bool framers);

/* Get the name and description of an Ethernet device by its index in the array returned by eth_devices().
 *
 * Returns the name on success, or NULL if not found.
 */
const char *eth_getname(int number, char *name, size_t name_size, char *desc, size_t desc_size);

/* Get the name and description of an Ethernet device by its name, as exactly matched in the array returned
 * by eth_devices().
 *
 * Returns the name on success, NULL if not found.
 */
const char *eth_getname_byname(const char *name, char *temp, size_t temp_size, char *desc, size_t desc_size);

/* Get the name and description of an Ethernet device by its description, as exactly matched in the array
 * returned by eth_devices().
 *
 * Returns the name on success, or NULL if not found.
 */
const char *eth_getname_bydesc(const char *desc, char *name, size_t name_size, char *ndesc, size_t ndesc_size);

/* Get the simulated Ethernet's description by case-insensitive matching its name. */
const char *eth_getdesc_byname(char *name, char *temp, size_t temp_size);

/* Return the number of open emulated Ethernet devices */
size_t eth_open_device_count();

/* Return the open emulated Ethernet device list */
ETH_DEV ** const eth_open_devices();

/* Add an `ETH_DEV` device to the open devices list. */
void eth_add_to_open_list(ETH_DEV *dev);

/* Remove an `ETH_DEV` device from the open devices list. */
void eth_remove_from_open_list(ETH_DEV *dev);

/* Get the underlying interface's MAC address */
void eth_get_nic_hw_addr(ETH_DEV *dev, const char *devname, int set_on);

#endif
