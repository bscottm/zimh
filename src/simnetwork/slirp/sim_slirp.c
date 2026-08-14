// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* sim_slirp.c:

   This module provides the interface needed between sim_ether and libslirp to
   provide NAT network functionality.

*/
#define SIMH_IP_NETWORK 0x0a000200    /* aka 10.0.2.0 */
#define SIMH_IP_NETMASK 0xffffff00    /* aka 255.255.255.0 */
#define SIMH_GATEWAY_ADDR 0x0a000202  /* aka 10.0.2.2 (SIMH's address) */
#define SIMH_RESOLVER_ADDR 0x0a000203 /* aka 10.0.2.3 (DNS resolver) */

/* IPv6 private address range (ULA) blessed by IANA */
#define SIMH_IP6_NETWORK "fd00:cafe:dead:beef::"
#define SIMH_IP6_PREFIX_LEN 64
#define SIMH_GW_ADDR6 "fd00:cafe:dead:beef::2" /* SIMH's IPv6 address */

#include <glib.h>
#include <errno.h>                             /* Paranoia for Win32/64 */

#include "sim_defs.h"
#include "sim_tailq.h"
#include "simnetwork/eth_types.h"
#include "simnetwork/eth_backends.h"
#include "sim_slirp.h"
#include "sim_ether.h"
#include "sim_printf_fmts.h"

#define IS_TCP 0
#define IS_UDP 1

static const char *tcpudp[] = {"TCP", "UDP"};

/* Additional debugging flags added to the device's debug table. */
DEBTAB slirp_dbgtable[] = {{"POLL", 0, "Show libslirp polling callback activity"},
                           {"SOCKET", 0, "Show libslirp socket registration activity"},
                           {NULL}};

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Port redirection management:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static int parse_redirect_port(struct redir_tcp_udp **head, const char *buff, int is_udp)
{
    char gbuf[4 * CBUFSIZE];
    uint32_t inaddr = INADDR_ANY;
    int port = 0;
    int lport = 0;
    char *ipaddrstr = NULL;
    char *portstr = NULL;
    struct redir_tcp_udp *newp;

    gbuf[sizeof(gbuf) - 1] = '\0';
    strncpy(gbuf, buff, sizeof(gbuf) - 1);
    if (((ipaddrstr = strchr(gbuf, ':')) == NULL) || (*(ipaddrstr + 1) == 0)) {
        sim_printf("redir %s syntax error\n", tcpudp[is_udp]);
        return -1;
    }
    *ipaddrstr++ = 0;

    if ((ipaddrstr) && (((portstr = strchr(ipaddrstr, ':')) == NULL) || (*(portstr + 1) == 0))) {
        sim_printf("redir %s syntax error\n", tcpudp[is_udp]);
        return -1;
    }
    *portstr++ = 0;

    sscanf(gbuf, "%d", &lport);
    sscanf(portstr, "%d", &port);
    if (ipaddrstr != NULL) {
        struct in_addr addr;

        if (inet_pton(AF_INET, ipaddrstr, &addr) > 0) {
            inaddr = addr.s_addr;
        }
    }

    if (inaddr == INADDR_ANY) {
        sim_printf("%s redirection error: an IP address must be specified\n", tcpudp[is_udp]);
        return -1;
    }

    if ((newp = (struct redir_tcp_udp *)calloc(1, sizeof(struct redir_tcp_udp))) == NULL)
        return -1;

    newp->is_udp = is_udp;
    newp->simh_host_port = port;
    inet_pton(AF_INET, ipaddrstr, &newp->sim_local_inaddr);
    newp->sim_local_port = lport;
    newp->next = *head;
    *head = newp;
    return 0;
}

/* do_redirects: Adds the proxied (forwarded) ports from the guest network to the
 * outside network. */
static int do_redirects(sim_slirp_network *slirp, struct redir_tcp_udp *head)
{
    struct in_addr host_addr;
    int ret = 0;

    host_addr.s_addr = htonl(INADDR_ANY);
    if (head != NULL) {
        ret = do_redirects(slirp, head->next);
        if (slirp_add_hostfwd(slirp->slirp_cxn, head->is_udp, host_addr, head->sim_local_port, head->sim_local_inaddr,
                              head->simh_host_port) < 0) {
            char local_addr[16];

            inet_ntop(AF_INET, &head->sim_local_inaddr, local_addr, sizeof(local_addr));
            sim_printf("Can't establish redirector for: redir %s   =%d:%s:%d\n", tcpudp[head->is_udp],
                       head->sim_local_port, local_addr, head->simh_host_port);
            ++ret;
        }
    }
    return ret;
}

