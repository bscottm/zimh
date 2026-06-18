/* sim_ether.h: OS-dependent network information */
// SPDX-FileCopyrightText: 2002-2005 David T. Hittner
// SPDX-License-Identifier: X11

/*
  ------------------------------------------------------------------------------

  Modification history:

  01-Mar-12  AGN  Cygwin doesn't have non-blocking pcap I/O pcap (it uses WinPcap)
  17-Nov-11  MP   Added dynamic loading of libpcap on *nix platforms
  30-Oct-11  MP   Added support for vde (Virtual Distributed Ethernet) networking
  18-Apr-11  MP   Fixed race condition with self loopback packets in
                  multithreaded environments
  09-Dec-10  MP   Added support to determine if network address conflicts exist
  07-Dec-10  MP   Reworked DECnet self detection to the more general approach
                  of loopback self when any Physical Address is being set.
  04-Dec-10  MP   Changed eth_write to do nonblocking writes when
                  USE_READER_THREAD is defined.
  07-Feb-08  MP   Added eth_show_dev to display ethernet state
  28-Jan-08  MP   Added eth_set_async
  23-Jan-08  MP   Added eth_packet_trace_ex and ethq_destroy
  30-Nov-05  DTH  Added CRC length to packet and more field comments
  04-Feb-04  DTH  Added debugging information
  14-Jan-04  MP   Generalized BSD support issues
  05-Jan-04  DTH  Added eth_mac_scan
  26-Dec-03  DTH  Added ethernet show and queue functions from pdp11_xq
  23-Dec-03  DTH  Added status to packet
  01-Dec-03  DTH  Added reflections, tweaked decnet fix items
  25-Nov-03  DTH  Verified DECNET_FIX, reversed ifdef to mainstream code
  14-Nov-03  DTH  Added #ifdef DECNET_FIX for problematic duplicate detection code
  07-Jun-03  MP   Added WIN32 support for DECNET duplicate address detection.
  05-Jun-03  DTH  Added used to struct eth_packet
  01-Feb-03  MP   Changed some uint8 strings to char* to reflect usage
  22-Oct-02  DTH  Added all_multicast and promiscuous support
  21-Oct-02  DTH  Corrected copyright again
  16-Oct-02  DTH  Fixed copyright
  08-Oct-02  DTH  Integrated with 2.10-0p4, added variable vector and copyrights
  03-Oct-02  DTH  Beta version of xq/sim_ether released for SIMH 2.09-11
  15-Aug-02  DTH  Started XQ simulation

  ------------------------------------------------------------------------------
*/

#ifndef SIM_ETHER_H
#define SIM_ETHER_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_defs.h"
#include "sim_sock.h"
#include "sim_types.h"

/* make common BSD code a bit easier to read in this file */
/* OS/X seems to define and compile using one of these BSD types */
#if defined(__NetBSD__) || defined (__OpenBSD__) || defined (__FreeBSD__)
#define xBSD 1
#endif
#if !defined(__FreeBSD__) && !defined(_WIN32) && !defined(__APPLE__)
#define USE_SETNONBLOCK 1
#endif

/* make common winpcap code a bit easier to read in this file */
#if defined(_WIN32)
#define PCAP_READ_TIMEOUT -1
#else
#define PCAP_READ_TIMEOUT  1
#endif

#include <time.h>
#if defined(__struct_timespec_defined) && !defined(_TIMESPEC_DEFINED)
#define _TIMESPEC_DEFINED
#endif

/* set related values to have correct relationships */
#if defined (USE_READER_THREAD)
#include <pthread.h>
#if defined (USE_SETNONBLOCK)
#undef USE_SETNONBLOCK
#endif /* USE_SETNONBLOCK */
#undef PCAP_READ_TIMEOUT
#define PCAP_READ_TIMEOUT 15
#if (!defined (xBSD) && !defined(_WIN32)) || defined (HAVE_TAP_NETWORK) || defined (HAVE_VDE_NETWORK)
#define MUST_DO_SELECT 1
#endif
#endif /* USE_READER_THREAD */

