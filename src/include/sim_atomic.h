// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Atomic get/put/add/sub/inc/dec support. Fashioned after the SDL2
 * approach to atomic variables. Uses C11 atomics, falling back to
 * compiler/platform intrinsics when necessary.
 *
 * Types:
 *
 *   sim_atomic_value_t: The wrapper structure that encapsulates the atomic
 *     value.
 *
 *   sim_atomic_type_t: The underlying (largest common) type inside
 *     the sim_atomic_value_t wrapper. `intptr_t` is a reasonable
 *     choice.
 *
 * sim_atomic_init(sim_atomic_value_t *): Initialize an atomic wrapper. The
 *   value is set to 0 and the mutex is initialized (when necessary.)
 *
 * sim_atomic_destroy(sim_atomic_value_t *p): The inverse of sim_atomic_init().
 *   The value is set to 0xcafef00d. When necessary, the mutex is destroyed.
 *
 * sim_atomic_get(sim_atomic_value_t *p): Atomically returns the
 * sim_atomic_type_t value in the wrapper. Uses SEQ_CST ordering.
 *
 * sim_atomic_get_explicit(sim_atomic_value_t *p, int order): Atomically
 * returns the value with explicit memory ordering.
 *
 * sim_atomic_put(sim_atomic_value_t *p, sim_atomic_type_t newval): Atomically
 * stores a new value in the wrapper. Uses SEQ_CST ordering.
 *
 * sim_atomic_put_explicit(sim_atomic_value_t *p, sim_atomic_type_t newval, int order):
 * Atomically stores a new value with explicit memory ordering.
 *
 * sim_atomic_add, sim_atomic_sub(sim_atomic_value_t *p, sim_atomic_type_t x):
 *   Atomically add or subtract a quantity to or from the wrapper's value.
 *   Uses SEQ_CST ordering.
 *
 * sim_atomic_inc, sim_atomic_dec(sim_atomic_value_t *p): Atomically increment or
 *   decrement the wrapper's value, returning the incremented or decremented value.
 *   Uses SEQ_CST ordering.
 *
 * sim_atomic_cas(sim_atomic_value_t *p, sim_atomic_type_t *expected,
 *                sim_atomic_type_t desired): Compare-and-swap operation.
 *   Returns non-zero (true) if the swap occurred, zero (false) otherwise.
 *   If the swap fails, *expected is updated with the current value.
 *   Uses SEQ_CST ordering.
 *
 * sim_atomic_exchange(sim_atomic_value_t *p, sim_atomic_type_t newval):
 *   Atomically exchanges the value, returning the old value.
 *   Uses SEQ_CST ordering.
 *
 * Memory Ordering Notes:
 * ----------------------
 * The default operations use sequentially consistent (SEQ_CST) ordering,
 * which provides the strongest guarantees but may have performance implications
 * on weakly-ordered architectures (ARM, PowerPC, etc.). For performance-critical
 * code where relaxed ordering is acceptable, use the _explicit variants.
 *
 * Memory order constants (when using _explicit variants):
 *   SIM_ATOMIC_RELAXED  - No synchronization or ordering constraints
 *   SIM_ATOMIC_ACQUIRE  - Prevents reordering of subsequent operations before this load
 *   SIM_ATOMIC_RELEASE  - Prevents reordering of prior operations after this store
 *   SIM_ATOMIC_ACQ_REL  - Combines ACQUIRE and RELEASE (for RMW operations)
 *   SIM_ATOMIC_SEQ_CST  - Full sequential consistency (default)
 */

#if !defined(SIM_ATOMIC_H)
#define SIM_ATOMIC_H

#if !defined(__STDC_NO_ATOMICS__) && __STDC_VERSION__ >= 201112L
   /* C11 or newer compiler -- use the compiler's support for atomic types. */
#  include <stdatomic.h>
#  define HAVE_STD_ATOMIC 1
#  define SIM_ATOMIC_TYPE(X) _Atomic(X)
#else
#  define HAVE_STD_ATOMIC 0
#  define SIM_ATOMIC_TYPE(X) X volatile

#  if (defined(_WIN32) || defined(_WIN64)) || \
       (defined(__ATOMIC_ACQ_REL) && defined(__ATOMIC_SEQ_CST) && defined(__ATOMIC_ACQUIRE) && \
        defined(__ATOMIC_RELEASE))
     /* Atomic operations available! */
#    define HAVE_ATOMIC_PRIMS 1
#  else
#    define HAVE_ATOMIC_PRIMS 0
#  endif
#endif

#if !HAVE_STD_ATOMIC && !HAVE_ATOMIC_PRIMS
#  error "Neither C11 atomic types nor atomic intrinsics available on this compiler/platform."
#endif

