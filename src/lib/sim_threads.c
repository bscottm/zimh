// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Platform-agnostic thread management and utility functions*/
#include "sim_threads.h"

#include <threads.h>
#include <string.h>

// Platform detection for thread naming
#if defined(_WIN32)
#    include <windows.h>
#    include <processthreadsapi.h>
#elif defined(__linux__) || defined(__native_client__)
#    include <pthread.h>
#    include <sys/prctl.h>
#elif defined(__APPLE__) && defined(__MACH__)
#    include <pthread.h>
#endif

void sim_set_thread_name(const char *name)
{
    if (name == NULL)
        return;

#if defined(_WIN32)
    // Windows 10 (1607+) and Windows Server 2016+ support SetThreadDescription
    // C11 thrd_t on Windows/MinGW is usually just a handle or wrapper around it
    wchar_t wName[64];
    // Quick ASCII to WCHAR conversion for simplicity
    mbstowcs(wName, name, 63);
    wName[63] = L'\0';
    SetThreadDescription(GetCurrentThread(), wName);
#elif defined(__linux__)
    // Linux restricts thread names to 16 characters (including the null
    // terminator)
    char truncated_name[16];
    strlcpy(truncated_name, name, sizeof(truncated_name));
    truncated_name[15] = '\0';

    // prctl is safest for the "current" thread on Linux
    prctl(PR_SET_NAME, truncated_name, 0, 0, 0);
#elif defined(__APPLE__) && defined(__MACH__)
    // Apple's pthread_setname_np only works on the current thread and takes
    // just the string
    char truncated_name[64];
    strlcpy(truncated_name, name, sizeof(truncated_name));
    truncated_name[63] = '\0';
    pthread_setname_np(truncated_name);
#elif HAVE_PTHREADS
    // Fallback for other POSIX platforms with pthreads, but no prctl or Apple
    // extensions Note: This may not actually set the thread name in debuggers
    // on all platforms
    pthread_setname_np(pthread_self(), name);
#else
    // Fallback for unsupported platforms (FreeBSD, OpenBSD, etc. have different
    // signatures) Do nothing gracefully
    (void)name;
#endif
}