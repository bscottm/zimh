// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Ethernet dispatch tables and function types */

#ifndef ETH_DISPATCH_H
#define ETH_DISPATCH_H

#include "sim_ether.h"

/*============================================================================*/
/*                    Reader/Writer Dispatch Function Types                  */
/*============================================================================*/

/* Reader dispatch function: performs one read iteration for the specified API.
 * Returns: >0 = packets received, 0 = timeout/no data, <0 = error
 */
typedef int (*eth_reader_dispatch_fn)(ETH_DEV *dev);

/* Writer dispatch function: writes one packet using the specified API.
 * Returns: 0 = success, non-zero = error
 */
typedef int (*eth_writer_dispatch_fn)(ETH_DEV *dev, const ETH_PACK *packet);

/*============================================================================*/
/*                    Reader/Writer State Machine Context                    */
/*============================================================================*/

/* Reader State Machine Context */
typedef struct eth_reader_context_s {
    ETH_DEV *dev;                     /* Device being serviced */
    int sel_ret;                      /* Select return value */
    int status;                       /* Last operation status */
    SOCKET select_fd;                 /* FD for select (non-Windows) */
#    if defined(_WIN32)
    HANDLE hWait; /* Event handle (Windows PCAP) */
#    endif
} eth_reader_context_t;

/* Writer State Machine Context */
typedef struct eth_writer_context_s {
    ETH_DEV *dev;                     /* Device being serviced */
    ETH_WRITE_REQUEST *request;       /* Current write request */
    int status;                       /* Last write status */
    uint32_t packet_delta_time;       /* Time since last packet (for throttling) */
} eth_writer_context_t;

/*============================================================================*/
/*                    Reader/Writer State Machine Types                      */
/*============================================================================*/

/* Reader State Machine States */
typedef enum eth_reader_state_e {
    ETH_READER_INIT,          /* Initial state - perform setup */
    ETH_READER_SELECT_WAIT,   /* Waiting for data (select/poll or Windows event)
                               */
    ETH_READER_DISPATCH_READ, /* Dispatch to API-specific read handler */
    ETH_READER_CHECK_ASYNC,   /* Check if async wakeup needed */
    ETH_READER_ERROR_HANDLER, /* Handle read errors */
    ETH_READER_SHUTDOWN,      /* Clean shutdown */
    ETH_READER_STATE_COUNT    /* Number of states (for array sizing) */
} eth_reader_state_t;

/* Writer State Machine States */
typedef enum eth_writer_state_e {
    ETH_WRITER_INIT,           /* Initial state - perform setup */
    ETH_WRITER_WAIT_WORK,      /* Wait for write requests (condition variable) */
    ETH_WRITER_GET_REQUEST,    /* Pull request from queue */
    ETH_WRITER_THROTTLE_CHECK, /* Check if throttle delay needed */
    ETH_WRITER_THROTTLE_DELAY, /* Sleep due to throttle */
    ETH_WRITER_DISPATCH_WRITE, /* Dispatch to API-specific write handler */
    ETH_WRITER_CLEANUP,        /* Return buffer to free list */
    ETH_WRITER_SHUTDOWN,       /* Clean shutdown */
    ETH_WRITER_STATE_COUNT     /* Number of states (for array sizing) */
} eth_writer_state_t;

/* State handler function types */
typedef eth_reader_state_t (*eth_reader_state_handler_t)(eth_reader_context_t *ctx);
typedef eth_writer_state_t (*eth_writer_state_handler_t)(eth_writer_context_t *ctx);

/* Dispatch tables - one entry per API type, indexed by eth_api_t */
extern const eth_reader_dispatch_fn eth_reader_dispatch_table[ETH_API_COUNT];
extern const eth_writer_dispatch_fn eth_writer_dispatch_table[ETH_API_COUNT];

#endif /* ETH_DISPATCH_H */