/* give priority to USE_NETWORK over USE_LOADED_WINPCAP */
#if defined(USE_NETWORK) && defined(USE_LOADED_WINPCAP)
#undef USE_LOADED_WINPCAP
#endif
/* USE_LOADED_WINPCAP is only for the Windows pcap runtime-loading path. */
#if defined(USE_LOADED_WINPCAP) && !defined(_WIN32)
#undef USE_LOADED_WINPCAP
#endif

/* USE_LOADED_WINPCAP provides pcap support, so force HAVE_PCAP_NETWORK */
#if defined(USE_LOADED_WINPCAP) && !defined(HAVE_PCAP_NETWORK)
#define HAVE_PCAP_NETWORK 1
#endif

/*
  USE_BPF is defined to let this code leverage the libpcap/OS kernel provided
  BPF packet filtering.  This generally will enhance performance.  It may not
  be available in some environments and/or it may not work correctly, so
  undefining this will still provide working code here.
*/
#if defined(HAVE_PCAP_NETWORK)
#define USE_BPF 1
#if defined (_WIN32) && !defined (BPF_CONST_STRING)
#define BPF_CONST_STRING 1
#endif
#else
#define DONT_USE_PCAP_FINDALLDEVS 1
#endif

#if defined (USE_READER_THREAD)
#include "sim_threads.h"
#include "sim_tailq.h"
#endif

/* structure declarations */

#define ETH_PROMISC            1                        /* promiscuous mode = true */
#define ETH_TIMEOUT           -1                        /* read timeout in milliseconds (immediate) */
#define ETH_FILTER_MAX        20                        /* maximum address filters */
#define ETH_DEV_NAME_MAX     256                        /* maximum device name size */
#define ETH_DEV_DESC_MAX     256                        /* maximum device description size */
#define ETH_MIN_PACKET        60                        /* minimum ethernet packet size */
#define ETH_MAX_PACKET      1514                        /* maximum ethernet packet size */
#define ETH_MAX_JUMBO_FRAME 65536                       /* maximum ethernet jumbo frame size (or Offload Segment Size) */
#define ETH_MAX_DEVICE        40                        /* maximum ethernet devices */
#define ETH_CRC_SIZE           4                        /* ethernet CRC size */
#define ETH_FRAME_SIZE (ETH_MAX_PACKET+ETH_CRC_SIZE)    /* ethernet maximum frame size */
#define ETH_MIN_JUMBO_FRAME ETH_MAX_PACKET              /* Threshold size for Jumbo Frame Processing */

#define LOOPBACK_SELF_FRAME(phy_mac, msg)                                                     \
    (((msg)[12] == 0x90) && ((msg)[13] == 0x00) &&              /* Ethernet Loopback */       \
     ((msg)[16] == 0x02) && ((msg)[17] == 0x00) &&              /* Forward Function */        \
     ((msg)[24] == 0x01) && ((msg)[25] == 0x00) &&              /* Next Function - Reply */   \
     (memcmp(phy_mac, (msg),    6) == 0) &&                     /* Ethernet Destination */    \
     (memcmp(phy_mac, (msg)+6,  6) == 0) &&                     /* Ethernet Source */         \
     (memcmp(phy_mac, (msg)+18, 6) == 0))                       /* Forward Address */

#define LOOPBACK_PHYSICAL_RESPONSE(dev, msg)                                                    \
    ((dev->have_host_nic_phy_addr) &&                                                           \
     ((msg)[12] == 0x90) && ((msg)[13] == 0x00) &&              /* Ethernet Loopback */         \
     ((msg)[14] == 0x08) && ((msg)[15] == 0x00) &&              /* Skipcount - 8 */             \
     ((msg)[16] == 0x02) && ((msg)[17] == 0x00) &&              /* Last Function - Forward */   \
     ((msg)[24] == 0x01) && ((msg)[25] == 0x00) &&              /* Function - Reply */          \
     (memcmp(dev->host_nic_phy_hw_addr, (msg)+18, 6) == 0) &&   /* Forward Address - Host MAC */\
     (memcmp(dev->host_nic_phy_hw_addr, (msg),    6) == 0) &&   /* Ethernet Source - Host MAC */\
     (memcmp(dev->physical_addr,  (msg)+6,  6) == 0))           /* Ethernet Source */

