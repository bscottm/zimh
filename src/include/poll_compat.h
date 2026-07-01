// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: X11

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// poll() [POSIX] and WSAPoll() [Windows] compatibility shim
//
// Shim types:
// sim_pollfd_t: The poll file descriptor structure -- struct pollfd on POSIX, WSAPOLLFD on Windows
// sim_nfds_t: Number of file descriptors in a sim_pollfd_t array (type varies)
// sim_polltmo_t: The poll() timeout (int, but you never know on Windows.)
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

#if !defined(SIM_POLL_COMPAT_H)
#define SIM_POLL_COMPAT_H

/* Sanity check: Either SIM_USE_POLL or SIM_USE_SELECT must equal 1, but not
 * both. One of them needs to be set to 1. */
#if SIM_USE_POLL + SIM_USE_SELECT > 1
#error                                                                         \
    "sim_ether.h: Configuration error: Cannot set both SIM_USE_SELECT and SIM_USE_POLL to 1."
#elif SIM_USE_POLL + SIM_USE_SELECT == 0
#error                                                                         \
    "sim_ether.h: Configuration error: set one of SIM_USE_SELECT, SIM_USE_POLL to 1."
#elif SIM_USE_POLL
// WSAPoll() comes from winsock2.h, which is included by sim_defs.h. sim_defs.h
// is generally included as the first header in each ZIMH source file.

#if !defined(_WIN32) && !defined(_WIN64)
#include <poll.h>

typedef struct pollfd sim_pollfd_t;
typedef nfds_t sim_nfds_t;
typedef int sim_polltmo_t;
#else
typedef WSAPOLLFD sim_pollfd_t;
typedef ULONG sim_nfds_t;
typedef INT sim_polltmo_t;

// inline shim so that ZIMH doesn't have to have separate preprocessor branches
// for POSIX vs. Windows.
static inline int poll(sim_pollfd_t *fds, sim_nfds_t nfds, sim_polltmo_t timeout) {
  return WSAPoll(fds, nfds, timeout);
}
#endif
#endif

#endif /* SIM_POLL_COMPAT_H */
