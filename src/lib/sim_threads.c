// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <string.h>

#include "sim_defs.h"
#include "sim_threads.h"
#include "string_compat.h"

/* Platform-specific headers - only include what we need */
#if defined(HAVE_SETTHREADDESCRIPTION)
#    include <processthreadsapi.h>
#elif defined(HAVE_PRCTL_SET_NAME)
#    include <sys/prctl.h>
#elif defined(HAVE_PTHREAD_SETNAME_NP_CURRENT) || defined(HAVE_PTHREAD_SET_NAME_NP) ||                                 \
    defined(HAVE_PTHREAD_SETNAME_NP_NETBSD) || defined(HAVE_PTHREAD_SETNAME_NP_GENERIC)
#    include <pthread.h>
#    if defined(HAVE_PTHREAD_SET_NAME_NP)
#        include <pthread_np.h>
#    endif
#endif

#if defined(__APPLE__)
#    include <mach/mach.h>
#    include <mach/thread_policy.h>
#endif

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Non-inlined thread functions from sim_threads.h:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

/* Create a new thread.
 *
 * Returns 0 on success, -1 (non-zero)on failure
 */
int sim_thread_create(sim_thread_t *thread_id, sim_thread_fn func, void *arg)
{
#if defined(HAVE_PTHREADS)
    pthread_attr_t attr;
    int retval;

    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    retval = pthread_create(thread_id, &attr, func, arg);
    pthread_attr_destroy(&attr);

    return (retval == 0) ? 0 : -1;
#endif
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Set a thread's name:
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#ifndef THREAD_NAME_MAX
#    define THREAD_NAME_MAX 64
#endif

void sim_set_thread_name(const char *name)
{
    if (name == NULL || *name == '\0')
        return;

#if defined(HAVE_SETTHREADDESCRIPTION)
    wchar_t wname[THREAD_NAME_MAX];
#    if defined(_MSC_VER)
    size_t converted;
    if (mbstowcs_s(&converted, wname, THREAD_NAME_MAX, name, _TRUNCATE) == 0) {
        SetThreadDescription(GetCurrentThread(), wname);
    }
#    else
    if (mbstowcs(wname, name, THREAD_NAME_MAX - 1) != (size_t)-1) {
        wname[THREAD_NAME_MAX - 1] = L'\0';
        SetThreadDescription(GetCurrentThread(), wname);
    }
#    endif

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

t_stat sim_os_get_process_affinity(sim_cpu_set_t *set)
{
    sim_cpu_set_zero(set);

#if defined(_WIN32)
    DWORD_PTR process_mask, system_mask;
    int cpu;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return SCPE_IOERR;
    for (cpu = 0; cpu < SIM_MAX_CPUS; cpu++)
        if (process_mask & ((DWORD_PTR)1 << cpu))
            sim_cpu_set_add(set, cpu);
    return SCPE_OK;

#elif defined(__linux__)
    cpu_set_t cpuset;
    int cpu;

    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0)
        return SCPE_IOERR;
    for (cpu = 0; cpu < SIM_MAX_CPUS; cpu++)
        if (CPU_ISSET(cpu, &cpuset))
            sim_cpu_set_add(set, cpu);
    return SCPE_OK;

#else
    /* No reliable "what am I actually restricted to" query here (macOS
     * has no equivalent -- Darwin doesn't expose or honor hard affinity
     * at all, per sim_os_set_thread_affinity() below). Fall back to the
     * naive full range. */
    {
        int cpu, ncpu = sim_os_get_cpu_count();
        for (cpu = 0; cpu < ncpu; cpu++)
            sim_cpu_set_add(set, cpu);
    }
    return SCPE_OK;
#endif
}

int sim_os_get_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 0;
#else
    return 0;
#endif
}

t_stat sim_os_set_thread_affinity(const sim_cpu_set_t *set)
{
    if (set == NULL || set->mask == 0)
        return SCPE_ARG;

#if defined(_WIN32)
    return (SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)set->mask) != 0) ? SCPE_OK : SCPE_IOERR;

#elif defined(__linux__)
    cpu_set_t cpuset;
    int cpu;
    CPU_ZERO(&cpuset);
    for (cpu = 0; cpu < SIM_MAX_CPUS; cpu++)
        if (set->mask & ((uint64_t)1 << cpu))
            CPU_SET(cpu, &cpuset);
    return (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) ? SCPE_OK : SCPE_IOERR;

#elif defined(__FreeBSD__)
    /* cpuset_t/CPU_SET here come from <pthread_np.h>, not <sched.h>. */
    cpuset_t cpuset;
    int cpu;
    CPU_ZERO(&cpuset);
    for (cpu = 0; cpu < SIM_MAX_CPUS; cpu++)
        if (set->mask & ((uint64_t)1 << cpu))
            CPU_SET(cpu, &cpuset);
    return (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) ? SCPE_OK : SCPE_IOERR;

