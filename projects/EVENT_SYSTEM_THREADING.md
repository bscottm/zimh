# Event System Thread Safety Design

## Context

This document supplements `EVENT_SYSTEM_REDESIGN.md` with detailed 
threading analysis and synchronization strategy for the non-intrusive 
event system.

## Current Async I/O Threading Model

From `sim_defs.h:419-434`, the current async system uses:

```c
#ifdef SIM_ASYNCH_IO
    void (*a_check_completion)(UNIT *);
    bool (*a_is_active)(UNIT *);
    UNIT *a_next;                /* next asynch active */
    int32_t a_event_time;
    ACTIVATE_API a_activate_call;
    
    /* Asynchronous Polling control */
    bool a_polling_now;          /* active flag */
    int32_t a_poll_waiter_count; /* waiting threads */
    
    /* Asynchronous Timer control */
    double a_due_time;           /* wall-clock event time */
    double a_due_gtime;          /* instruction event time */
    double a_usec_delay;         /* timer delay */
#endif
```

**Threading pattern:**
- **Main simulator thread:** Processes instruction queue (`sim_clock_queue`)
- **Async I/O threads:** Poll devices, insert timer events into 
  `sim_asynch_queue`
- **Coordination:** `sim_tmxr_poll_lock` mutex protects async queue

## MPSC Characteristics Analysis

Event insertion is **Multi-Producer, Single-Consumer:**

**Producers (multi-threaded):**
- Async I/O completion handlers
- Timer threads (calibrated device timers)
- TMXR polling threads (multiplexer units)
- External event injection (future: network sync)

**Consumer (single-threaded):**
- Main simulator instruction loop
- Pops minimum event, executes `uptr->action()`

**Key insight:** Consumer never runs concurrently with itself. Main 
simulator thread is single-threaded event processing loop.

## Synchronization Strategy Comparison

### Strategy 1: Simple Mutex (Recommended for Phase 1)

```c
typedef struct event_heap {
    UNIT_EVENT **events;
    size_t count;
    size_t capacity;
    uint64_t now;
    pthread_mutex_t lock;
} EVENT_HEAP;

t_stat sim_activate(UNIT *uptr, int32_t interval)
{
    pthread_mutex_lock(&sim_event_heap->lock);
    
    /* ... heap insertion ... */
    
    pthread_mutex_unlock(&sim_event_heap->lock);
    return SCPE_OK;
}

t_stat sim_process_event(void)
{
    pthread_mutex_lock(&sim_event_heap->lock);
    
    if (sim_event_heap->count == 0) {
        pthread_mutex_unlock(&sim_event_heap->lock);
        return SCPE_STOP;
    }
    
    UNIT_EVENT *evt = heap_pop_min(sim_event_heap);
    pthread_mutex_unlock(&sim_event_heap->lock);
    
    /* Execute action OUTSIDE lock */
    UNIT *uptr = evt->unit;
    uptr->event = NULL;
    free(evt);
    return uptr->action(uptr);
}
```

**Pros:**
- Simple, correct, testable
- Matches existing `sim_tmxr_poll_lock` pattern
- Held only during heap manipulation (not during `action()` callback)
- Sufficient for typical async I/O workloads

**Cons:**
- Serializes all insertions (not a problem unless >10k events/sec)
- No wait-free progress guarantee

**When this is sufficient:**
- Async I/O threads spend 99% of time in actual I/O, not event queueing
- Event insertion is O(log n) and fast
- Lock contention unlikely with <100 active async operations

### Strategy 2: Lock-Free MPSC Queue + Heap (Best Performance)

Inspired by `buf_ring.h` (already in repository):

```c
typedef struct incoming_event_node {
    UNIT_EVENT *event;
    _Atomic(struct incoming_event_node *) next;
} INCOMING_NODE;

typedef struct event_heap {
    /* Main heap (consumer-only, no locking) */
    UNIT_EVENT **events;
    size_t count;
    size_t capacity;
    uint64_t now;
    
    /* Lock-free incoming queue */
    _Atomic(INCOMING_NODE *) incoming_head;
    _Atomic(INCOMING_NODE *) incoming_tail;
} EVENT_HEAP;
```

