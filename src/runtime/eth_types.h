// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_TYPES_H)
#define SIM_ETH_TYPES_H

/* structure declarations */

#define ETH_PROMISC 1             /* promiscuous mode = true */
#define ETH_TIMEOUT -1            /* read timeout in milliseconds (immediate) */
#define ETH_FILTER_MAX 20         /* maximum address filters */
#define ETH_DEV_NAME_MAX 256      /* maximum device name size */
#define ETH_DEV_DESC_MAX 256      /* maximum device description size */
#define ETH_MIN_PACKET 60         /* minimum ethernet packet size */
#define ETH_MAX_PACKET 1514       /* maximum ethernet packet size */
#define ETH_MAX_JUMBO_FRAME 65536 /* maximum ethernet jumbo frame size (or Offload Segment Size) */
#define ETH_MAX_DEVICE 40         /* maximum ethernet devices */
#define ETH_CRC_SIZE 4            /* ethernet CRC size */
#define ETH_FRAME_SIZE (ETH_MAX_PACKET + ETH_CRC_SIZE) /* ethernet maximum frame size */
#define ETH_MIN_JUMBO_FRAME ETH_MAX_PACKET             /* Threshold size for Jumbo Frame Processing */

struct eth_packet {
    uint8_t msg[ETH_FRAME_SIZE]; /* ethernet frame (message) */
    uint8_t *oversize;           /* oversized frame (message) */
    uint32_t len;                /* packet length without CRC */
    uint32_t used;               /* bytes processed (used in packet chaining) */
    int status;                  /* transmit/receive status */
    uint32_t crc_len;            /* packet length with CRC */
};

struct eth_item {
    int type; /* receive (0=setup, 1=loopback, 2=normal) */
#define ETH_ITM_SETUP 0
#define ETH_ITM_LOOPBACK 1
#define ETH_ITM_NORMAL 2
    struct eth_packet packet;
};

struct eth_queue {
    int max;
    int count;
    int head;
    int tail;
    int loss;
    int high;
    struct eth_item *item;
};

typedef uchar_t ETH_MAC[6];

struct eth_list {
    char name[ETH_DEV_NAME_MAX];
    char desc[ETH_DEV_DESC_MAX];
    int eth_api;
};

typedef uchar_t ETH_MULTIHASH[8];
typedef struct eth_packet ETH_PACK;
typedef void (*ETH_PCALLBACK)(int status);
typedef struct eth_list ETH_LIST;
typedef struct eth_queue ETH_QUE;
typedef struct eth_item ETH_ITEM;
struct eth_write_request {
    struct eth_write_request *next;
    ETH_PACK packet;
};
typedef struct eth_write_request ETH_WRITE_REQUEST;

#endif