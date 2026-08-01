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

#endif                   /* ETH_DISPATCH_H */