**Producer path (lock-free):**
```c
t_stat sim_activate_async(UNIT *uptr, int32_t interval)
{
    UNIT_EVENT *evt = malloc(sizeof(UNIT_EVENT));
    evt->unit = uptr;
    evt->fire_time = __atomic_load_n(&sim_event_heap->now, __ATOMIC_ACQUIRE) 
                     + interval;
    
    INCOMING_NODE *node = malloc(sizeof(INCOMING_NODE));
    node->event = evt;
    
    /* Lock-free enqueue (Michael-Scott algorithm) */
    INCOMING_NODE *tail, *next;
    while (true) {
        tail = __atomic_load_n(&sim_event_heap->incoming_tail, 
                               __ATOMIC_ACQUIRE);
        next = __atomic_load_n(&tail->next, __ATOMIC_ACQUIRE);
        
        if (next == NULL) {
            if (__atomic_compare_exchange_n(&tail->next, &next, node,
                                           false, __ATOMIC_RELEASE, 
                                           __ATOMIC_ACQUIRE))
                break;
        } else {
            /* Help other thread complete */
            __atomic_compare_exchange_n(&sim_event_heap->incoming_tail,
                                       &tail, next,
                                       false, __ATOMIC_RELEASE, 
                                       __ATOMIC_ACQUIRE);
        }
    }
    
    /* Try to swing tail pointer */
    __atomic_compare_exchange_n(&sim_event_heap->incoming_tail, &tail, node,
                               false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    return SCPE_OK;
}
```

**Consumer path (drain queue into heap, no locks):**
```c
void drain_incoming_events(EVENT_HEAP *heap)
{
    INCOMING_NODE *head = __atomic_load_n(&heap->incoming_head, 
                                         __ATOMIC_ACQUIRE);
    INCOMING_NODE *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    
    while (next != NULL) {
        UNIT_EVENT *evt = next->event;
        
        /* Insert into heap (single-threaded, no locking) */
        heap_insert_internal(heap, evt);
        
        /* Advance head */
        __atomic_store_n(&heap->incoming_head, next, __ATOMIC_RELEASE);
        free(head);
        
        head = next;
        next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    }
}

t_stat sim_process_event(void)
{
    drain_incoming_events(sim_event_heap);  /* O(k log n) for k incoming */
    
    if (sim_event_heap->count == 0)
        return SCPE_STOP;
    
    UNIT_EVENT *evt = heap_pop_min(sim_event_heap);
    /* ... execute event ... */
}
```

**Pros:**
- Wait-free producer progress (async threads never block)
- Consumer still single-threaded (no heap locking)
- Scales to high async I/O rates

**Cons:**
- Complex, subtle to test
- Requires C11 atomics or compiler intrinsics
- Memory ordering discipline required

**When this is necessary:**
- Profiling shows mutex contention in Strategy 1
- >1000 async I/O operations/second
- Latency-sensitive async event injection

### Strategy 3: Per-Thread Event Buffers (Lowest Contention)

```c
typedef struct thread_event_buffer {
    UNIT_EVENT *events[256];  /* fixed-size ring */
    size_t head;              /* producer index */
    size_t tail;              /* consumer index */
    pthread_t owner_tid;
} THREAD_BUFFER;

/* Thread-local storage */
static __thread THREAD_BUFFER *my_event_buffer;

t_stat sim_activate_async(UNIT *uptr, int32_t interval)
{
    THREAD_BUFFER *buf = my_event_buffer;
    
    /* Fast path: thread-local buffer not full */
    size_t next_head = (buf->head + 1) % 256;
    if (next_head != buf->tail) {
        UNIT_EVENT *evt = &buf->events[buf->head];
        evt->unit = uptr;
        evt->fire_time = sim_event_heap->now + interval;
        buf->head = next_head;
        return SCPE_OK;
    }
    
    /* Slow path: buffer full, fall back to mutex */
    return sim_activate_with_lock(uptr, interval);
}

void merge_thread_buffers(EVENT_HEAP *heap)
{
    for (each registered thread buffer) {
        while (buffer not empty) {
            UNIT_EVENT evt = buffer_pop();
            heap_insert(heap, &evt);
        }
    }
}
```

**Pros:**
- Zero contention in common case
- Bounded memory per thread
- Batched insertion reduces heap operations

**Cons:**
- Most complex to implement
- Thread registration/deregistration overhead
- Unbounded insertion latency if buffer full

**When this is necessary:**
- >10,000 async events/second
- Many concurrent async I/O threads (>16)
- Never needed for typical SIMH workload

## Recommended Implementation Sequence

### Phase 1: Mutex-Based (Target: Current Development)

**Goal:** Correct, simple, maintainable

1. Single `pthread_mutex_t` protecting heap
2. Match current `sim_tmxr_poll_lock` pattern
3. Lock held only during heap manipulation, not callbacks
4. Measure contention with `pthread_mutex_trylock` failure rate

