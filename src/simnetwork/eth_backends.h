// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(SIM_ETH_BACKENDS_H)
#    define SIM_ETH_BACKENDS_H

#    include <stdint.h>
#    include <stdbool.h>

#    include "sim_defs.h"
#    include "sim_sock.h"
#    include "simnetwork/eth_types.h"

/* On BSD/macOS, include net/bpf.h BEFORE any pcap headers to establish BPF definitions.
 * This prevents redefinition errors on macOS 15.5+ where both net/bpf.h and pcap/bpf.h
 * define bpf_program/bpf_insn. pcap.h is included transitively via sim_ether_internal.h. */
#    if (defined(xBSD) || defined(__APPLE__)) && (defined(HAVE_TAP_NETWORK) || defined(HAVE_PCAP_NETWORK))
#        include <sys/ioctl.h>
#        include <net/bpf.h>
#    endif

#    if defined(HAVE_PCAP_NETWORK)
#        include <pcap.h>
#        include <string.h>
#    endif /* HAVE_PCAP_NETWORK */

#    ifdef HAVE_VDE_NETWORK
#        include <libvdeplug.h>
#    endif /* HAVE_VDE_NETWORK */

#    ifdef HAVE_SLIRP_NETWORK
#        include "simnetwork/eth_slirp/sim_slirp.h"
#    endif

/* Ethernet testing backend. */
typedef struct eth_test_backend {
    char *name;
    ETH_QUE rx_to_guest;
    ETH_QUE tx_from_guest;
    int write_status;
    struct eth_test_backend *next;
} ETH_TEST_BACKEND;

/* eth_api_t movde to simnetwork/eth_types.h */

/* Discriminated union for API-specific state. */
struct eth_backend_s {
    /* API being used to move packets */
    eth_api_t eth_api;

    /* API interface: */

    /* Wait for a packet's arrival at the reader. This is the poll/select point.
     * Returns:
     * > 0: One or more packets have arrived.
     *   0: No packet arrival, not an error.
     * < 0: Error waiting for packet arrival.
     */
    int (*packet_wait)(struct eth_backend_s *backend, ETH_DEV *dev);
    /* Read a packet and queue it for simulator device processing.
     * > 0: Packet read successfully, queued for simulator device.
     *   0: No packet read (not an error, safe to retry)
     * < 0: Error reading packet.
     */
    int (*packet_read)(struct eth_backend_s *backend, ETH_DEV *dev);

    /* Housekeeping before invoking write_packet(), optional and may be NULL.
     * Returns true if successful, false on error.
     *
     * Note: This is used by the libslirp backend to acquire the mutex that serializes access to libslirp,
     * which is not thread safe.
     */
    bool (*before_packet_write)(struct eth_backend_s *self, ETH_DEV *dev);
    /* Packet writer function: writes one packet for the API.
     * Returns: 0 = success, non-zero = error
     */
    int (*write_packet)(ETH_DEV *dev, const ETH_PACK *packet);
    /* Housekeeping after invoking write_packet(), optional and may be NULL.
     * Returns true if successful, false on error.
     *
     * Note: This is used by the libslirp backend to release the previously acquired mutex that serializes
     * access to libslirp, which is not thread safe.
     */
    bool (*after_packet_write)(struct eth_backend_s *self, ETH_DEV *dev);

    /* Reader-side thread shutdown hook. Optional -- may be NULL. */
    void (*reader_shutdown)(struct eth_backend_s *self, ETH_DEV *dev);

    /* Writer-side thread shutdown hook. Optional -- may be NULL. */
    void (*writer_shutdown)(struct eth_backend_s *self, ETH_DEV *dev);
    
    /* Per-backend state.*/
    union {
#    ifdef HAVE_PCAP_NETWORK
        pcap_t *pcap;                   /* PCAP handle */
#    endif
#    if defined(HAVE_VDE_NETWORK)
        VDECONN *vde;                   /* VDE connection */
#    endif
#    if defined(HAVE_SLIRP_NETWORK)
        sim_slirp_network *slirp;       /* SLiRP network state */
#    endif
        ETH_TEST_BACKEND *test_backend; /* Test backend handle */

        /* Network socket for UDP and TAP backends.*/
        SOCKET eth_socket;
    } state;
};

// Default socket read timeout. Note: This can be made longer, which only
// affects how quickly the reader thread exits.
enum {
    ETH_READER_POLL_TMO = 500 /* ms */
};

/* Socket polling function */
int poll_eth_socket(eth_backend_t *backend, long timeout_ms);

/*--- API functions for eth_backend_t ---*/
int eth_wait_pcap(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_nat(eth_backend_t *backend, ETH_DEV *dev);
int eth_wait_test(eth_backend_t *backend, ETH_DEV *dev);

/* PCAP reader*/
int eth_reader_pcap(eth_backend_t *backend, ETH_DEV *dev);
/* NAT (libslirp) reader */
int eth_reader_nat(eth_backend_t *backend, ETH_DEV *dev);
/* No (null) network reader */
int eth_reader_none(eth_backend_t *backend, ETH_DEV *dev);
/* Test backend reader*/
int eth_reader_test(eth_backend_t *backend, ETH_DEV *dev);

/* PCAP writer */
int eth_writer_pcap(ETH_DEV *dev, const ETH_PACK *packet);
/* libslirp mutex acquisition */
bool before_slirp_send(eth_backend_t *self, ETH_DEV *dev);
/* libslirp writer */
int eth_writer_nat(ETH_DEV *dev, const ETH_PACK *packet);
/* libslirp mutex release */
bool after_slirp_send(eth_backend_t *self, ETH_DEV *dev);
/* Empty/no network writer: This does nothing. Really. */
int eth_writer_none(ETH_DEV *dev, const ETH_PACK *packet);
/* Test backend writer. */
int eth_writer_test(ETH_DEV *dev, const ETH_PACK *packet);

/* libslirp shutdown hooks: */
void sim_slirp_reader_shutdown(eth_backend_t *backend, ETH_DEV *dev);
void sim_slirp_writer_shutdown(eth_backend_t *backend, ETH_DEV *dev);

#endif