/* Largest common type for atomic values. */
#if (defined(_WIN32) || defined(_WIN64))
#  define WINDOWS_LEAN_AND_MEAN
#  include <windows.h>
   typedef LONG sim_atomic_type_t;
#  define PRIsim_atomic "ld"
#else
   typedef long sim_atomic_type_t;
#  define PRIsim_atomic "ld"
#endif

/* Memory ordering type and constants.
 * 
 * On C11: Uses standard memory_order enum
 * On GCC/Clang: Maps to __ATOMIC_* values
 * On Windows: Defines custom enum (values ignored by Interlocked*)
 */
#if HAVE_STD_ATOMIC
   typedef memory_order sim_memory_order_t;
   
#  define SIM_ATOMIC_RELAXED memory_order_relaxed
#  define SIM_ATOMIC_ACQUIRE memory_order_acquire
#  define SIM_ATOMIC_RELEASE memory_order_release
#  define SIM_ATOMIC_ACQ_REL memory_order_acq_rel
#  define SIM_ATOMIC_SEQ_CST memory_order_seq_cst

#elif defined(__ATOMIC_ACQUIRE)
   typedef enum sim_memory_order_e {
       SIM_ATOMIC_RELAXED = __ATOMIC_RELAXED,
       SIM_ATOMIC_ACQUIRE = __ATOMIC_ACQUIRE,
       SIM_ATOMIC_RELEASE = __ATOMIC_RELEASE,
       SIM_ATOMIC_ACQ_REL = __ATOMIC_ACQ_REL,
       SIM_ATOMIC_SEQ_CST = __ATOMIC_SEQ_CST
   } sim_memory_order_t;

#else
   /* Windows Interlocked* don't use memory order parameters,
    * but we define the enum for API consistency */
   typedef enum sim_memory_order_e {
       SIM_ATOMIC_RELAXED = 0,
       SIM_ATOMIC_ACQUIRE = 1,
       SIM_ATOMIC_RELEASE = 2,
       SIM_ATOMIC_ACQ_REL = 3,
       SIM_ATOMIC_SEQ_CST = 4
   } sim_memory_order_t;
#endif

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Value type and wrapper for integral (numeric) atomics:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~*/

typedef struct sim_atomic_value_s {
    SIM_ATOMIC_TYPE(sim_atomic_type_t) value;
} sim_atomic_value_t;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Initialization, destruction:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline void sim_atomic_init(sim_atomic_value_t *p)
{
    p->value = 0;
}

static inline void sim_atomic_destroy(sim_atomic_value_t *p)
{
    p->value = 0xcafef00d;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Primitives:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~*/

static inline sim_atomic_type_t sim_atomic_get_explicit(const sim_atomic_value_t *p, sim_memory_order_t order)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
    retval = atomic_load_explicit(&p->value, order);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))
        __atomic_load(&p->value, &retval, order);
#  elif defined(_WIN32) || defined(_WIN64)
        /* Windows Interlocked* functions don't support explicit memory ordering.
         * InterlockedOr with 0 provides a full barrier (acts as SEQ_CST). */
        (void)order;  /* Suppress unused parameter warning */
        retval = InterlockedOr((LONG volatile *)&p->value, 0);
#  endif
#endif

    return retval;
}

static inline sim_atomic_type_t sim_atomic_get(const sim_atomic_value_t *p)
{
    return sim_atomic_get_explicit(p, SIM_ATOMIC_SEQ_CST);
}

static inline void sim_atomic_put_explicit(sim_atomic_value_t *p, sim_atomic_type_t newval, sim_memory_order_t order)
{
#if HAVE_STD_ATOMIC
    atomic_store_explicit(&p->value, newval, order);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_RELEASE) && (defined(__GNUC__) || defined(__clang__))
    __atomic_store(&p->value, &newval, order);
#  elif defined(_WIN32) || defined(_WIN64)
    /* InterlockedExchange provides full barrier (SEQ_CST) */
    (void)order;  /* Suppress unused parameter warning */
    InterlockedExchange(&p->value, newval);
#  endif
#endif
}

static inline void sim_atomic_put(sim_atomic_value_t *p, sim_atomic_type_t newval)
{
    sim_atomic_put_explicit(p, newval, SIM_ATOMIC_SEQ_CST);
}

static inline sim_atomic_type_t sim_atomic_add(sim_atomic_value_t *p, sim_atomic_type_t x)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
    /* atomic_fetch_add returns the old p->value value. */
    retval = atomic_fetch_add_explicit(&p->value, x, SIM_ATOMIC_SEQ_CST) + x;
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST)
#    if (defined(__GNUC__) || defined(__clang__))
    retval = __atomic_add_fetch(&p->value, x, __ATOMIC_SEQ_CST);
#    else
#      error "sim_atomic_add: No atomic add intrinsic?"
#    endif
#  elif defined(_WIN32) || defined(_WIN64)
#    if defined(InterlockedAdd)
        retval = InterlockedAdd(&p->value, x);
