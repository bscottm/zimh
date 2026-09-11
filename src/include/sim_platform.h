// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// Standard header prelude, platform-specific headers.
//
// This header avoids all of the extra headers that are included by sim_platform.h, and is intended to be used
// in header files that are included by other header files.
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

#if !defined(SIM_PLATFORM_H)
#    define SIM_PLATFORM_H 1

#    include <ctype.h>
#    include <errno.h>
#    include <limits.h>
#    include <math.h>
#    include <setjmp.h>
#    include <stdbool.h>
#    include <stdarg.h>
#    include <stddef.h>
#    include <stdint.h>
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>

#    ifndef EXIT_FAILURE
#        define EXIT_FAILURE 1
#    endif
#    ifndef EXIT_SUCCESS
#        define EXIT_SUCCESS 0
#    endif

#    ifdef _WIN32
#        include <winsdkver.h>
#        include <sdkddkver.h>

#        if WINVER < 0x0A00 || _WIN32_WINNT < 0x0A00
#            error ZIMH requires a Windows 10 or newer API target.
#        endif

#        define WINDOWS_LEAN_AND_MEAN

#        include <winsock2.h>
#        include <ws2tcpip.h>
#        include <windows.h>
#        include <winerror.h>
#        undef PACKED     /* avoid macro name collision */
#        undef ERROR      /* avoid macro name collision */
#        undef MEM_MAPPED /* avoid macro name collision */
#        include <process.h>
#    endif

#    include "c_attrs.h"
#    include "string_compat.h"

#    if defined(_WIN32)
#        include "sim_win32_compat.h"
#    endif

/* avoid macro names collisions */
#    ifdef PMASK
#        undef PMASK
#    endif
#    ifdef RS
#        undef RS
#    endif
#    ifdef PAGESIZE
#        undef PAGESIZE
#    endif

// Prefer the type-safe versions of MIN and MAX available if on GNU C and Clang. The MSVC versions are not
// type-safe, but they are the best available on that platform. The generic versions are provided as a
// fallback for other compilers.
#    if defined(__GNUC__) || defined(__clang__)
#        if defined(MIN)
#            undef MIN
#        endif
#        if defined(MAX)
#            undef MAX
#        endif
#        define MIN(a, b)                                                                                              \
            ({                                                                                                         \
                __auto_type _a = (a);                                                                                  \
                __auto_type _b = (b);                                                                                  \
                _a < _b ? _a : _b;                                                                                     \
            })
#        define MAX(a, b)                                                                                              \
            ({                                                                                                         \
                __auto_type _a = (a);                                                                                  \
                __auto_type _b = (b);                                                                                  \
                _a > _b ? _a : _b;                                                                                     \
            })
#    elif defined(_MSC_VER)
#        if defined(MIN)
#            undef MIN
#        endif
#        if defined(MAX)
#            undef MAX
#        endif
#        define MIN _min
#        define MAX _max
#    else
#        if !defined(MAX)
#            define MAX(a, b) (((a) >= (b)) ? (a) : (b))
#        endif
#        if !defined(MIN)
#            define MIN(a, b) (((a) <= (b)) ? (a) : (b))
#        endif
#    endif

#endif /* SIM_PLATFORM_H */
