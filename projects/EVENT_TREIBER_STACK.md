# Event System: Treiber Stack for Cross-Thread Coordination

## Overview

Alternative to mutex-based free list: use a lock-free Treiber stack for
producers to push events, with the consumer (main simulator thread) atomically
draining the entire stack and bulk-inserting into the min-heap.

## Motivation

**MPSC characteristics:**
- **Producers:** Multiple async I/O threads insert events concurrently
- **Consumer:** Single main simulator thread processes events sequentially

**Treiber stack advantages:**
- Lock-free producer push (no contention)
- Atomic drain operation (consumer takes entire stack in one CAS)
- Natural LIFO ordering for stack, but...
- Requires sequence numbers to restore time ordering

## Design

### UNIT_EVENT Structure Enhancement

```c
struct unit_event {
    UNIT *unit;              /* backpointer to unit */
    uint64_t fire_time;      /* absolute instruction time */
    double fire_wallclock;   /* wall-clock time for timers */
    uint32_t heap_index;     /* position in heap (when scheduled) */
    bool is_timer_event;     /* instruction vs wall-clock */
    
    /* Treiber stack support for cross-thread coordination */
    UNIT_EVENT *next_incoming;   /* link for incoming stack */
    uint64_t sequence;           /* arrival order for time-tie breaking */
};
```

**Additions:**
- `next_incoming` - Links events in the Treiber stack
- `sequence` - Global monotonic counter ensures FIFO within same fire_time

### EVENT_MANAGER Structure

```c
typedef struct event_manager {
    /* Min-heap (consumer-only access, no locking) */
    UNIT_EVENT **heap_events;
    size_t heap_count;
    size_t heap_capacity;
    
    /* Free list (consumer-only access after drain) */
    EVENT_FREELIST_NODE *free_head;
    size_t free_count;
    
    /* Treiber stack for incoming events (lock-free MPSC) */
    sim_atomic_value_t incoming_head;    /* atomic pointer to stack top */
    sim_atomic_value_t sequence_counter; /* monotonic arrival order */
    
    /* State (consumer-only) */
    uint64_t now;
    
    /* Statistics */
    sim_atomic_value_t total_allocs_atomic;
    sim_atomic_value_t total_frees_atomic;
    sim_atomic_value_t incoming_pushes;  /* producer push count */
    sim_atomic_value_t incoming_drains;  /* consumer drain count */
    
    /* No mutex needed! Lock-free design */
} EVENT_MANAGER;
```

**Key changes:**
- `incoming_head` - Atomic pointer to top of Treiber stack
- `sequence_counter` - Atomic counter for arrival ordering
- **No mutex** - Producers never block each other or consumer

### Producer: Lock-Free Push

```c
/* Async I/O thread pushes event onto incoming stack */
t_stat sim_event_push_incoming(UNIT_EVENT *evt)
{
    if (sim_event_manager == NULL)
        return SCPE_IERR;
    
    /* Assign sequence number for arrival order */
    evt->sequence = sim_atomic_inc(&sim_event_manager->sequence_counter);
    
    /* Treiber stack push: retry until CAS succeeds */
    UNIT_EVENT *old_head;
    do {
        old_head = (UNIT_EVENT *)sim_atomic_get(&sim_event_manager->incoming_head);
        evt->next_incoming = old_head;
    } while (!sim_atomic_cas_ptr(&sim_event_manager->incoming_head,
                                 (void **)&old_head,
                                 evt));
    
    sim_atomic_inc(&sim_event_manager->incoming_pushes);
    return SCPE_OK;
}
```

**Properties:**
- **Wait-free** - Always makes progress (no loops on contention)
- **No locks** - Uses only atomic CAS
- **Sequence assignment** - Ensures FIFO within same fire_time

### Consumer: Atomic Drain and Bulk Insert