#define LOOPBACK_PHYSICAL_REFLECTION(dev, msg)                                                  \
    ((dev->have_host_nic_phy_addr) &&                                                           \
     ((msg)[12] == 0x90) && ((msg)[13] == 0x00) &&              /* Ethernet Loopback */         \
     ((msg)[16] == 0x02) && ((msg)[17] == 0x00) &&              /* Forward Function */          \
     ((msg)[24] == 0x01) && ((msg)[25] == 0x00) &&              /* Next Function - Reply */     \
     (memcmp(dev->host_nic_phy_hw_addr, (msg)+6,  6) == 0) &&   /* Ethernet Source - Host MAC */\
     (memcmp(dev->host_nic_phy_hw_addr, (msg)+18, 6) == 0))     /* Forward Address - Host MAC */

#define LOOPBACK_REFLECTION_TEST_PACKET(dev, msg)                                                \
    ((dev->have_host_nic_phy_addr) &&                                                            \
     ((msg)[12] == 0x90) && ((msg)[13] == 0x00) &&             /* Ethernet Loopback */           \
     ((msg)[14] == 0x00) && ((msg)[15] == 0x00) &&             /* Skipcount - 0 */               \
     ((msg)[16] == 0x02) && ((msg)[17] == 0x00) &&             /* Forward Function */            \
     ((msg)[24] == 0x01) && ((msg)[25] == 0x00) &&             /* Next Function - Reply */       \
     ((msg)[00] == 0xFE) && ((msg)[01] == 0xFF) &&             /* Ethernet Destination - Reflection Test MAC */\
     ((msg)[02] == 0xFF) && ((msg)[03] == 0xFF) &&                                               \
     ((msg)[04] == 0xFF) && ((msg)[05] == 0xFE) &&                                               \
     (memcmp(dev->host_nic_phy_hw_addr, (msg)+6,  6) == 0))    /* Ethernet Source - Host MAC */

struct eth_packet {
  uint8_t msg[ETH_FRAME_SIZE];                          /* ethernet frame (message) */
  uint8_t *oversize;                                    /* oversized frame (message) */
  uint32_t len;                                         /* packet length without CRC */
  uint32_t used;                                        /* bytes processed (used in packet chaining) */
  int     status;                                       /* transmit/receive status */
  uint32_t crc_len;                                     /* packet length with CRC */
};

struct eth_item {
  int                 type;                             /* receive (0=setup, 1=loopback, 2=normal) */
#define ETH_ITM_SETUP    0
#define ETH_ITM_LOOPBACK 1
#define ETH_ITM_NORMAL   2
  struct eth_packet   packet;
};

struct eth_queue {
  int                 max;
  int                 count;
  int                 head;
  int                 tail;
  int                 loss;
  int                 high;
  struct eth_item*    item;
};

typedef uchar_t ETH_MAC[6];

struct eth_list {
  char    name[ETH_DEV_NAME_MAX];
  char    desc[ETH_DEV_DESC_MAX];
  int     eth_api;
};

typedef int ETH_BOOL;
typedef uchar_t ETH_MULTIHASH[8];
typedef struct eth_packet  ETH_PACK;
typedef void (*ETH_PCALLBACK)(int status);
typedef struct eth_list ETH_LIST;
typedef struct eth_queue ETH_QUE;
typedef struct eth_item ETH_ITEM;
struct eth_write_request {
  struct eth_write_request *next;
  ETH_PACK packet;
  };
typedef struct eth_write_request ETH_WRITE_REQUEST;

/* Ethernet API type - designates which network backend is in use */
typedef enum eth_api_e {
  ETH_API_NONE = 0,                                     /* No API in use yet */
  ETH_API_PCAP = 1,                                     /* Pcap API in use */
  ETH_API_TAP  = 2,                                     /* tun/tap API in use */
  ETH_API_VDE  = 3,                                     /* VDE API in use */
  ETH_API_UDP  = 4,                                     /* UDP API in use */
  ETH_API_NAT  = 5,                                     /* NAT (SLiRP) API in use */
  ETH_API_TEST = 6,                                     /* test API in use */
  ETH_API_COUNT                                         /* Number of API types (for array sizing) */
} eth_api_t;

