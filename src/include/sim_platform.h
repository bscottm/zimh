// SPDX-FileCopyrightText: 1993-2016 Robert M Supnik
// SPDX-License-Identifier: X11

#if !defined(SIM_PLATFORM_H_)
#    define SIM_PLATFORM_H_ 1

/* Windows is very funny and picky if winsock2.h isn't included before windows.h. */
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

/* string_compat.h also includes the ctype functions. */
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

/*
 * Prevent inlining where call frame boundaries are useful for debugging,
 * instrumentation, or host-specific behavior.
 */
#    if !defined(SIM_NOINLINE)
#        if defined(_MSC_VER)
#            define SIM_NOINLINE _declspec(noinline)
#        elif defined(__GNUC__) || defined(__clang__)
#            define SIM_NOINLINE __attribute__((noinline))
#        else
#            define SIM_NOINLINE
#        endif
#    endif

/* Unused argument macro: easier to comprehend than a "(void) var;" statement. */
#    define SIM_UNUSED_ARG(x) (void)x

/* Unused function attribute: easier to comprehend than a complicated compiler
   attribute */
#    if defined(__GNUC__) || defined(__clang__)
#        define SIM_UNUSED_FUNC __attribute__((unused))
#    elif defined(_MSC_VER)
#        if __STDC_VERSION >= 201710L
#            define SIM_UNUSED_FUNC [[maybe_unused]]
#        else
#            define SIM_UNUSED_FUNC
#            pragma warning(suppress : 4505)
#        endif
#    endif

/*
 * Allow the compiler to validate printf-style format arguments. The
 * arguments are the one-based format string and first variadic argument
 * positions used by GCC and Clang's format attribute.
 */
#    ifndef PRINTF_FMT
#        if defined(__cppcheck__)
#            define PRINTF_FMT(n, m)
#        elif defined(__has_attribute)
#            if __has_attribute(format)
#                define PRINTF_FMT(n, m) __attribute__((format(printf, n, m)))
#            endif
#        endif
#        if !defined(PRINTF_FMT)
#            define PRINTF_FMT(n, m)
#        endif
#    endif

/*
 * Mark an intentional switch fallthrough. Prefer the C23 spelling when
 * available, and fall back to compiler-specific spellings for older C modes.
 */
#    if !defined(FALLTHROUGH)
#        if defined(__has_c_attribute)
#            if __has_c_attribute(fallthrough)
#                define FALLTHROUGH [[fallthrough]]
#            endif
#        endif

#        if !defined(FALLTHROUGH) && defined(__has_attribute)
#            if __has_attribute(fallthrough)
#                define FALLTHROUGH __attribute__((fallthrough))
#            endif
#        endif

#        if !defined(FALLTHROUGH) && defined(__GNUC__) && __GNUC__ >= 7
#            define FALLTHROUGH __attribute__((fallthrough))
#        endif

#        if !defined(FALLTHROUGH)
#            define FALLTHROUGH ((void)0)
#        endif
#    endif

// Unreachable code.
#    if defined(_MSC_VER)
#        define SIM_UNREACHABLE() __assume(0)
#    elif defined(__GNUC__) || defined(__clang__)
#        define SIM_UNREACHABLE() __builtin_unreachable()
#    else
#        define SIM_UNREACHABLE()                                                                                      \
            do {                                                                                                       \
            } while (0)
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
#        define MIN __min
#        define MAX __max
#    else
#        if !defined(MAX)
#            define MAX(a, b) (((a) >= (b)) ? (a) : (b))
#        endif
#        if !defined(MIN)
#            define MIN(a, b) (((a) <= (b)) ? (a) : (b))
#        endif
#    endif

//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
// Meta-types: These are cross-platform types, such as sim_off_t for file offsets. Unix style names, i.e.,
// lowercased names are preferred over Windows style uppercased "shouting" names.
//
// Also included are the custom PRI macros for printing meta-types.
//
// sim_off_t: File offset type
//=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=

#    if !defined(_WIN32) && !defined(_WIN64)
// POSIX
typedef off_t sim_off_t;

#        define PRIsim_off_t "lld"
#    else
// Windows...
typedef LONGLONG sim_off_t;

#        define PRIsim_off_t PRId64
#    endif

#endif /* SIM_PLATFORM_H_ */