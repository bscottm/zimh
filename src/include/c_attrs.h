/* c_attrs.h: C attribute compatibility macros */
// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#if !defined(C_ATTRS_H_)
#    define C_ATTRS_H_ 1

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
/* C23 and later */
#        if defined(__has_c_attribute) && __has_c_attribute(fallthrough)
#            define FALLTHROUGH [[fallthrough]]

/* GCC/clang __attribute__((fallthrough)) */
#        elif defined(__has_attribute) && __has_attribute(fallthrough)
#            define FALLTHROUGH __attribute__((fallthrough))

/* GCC 7+ */
#        elif defined(__GNUC__) && __GNUC__ >= 7
#            define FALLTHROUGH __attribute__((fallthrough))

/* MSVC (no-op) */
#        elif defined(_MSC_VER)
#            define FALLTHROUGH __fallthrough

/* Fallback (no-op) */
#        else
#            define FALLTHROUGH ((void)0)
#        endif
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

/* Force a function to be inlined. */
#    if !defined(SIM_FORCE_INLINE)
#        if defined(__GNUC__) || defined(__clang__)
#            define SIM_FORCE_INLINE inline __attribute__((always_inline))
#        elif defined(_MSC_VER)
#            define SIM_FORCE_INLINE __forceinline
#        else
#            define SIM_FORCE_INLINE inline
#        endif
#    endif

/* Branch prediction hints. N.B.: These are compiler hints, not processor hints.
 * They merely hint at the compiler "this is the more (un)likely branch" so that
 * basic blocks are lengthened when possible. */
#    if defined(__has_c_attribute) && __has_c_attribute(likely)
// C23 or later with support for these attributes
#        define SIM_LIKELY(x) ((x) __attribute__((likely)))
#        define SIM_UNLIKELY(x) ((x) __attribute__((unlikely)))
#    elif defined(__GNUC__) || defined(__clang__)
#        define SIM_LIKELY(x) __builtin_expect(!!(x), 1)
#        define SIM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#    elif defined(_MSC_VER)
#        define SIM_LIKELY(x) (__assume(!!(x)), (x))
#        define SIM_UNLIKELY(x) (__assume(!(x)), (x))
#    else
#        define SIM_LIKELY(x) (x)
#        define SIM_UNLIKELY(x) (x)
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

#endif