```c
/* Main simulator thread drains incoming stack and inserts into heap */
static void drain_incoming_events(void)
{
    if (sim_event_manager == NULL)
        return;
    
    /* Atomically steal entire incoming stack */
    UNIT_EVENT *incoming = (UNIT_EVENT *)sim_atomic_exchange(
        &sim_event_manager->incoming_head, (sim_atomic_type_t)NULL);
    
    if (incoming == NULL)
        return;  /* nothing to drain */
    
    /* Count events and reverse list (Treiber stack is LIFO) */
    size_t count = 0;
    UNIT_EVENT *prev = NULL;
    UNIT_EVENT *curr = incoming;
    while (curr != NULL) {
        UNIT_EVENT *next = curr->next_incoming;
        curr->next_incoming = prev;
        prev = curr;
        curr = next;
        count++;
    }
    incoming = prev;  /* now FIFO order by push time */
    
    /* Bulk insert into heap */
    for (UNIT_EVENT *evt = incoming; evt != NULL; evt = evt->next_incoming) {
        heap_insert_internal(sim_event_manager, evt);
    }
    
    sim_atomic_add(&sim_event_manager->incoming_drains, count);
}

/* Call before processing next event */
t_stat sim_process_event(void)
{
    drain_incoming_events();  /* Pull in any new events */
    
    if (sim_event_manager->heap_count == 0)
        return SCPE_STOP;
    
    UNIT_EVENT *evt = heap_pop_min(sim_event_manager);
    /* ... execute event ... */
}
```

**Properties:**
- **Single atomic operation** - `atomic_exchange` captures entire stack
- **No malloc** - Events already allocated by producers
- **Bulk insertion** - Amortizes heap fixup cost
- **Reversed to FIFO** - Restores push order before insertion

### Event Ordering

**Within same fire_time, use sequence number:**

```c
static void heap_sift_up(EVENT_HEAP *heap, size_t index)
{
    while (index > 0) {
        size_t parent = heap_parent(index);
        
        UNIT_EVENT *child = heap->events[index];
        UNIT_EVENT *parent_evt = heap->events[parent];
        
        /* Compare: fire_time first, sequence second */
        bool child_fires_earlier = 
            (child->fire_time < parent_evt->fire_time) ||
            (child->fire_time == parent_evt->fire_time && 
             child->sequence < parent_evt->sequence);
        
        if (!child_fires_earlier)
            break;
        
        /* Swap and continue */
        heap->events[index] = parent_evt;
        heap->events[parent] = child;
        child->heap_index = parent;
        parent_evt->heap_index = index;
        
        index = parent;
    }
}
```

**Ordering guarantee:**
- Events fire in `fire_time` order (primary key)
- Events with same `fire_time` fire in `sequence` order (tie-breaker)
- **Result:** FIFO scheduling for simultaneous events

## Comparison: Treiber Stack vs Mutex

### Treiber Stack Approach

**Advantages:**
- **Lock-free producers** - No contention between async threads
- **Single atomic drain** - Consumer takes entire stack at once
- **Scalable** - Handles 100+ concurrent producers efficiently
- **No priority inversion** - Producers never wait

**Disadvantages:**
- **Sequence counter overhead** - Extra atomic increment per push
- **Reversal cost** - Must reverse LIFO stack to FIFO list
- **Memory ordering complexity** - Requires careful atomic usage
- **Slightly larger UNIT_EVENT** - +16 bytes (next_incoming + sequence)

**Best for:**
- High async I/O rate (>1000 events/sec)
- Many concurrent producers (>4 threads)
- Bursty network traffic

### Mutex Approach

**Advantages:**
- **Simpler** - Easier to reason about and debug
- **Smaller memory** - No sequence or next_incoming fields
- **Direct insertion** - Can insert into heap immediately (no staging)

**Disadvantages:**
- **Serialization** - All producers contend on one lock
- **Priority inversion risk** - Producer can block consumer
- **Doesn't scale** - Bottleneck with many producers

**Best for:**
- Moderate async I/O rate (<100 events/sec)
- Few concurrent producers (<4 threads)
- Simplicity and maintainability priority

