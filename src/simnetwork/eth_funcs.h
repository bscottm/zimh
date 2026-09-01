// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_FUNCS_H)
#define SIM_ETH_FUNCS_H

#include <stdbool.h>

#include "simnetwork/eth_types.h"

/* Function declarations for Ethernet emulation */

/* Enumerate available network devices suitable for Ethernet emulation backends. */
int eth_devices(int max, ETH_LIST *list, bool framers);
/* Get the name and description of an Ethernet device by its index in the array returned by eth_devices().
 *
 * Returns the name on success, or NULL if not found.
 */
const char *eth_getname(int number, char *name, size_t name_size, char *desc, size_t desc_size);
/* Get the name and description of an Ethernet device by its description, as exactly matched in the array
 * returned by eth_devices().
 *
 * Returns the name on success, or NULL if not found.
 */
const char *eth_getname_bydesc(const char *desc, char *name, size_t name_size, char *ndesc, size_t ndesc_size);

#endif