**Exit criteria:**
- All async I/O tests pass
- No deadlocks in 24-hour stress test
- Mutex contention <1% in typical workload

### Phase 2: Profile-Guided Optimization (Future)

**If** profiling shows mutex as bottleneck:

1. Instrument lock hold time and contention rate
2. If contention >5%, implement Strategy 2 (lock-free MPSC)
3. If contention >20%, consider Strategy 3 (per-thread buffers)

**Likely outcome:** Mutex is sufficient. Async I/O is infrequent relative 
to instruction execution.

## Platform Considerations

### POSIX (Linux, macOS, BSD)

```c
#include <pthread.h>

typedef struct event_heap {
    pthread_mutex_t lock;
    /* ... */
} EVENT_HEAP;

void heap_init(EVENT_HEAP *heap)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&heap->lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
```

### Windows

```c
#include <windows.h>

typedef struct event_heap {
    CRITICAL_SECTION lock;  /* faster than HANDLE mutex */
    /* ... */
} EVENT_HEAP;

void heap_init(EVENT_HEAP *heap)
{
    InitializeCriticalSection(&heap->lock);
    /* Optional: SetCriticalSectionSpinCount for low contention */
}
```

### Abstraction Layer

```c
/* sim_thread_lock.h */
#ifdef _WIN32
typedef CRITICAL_SECTION sim_lock_t;
#else
typedef pthread_mutex_t sim_lock_t;
#endif

void sim_lock_init(sim_lock_t *lock);
void sim_lock_destroy(sim_lock_t *lock);
void sim_lock_acquire(sim_lock_t *lock);
void sim_lock_release(sim_lock_t *lock);
```

## Testing Strategy

### Correctness Tests

1. **Single-threaded baseline:** Verify heap behavior without threads
2. **Two-thread ping-pong:** Producer and consumer alternate
3. **Many-producer stress:** 16 threads insert, 1 consumes
4. **Cancellation under contention:** Insert + cancel from different threads

### Race Detection

```bash
# Thread Sanitizer (Clang/GCC)
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
ctest --parallel 1  # TSan requires serial execution

# Helgrind (Valgrind)
valgrind --tool=helgrind ./test_event_threading
```

### Performance Benchmarks

```c
/* Measure lock contention */
void benchmark_contention(void)
{
    const int NUM_THREADS = 16;
    const int EVENTS_PER_THREAD = 10000;
    
    pthread_t threads[NUM_THREADS];
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, insert_events, 
                      &EVENTS_PER_THREAD);
    
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    double events_per_sec = (NUM_THREADS * EVENTS_PER_THREAD) / elapsed;
    
    printf("Throughput: %.0f events/sec\n", events_per_sec);
}
```

**Target:** >100,000 events/sec with mutex, >1M with lock-free

## Migration from Current Async System

Current async queue (`sim_asynch_queue`, `UNIT.a_next`):

```c
/* OLD: scp.c AIO_ACTIVATE macro */
#define AIO_ACTIVATE(proc, uptr, interval) \
    if (sim_asynch_enabled) { \
        /* Insert into sim_asynch_queue with sim_tmxr_poll_lock */ \
    }
```

**New unified approach:**

```c
/* NEW: Single event heap for both instruction and timer events */
t_stat sim_activate(UNIT *uptr, int32_t interval)
{
    if (uptr->dynflags & UNIT_TMR_UNIT)
        return sim_activate_timer(uptr, interval);  /* wall-clock */
    else
        return sim_activate_instruction(uptr, interval);  /* cycles */
}

/* Both use same heap with different fire_time calculation */
```

**Backward compatibility:**
1. Keep `AIO_ACTIVATE` macro as thin wrapper
2. Route to new heap-based insertion
3. Remove `sim_asynch_queue` and `UNIT.a_next` in Phase 3

## Open Questions for User

1. **Contention baseline:** What is current `sim_tmxr_poll_lock` hold time?
2. **Async workload:** How many concurrent async I/O operations in typical 
   VAX config?
3. **Platform priority:** Windows, Linux, or both for initial implementation?
4. **Testing resources:** Can we run 24-hour stress tests with async I/O?

## References

- `src/include/buf_ring.h`: Lock-free MPSC ring buffer example
- M. Michael and M. Scott, "Simple, Fast, and Practical Non-Blocking and 
  Blocking Concurrent Queue Algorithms," PODC 1996
- A. Williams, "C++ Concurrency in Action," 2nd ed., Ch. 7
- POSIX `pthread_mutex_*` specification
- Windows `CRITICAL_SECTION` documentation