static unsigned int collect_slirp_debug(const char *dbg_tokens, int *err)
{
    unsigned int slirp_dbg = 0;

    while (dbg_tokens != NULL && *dbg_tokens != '\0' && *err == 0) {
#if SLIRP_CONFIG_VERSION_MAX >= 6
        if (!strncasecmp(dbg_tokens, "CALL", 4)) {
            slirp_dbg |= SLIRP_DBG_CALL;
        } else if (!strncasecmp(dbg_tokens, "MISC", 4)) {
            slirp_dbg |= SLIRP_DBG_MISC;
        } else if (!strncasecmp(dbg_tokens, "ERROR", 5)) {
            slirp_dbg |= SLIRP_DBG_ERROR;
        } else if (!strncasecmp(dbg_tokens, "VERBOSE_CALL", 13)) {
            slirp_dbg |= SLIRP_DBG_VERBOSE_CALL;
        } else if (!strncasecmp(dbg_tokens, "ALL", 3)) {
            slirp_dbg |= (SLIRP_DBG_CALL | SLIRP_DBG_MISC | SLIRP_DBG_ERROR | SLIRP_DBG_VERBOSE_CALL);
        } else
            *err = 1;
#else
        // libslirp debug is not visible until 4.9. <sigh!>
        *err = 1;
#endif

        if (*err == 0) {
            dbg_tokens = strchr(dbg_tokens, ';');
            if (dbg_tokens != NULL)
                ++dbg_tokens;
        }
    }

    return slirp_dbg;
}

static void libslirp_guest_error(const char *msg, void *opaque)
{
    /* sim_slirp_network *slirp = (sim_slirp_network *) opaque; */
    /* Avoid unused parameter warning */
    SIM_UNUSED_ARG(opaque);

    if (sim_deb != NULL) {
        fprintf(sim_deb, "libslirp guest error: %s\n", msg);
        fflush(sim_deb);
    }

    fprintf(stderr, "libslirp guest error: %s\n", msg);
    fflush(stderr);
}

/* Forward decl's... */
static int initialize_poll_fds(sim_slirp_network *slirp);
static slirp_ssize_t sim_slirp_receiver(const void *buf, size_t len, void *opaque);
static void notify_callback(void *opaque);

