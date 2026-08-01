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

/* Ethernet item type enumeration */
typedef enum eth_item_type_e {
    ETH_ITM_SETUP = 0,
    ETH_ITM_LOOPBACK = 1,
    ETH_ITM_NORMAL = 2
} eth_item_type_t;

struct eth_item {
    eth_item_type_t type;
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

typedef uint8_t ETH_MAC[6];

/* Ethernet API type - designates which network backend is in use. */
typedef enum eth_api_e {
    ETH_API_NONE = 0, /* No API in use yet */
    ETH_API_PCAP = 1, /* Pcap API in use */
    ETH_API_TAP = 2,  /* tun/tap API in use */
    ETH_API_VDE = 3,  /* VDE API in use */
    ETH_API_UDP = 4,  /* UDP API in use */
    ETH_API_NAT = 5,  /* NAT (SLiRP) API in use */
    ETH_API_TEST = 6, /* test API in use */
    ETH_API_COUNT     /* Number of API types (for array sizing) */
} eth_api_t;

struct eth_list {
    char name[ETH_DEV_NAME_MAX];
    char desc[ETH_DEV_DESC_MAX];
    eth_api_t eth_api;
};

/* Actual struct defined in eth_backends.h */
typedef struct eth_backend_s eth_backend_t;

typedef uint8_t ETH_MULTIHASH[8];
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

/* Actual struct eth_device declared in sim_ether.h */
typedef struct eth_device ETH_DEV;


#endif
