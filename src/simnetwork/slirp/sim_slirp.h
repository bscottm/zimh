#if !defined(SIM_SLIRP_H)

#    if defined(HAVE_SLIRP_NETWORK)

#        include "sim_atomic.h"
#        include "libslirp.h"
#        include "poll_compat.h"
#        include "simnetwork/eth_types.h"

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// SLiRP network state and associated data structures
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

/* WSAPoll() is available on Windows Vista (> 0x0600) and higher, otherwise revert to
 * select(). */
#        if SIM_USE_POLL && defined(WINVER) && WINVER < 0x0600
#            undef SIM_USE_SELECT
#            undef SIM_USE_POLL
#            define SIM_USE_SELECT 1
#            define SIM_USE_POLL 0
#        endif

#        if !defined(SIM_USE_SELECT) && !defined(SIM_USE_POLL)
#            error "sim_slirp.c: Configuration error: define SIM_USE_SELECT, SIM_USE_POLL"
#        endif

#        if SIM_USE_SELECT + SIM_USE_POLL > 1
#            error "sim_slirp.c: Configuration error: set only one of SIM_USE_SELECT, SIM_USE_POLL to 1."
#        endif

#        if SLIRP_CONFIG_VERSION_MAX < 6
// Older libslirp before slirp_os_socket reflects the underlying system's actual
// type...
typedef int slirp_os_socket;
typedef ssize_t slirp_ssize_t;
#        endif

#        if SIM_USE_SELECT
#            define SIM_INVALID_MAX_FD ((slirp_os_socket) -1)
#        endif

/* sim_slirp debugging: */
enum { DBG_POLL = 0, DBG_SOCKET = 1 };

// SLiRP network state:
struct sim_slirp {
    SlirpConfig slirp_config;
    SlirpCb slirp_callbacks;
    Slirp *slirp_cxn;

    char *args;

#        if defined(USE_READER_THREAD)
    /* Access lock to libslirp. libslirp is not threaded or protected. */
    pthread_mutex_t libslirp_lock;

    /* Condvar, mutex when there are no sockets to poll or select for reading. */
    pthread_cond_t no_sockets_cv;
    pthread_mutex_t no_sockets_lock;
#        endif

    // Number of currently active sockets.
    sim_atomic_value_t n_sockets;

    /* DNS search domains (argument copy) */
    char *dns_search;
    char **dns_search_domains;
    /* Boot file and TFTP path prefix (argument copy) */
    char *the_bootfile;
    char *the_tftp_path;
    /* UDP and TCP ports that SIMH proxies to the Slirp network */
    struct redir_tcp_udp *rtcp;
    /* Associated simulator Ethernet device */
    ETH_DEV *eth_dev;

    /* Debugging bitmasks: */
    DEBTAB *original_debflags;
    size_t flag_offset;

    /* I/O event tracking/handling (used to be the GPollFD array): */
#        if SIM_USE_SELECT
    /* select() needs a lookup table to map SOCKETs to an integer index. */
    fd_set readfds;
    fd_set writefds;
    fd_set exceptfds;
    slirp_os_socket max_fd;

    /* Lookup table: */
    slirp_os_socket *lut;
    size_t lut_alloc;
#        elif SIM_USE_POLL
    /* Next descriptor to use */
    size_t fd_idx;
    /* Total allocated descriptors */
    size_t n_fds;
    /* Poll file descriptor array */
    sim_pollfd_t *fds;
#        endif

    /* SIMH debug info: */
    DEVICE *dptr;
    uint32_t dbit;
};

/* Simulator -> host network redirection state. */
struct redir_tcp_udp {
    int is_udp;
    /* SIMH host port, e.g., 2223.  */
    int simh_host_port;
    /* The simulator's IP address, e.g., 10.0.2.4 or 10.0.2.15 */
    struct in_addr sim_local_inaddr;
    /* The simulator's port, e.g., 23 */
    int sim_local_port;
    struct redir_tcp_udp *next;
};

/* File descriptor array initial allocation, incremental (linear) allocation. */
#        define FDS_ALLOC_INIT 32
#        define FDS_ALLOC_INCR 32

typedef struct sim_slirp sim_slirp_network;

sim_slirp_network *sim_slirp_open(const char *args, ETH_DEV *eth_dev, DEVICE *dptr, uint32_t dbit, char *errbuf,
                                  size_t errbuf_size);
void sim_slirp_close(sim_slirp_network *slirp);
int sim_slirp_send(sim_slirp_network *slirp, const char *msg, size_t len, int flags);
int sim_slirp_select(sim_slirp_network *slirp, int ms_timeout);
t_stat sim_slirp_attach_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32_t flag, const char *cptr);
void sim_slirp_show(sim_slirp_network *slirp, FILE *st);

/* SLiRP-specific callbacks: */
int slirp_get_events_callback(int idx, void *opaque);

/* Internal support functions: */
int64_t sim_clock_get_ns(void *opaque);

/* slirp_poll.c externs: */
void register_poll_socket(slirp_os_socket fd, void *opaque);
void unregister_poll_socket(slirp_os_socket fd, void *opaque);

void *simh_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque);
void simh_timer_free(void *the_timer, void *opaque);
void simh_timer_mod(void *timer, int64_t expire_time, void *opaque);

#    endif /* HAVE_SLIRP_NETWORK */

#    define SIM_SLIRP_H
#endif