## Hybrid Approach: Per-Core Treiber Stacks

For extreme scalability (Phase 4):

```c
#define MAX_CORES 128

typedef struct event_manager {
    /* ... heap and free list ... */
    
    /* One Treiber stack per CPU core */
    sim_atomic_value_t incoming_heads[MAX_CORES];
    sim_atomic_value_t sequence_counters[MAX_CORES];
    
    /* Thread-local core ID cache */
    __thread int my_core_id;
} EVENT_MANAGER;

/* Producer uses core-local stack (minimal contention) */
t_stat sim_event_push_incoming(UNIT_EVENT *evt)
{
    int core = my_core_id;  /* thread-local, no syscall */
    evt->sequence = sim_atomic_inc(&sim_event_manager->sequence_counters[core]);
    
    /* Push to this core's stack */
    UNIT_EVENT *old_head;
    do {
        old_head = (UNIT_EVENT *)sim_atomic_get(&sim_event_manager->incoming_heads[core]);
        evt->next_incoming = old_head;
    } while (!sim_atomic_cas_ptr(&sim_event_manager->incoming_heads[core],
                                 (void **)&old_head, evt));
    return SCPE_OK;
}

/* Consumer drains all core stacks */
static void drain_all_incoming_events(void)
{
    for (int core = 0; core < num_cores; core++) {
        UNIT_EVENT *incoming = (UNIT_EVENT *)sim_atomic_exchange(
            &sim_event_manager->incoming_heads[core], NULL);
        /* ... reverse and insert ... */
    }
}
```

**Benefits:**
- **Minimal contention** - Threads on same core share stack, others don't contend
- **Cache-line friendly** - Each core's stack in separate cache line
- **Scalable to 100+ cores**

**Use only if profiling shows contention on single Treiber stack.**

## Atomic Pointer Support in sim_atomic.h

**Need to add pointer-width atomics:**

```c
/* Add to sim_atomic.h */

typedef struct sim_atomic_ptr_s {
    SIM_ATOMIC_TYPE(void *) ptr;
} sim_atomic_ptr_t;

static FORCE_INLINE void *sim_atomic_get_ptr(const sim_atomic_ptr_t *p)
{
#if HAVE_STD_ATOMIC
    return atomic_load_explicit(&p->ptr, SIM_ATOMIC_SEQ_CST);
#elif defined(__ATOMIC_SEQ_CST)
    void *result;
    __atomic_load(&p->ptr, &result, __ATOMIC_SEQ_CST);
    return result;
#elif defined(_WIN32) || defined(_WIN64)
    return (void *)InterlockedOr64((LONG64 volatile *)&p->ptr, 0);
#endif
}

static FORCE_INLINE bool sim_atomic_cas_ptr(sim_atomic_ptr_t *p,
                                            void **expected, void *desired)
{
#if HAVE_STD_ATOMIC
    return atomic_compare_exchange_strong(&p->ptr, expected, desired);
#elif defined(__ATOMIC_SEQ_CST)
    return __atomic_compare_exchange(&p->ptr, expected, &desired, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#elif defined(_WIN32) || defined(_WIN64)
    void *old = InterlockedCompareExchangePointer(&p->ptr, desired, *expected);
    if (old == *expected)
        return true;
    *expected = old;
    return false;
#endif
}

static FORCE_INLINE void *sim_atomic_exchange_ptr(sim_atomic_ptr_t *p, void *newval)
{
#if HAVE_STD_ATOMIC
    return atomic_exchange(&p->ptr, newval);
#elif defined(__ATOMIC_SEQ_CST)
    return __atomic_exchange_n(&p->ptr, newval, __ATOMIC_SEQ_CST);
#elif defined(_WIN32) || defined(_WIN64)
    return InterlockedExchangePointer(&p->ptr, newval);
#endif
}
```

**Then use in EVENT_MANAGER:**

```c
typedef struct event_manager {
    /* ... */
    sim_atomic_ptr_t incoming_head;  /* atomic UNIT_EVENT * */
    /* ... */
} EVENT_MANAGER;
```