/* Forward declarations for handle types - only where not already defined */
#ifdef HAVE_PCAP_NETWORK
typedef struct pcap pcap_t;
#endif
#ifdef HAVE_VDE_NETWORK
typedef struct vdeconn VDECONN;
#endif
/* sim_slirp_handle is defined in sim_slirp.h - no forward declaration needed */

/* Union for API-specific handles (discriminated by eth_api field in ETH_DEV) */
typedef union eth_handle_u {
  void *generic;                                        /* Generic pointer for initialization */
#ifdef HAVE_PCAP_NETWORK
  pcap_t *pcap;                                         /* PCAP handle */
#endif
#ifdef HAVE_VDE_NETWORK
  VDECONN *vde;                                         /* VDE connection */
#endif
  /* sim_slirp_handle* goes here but we can't reference it without including sim_slirp.h,
   * which would create circular dependency. Legacy void* handle field used for SLiRP. */
  /* TAP/UDP use fd_handle, no separate pointer needed */
} eth_handle_t;

/*============================================================================*/
/*                    Reader/Writer State Machine Types                      */
/*============================================================================*/

#if defined(USE_READER_THREAD)

/* Reader State Machine States */
typedef enum eth_reader_state_e {
    ETH_READER_INIT,            /* Initial state - perform setup */
    ETH_READER_SELECT_WAIT,     /* Waiting for data (select/poll or Windows event) */
    ETH_READER_DISPATCH_READ,   /* Dispatch to API-specific read handler */
    ETH_READER_CHECK_ASYNC,     /* Check if async wakeup needed */
    ETH_READER_ERROR_HANDLER,   /* Handle read errors */
    ETH_READER_SHUTDOWN,        /* Clean shutdown */
    ETH_READER_STATE_COUNT      /* Number of states (for array sizing) */
} eth_reader_state_t;

/* Writer State Machine States */
typedef enum eth_writer_state_e {
    ETH_WRITER_INIT,            /* Initial state - perform setup */
    ETH_WRITER_WAIT_WORK,       /* Wait for write requests (condition variable) */
    ETH_WRITER_GET_REQUEST,     /* Pull request from queue */
    ETH_WRITER_THROTTLE_CHECK,  /* Check if throttle delay needed */
    ETH_WRITER_THROTTLE_DELAY,  /* Sleep due to throttle */
    ETH_WRITER_DISPATCH_WRITE,  /* Dispatch to API-specific write handler */
    ETH_WRITER_CLEANUP,         /* Return buffer to free list */
    ETH_WRITER_SHUTDOWN,        /* Clean shutdown */
    ETH_WRITER_STATE_COUNT      /* Number of states (for array sizing) */
} eth_writer_state_t;

#endif /* USE_READER_THREAD */

