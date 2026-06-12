// SPDX-FileCopyrightText: 2026 B. Scott Michel
// SPDX-License-Identifier: X11

/*
 * This is a thin wrapper around the C11+ concurrency library that falls
 * back to pthreads if C11 concurrency isn't supported.
 *
 * Macros:
 *
 * THREAD_FUNC_DECL(FUNC)  Thread function declaration macro. Use it in
 *                         forward declarations.
 * THREAD_FUNC_DEFN(FUNC)  Thread function definition macro. Use it in
 *                         front of the function's body (implementation).
 *
 * Types:
 *
 * sim_thread_fn           Wrapper type for a thread function pointer
 * sim_thread_exit_t       Wrapper type for a thread's exit value
 * sim_thread_t            Wrapper type for a thread identifier (TID)
 * sim_cond_t              Wrapper type for condition variables.
 * sim_mutex_t             Wrapper type for mutexes
 */

#if !defined(SIM_THREADS_H)

#if defined(HAVE_C11_THREADS) || (__STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__))
#  include <threads.h>
#  define HAVE_STD_THREADS 1
#  define THREAD_FUNC_DECL(FUNC) int FUNC(void *arg)
#  define THREAD_FUNC_DEFN(FUNC) int FUNC(void *arg)
#  define THREAD_FUNC_RETURN(VAL) ((int) VAL)

typedef int    (*sim_thread_fn)(void *arg);
typedef int    sim_thread_exit_t;
typedef thrd_t sim_thread_t;   
typedef cnd_t  sim_cond_t;
typedef mtx_t  sim_mutex_t;
#elif defined(HAVE_PTHREADS)
#  include <pthread.h>

#  define HAVE_STD_THREADS 0
#  define THREAD_FUNC_DECL(FUNC) void *FUNC(void *arg)
#  define THREAD_FUNC_DEFN(FUNC) void *FUNC(void *arg)
#  define THREAD_FUNC_RETURN(VAL) ((void *) VAL)

typedef void           *(*sim_thread_fn)(void *arg);
typedef void           *sim_thread_exit_t;
typedef pthread_t       sim_thread_t;
typedef pthread_cond_t  sim_cond_t;
typedef pthread_mutex_t sim_mutex_t;
#else
#error "No standard threads or pthreads?"
#endif

/* Create a new thread.
 *
 * Returns 0 on success, -1 (non-zero)on failure 
 */
static inline int sim_thread_create(sim_thread_t *thread_id, sim_thread_fn func, void *arg)
{
#if HAVE_STD_THREADS
    return (thrd_create(thread_id, func, arg) == thrd_success) ? 0 : -1;
#elif defined(HAVE_PTHREADS)
    pthread_attr_t attr;
    int retval;

    pthread_attr_init (&attr);
    pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM);
    retval = pthread_create(thread_id, &attr, func, arg);
    pthread_attr_destroy( &attr);

    return (retval == 0) ? 0 : -1;
#endif
}

static inline int sim_thread_equal(sim_thread_t left, sim_thread_t right)
{
#if HAVE_STD_THREADS
    return thrd_equal(left, right);
#elif defined(HAVE_PTHREADS)
    return pthread_equal(left, right);
#endif
}

static inline sim_thread_t sim_thread_self()
{
#if HAVE_STD_THREADS
    return thrd_current();
#elif defined(HAVE_PTHREADS)
    return pthread_self();
#endif
}

static inline int sim_thread_join(sim_thread_t thread_id, sim_thread_exit_t *exit_val)
{
#if HAVE_STD_THREADS
    return thrd_join(thread_id, exit_val);
#elif defined(HAVE_PTHREADS)
    return pthread_join(thread_id, exit_val);
#endif
}

static inline int sim_cond_init(sim_cond_t *cond)
{
#if HAVE_STD_THREADS
    return cnd_init(cond);
#elif defined(HAVE_PTHREADS)
    return pthread_cond_init(cond, NULL);
#endif
}

static inline void sim_cond_destroy(sim_cond_t *cond)
{
#if HAVE_STD_THREADS
    cnd_destroy(cond);
#elif defined(HAVE_PTHREADS)
    pthread_cond_destroy(cond);
#endif
}

static inline int sim_cond_signal(sim_cond_t *cond)
{
#if HAVE_STD_THREADS
    return cnd_signal(cond);
#elif defined(HAVE_PTHREADS)
    return pthread_cond_signal(cond);
#endif
}

static inline int sim_cond_broadcast(sim_cond_t *cond)
{
#if HAVE_STD_THREADS
    return cnd_broadcast(cond);
#elif defined(HAVE_PTHREADS)
    return pthread_cond_broadcast(cond);
#endif
}

static inline int sim_cond_wait(sim_cond_t *cond, sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    return cnd_wait(cond, mtx);
#elif defined(HAVE_PTHREADS)
    return pthread_cond_wait(cond, mtx);
#endif
}

static inline int sim_cond_timedwait(sim_cond_t *cond, sim_mutex_t *mtx, const struct timespec *tmo)
{
#if HAVE_STD_THREADS
    return cnd_timedwait(cond, mtx, tmo);
#elif defined(HAVE_PTHREADS)
    return pthread_cond_timedwait(cond, mtx, tmo);
#endif
}

static inline int sim_mutex_init(sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    return mtx_init(mtx, mtx_plain);
#elif defined(HAVE_PTHREADS)
    return pthread_mutex_init(mtx, NULL);
#endif
}

static inline int sim_mutex_recursive(sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    return mtx_init(mtx, mtx_plain | mtx_recursive);
#elif defined(HAVE_PTHREADS)
    pthread_mutexattr_t recursive;
    int retval;

    pthread_mutexattr_init (&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    retval = pthread_mutex_init(mtx, &recursive);
    pthread_mutexattr_destroy(&recursive);
    return retval;
#endif
}

static inline void sim_mutex_lock(sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    mtx_lock(mtx);
#elif defined(HAVE_PTHREADS)
    pthread_mutex_lock(mtx);
#endif
}

static inline void sim_mutex_unlock(sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    mtx_unlock(mtx);
#elif defined(HAVE_PTHREADS)
    pthread_mutex_unlock(mtx);
#endif
}

static inline void sim_mutex_destroy(sim_mutex_t *mtx)
{
#if HAVE_STD_THREADS
    mtx_destroy(mtx);
#elif defined(HAVE_PTHREADS)
    pthread_mutex_destroy(mtx);
#endif
}

#define SIM_THREADS_H
#endif