sim_slirp_network *sim_slirp_open(const char *args, ETH_DEV *eth_dev, DEVICE *dptr, uint32_t dbit, char *errbuf,
                                  size_t errbuf_size)
{
    sim_slirp_network *slirp = (sim_slirp_network *)calloc(1, sizeof(*slirp));
    SlirpConfig *cfg = &slirp->slirp_config;
    SlirpCb *cbs = &slirp->slirp_callbacks;

    char *targs = strdup(args);
    const char *tptr = targs;
    const char *cptr;
    char tbuf[CBUFSIZE], gbuf[CBUFSIZE], abuf[CBUFSIZE];
    int err;
    struct in6_addr default_ipv6_prefix, default_ipv6_gw;

    /* Default IPv6 address -- FIXME */
    inet_pton(AF_INET6, SIMH_IP6_NETWORK, &default_ipv6_prefix);
    inet_pton(AF_INET6, SIMH_GW_ADDR6, &default_ipv6_gw);

    /* Version 1 config */
    cfg->version = SLIRP_CONFIG_VERSION_MAX;
    cfg->restricted = 0;
    cfg->in_enabled = 1;
    cfg->vnetwork.s_addr = htonl(SIMH_IP_NETWORK);
    cfg->vnetmask.s_addr = htonl(SIMH_IP_NETMASK);
    cfg->vhost.s_addr = htonl(SIMH_GATEWAY_ADDR);
    cfg->in6_enabled = 0;
    cfg->vprefix_addr6 = default_ipv6_prefix;
    cfg->vprefix_len = SIMH_IP6_PREFIX_LEN;
    cfg->vhost6 = default_ipv6_gw;
    cfg->vnameserver.s_addr = htonl(SIMH_RESOLVER_ADDR);

    /* Version 2 config: nothing used */
    /* Version 3 config: */
    cfg->disable_dns = 0;

    /* Version 4 config */
    /* DHCP enabled by default. */
    cfg->disable_dhcp = 0;

    /* Version 5 config: nothing used */

    /* SIMH state/config */
    slirp->args = (char *)calloc(1 + strlen(args), sizeof(char));
    strlcpy(slirp->args, args, 1 + strlen(args));
    pthread_mutex_init(&slirp->libslirp_lock, NULL);
    pthread_cond_init(&slirp->no_sockets_cv, NULL);
    pthread_mutex_init(&slirp->no_sockets_lock, NULL);
    sim_atomic_init(&slirp->n_sockets);

    slirp->original_debflags = dptr->debflags;
    dptr->debflags = sim_combine_debtabs(dptr->debflags, slirp_dbgtable);
    sim_fill_debtab_flags(dptr->debflags);
    slirp->flag_offset = sim_debtab_nelems(slirp->original_debflags);

    /* Parse through arguments... */
    err = 0;
    while (*tptr && !err) {
        tptr = get_glyph_nc(tptr, tbuf, ',');
        if (!tbuf[0])
            break;
        cptr = tbuf;
        cptr = get_glyph(cptr, gbuf, '=');
        if (0 == MATCH_CMD(gbuf, "DHCP")) {
            cfg->disable_dhcp = 0;
            if (cptr != NULL && *cptr != '\0')
                inet_pton(AF_INET, cptr, &cfg->vdhcp_start);
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "TFTP")) {
            if (cptr != NULL && *cptr != '\0')
                slirp->the_tftp_path = strdup(cptr);
            else {
                strlcpy(errbuf, "Missing TFTP Path", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "BOOTFILE")) {
            if (cptr && *cptr)
                slirp->the_bootfile = strdup(cptr);
            else {
                strlcpy(errbuf, "Missing DHCP Boot file name", errbuf_size);
                err = 1;
            }
            continue;
        }
        if ((0 == MATCH_CMD(gbuf, "NAMESERVER")) || (0 == MATCH_CMD(gbuf, "DNS"))) {
            if (cptr && *cptr)
                inet_pton(AF_INET, cptr, &cfg->vnameserver);
            else {
                strlcpy(errbuf, "Missing nameserver", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "DNSSEARCH")) {
            if (cptr != NULL && *cptr) {
                int count = 0;
                char *name;

                slirp->dns_search = strdup(cptr);
                name = slirp->dns_search;
                do {
                    ++count;
                    slirp->dns_search_domains = (char **)realloc(cfg->vdnssearch, (count + 1) * sizeof(char *));
                    slirp->dns_search_domains[count] = NULL;
                    slirp->dns_search_domains[count - 1] = name;
                    if (NULL != (name = strchr(name, ','))) {
                        *name = '\0';
                        ++name;
                    }
                } while (NULL != name && *name);
            } else {
                strlcpy(errbuf, "Missing DNS search list", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "GATEWAY") || 0 == MATCH_CMD(gbuf, "GW")) {
            if (cptr && *cptr) {
                cptr = get_glyph(cptr, abuf, '/');
                if (cptr && *cptr)
                    cfg->vnetmask.s_addr = htonl(~((1 << (32 - atoi(cptr))) - 1));
                inet_pton(AF_INET, abuf, &cfg->vhost);
            } else {
                strlcpy(errbuf, "Missing host", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "NETWORK")) {
            if (cptr && *cptr) {
                cptr = get_glyph(cptr, abuf, '/');
                if (cptr && *cptr)
                    cfg->vnetmask.s_addr = htonl(~((1 << (32 - atoi(cptr))) - 1));
                inet_pton(AF_INET, abuf, &cfg->vnetwork);
            } else {
                strlcpy(errbuf, "Missing network", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "NODHCP")) {
            cfg->disable_dhcp = 1;
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "UDP")) {
            if (cptr && *cptr)
                err = parse_redirect_port(&slirp->rtcp, cptr, IS_UDP);
            else {
                strlcpy(errbuf, "Missing UDP port mapping", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "TCP")) {
            if (cptr && *cptr)
                err = parse_redirect_port(&slirp->rtcp, cptr, IS_TCP);
            else {
                strlcpy(errbuf, "Missing TCP port mapping", errbuf_size);
                err = 1;
            }
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "IPV6")) {
            cfg->in6_enabled = 1;
            continue;
        }
        if (0 == MATCH_CMD(gbuf, "NOIPV6")) {
            cfg->in6_enabled = 0;
            continue;
        }
#if SLIRP_CONFIG_VERSION_MAX >= 6
        if (0 == MATCH_CMD(gbuf, "SLIRP")) {
            unsigned int slirp_dbg = collect_slirp_debug(cptr, &err);

            if (!err) {
                slirp_set_debug(slirp_dbg);
                continue;
            }
        }
        if (0 == MATCH_CMD(gbuf, "NOSLIRP")) {
            unsigned int slirp_dbg = collect_slirp_debug(cptr, &err);

            if (!err) {
                slirp_reset_debug(slirp_dbg);
                continue;
            }
        }
#endif
        snprintf(errbuf, errbuf_size - 1, "Unexpected NAT argument: %s", gbuf);
        err = 1;
    }

    if (err) {
        sim_slirp_close(slirp);
        free(targs);
        return NULL;
    }

    /* Adjust the network prefix, update the guest's configuration. */
    cfg->vnetwork.s_addr = cfg->vhost.s_addr & cfg->vnetmask.s_addr;
    if ((cfg->vhost.s_addr & ~cfg->vnetmask.s_addr) == 0)
        cfg->vhost.s_addr = htonl(ntohl(cfg->vnetwork.s_addr) | 2);
    if (cfg->vdhcp_start.s_addr == 0)
        cfg->vdhcp_start.s_addr = htonl(ntohl(cfg->vnetwork.s_addr) | 15);
    if (cfg->vnameserver.s_addr == 0)
        cfg->vnameserver.s_addr = htonl(ntohl(cfg->vnetwork.s_addr) | 3);

    /* Set the DNS search domains */
    cfg->vdnssearch = (const char **)slirp->dns_search_domains;

    /* Set the BOOTP file and TFTP path in the Slirp config: */
    cfg->bootfile = slirp->the_bootfile;
    cfg->tftp_path = slirp->the_tftp_path;

    /* Initialize the callbacks: */
    slirp->eth_dev = eth_dev;

    cbs->send_packet = sim_slirp_receiver;
    cbs->guest_error = libslirp_guest_error;
    cbs->clock_get_ns = sim_clock_get_ns;
#if SLIRP_CONFIG_VERSION_MAX >= 6
    cbs->register_poll_socket = register_poll_socket;
    cbs->unregister_poll_socket = unregister_poll_socket;
#else
    cbs->register_poll_fd = register_poll_socket;
    cbs->unregister_poll_fd = unregister_poll_socket;
#endif
    cbs->notify = notify_callback;
    cbs->timer_mod = simh_timer_mod;
    cbs->timer_free = simh_timer_free;
    cbs->timer_new_opaque = simh_timer_new_opaque;

    slirp->slirp_cxn = slirp_new(cfg, cbs, (void *)slirp);

    /* Capture the debugging info. */
    slirp->dbit = dptr->dctrl = dbit;
    slirp->dptr = dptr;

    initialize_poll_fds(slirp);

    if (do_redirects(slirp, slirp->rtcp)) {
        sim_slirp_close(slirp);
        slirp = NULL;
    } else {
        sim_slirp_show(slirp, stdout);
        if (sim_log != NULL && sim_log != stdout) {
            sim_slirp_show(slirp, sim_log);
            if (sim_deb != sim_log)
                sim_slirp_show(slirp, sim_deb);
        }
    }

    free(targs);
    return slirp;
}

void sim_slirp_reader_shutdown(eth_backend_t *backend, ETH_DEV *dev)
{
    sim_slirp_network *slirp = backend->state.slirp;
    volatile sim_atomic_type_t n_sockets = sim_atomic_get(&slirp->n_sockets);

    /* Set the reader thread's exit condition. If the reader thread is waiting
     * on the condvar, signal the condition. */
    sim_atomic_put(&slirp->n_sockets, -1);
    if (n_sockets == 0)
        pthread_cond_broadcast(&slirp->no_sockets_cv);
}

void sim_slirp_writer_shutdown(eth_backend_t *backend, ETH_DEV *dev)
{
    (void) backend;
    (void) dev;
}

void sim_slirp_close(sim_slirp_network *slirp)
{
    if (slirp == NULL)
        return;

    free(slirp->args);
    free(slirp->the_tftp_path);
    free(slirp->the_bootfile);
    free(slirp->dns_search);
    free(slirp->dns_search_domains);

    if (slirp->slirp_cxn != NULL) {
        struct redir_tcp_udp *rtmp, *rnext;

        for (rtmp = rnext = slirp->rtcp; rtmp != NULL; rtmp = rnext) {
            slirp_remove_hostfwd(slirp->slirp_cxn, rtmp->is_udp, rtmp->sim_local_inaddr, rtmp->sim_local_port);
            rnext = rtmp->next;
            free(rtmp);
        }

        slirp->rtcp = NULL;
        slirp_cleanup(slirp->slirp_cxn);
        slirp->slirp_cxn = NULL;
    }

    if (slirp->dptr != NULL) {
        free(slirp->dptr->debflags);
        slirp->dptr->debflags = slirp->original_debflags;
    }

#if SIM_USE_SELECT
    free(slirp->lut);
    slirp->lut = NULL;
#elif SIM_USE_POLL
    free(slirp->fds);
    slirp->fds = NULL;
#endif

    pthread_mutex_destroy(&slirp->libslirp_lock);
    sim_atomic_destroy(&slirp->n_sockets);
    pthread_cond_destroy(&slirp->no_sockets_cv);
    pthread_mutex_destroy(&slirp->no_sockets_lock);
    sim_atomic_destroy(&slirp->n_sockets);

    free(slirp);
}

t_stat sim_slirp_attach_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32_t flag, const char *cptr)
{
    SIM_UNUSED_ARG(dptr);
    SIM_UNUSED_ARG(uptr);
    SIM_UNUSED_ARG(flag);
    SIM_UNUSED_ARG(cptr);

    fprintf(st, "%s",
            "NAT options:\n"
            "    DHCP{=dhcp_start_address}           Enables DHCP server and specifies\n"
            "                                        guest LAN DHCP start IP address\n"
            "    BOOTFILE=bootfilename               specifies DHCP returned Boot Filename\n"
            "    TFTP=tftp-base-path                 Enables TFTP server and specifies\n"
            "                                        base file path\n"
            "    NAMESERVER=nameserver_ipaddres      specifies DHCP nameserver IP address\n"
            "    DNS=nameserver_ipaddres             specifies DHCP nameserver IP address\n"
            "    DNSSEARCH=domain{:domain{:domain}}  specifies DNS Domains search suffixes\n"
            "    GATEWAY=host_ipaddress{/masklen}    specifies LAN gateway IP address\n"
            "    NETWORK=network_ipaddress{/masklen} specifies LAN network address\n"
            "    UDP=port:address:address's-port     maps host UDP port to guest port\n"
            "    TCP=port:address:address's-port     maps host TCP port to guest port\n"
            "    NODHCP                              disables DHCP server\n\n"
            "Default NAT Options: GATEWAY=10.0.2.2, masklen=24(netmask is 255.255.255.0)\n"
            "                     DHCP=10.0.2.15, NAMESERVER=10.0.2.3\n"
            "    Nameserver defaults to proxy traffic to host system's active nameserver\n\n"
            "The 'address' field in the UDP and TCP port mappings are the simulated\n"
            "(guest) system's IP address which, if DHCP allocated would default to\n"
            "10.0.2.15 or could be statically configured to any address including\n"
            "10.0.2.4 thru 10.0.2.14.\n\n"
            "NAT limitations\n\n"
            "There are four limitations of NAT mode which users should be aware of:\n\n"
            " 1) ICMP protocol limitations:\n"
            "    Some frequently used network debugging tools (e.g. ping or tracerouting)\n"
            "    rely on the ICMP protocol for sending/receiving messages. While some\n"
            "    ICMP support is available on some hosts (ping may or may not work),\n"
            "    some other tools may not work reliably.\n\n"
            " 2) Receiving of UDP broadcasts is not reliable:\n"
            "    The guest does not reliably receive broadcasts, since, in order to save\n"
            "    resources, it only listens for a certain amount of time after the guest\n"
            "    has sent UDP data on a particular port.\n\n"
            " 3) Protocols such as GRE, DECnet, LAT and Clustering are unsupported:\n"
            "    Protocols other than TCP and UDP are not supported.\n\n"
            " 4) Forwarding host ports < 1024 impossible:\n"
            "    On Unix-based hosts (e.g. Linux, Solaris, Mac OS X) it is not possible\n"
            "    to bind to ports below 1024 from applications that are not run by root.\n"
            "    As a result, if you try to configure such a port forwarding, the attach\n"
            "    will fail.\n\n"
            "These limitations normally don't affect standard network use. But the\n"
            "presence of NAT has also subtle effects that may interfere with protocols\n"
            "that are normally working. One example is NFS, where the server is often\n"
            "configured to refuse connections from non-privileged ports (i.e. ports not\n"
            " below 1024).\n");
    return SCPE_OK;
}

/* Initialize the select/poll file descriptor arrays. */
static int initialize_poll_fds(sim_slirp_network *slirp)
{
    size_t i;

#if SIM_USE_SELECT
    FD_ZERO(&slirp->readfds);
    FD_ZERO(&slirp->writefds);
    FD_ZERO(&slirp->exceptfds);

    /* Start out with a generous number of LUT slots */
    slirp->lut_alloc = FDS_ALLOC_INIT;
    slirp->lut = (SOCKET *)malloc(slirp->lut_alloc * sizeof(SOCKET));

    for (i = 0; i < slirp->lut_alloc; ++i)
        slirp->lut[i] = INVALID_SOCKET;
#elif SIM_USE_POLL
    /* poll()-based file descriptor polling. */
    static const sim_pollfd_t poll_initializer = {INVALID_SOCKET, 0, 0};

    slirp->n_fds = FDS_ALLOC_INIT;
    slirp->fd_idx = 0;
    slirp->fds = (sim_pollfd_t *)malloc(slirp->n_fds * sizeof(sim_pollfd_t));
    for (i = 0; i < slirp->n_fds; ++i) {
        slirp->fds[i] = poll_initializer;
    }
#endif

    return 0;
}

/* Show NAT network statistics. */
void sim_slirp_show(sim_slirp_network *slirp, FILE *st)
{
    struct redir_tcp_udp *rtmp;
    const SlirpConfig *cfg = &slirp->slirp_config;
    char *conn_info;
    char gateway_addr[16], netmask[16], nameserver[16], dhcp_start[16];

    if (slirp == NULL || slirp->slirp_cxn == NULL)
        return;

    inet_ntop(AF_INET, &cfg->vhost, gateway_addr, sizeof(gateway_addr));
    inet_ntop(AF_INET, &cfg->vnetmask, netmask, sizeof(netmask));
    inet_ntop(AF_INET, &cfg->vnameserver, nameserver, sizeof(nameserver));
    inet_ntop(AF_INET, &cfg->vdhcp_start, dhcp_start, sizeof(dhcp_start));

    fprintf(st, "NAT args: %s\n", (slirp->args != NULL ? slirp->args : "(none given)"));
    fprintf(st, "NAT network setup:\n");
    fprintf(st, "        gateway       = %s (%s)\n", gateway_addr, netmask);
#if defined(AF_INET6)
    char v6_prefix[45], v6_gateway[45];

    inet_ntop(AF_INET6, &slirp->slirp_config.vprefix_addr6, v6_prefix, sizeof(v6_prefix));
    inet_ntop(AF_INET6, &slirp->slirp_config.vhost6, v6_gateway, sizeof(v6_gateway));

    fprintf(st, "        IPv6          = %sabled.\n", slirp->slirp_config.in6_enabled ? "en" : "dis");
    if (slirp->slirp_config.in6_enabled) {
        fprintf(st, "          V6 Prefix   = %s/%d\n", v6_prefix, slirp->slirp_config.vprefix_len);
        fprintf(st, "          V6 Gateway  = %s\n", v6_gateway);
    }
#endif
    fprintf(st, "        DNS           = %s\n", nameserver);
    if (cfg->vdhcp_start.s_addr != 0)
        fprintf(st, "        dhcp_start    = %s\n", dhcp_start);
    if (cfg->bootfile != NULL)
        fprintf(st, "        dhcp bootfile = %s\n", cfg->bootfile);
    if (cfg->vdnssearch) {
        const char **domains = cfg->vdnssearch;

        fprintf(st, "        DNS domains   = ");
        while (*domains != NULL) {
            fprintf(st, "%s%s", (domains != cfg->vdnssearch) ? ", " : "", *domains);
            ++domains;
        }
        fprintf(st, "\n");
    }
    if (cfg->tftp_path != NULL)
        fprintf(st, "        tftp prefix   = %s\n", cfg->tftp_path);
    rtmp = slirp->rtcp;
    while (rtmp != NULL) {
        char local_addr[16];

        inet_ntop(AF_INET, &rtmp->sim_local_inaddr, local_addr, sizeof(local_addr));
        fprintf(st, "        redir %3s     = %d:%s:%d\n", tcpudp[rtmp->is_udp], rtmp->sim_local_port, local_addr,
                rtmp->simh_host_port);
        rtmp = rtmp->next;
    }

    if ((conn_info = slirp_connection_info(slirp->slirp_cxn)) != NULL) {
        fputs(conn_info, st);
        free(conn_info);
    }
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * The libslirp interface.
 *
 * libslirp has an inverted sense of input and output. "Input" means "input into libslirp", whereas "output" means
 * output to the guest (the simulator via sim_ether and friends.)
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

/* Process the incoming IP packet (output from libslirp) */
 slirp_ssize_t sim_slirp_receiver(const void *buf, size_t len, void *opaque)
{
    sim_slirp_network *slirp = (sim_slirp_network *)opaque;
    ETH_DEV *eth_dev = slirp->eth_dev;

    sim_debug(eth_dev->dbit, eth_dev->dptr, "NAT: _slirp_callback() received %zu bytes\n", len);
    eth_process_received_packet(eth_dev, buf, len, len);
    sim_debug(eth_dev->dbit, eth_dev->dptr, "NAT: _slirp_callback() delivered to eth_process_received_packet\n");

    /* FIXME: the packet callback should tell us how many octets were written.
     * For the time being, though, assume it was successful. */
    return len;
}

int sim_slirp_send(sim_slirp_network *slirp, const char *msg, size_t len, int flags)
{
    SIM_UNUSED_ARG(flags);

    slirp_input(slirp->slirp_cxn, (const uint8_t *)msg, (int)len);
    return (int)len;
}

bool before_slirp_send(eth_backend_t *backend, ETH_DEV *dev)
{
    sim_slirp_network *slirp = backend->state.slirp;

    pthread_mutex_lock(&slirp->libslirp_lock);
    return true;
}

bool after_slirp_send(eth_backend_t *backend, ETH_DEV *dev)
{
    sim_slirp_network *slirp = backend->state.slirp;

    /* Fun fact: libslirp will add immediate responses to the read queue for certain IP messages, such as
     * ARP, some ICMP requests, DNS replies, ... Instead of waiting for the reader thread to wake up,
     * dispatch them to the simulator immediately. */
    if (!sim_tailq_empty(&dev->read_queue)) {
        sim_activate_abs(dev->dptr->units, dev->asynch_io_latency);
    }

    pthread_mutex_unlock(&slirp->libslirp_lock);

    return true;
}

/* I/O thread notify callback from Slirp. Indicates that there's packet data waiting, I/O events
 * are pending. */
static void notify_callback(void *opaque)
{
    /* sim_slirp_network *slirp = (sim_slirp_network *) opaque; */
    SIM_UNUSED_ARG(opaque);
}

int64_t sim_clock_get_ns(void *opaque)
{
    SIM_UNUSED_ARG(opaque);

    /* Internally, libslirp cuts the nanoseconds down to milliseconds. */
    return ((uint64_t)sim_os_msec()) * 10000000ull;
}