struct eth_device {
  char*         name;                                   /* name of ethernet device */
  eth_handle_t  handle_union;                           /* Discriminated union of API-specific handles */
  void*         handle;                                 /* Legacy generic handle pointer (for compatibility) */
  SOCKET        fd_handle;                              /* fd to kernel device (where needed) */
  char*         bpf_filter;                             /* bpf filter currently in effect */
  eth_api_t eth_api;                               /* Designator for which API is being used to move packets */
  ETH_PCALLBACK read_callback;                          /* read callback function */
  ETH_PCALLBACK write_callback;                         /* write callback function */
  ETH_PACK*     read_packet;                            /* read packet */
  ETH_MAC       filter_address[ETH_FILTER_MAX];         /* filtering addresses */
  int           addr_count;                             /* count of filtering addresses */
  ETH_BOOL      promiscuous;                            /* promiscuous mode flag */
  ETH_BOOL      all_multicast;                          /* receive all multicast messages */
  ETH_BOOL      hash_filter;                            /* filter using AUTODIN II multicast hash */
  ETH_MULTIHASH hash;                                   /* AUTODIN II multicast hash */
  int32_t       loopback_self_sent;                     /* loopback packets sent but not seen */
  int32_t       loopback_self_sent_total;               /* total loopback packets sent */
  int32_t       loopback_self_rcvd_total;               /* total loopback packets seen */
  ETH_MAC       physical_addr;                          /* physical address of interface */
  int32_t       have_host_nic_phy_addr;                 /* flag indicating that the host_nic_phy_hw_addr is valid */
  ETH_MAC       host_nic_phy_hw_addr;                   /* MAC address of the attached NIC */
  uint32_t      jumbo_fragmented;                       /* Giant IPv4 Frames Fragmented */
  uint32_t      jumbo_dropped;                          /* Giant Frames Dropped */
  uint32_t      jumbo_truncated;                        /* Giant Frames too big for capture buffer - Dropped */
  uint32_t      packets_sent;                           /* Total Packets Sent */
  uint32_t      packets_received;                       /* Total Packets Received */
  uint32_t      loopback_packets_processed;             /* Total Loopback Packets Processed */
  uint32_t      transmit_packet_errors;                 /* Total Send Packet Errors */
  uint32_t      receive_packet_errors;                  /* Total Read Packet Errors */
  int32_t       error_waiting_threads;                  /* Count of threads currently waiting after an error */
  ETH_BOOL      error_needs_reset;                      /* Flag indicating to force reset */
#define ETH_ERROR_REOPEN_THRESHOLD 10                   /* Attempt ReOpen after 20 send/receive errors */
#define ETH_ERROR_REOPEN_PAUSE 4                        /* Seconds to pause between closing and reopening LAN */
  uint32_t      error_reopen_count;                     /* Count of ReOpen Attempts */
  DEVICE*       dptr;                                   /* device ethernet is attached to */
  uint32_t      dbit;                                   /* debugging bit */
  int           reflections;                            /* packet reflections on interface */
  int           need_crc;                               /* device needs CRC (Cyclic Redundancy Check) */
  /* Throttling control parameters: */
  uint32_t      throttle_time;                          /* ms burst time window */
#define ETH_THROT_DEFAULT_TIME 5                        /* 5ms Default burst time window */
  uint32_t      throttle_burst;                         /* packets passed with throttle_time which trigger throttling */
#define ETH_THROT_DEFAULT_BURST 4                       /* 4 Packet burst in time window */
  uint32_t      throttle_delay;                         /* ms to delay when throttling.  0 disables throttling */
#define ETH_THROT_DISABLED_DELAY 0                      /* 0 Delay disables throttling */
#define ETH_THROT_DEFAULT_DELAY 10                      /* 10ms Delay during burst */
  /* Throttling state variables: */
  uint32_t      throttle_mask;                          /* match test for threshold detection (1 << throttle_burst) - 1 */
  uint32_t      throttle_events;                        /* keeps track of packet arrival values */
  uint32_t      throttle_packet_time;                   /* time last packet was transmitted */
  uint32_t      throttle_count;                         /* Total Throttle Delays */
#if defined (USE_READER_THREAD)
  bool          asynch_io;                              /* Asynchronous Interrupt scheduling enabled */
  int           asynch_io_latency;                      /* instructions to delay pending interrupt */
  sim_tailq_t   read_queue;                             /* Lock-free SPSC packet queue */
  sim_mutex_t   lock;
  sim_thread_t  reader_thread;                          /* Reader Thread Id */
  sim_thread_t  writer_thread;                          /* Writer Thread Id */
  bool          threading_initialized;                  /* Thread state needs cleanup */
  /* Startup synchronization barrier */
  sim_mutex_t   startup_lock;
  sim_cond_t    startup_cond;
  int           threads_ready;                          /* Count of threads that have signaled ready */
  sim_mutex_t   writer_lock;
  sim_mutex_t   self_lock;
  sim_cond_t    writer_cond;
  ETH_WRITE_REQUEST *write_requests;
  int write_queue_peak;
  ETH_WRITE_REQUEST *write_buffers;
  t_stat write_status;
  /* State machine states */
  eth_reader_state_t reader_state;                      /* Current reader state */
  eth_writer_state_t writer_state;                      /* Current writer state */
#endif
};