#elif defined(__APPLE__)
    /* No hard pin on Darwin -- THREAD_AFFINITY_POLICY is an advisory
     * cache-locality hint only, and the P/E-core scheduler on Apple
     * Silicon ignores it regardless. Always report SCPE_NOFNC so
     * callers don't mistake this for a real guarantee. */
    thread_affinity_policy_data_t policy;
    int cpu;
    for (cpu = 0; cpu < SIM_MAX_CPUS && !(set->mask & ((uint64_t)1 << cpu)); cpu++)
        ;
    policy.affinity_tag = cpu + 1;
    thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, (thread_policy_t)&policy,
                      THREAD_AFFINITY_POLICY_COUNT);
    return SCPE_NOFNC;

#else
    (void)set;
    return SCPE_NOFNC;
#endif
}

/*=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * "Call once" support:
 *=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#if defined(_WIN32)
typedef INIT_ONCE sim_once_t;
#    define SIM_ONCE_INIT INIT_ONCE_STATIC_INIT

static BOOL CALLBACK sim_once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}

static inline void sim_call_once(sim_once_t *once, void (*fn)(void))
{
    InitOnceExecuteOnce(once, sim_once_trampoline, (PVOID)fn, NULL);
}

#elif defined(HAVE_PTHREADS)
typedef pthread_once_t sim_once_t;
#    define SIM_ONCE_INIT PTHREAD_ONCE_INIT

static inline void sim_call_once(sim_once_t *once, void (*fn)(void))
{
    pthread_once(once, fn);
}
#endif

/* Inner callback function for computing the CPU affinity partitions between the main and the I/O threads. */
static void sim_os_compute_cpu_partition(sim_cpu_set_t *main_set, sim_cpu_set_t *io_set, sim_cpu_set_t *sdl_set)
{
    sim_cpu_set_t allowed;
    int cpu, n = 0;
    int cpus[SIM_MAX_CPUS];

    sim_cpu_set_zero(sdl_set);
    sim_cpu_set_zero(main_set);
    sim_cpu_set_zero(io_set);

    if (sim_os_get_process_affinity(&allowed) != SCPE_OK)
        return;

    for (cpu = 0; cpu < SIM_MAX_CPUS; cpu++)
        if (allowed.mask & ((uint64_t)1 << cpu))
            cpus[n++] = cpu;

    if (n == 0)
        return;

#if defined(HAVE_LIBSDL)
    if (n >= 3) {
        sim_cpu_set_add(sdl_set, cpus[0]);
        sim_cpu_set_add(main_set, cpus[1]);
        for (cpu = 2; cpu < n; cpu++)
            sim_cpu_set_add(io_set, cpus[cpu]);
    } else if (n == 2) {
        /* Not enough CPUs to isolate all three. SDL shares with the
         * instruction loop rather than with I/O -- it's idle waiting
         * on the event queue almost all the time, closer in behavior
         * to reader/writer than to the interpreter, but the loop is
         * the one thing that must never be starved, so it wins the
         * dedicated slot and SDL is the one that doubles up. */
        sim_cpu_set_add(sdl_set, cpus[0]);
        sim_cpu_set_add(main_set, cpus[0]);
        sim_cpu_set_add(io_set, cpus[1]);
    } else {
        sim_cpu_set_add(sdl_set, cpus[0]);
        sim_cpu_set_add(main_set, cpus[0]);
        sim_cpu_set_add(io_set, cpus[0]);
    }
#else
    /* Headless build -- no SDL thread exists at all. sdl_set stays
     * empty; callers must check sim_cpu_set_empty() before using it. */
    sim_cpu_set_add(main_set, cpus[0]);
    for (cpu = 1; cpu < n; cpu++)
        sim_cpu_set_add(io_set, cpus[cpu]);
#endif
}

/* CPU affinity sets: g_main_cpu_set for the main thread's CPU affinity, g_io_cpu_set for the
 * I/O threads. */
static sim_cpu_set_t g_main_cpu_set;
static sim_cpu_set_t g_io_cpu_set;
static sim_cpu_set_t g_sdl_cpu_set;
static sim_once_t g_cpu_partition_once = SIM_ONCE_INIT;

static void compute_cpu_partition_once(void)
{
    sim_os_compute_cpu_partition(&g_main_cpu_set, &g_io_cpu_set, &g_sdl_cpu_set);
}

t_stat sim_os_get_cpu_partition(sim_cpu_set_t *main_set, sim_cpu_set_t *io_set, sim_cpu_set_t *sdl_set)
{
    sim_call_once(&g_cpu_partition_once, compute_cpu_partition_once);
    if (main_set != NULL)
        *main_set = g_main_cpu_set;
    if (io_set != NULL)
        *io_set = g_io_cpu_set;
    if (sdl_set != NULL)
        *sdl_set = g_sdl_cpu_set;

    return SCPE_OK;
}