## Implementation Roadmap

### Phase 1: Mutex (Current)
- Simple, correct, testable
- Profile to measure contention

### Phase 2: Treiber Stack (If Needed)
**Trigger:** Mutex contention >5% in async I/O profiling

1. Add `next_incoming` and `sequence` to UNIT_EVENT
2. Add `sim_atomic_ptr_t` support to sim_atomic.h
3. Implement `sim_event_push_incoming()` with Treiber stack
4. Implement `drain_incoming_events()` with atomic exchange
5. Test with 16+ concurrent producers
6. Verify event ordering with sequence numbers

### Phase 3: Per-Core Stacks (If Needed)
**Trigger:** Single Treiber stack contention >10%

Unlikely to be needed for typical SIMH workloads.

## Testing Strategy

### Unit Tests

```c
/* Test: Treiber stack maintains FIFO within same fire_time */
static void test_treiber_stack_fifo_order(void **state)
{
    UNIT_EVENT events[10];
    
    /* Push 10 events with same fire_time from different threads */
    #pragma omp parallel for
    for (int i = 0; i < 10; i++) {
        events[i].fire_time = 1000;  /* all same */
        sim_event_push_incoming(&events[i]);
    }
    
    /* Drain and verify sequence order */
    drain_incoming_events();
    
    /* Pop events - should be sorted by sequence */
    uint64_t last_seq = 0;
    for (int i = 0; i < 10; i++) {
        UNIT_EVENT *evt = sim_event_heap_pop();
        assert_true(evt->sequence > last_seq);
        last_seq = evt->sequence;
    }
}

/* Test: Concurrent push stress */
static void *pusher_thread(void *arg)
{
    for (int i = 0; i < 1000; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        evt->fire_time = rand() % 10000;
        sim_event_push_incoming(evt);
    }
    return NULL;
}

static void test_treiber_stack_concurrent_push(void **state)
{
    pthread_t threads[16];
    
    for (int i = 0; i < 16; i++)
        pthread_create(&threads[i], NULL, pusher_thread, NULL);
    
    for (int i = 0; i < 16; i++)
        pthread_join(threads[i], NULL);
    
    /* Drain and verify heap property */
    drain_incoming_events();
    assert_true(sim_event_heap_validate());
}
```

### Performance Benchmark

```c
void benchmark_treiber_vs_mutex(void)
{
    const int NUM_THREADS = 16;
    const int EVENTS_PER_THREAD = 10000;
    
    /* Benchmark Treiber stack */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* ... spawn threads doing sim_event_push_incoming() ... */
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double treiber_time = timespec_diff(&start, &end);
    
    /* Benchmark mutex */
    /* ... same test with mutex version ... */
    double mutex_time = ...;
    
    printf("Throughput comparison (16 threads, 10K events each):\n");
    printf("  Treiber stack: %.1f M events/sec\n", 
           (NUM_THREADS * EVENTS_PER_THREAD) / treiber_time / 1e6);
    printf("  Mutex:         %.1f M events/sec\n",
           (NUM_THREADS * EVENTS_PER_THREAD) / mutex_time / 1e6);
    printf("  Speedup:       %.1fx\n", mutex_time / treiber_time);
}
```

**Expected:** 2-5× speedup with Treiber stack at 16 threads.

## Recommendation

**Phase 1:** Start with mutex (simpler, correct)

**Phase 2:** Profile async I/O workload:
- If mutex contention <5%: Keep mutex (good enough)
- If mutex contention >5%: Implement Treiber stack

**Phase 3:** Only if Treiber stack contention >10%: Per-core stacks

**Most likely outcome:** Mutex is sufficient. Async I/O is infrequent relative
to instruction execution, so even with bursts, the mutex won't be a bottleneck.

**Defer Treiber stack until profiling proves it's needed.** The complexity
(sequence numbers, reversal, pointer atomics) isn't justified without data.