typedef struct eth_device  ETH_DEV;

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

#if defined(USE_READER_THREAD)

/* Reader State Machine Context */
typedef struct eth_reader_context_s {
    ETH_DEV *dev;                       /* Device being serviced */
    eth_reader_state_t current_state;   /* Current state */
    int sel_ret;                        /* Select return value */
    int status;                         /* Last operation status */
    int do_select;                      /* Whether select/poll is needed */
    SOCKET select_fd;                   /* FD for select (non-Windows) */
#if defined(_WIN32)
    HANDLE hWait;                       /* Event handle (Windows PCAP) */
#endif
} eth_reader_context_t;

/* Writer State Machine Context */
typedef struct eth_writer_context_s {
    ETH_DEV *dev;                       /* Device being serviced */
    eth_writer_state_t current_state;   /* Current state */
    ETH_WRITE_REQUEST *request;         /* Current write request */
    int status;                         /* Last write status */
    uint32_t packet_delta_time;         /* Time since last packet (for throttling) */
} eth_writer_context_t;

/* State handler function types */
typedef eth_reader_state_t (*eth_reader_state_handler_t)(eth_reader_context_t *ctx);
typedef eth_writer_state_t (*eth_writer_state_handler_t)(eth_writer_context_t *ctx);

#endif /* USE_READER_THREAD */

/* prototype declarations*/

t_stat eth_open   (ETH_DEV* dev, const char* name,      /* open ethernet interface */
                   DEVICE* dptr, uint32_t dbit);
t_stat eth_close  (ETH_DEV* dev);                       /* close ethernet interface */
t_stat eth_attach_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32_t flag, const char *cptr);
t_stat eth_write  (ETH_DEV* dev, ETH_PACK* packet,      /* write synchronous packet; */
                   ETH_PCALLBACK routine);              /*  callback when done */
int eth_read      (ETH_DEV* dev, ETH_PACK* packet,      /* read single packet; */
                   ETH_PCALLBACK routine);              /*  callback when done*/
t_stat eth_filter (ETH_DEV* dev, int addr_count,        /* set filter on incoming packets */
                   const ETH_MAC addresses[],
                   ETH_BOOL all_multicast,
                   ETH_BOOL promiscuous);
t_stat eth_filter_hash (ETH_DEV* dev, int addr_count,   /* set filter on incoming packets with hash */
                        const ETH_MAC addresses[],
                        ETH_BOOL all_multicast,
                        ETH_BOOL promiscuous,
                        ETH_MULTIHASH* const hash);     /* AUTODIN II based 8 byte imperfect hash */
t_stat eth_filter_hash_ex (ETH_DEV* dev, int addr_count,/* set filter on incoming packets with hash */
                           const ETH_MAC addresses[],
                           ETH_BOOL all_multicast,
                           ETH_BOOL promiscuous,
                           ETH_BOOL match_broadcast,
                           ETH_MULTIHASH* const hash);  /* AUTODIN II based 8 byte imperfect hash */
t_stat eth_check_address_conflict (ETH_DEV* dev,
                                   const ETH_MAC address);
const char *eth_version (void);                         /* Version of dynamically loaded library (pcap) */
void eth_setcrc   (ETH_DEV* dev, int need_crc);         /* enable/disable CRC mode */
t_stat eth_set_async (ETH_DEV* dev, int latency);       /* set read behavior to be async */
t_stat eth_clr_async (ETH_DEV* dev);                    /* set read behavior to be not async */
t_stat eth_set_throttle (ETH_DEV* dev, uint32_t time, uint32_t burst, uint32_t delay); /* set transmit throttle parameters */
uint32_t eth_crc32(uint32_t crc, const void* vbuf, size_t len); /* Compute Ethernet Autodin II CRC for buffer */

void eth_packet_trace (ETH_DEV* dev, const uint8_t *msg, int len, const char* txt); /* trace ethernet packet header+crc */
void eth_packet_trace_ex (ETH_DEV* dev, const uint8_t *msg, int len, const char* txt, int detail, uint32_t reason); /* trace ethernet packet */
t_stat eth_show (FILE* st, UNIT* uptr,                  /* show ethernet devices */
                 int32_t val, const void* desc);
