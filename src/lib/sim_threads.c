// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <string.h>

#include "sim_threads.h"
#include "string_compat.h"

/* Platform-specific headers - only include what we need */
#if defined(HAVE_SETTHREADDESCRIPTION)
#  include <windows.h>
#  include <processthreadsapi.h>
#elif defined(HAVE_PRCTL_SET_NAME)
#  include <sys/prctl.h>
#elif defined(HAVE_PTHREAD_SETNAME_NP_CURRENT) || \
      defined(HAVE_PTHREAD_SET_NAME_NP) || \
      defined(HAVE_PTHREAD_SETNAME_NP_NETBSD) || \
      defined(HAVE_PTHREAD_SETNAME_NP_GENERIC)
#  include <pthread.h>
#  if defined(HAVE_PTHREAD_SET_NAME_NP)
#    include <pthread_np.h>
#  endif
#endif

#ifndef THREAD_NAME_MAX
#  define THREAD_NAME_MAX 64
#endif

void sim_set_thread_name(const char *name)
{
    if (name == NULL || *name == '\0')
        return;

#if defined(HAVE_SETTHREADDESCRIPTION)
    wchar_t wname[THREAD_NAME_MAX];
#  if defined(_MSC_VER)
    size_t converted;
    if (mbstowcs_s(&converted, wname, THREAD_NAME_MAX, name, _TRUNCATE) == 0) {
        SetThreadDescription(GetCurrentThread(), wname);
    }
#  else
    if (mbstowcs(wname, name, THREAD_NAME_MAX - 1) != (size_t)-1) {
        wname[THREAD_NAME_MAX - 1] = L'\0';
        SetThreadDescription(GetCurrentThread(), wname);
    }
#  endif

#elif defined(HAVE_PRCTL_SET_NAME)
    char truncated[THREAD_NAME_MAX];
    strlcpy(truncated, name, sizeof(truncated));
    prctl(PR_SET_NAME, truncated, 0, 0, 0);

#elif defined(HAVE_PTHREAD_SETNAME_NP_CURRENT)
    char truncated[THREAD_NAME_MAX];
    strlcpy(truncated, name, sizeof(truncated));
    pthread_setname_np(truncated);

#elif defined(HAVE_PTHREAD_SET_NAME_NP)
    char truncated[THREAD_NAME_MAX];
    strlcpy(truncated, name, sizeof(truncated));
    pthread_set_name_np(pthread_self(), truncated);

#elif defined(HAVE_PTHREAD_SETNAME_NP_NETBSD)
    char truncated[THREAD_NAME_MAX];
    strlcpy(truncated, name, sizeof(truncated));
    pthread_setname_np(pthread_self(), "%s", truncated);

#elif defined(HAVE_PTHREAD_SETNAME_NP_GENERIC)
    char truncated[THREAD_NAME_MAX];
    strlcpy(truncated, name, sizeof(truncated));
    pthread_setname_np(pthread_self(), truncated);

#else
    /* Unsupported platform - silently ignore */
    (void)name;
#endif
}