#    else
        /* Older Windows InterlockedExchangeAdd, which returns the original value in p->value. */
        retval = InterlockedExchangeAdd(&p->value, x) + x;
#    endif
#  endif
#endif

    return retval;
}

static inline sim_atomic_type_t sim_atomic_sub(sim_atomic_value_t *p, sim_atomic_type_t x)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
    /* atomic_fetch_sub returns the old p->value value. */
    retval = atomic_fetch_sub_explicit(&p->value, x, SIM_ATOMIC_SEQ_CST) - x;
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST)
#    if defined(__GNUC__) || defined(__clang__)
        retval = __atomic_sub_fetch(&p->value, x, __ATOMIC_SEQ_CST);
#    else
#      error "sim_atomic_sub: No atomic sub intrinsic?"
#    endif
#  elif defined(_WIN32) || defined(_WIN64)
    /* There isn't a InterlockedSub function. Revert to basic math(s). */
#    if defined(InterlockedAdd)
        retval = InterlockedAdd(&p->value, -x);
#    else
        /* Older Windows InterlockedExchangeAdd, which returns the original value in p->value. */
        retval = InterlockedExchangeAdd(&p->value, -x) - x;
#    endif
#  endif
#endif

    return retval;
}

static inline sim_atomic_type_t sim_atomic_inc(sim_atomic_value_t *p)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
    retval = sim_atomic_add(p, 1);
#elif HAVE_ATOMIC_PRIMS
#  if !defined(_WIN32) && !defined(_WIN64)
        retval = sim_atomic_add(p, 1);
#  elif defined(_WIN32) || defined(_WIN64)
        retval = InterlockedIncrement(&p->value);
#  endif
#endif

    return retval;
}

static inline sim_atomic_type_t sim_atomic_dec(sim_atomic_value_t *p)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
       retval = sim_atomic_sub(p, 1);
#elif HAVE_ATOMIC_PRIMS
#  if !defined(_WIN32) && !defined(_WIN64)
        retval = sim_atomic_sub(p, 1);
#  elif defined(_WIN32) || defined(_WIN64)
        retval = InterlockedDecrement(&p->value);
#  endif
#endif

    return retval;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Compare-and-swap (CAS) and exchange operations:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~*/

/* Compare-and-swap: Atomically compares the value at p with *expected.
 * If they match, stores desired and returns non-zero (true).
 * If they don't match, stores the current value in *expected and returns zero (false).
 * Uses SEQ_CST ordering for both success and failure.
 */
static inline int sim_atomic_cas(sim_atomic_value_t *p, 
                                  sim_atomic_type_t *expected,
                                  sim_atomic_type_t desired)
{
    int result;

#if HAVE_STD_ATOMIC
    result = atomic_compare_exchange_strong_explicit(&p->value, expected, desired,
                                                       SIM_ATOMIC_SEQ_CST,
                                                       SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    result = __atomic_compare_exchange(&p->value, expected, &desired,
                                        0,  /* strong CAS */
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    {
        sim_atomic_type_t old_val = InterlockedCompareExchange(&p->value, desired, *expected);
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
 * Useful in loops where retry is acceptable. Generally faster on some architectures.
 */
static inline int sim_atomic_cas_weak(sim_atomic_value_t *p,
                                       sim_atomic_type_t *expected,
                                       sim_atomic_type_t desired)
{
    int result;

#if HAVE_STD_ATOMIC
    result = atomic_compare_exchange_weak_explicit(&p->value, expected, desired,
                                                     SIM_ATOMIC_SEQ_CST,
                                                     SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    result = __atomic_compare_exchange(&p->value, expected, &desired,
                                        1,  /* weak CAS */
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    /* Windows doesn't have a weak CAS, so fall back to strong */
    result = sim_atomic_cas(p, expected, desired);
#  endif
#endif

    return result;
}

/* Atomic exchange: Atomically replaces the value with newval and returns the old value.
 * Uses SEQ_CST ordering.
 */
static inline sim_atomic_type_t sim_atomic_exchange(sim_atomic_value_t *p,
                                                      sim_atomic_type_t newval)
{
    sim_atomic_type_t retval;

#if HAVE_STD_ATOMIC
    retval = atomic_exchange_explicit(&p->value, newval, SIM_ATOMIC_SEQ_CST);
#elif HAVE_ATOMIC_PRIMS
#  if defined(__ATOMIC_SEQ_CST) && (defined(__GNUC__) || defined(__clang__))
    retval = __atomic_exchange_n(&p->value, newval, __ATOMIC_SEQ_CST);
#  elif defined(_WIN32) || defined(_WIN64)
    retval = InterlockedExchange(&p->value, newval);
#  endif
#endif

    return retval;
}

#endif