t_stat eth_show_devices (FILE* st, DEVICE *dptr,        /* show ethernet devices */
                         UNIT* uptr, int32_t val, const char* desc);
int eth_devices (int max, ETH_LIST* dev, ETH_BOOL framers); /* get ethernet devices on host */
void eth_show_dev (FILE*st, ETH_DEV* dev);              /* show ethernet device state */

#define ETH_MAC_STRING_SIZE sizeof("XX:XX:XX:XX:XX:XX")
void eth_mac_fmt (const ETH_MAC add, char* buffer,
                  size_t buffer_size);                  /* format ethernet mac address */
t_stat eth_mac_scan (ETH_MAC mac, const char* strmac);  /* scan string for mac, put in mac */
t_stat eth_mac_scan_ex (ETH_MAC mac,                    /* scan string for mac, put in mac */
                        const char* strmac, UNIT *uptr);/* for specified unit */

/* Legacy ETH_QUE functions - always available for test backend */
t_stat ethq_init (ETH_QUE* que, int max);              /* initialize FIFO queue */
void ethq_clear  (ETH_QUE* que);                       /* clear FIFO queue */
void ethq_remove (ETH_QUE* que);                       /* remove item from FIFO queue */
void ethq_insert (ETH_QUE* que, int32_t type,          /* insert item into FIFO queue */
                  ETH_PACK* packet, int32_t status);
void ethq_insert_data(ETH_QUE* que, int32_t type,      /* insert item into FIFO queue */
                  const uint8_t *data, int used, size_t len,
                  size_t crc_len, const uint8_t *crc_data, int32_t status);
t_stat ethq_destroy(ETH_QUE* que);                     /* release FIFO queue */

#if defined(USE_READER_THREAD)
/* Adapter functions for lock-free SPSC queue - only used internally in sim_ether.c */
t_stat eth_tailq_init (sim_tailq_t* que, int max);     /* initialize lock-free queue */
void eth_tailq_clear  (sim_tailq_t* que);              /* clear lock-free queue */
void eth_tailq_destroy(sim_tailq_t* que);              /* destroy lock-free queue */
void eth_tailq_insert_data(sim_tailq_t* que, int32_t type, /* insert into lock-free queue */
                  const uint8_t *data, int used, size_t len,
                  size_t crc_len, const uint8_t *crc_data, int32_t status);
#endif

const char *eth_capabilities(void);
t_stat sim_ether_test (DEVICE *dptr, const char *cptr); /* unit test routine */

/* Well-known Ethernet MAC addresses:
 *
 * eth_mac_any: All zeroes/any address
 * eth_mac_bcast: All ones broadcast.
 */
extern const ETH_MAC eth_mac_any;
extern const ETH_MAC eth_mac_bcast;

/* Type-enforcing MAC address copy function.
 *
 * This inline helps to prevent the following situation:
 *
 *   void network_func(DEVICE *dev, ETH_MAC *mac)
 *   {
 *     ETH_MAC other_mac;
 *
 *     ...
 *     memcpy(other_mac, mac, sizeof(ETH_MAC));
 *   }
 *
 * The compiler will happily accept the memcpy() as valid because src and dst are
 * converted to "void *". This is a subtle bug -- mac is a pointer to an ETH_MAC
 * and memcpy will copy from somewhere other than the first byte of the source MAC
 * address.
 */
static inline void eth_copy_mac(ETH_MAC dst, const ETH_MAC src)
{
  memcpy(dst, src, sizeof(ETH_MAC));
}

/* Type-enforcing MAC comparison function. Helps to avoid subtle memcmp() issues
 * (see above).
 */
static inline int eth_mac_cmp(const ETH_MAC a, const ETH_MAC b)
{
  return memcmp(a, b, sizeof(ETH_MAC));
}

#if !defined(SIM_TEST_INIT)     /* Need stubs for test APIs */
#define SIM_TEST_INIT
#define SIM_TEST(xxx)
#endif

#endif                                                  /* _SIM_ETHER_H */
