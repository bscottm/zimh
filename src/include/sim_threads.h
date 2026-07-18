// SPDX-FileCopyrightText: 2026 B. Scott Michel
// SPDX-License-Identifier: X11

/*
 * This is a thin wrapper abstraction around the pthreads library that permits additional thread library
 * support, if ever needed.
 *
 * C11 concurrency was an interesting experiment, but should not be considered unless an absolutely bare boned
 * threading implementation is required as a fallback. Consider, though, that features such as thread naming
 * and thread affinity require platform or pthread functionality.
 *
 * Macros:
 *
 * THREAD_FUNC_DECL(FUNC)  Thread function declaration macro. Use it in
 *                         forward declarations.
 * THREAD_FUNC_DEFN(FUNC)  Thread function definition macro. Use it in
 *                         front of the function's body (implementation).
 * THREAD_FUNC_RETURN(VAL) Thread function return value. Notably, this is used as:
 *
 *                             return THREAD_FUNC_RETURN(val);
 *
 *                         pthreads returns a (void *) and the macro handles the casting.
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

#    include <stdint.h>

#    if defined(HAVE_PTHREADS)
#        include <pthread.h>

#        define THREAD_FUNC_DECL(FUNC) void *FUNC(void *arg)
#        define THREAD_FUNC_DEFN(FUNC) void *FUNC(void *arg)
#        define THREAD_FUNC_RETURN(VAL) ((void *)VAL)

typedef void *(*sim_thread_fn)(void *arg);
typedef void *sim_thread_exit_t;
typedef pthread_t sim_thread_t;
typedef pthread_cond_t sim_cond_t;
typedef pthread_mutex_t sim_mutex_t;
#    else
#        error "No standard threads or pthreads?"
#    endif

static inline int sim_thread_equal(sim_thread_t left, sim_thread_t right)
{
#    if defined(HAVE_PTHREADS)
    return pthread_equal(left, right);
#    endif
}

static inline sim_thread_t sim_thread_self()
{
#    if defined(HAVE_PTHREADS)
    return pthread_self();
#    endif
}

static inline void sim_thread_yield(void)
{
#    if defined(_WIN32) || defined(_WIN64)
    SwitchToThread();
#    elif defined(HAVE_PTHREADS)
    sched_yield();
#    endif
}
static inline int sim_thread_join(sim_thread_t thread_id, sim_thread_exit_t *exit_val)
{
#    if defined(HAVE_PTHREADS)
    return pthread_join(thread_id, exit_val);
#    endif
}

static inline int sim_cond_init(sim_cond_t *cond)
{
#    if defined(HAVE_PTHREADS)
    return pthread_cond_init(cond, NULL);
#    endif
}

static inline void sim_cond_destroy(sim_cond_t *cond)
{
#    if defined(HAVE_PTHREADS)
    pthread_cond_destroy(cond);
#    endif
}

static inline int sim_cond_signal(sim_cond_t *cond)
{
#    if defined(HAVE_PTHREADS)
    return pthread_cond_signal(cond);
#    endif
}

static inline int sim_cond_broadcast(sim_cond_t *cond)
{
#    if defined(HAVE_PTHREADS)
    return pthread_cond_broadcast(cond);
#    endif
}

static inline int sim_cond_wait(sim_cond_t *cond, sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    return pthread_cond_wait(cond, mtx);
#    endif
}

static inline int sim_cond_timedwait(sim_cond_t *cond, sim_mutex_t *mtx, const struct timespec *tmo)
{
#    if defined(HAVE_PTHREADS)
    return pthread_cond_timedwait(cond, mtx, tmo);
#    endif
}

static inline int sim_mutex_init(sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    return pthread_mutex_init(mtx, NULL);
#    endif
}

static inline int sim_mutex_recursive(sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    pthread_mutexattr_t recursive;
    int retval;

    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    retval = pthread_mutex_init(mtx, &recursive);
    pthread_mutexattr_destroy(&recursive);
    return retval;
#    endif
}

static inline void sim_mutex_lock(sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    pthread_mutex_lock(mtx);
#    endif
}

static inline void sim_mutex_unlock(sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    pthread_mutex_unlock(mtx);
#    endif
}

static inline void sim_mutex_destroy(sim_mutex_t *mtx)
{
#    if defined(HAVE_PTHREADS)
    pthread_mutex_destroy(mtx);
#    endif
}

/*=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Extern interface function declarations:
 *=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

extern int sim_thread_create(sim_thread_t *thread_id, sim_thread_fn func, void *arg);

#    if !defined(THREAD_NAME_MAX)
#        define THREAD_NAME_MAX 64
#    endif

extern void sim_set_thread_name(const char *);

/*=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Thread affinity:
 *=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#    define SIM_MAX_CPUS 64

typedef struct {
    uint64_t mask;
} sim_cpu_set_t;

static inline void sim_cpu_set_zero(sim_cpu_set_t *s)
{
    s->mask = 0;
}

static inline void sim_cpu_set_add(sim_cpu_set_t *s, int cpu)
{
    if (cpu >= 0 && cpu < SIM_MAX_CPUS)
        s->mask |= ((uint64_t)1 << cpu);
}

static inline int sim_cpu_set_empty(const sim_cpu_set_t *s)
{
    return s->mask == 0;
}

extern int sim_os_get_cpu_count(void);
extern t_stat sim_os_get_process_affinity(sim_cpu_set_t *set);
extern t_stat sim_os_set_thread_affinity(const sim_cpu_set_t *set);
extern t_stat sim_os_get_cpu_partition(sim_cpu_set_t *main_set, sim_cpu_set_t *io_set);

#    define SIM_THREADS_H
#endif
