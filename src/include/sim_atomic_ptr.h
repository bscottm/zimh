// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Atomic pointer operations. Fashioned after sim_atomic.h but specifically
 * for pointer types.
 *
 * Uses C11 atomics, falling back to compiler/platform intrinsics when necessary.
 *
 * Types:
 *
 *   sim_atomic_ptr_t: The wrapper structure that encapsulates an atomic pointer.
 *
 * sim_atomic_ptr_init(sim_atomic_ptr_t *p): Initialize an atomic pointer wrapper.
 *   The pointer is set to NULL.
 *
 * sim_atomic_ptr_destroy(sim_atomic_ptr_t *p): The inverse of sim_atomic_ptr_init().
 *   The pointer is set to a sentinel value.
 *
 * sim_atomic_ptr_get(sim_atomic_ptr_t *p): Atomically returns the pointer value
 *   in the wrapper. Uses ACQUIRE ordering.
 *
 * sim_atomic_ptr_get_explicit(sim_atomic_ptr_t *p, int order): Atomically returns
 *   the pointer with explicit memory ordering.
 *
 * sim_atomic_ptr_put(sim_atomic_ptr_t *p, void *newval): Atomically stores a new
 *   pointer in the wrapper. Uses RELEASE ordering.
 *
 * sim_atomic_ptr_put_explicit(sim_atomic_ptr_t *p, void *newval, int order):
 *   Atomically stores a new pointer with explicit memory ordering.
 *
 * sim_atomic_ptr_cas(sim_atomic_ptr_t *p, void **expected, void *desired):
 *   Compare-and-swap operation for pointers. Returns non-zero (true) if the swap
 *   occurred, zero (false) otherwise. If the swap fails, *expected is updated with
 *   the current value. Uses SEQ_CST ordering.
 *
 * sim_atomic_ptr_exchange(sim_atomic_ptr_t *p, void *newval): Atomically exchanges
 *   the pointer, returning the old value. Uses SEQ_CST ordering.
 *
 * Memory Ordering Notes:
 * ----------------------
 * The default operations use appropriate ordering for typical use cases:
 *   - Loads use ACQUIRE (prevents subsequent operations from moving before the load)
 *   - Stores use RELEASE (prevents prior operations from moving after the store)
 *   - CAS and exchange use SEQ_CST (full sequential consistency)
 *
 * For performance-critical code where relaxed ordering is acceptable, use the
 * _explicit variants with the memory order constants from sim_atomic.h.
 */

#if !defined(SIM_ATOMIC_PTR_H)
#define SIM_ATOMIC_PTR_H

#include <stdint.h>
#include "sim_atomic.h"

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Pointer wrapper type:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

typedef struct sim_atomic_ptr_s {
    SIM_ATOMIC_TYPE(void *) ptr;
} sim_atomic_ptr_t;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Initialization, destruction:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline void sim_atomic_ptr_init(sim_atomic_ptr_t *p)
{
#if HAVE_STD_ATOMIC
    atomic_init(&p->ptr, NULL);
#else
    p->ptr = NULL;
#endif
}

static inline void sim_atomic_ptr_destroy(sim_atomic_ptr_t *p)
{
    /* Set to recognizable sentinel value for debugging */
    p->ptr = (void *)(uintptr_t)0xDEADBEEF;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Load operations:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline void *sim_atomic_ptr_get_explicit(const sim_atomic_ptr_t *p, int order)
{
    void *retval;

#if HAVE_STD_ATOMIC
    retval = atomic_load_explicit(&p->ptr, order);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))
    __atomic_load(&p->ptr, &retval, order);
#  elif defined(_WIN32) || defined(_WIN64)
    /* Windows Interlocked* functions don't support explicit memory ordering.
     * All operations provide full barrier semantics (SEQ_CST).
     *
     * CRITICAL: Must preserve volatile qualifier in the cast. The underlying
     * p->ptr is `void * volatile` (via SIM_ATOMIC_TYPE macro), and casting to
     * plain `(PVOID*)` would discard volatile, potentially allowing the compiler
     * to cache the pointer value across the InterlockedCompareExchangePointer
     * call, breaking atomicity guarantees. This was causing hangs in the
     * zimh-unit-tailq test on MSVC builds.
     */
    (void)order;
    /* Use CompareExchange with NULL to perform atomic read */
    retval = InterlockedCompareExchangePointer((PVOID volatile *)&p->ptr, NULL, NULL);
    /* Special case: if value is actually NULL, the above works correctly.
     * If value is non-NULL, retval contains the actual value. */
#  endif
#endif

    return retval;
}

static inline void *sim_atomic_ptr_get(const sim_atomic_ptr_t *p)
{
    return sim_atomic_ptr_get_explicit(p, SIM_ATOMIC_ACQUIRE);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Store operations:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline void sim_atomic_ptr_put_explicit(sim_atomic_ptr_t *p, void *newval, int order)
{
#if HAVE_STD_ATOMIC
    atomic_store_explicit(&p->ptr, newval, order);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_RELEASE) && (defined(__GNUC__) || defined(__clang__))
    __atomic_store(&p->ptr, &newval, order);
#  elif defined(_WIN32) || defined(_WIN64)
    /* InterlockedExchangePointer provides full barrier (SEQ_CST).
     * CRITICAL: Must preserve volatile qualifier - see load operation above. */
    (void)order;
    InterlockedExchangePointer((PVOID volatile *)&p->ptr, newval);
#  endif
#endif
}

static inline void sim_atomic_ptr_put(sim_atomic_ptr_t *p, void *newval)
{
    sim_atomic_ptr_put_explicit(p, newval, SIM_ATOMIC_RELEASE);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Compare-and-swap:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline int sim_atomic_ptr_cas(sim_atomic_ptr_t *p, void **expected, void *desired)
{
    int result;

#if HAVE_STD_ATOMIC
    result = atomic_compare_exchange_strong_explicit(&p->ptr, expected, desired,
                                                      SIM_ATOMIC_SEQ_CST,
                                                      SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    result = __atomic_compare_exchange(&p->ptr, expected, &desired,
                                       0,  /* strong CAS */
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    {
        void *old_val = InterlockedCompareExchangePointer((PVOID*)&p->ptr, desired, *expected);
        result = (old_val == *expected);
        if (!result) {
            *expected = old_val;
        }
    }
#  endif
#endif

    return result;
}

/* Weak CAS variant: May spuriously fail even when values match.
 * Useful in loops where retry is acceptable. */
static inline int sim_atomic_ptr_cas_weak(sim_atomic_ptr_t *p, void **expected, void *desired)
{
    int result;

#if HAVE_STD_ATOMIC
    result = atomic_compare_exchange_weak_explicit(&p->ptr, expected, desired,
                                                    SIM_ATOMIC_SEQ_CST,
                                                    SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    result = __atomic_compare_exchange(&p->ptr, expected, &desired,
                                       1,  /* weak CAS */
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    /* Windows doesn't have weak CAS, fall back to strong */
    result = sim_atomic_ptr_cas(p, expected, desired);
#  endif
#endif

    return result;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Exchange:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline void *sim_atomic_ptr_exchange(sim_atomic_ptr_t *p, void *newval)
{
    void *retval;

#if HAVE_STD_ATOMIC
    retval = atomic_exchange_explicit(&p->ptr, newval, SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    retval = __atomic_exchange_n(&p->ptr, newval, __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    retval = InterlockedExchangePointer((PVOID*)&p->ptr, newval);
#  endif
#endif

    return retval;
}

#endif /* SIM_ATOMIC_PTR_H */
