// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet dispatch tables and function types */

#ifndef ETH_DISPATCH_H
#define ETH_DISPATCH_H

/*============================================================================*/
/*                    Reader/Writer Machine Status                            */
/*============================================================================*/

/* Reader State Machine States */
typedef enum eth_reader_status_e {
    ETH_READER_INIT,     /* Initial state - perform setup */
    ETH_READER_RUNNING,  /* Running, active status */
    ETH_READER_SHUTDOWN, /* Clean shutdown */
    ETH_READER_ERROR,    /* Error-ed out. */
} eth_reader_status_t;

/* Writer State Machine States */
typedef enum eth_writer_state_e {
    ETH_WRITER_INIT,     /* Initial state - perform setup */
    ETH_WRITER_RUNNING,  /* Running, active status */
    ETH_WRITER_SHUTDOWN, /* Clean shutdown */
    ETH_WRITER_ERROR,    /* Error-ed out. */
} eth_writer_state_t;

/*============================================================================*/
/*                    Reader/Writer Dispatch Function Types                  */
/*============================================================================*/

/* Reader dispatch function: performs one read iteration for the specified API.
 * Returns: >0 = packets received, 0 = timeout/no data, <0 = error
 */
typedef int (*eth_reader_dispatch_fn)(ETH_DEV *dev);

/* Dispatch tables - one entry per API type, indexed by eth_api_t */
extern const eth_reader_dispatch_fn eth_reader_dispatch_table[ETH_API_COUNT];

#endif /* ETH_DISPATCH_H */
