# Event Memory Management: Free List Design

## Motivation

Event allocation/deallocation can become a hot path under bursty workloads:

**Scenarios:**
- Network packet bursts (10-1000+ events in rapid succession)
- Disk array I/O (multiple drives completing simultaneously)
- Terminal multiplexer polling (tmxr devices with many lines)
- Timer expiry clusters (multiple devices with similar timeouts)

**Problem with malloc/free:**
- Each event insertion: `malloc(sizeof(UNIT_EVENT))` → heap allocation
- Each event completion: `free(event)` → heap deallocation
- Bursty traffic = frequent malloc/free = contention + fragmentation

**Profile data needed, but likely:**
- Typical VAX config: 10-100 events/second baseline
- Network burst: 1000+ events/second for 1-10 seconds
- Disk completion burst: 10-50 simultaneous completions

## Design: Unified Event Manager Structure

**Key insight:** Yes, you need an administrative wrapper! The free list, heap,
and global state should be unified in a single manager structure rather than
scattered globals.

### Unified Structure

```c
/* Event manager - wraps heap + free list + state */
typedef struct event_manager {
    /* Min-heap for scheduled events */
    UNIT_EVENT **heap_events;   /* array of event pointers */
    size_t heap_count;          /* events in heap */
    size_t heap_capacity;       /* allocated heap capacity */
    
    /* Free list for recycled events */
    EVENT_FREELIST_NODE *free_head;  /* head of free list */
    size_t free_count;               /* events in free list */
    size_t free_high_water;          /* peak free list size */
    
    /* Global state */
    uint64_t now;               /* current instruction time */
    
    /* Statistics - atomic for lock-free slow path updates */
    uint64_t heap_inserts;      /* total insertions (protected by lock) */
    uint64_t heap_removes;      /* total removals (protected by lock) */
    sim_atomic_value_t total_allocs_atomic;   /* atomic counter */
    sim_atomic_value_t total_frees_atomic;    /* atomic counter */
    sim_atomic_value_t malloc_slow_atomic;    /* atomic counter */
    
    /* Thread safety (Phase 1: simple mutex; Phase 4: lock-free) */
    pthread_mutex_t lock;       /* protects entire structure */
} EVENT_MANAGER;

/* Free list node - embeds full event structure */
typedef struct event_freelist_node {
    UNIT_EVENT event;                   /* actual event (32 bytes) */
    struct event_freelist_node *next;   /* free list link (8 bytes) */
} EVENT_FREELIST_NODE;

/* Global singleton */
static EVENT_MANAGER *sim_event_manager = NULL;

/* Configuration */
#define EVENT_FREELIST_MAX 256     /* cap free list size */
#define EVENT_FREELIST_PREALLOC 32 /* pre-allocate at init */
#define INITIAL_HEAP_CAPACITY 128  /* initial heap size */
```

**Key changes from scattered globals:**
1. **sim_atomic_value_t** - Uses portable atomics from `sim_atomic.h` instead of `__sync_*` intrinsics
2. **Lazy initialization** - No public `sim_event_heap_init()`, manager created on first `sim_event_alloc()`
3. **Atomic counters** - `malloc_slow`, `total_allocs`, `total_frees` use `sim_atomic_add()` for lock-free updates in slow path

**Benefits:**
1. **Single allocation point** - One `malloc(sizeof(EVENT_MANAGER))` at init
2. **Locality** - Heap metadata + free list metadata in same cache line
3. **Single lock** - One mutex protects both heap and free list
4. **Easy statistics** - All counters in one place
5. **Clean API** - Pass `EVENT_MANAGER *` instead of scattered globals

### Allocation (Unified)

```c
UNIT_EVENT *sim_event_alloc(void)
{
    if (sim_event_manager == NULL) {
        /* Lazy initialization on first use */
        sim_event_manager_init();
        if (sim_event_manager == NULL)
            return NULL;
    }
    
    pthread_mutex_lock(&sim_event_manager->lock);
    
    /* Fast path: free list has events */
    if (sim_event_manager->free_head != NULL) {
        EVENT_FREELIST_NODE *node = sim_event_manager->free_head;
        sim_event_manager->free_head = node->next;
        sim_event_manager->free_count--;
        sim_event_manager->total_allocs++;
        
        pthread_mutex_unlock(&sim_event_manager->lock);
        
        /* Zero event structure (don't leak stale data) */
        memset(&node->event, 0, sizeof(UNIT_EVENT));
        return &node->event;
    }
    
    /* Slow path: allocate new node */
    pthread_mutex_unlock(&sim_event_manager->lock);
    
    EVENT_FREELIST_NODE *node = malloc(sizeof(EVENT_FREELIST_NODE));
    if (node == NULL)
        return NULL;
    
    /* Use portable atomic add from sim_atomic.h */
    sim_atomic_add(&sim_event_manager->malloc_slow_atomic, 1);
    sim_atomic_add(&sim_event_manager->total_allocs_atomic, 1);
    
    memset(&node->event, 0, sizeof(UNIT_EVENT));
    return &node->event;
}
```

**Note:** Uses `sim_atomic_add()` from `src/include/sim_atomic.h` for
portable cross-platform atomic operations instead of raw `__sync_fetch_and_add`.

### Deallocation (Unified)

```c
void sim_event_free(UNIT_EVENT *event)
{
    if (event == NULL || sim_event_manager == NULL)
        return;
    
    /* Get containing node (container_of pattern) */
    EVENT_FREELIST_NODE *node = 
        (EVENT_FREELIST_NODE *)((char *)event - 
                                offsetof(EVENT_FREELIST_NODE, event));
    
    pthread_mutex_lock(&sim_event_manager->lock);
    
    /* Cap free list to avoid unbounded growth */
    if (sim_event_manager->free_count >= EVENT_FREELIST_MAX) {
        pthread_mutex_unlock(&sim_event_manager->lock);
        free(node);
        return;
    }
    
    /* Return to free list */
    node->next = sim_event_manager->free_head;
    sim_event_manager->free_head = node;
    sim_event_manager->free_count++;
    sim_event_manager->total_frees++;
    
    if (sim_event_manager->free_count > sim_event_manager->free_high_water)
        sim_event_manager->free_high_water = sim_event_manager->free_count;
    
    pthread_mutex_unlock(&sim_event_manager->lock);
}
```

### Initialization

```c
static t_stat sim_event_manager_init(void)
{
    if (sim_event_manager != NULL)
        return SCPE_OK;  /* already initialized */
    
    /* Allocate manager structure */
    sim_event_manager = calloc(1, sizeof(EVENT_MANAGER));
    if (sim_event_manager == NULL)
        return SCPE_MEM;
    
    /* Initialize mutex */
    pthread_mutex_init(&sim_event_manager->lock, NULL);
    
    /* Initialize atomic counters */
    sim_atomic_init(&sim_event_manager->total_allocs_atomic);
    sim_atomic_init(&sim_event_manager->total_frees_atomic);
    sim_atomic_init(&sim_event_manager->malloc_slow_atomic);
    
    /* Allocate heap array */
    sim_event_manager->heap_events = 
        calloc(INITIAL_HEAP_CAPACITY, sizeof(UNIT_EVENT *));
    if (sim_event_manager->heap_events == NULL) {
        pthread_mutex_destroy(&sim_event_manager->lock);
        free(sim_event_manager);
        sim_event_manager = NULL;
        return SCPE_MEM;
    }
    
    sim_event_manager->heap_capacity = INITIAL_HEAP_CAPACITY;
    sim_event_manager->heap_count = 0;
    sim_event_manager->now = 0;
    
    /* Pre-allocate free list */
    for (size_t i = 0; i < EVENT_FREELIST_PREALLOC; i++) {
        EVENT_FREELIST_NODE *node = malloc(sizeof(EVENT_FREELIST_NODE));
        if (node == NULL)
            break;  /* partial pre-alloc okay */
        
        node->next = sim_event_manager->free_head;
        sim_event_manager->free_head = node;
        sim_event_manager->free_count++;
    }
    
    return SCPE_OK;
}

/* Note: No public sim_event_heap_init() - initialization happens
   automatically on first sim_event_alloc() call */
```

### Cleanup (Unified)

```c
void sim_event_manager_cleanup(void)
{
    if (sim_event_manager == NULL)
        return;
    
    pthread_mutex_lock(&sim_event_manager->lock);
    
    /* Free all pending events in heap */
    for (size_t i = 0; i < sim_event_manager->heap_count; i++) {
        if (sim_event_manager->heap_events[i] != NULL) {
            EVENT_FREELIST_NODE *node = 
                (EVENT_FREELIST_NODE *)((char *)sim_event_manager->heap_events[i] - 
                                        offsetof(EVENT_FREELIST_NODE, event));
            free(node);
        }
    }
    
    /* Free heap array */
    free(sim_event_manager->heap_events);
    
    /* Free all nodes in free list */
    EVENT_FREELIST_NODE *node = sim_event_manager->free_head;
    while (node != NULL) {
        EVENT_FREELIST_NODE *next = node->next;
        free(node);
        node = next;
    }
    
    /* Destroy atomic counters */
    sim_atomic_destroy(&sim_event_manager->total_allocs_atomic);
    sim_atomic_destroy(&sim_event_manager->total_frees_atomic);
    sim_atomic_destroy(&sim_event_manager->malloc_slow_atomic);
    
    pthread_mutex_unlock(&sim_event_manager->lock);
    pthread_mutex_destroy(&sim_event_manager->lock);
    
    free(sim_event_manager);
    sim_event_manager = NULL;
}

/* Public API wrapper */
void sim_event_heap_cleanup(void)
{
    sim_event_manager_cleanup();
}
```

## Alternative: Lock-Free Free List (Phase 4)

For higher async I/O rates, use lock-free stack:

```c
typedef struct {
    _Atomic(EVENT_FREELIST_NODE *) head;
    _Atomic(size_t) count;  /* approximate, may lag */
} LOCKFREE_FREELIST;

UNIT_EVENT *sim_event_alloc_lockfree(void)
{
    EVENT_FREELIST_NODE *node, *next;
    
    do {
        node = __atomic_load_n(&global_freelist.head, __ATOMIC_ACQUIRE);
        if (node == NULL)
            goto slow_path;  /* empty, allocate */
        
        next = node->next_free;
    } while (!__atomic_compare_exchange_n(&global_freelist.head, 
                                         &node, next,
                                         false, 
                                         __ATOMIC_RELEASE, 
                                         __ATOMIC_ACQUIRE));
    
    __atomic_fetch_sub(&global_freelist.count, 1, __ATOMIC_RELAXED);
    
    memset(&node->event, 0, sizeof(UNIT_EVENT));
    return &node->event;

slow_path:
    node = malloc(sizeof(EVENT_FREELIST_NODE));
    if (node != NULL)
        memset(&node->event, 0, sizeof(UNIT_EVENT));
    return node ? &node->event : NULL;
}
```

**Use lock-free only if profiling shows mutex contention.**

## API Changes

Replace direct malloc/free in event code:

```c
/* Old (Phase 1): */
UNIT_EVENT *evt = malloc(sizeof(UNIT_EVENT));
// ...
free(evt);

/* New (Phase 1b): */
UNIT_EVENT *evt = sim_event_alloc();
// ...
sim_event_free(evt);
```

**External API unchanged** - allocation is internal to event system.

## Performance Benefits

### Malloc Overhead Eliminated

**Without free list:**
```
Event burst (100 events):
  malloc() × 100 = ~5,000 cycles each = 500,000 cycles
  free() × 100 = ~3,000 cycles each = 300,000 cycles
  Total: 800,000 cycles (~0.4ms @ 2GHz)
```

**With free list:**
```
Event burst (100 events):
  mutex + pointer update × 100 = ~100 cycles each = 10,000 cycles
  Total: 10,000 cycles (~5μs @ 2GHz)
  
  80× faster for bursts after warmup
```

### Reduced Fragmentation

Repeated malloc/free of same-size objects fragments the heap. Free list
keeps UNIT_EVENT allocations separate from other malloc traffic.

### Cache Friendliness

Reusing recently-freed events improves cache hit rate vs. cold malloc pages.

## Configuration Tuning

### Adjust Based on Workload

```c
/* Conservative (minimal memory) */
#define EVENT_FREELIST_MAX 64
#define EVENT_FREELIST_PREALLOC 16

/* Aggressive (network-heavy) */
#define EVENT_FREELIST_MAX 512
#define EVENT_FREELIST_PREALLOC 128

/* Auto-tune based on high_water mark */
void sim_event_freelist_tune(void)
{
    /* After 1 minute of runtime, adjust prealloc */
    size_t target = global_freelist.high_water * 1.5;
    if (target < EVENT_FREELIST_PREALLOC)
        return;  /* current prealloc is fine */
    
    /* Grow to target (capped at MAX) */
    while (global_freelist.count < target && 
           global_freelist.count < EVENT_FREELIST_MAX) {
        EVENT_FREELIST_NODE *node = malloc(sizeof(EVENT_FREELIST_NODE));
        if (node == NULL)
            break;
        sim_event_free(&node->event);
    }
}
```

### Statistics Command (Using Manager)

```c
t_stat sim_show_event_stats(FILE *st, DEVICE *dptr, UNIT *uptr, 
                           int32_t flag, const char *cptr)
{
    if (sim_event_manager == NULL) {
        fprintf(st, "Event manager not initialized\n");
        return SCPE_OK;
    }
    
    pthread_mutex_lock(&sim_event_manager->lock);
    
    /* Read atomic counters */
    uint64_t total_allocs = sim_atomic_get(&sim_event_manager->total_allocs_atomic);
    uint64_t total_frees = sim_atomic_get(&sim_event_manager->total_frees_atomic);
    uint64_t malloc_slow = sim_atomic_get(&sim_event_manager->malloc_slow_atomic);
    
    fprintf(st, "Event system statistics:\n");
    fprintf(st, "  Heap:\n");
    fprintf(st, "    Active events:    %zu\n", sim_event_manager->heap_count);
    fprintf(st, "    Capacity:         %zu\n", sim_event_manager->heap_capacity);
    fprintf(st, "    Total insertions: %lu\n", sim_event_manager->heap_inserts);
    fprintf(st, "    Total removals:   %lu\n", sim_event_manager->heap_removes);
    fprintf(st, "\n");
    fprintf(st, "  Free list:\n");
    fprintf(st, "    Available:        %zu\n", sim_event_manager->free_count);
    fprintf(st, "    High water mark:  %zu\n", sim_event_manager->free_high_water);
    fprintf(st, "\n");
    fprintf(st, "  Allocation:\n");
    fprintf(st, "    Total allocs:     %lu\n", total_allocs);
    fprintf(st, "    Total frees:      %lu\n", total_frees);
    fprintf(st, "    Malloc fallbacks: %lu", malloc_slow);
    
    if (total_allocs > 0) {
        double pct = 100.0 * malloc_slow / total_allocs;
        fprintf(st, " (%.1f%%)\n", pct);
        
        if (pct > 10.0)
            fprintf(st, "\n  WARNING: >10%% malloc fallbacks - "
                       "increase EVENT_FREELIST_MAX\n");
    } else {
        fprintf(st, "\n");
    }
    
    pthread_mutex_unlock(&sim_event_manager->lock);
    
    return SCPE_OK;
}
```

**Note:** Uses `sim_atomic_get()` from `sim_atomic.h` to safely read
atomic counters that may be updated concurrently by slow path allocation.

Expose via: `SHOW EVENT STATISTICS`

## Architectural Benefits of Wrapper Structure

### Single Point of Control

**Without wrapper (scattered globals):**
```c
static EVENT_HEAP *sim_event_heap = NULL;
static EVENT_FREELIST global_freelist;
static pthread_mutex_t freelist_lock;
static pthread_mutex_t heap_lock;
static uint64_t event_now;
```

**With wrapper (unified):**
```c
static EVENT_MANAGER *sim_event_manager = NULL;
```

**Benefits:**
1. **One malloc/free at init/cleanup** - simpler lifecycle
2. **One mutex** - less contention, simpler locking
3. **Atomic swap** - can replace entire manager (useful for testing)
4. **Cache locality** - related data in same structure

### Memory Layout Optimization

```c
/* Typical cache line: 64 bytes */
struct event_manager {
    /* Hot path: accessed on every alloc/free/insert/remove */
    UNIT_EVENT **heap_events;        /* 8 bytes - HOT */
    size_t heap_count;               /* 8 bytes - HOT */
    EVENT_FREELIST_NODE *free_head;  /* 8 bytes - HOT */
    size_t free_count;               /* 8 bytes - HOT */
    uint64_t now;                    /* 8 bytes - HOT */
    /* = 40 bytes, fits in single cache line with padding */
    
    /* Cold path: accessed rarely */
    size_t heap_capacity;            /* 8 bytes - COLD */
    size_t free_high_water;          /* 8 bytes - COLD */
    uint64_t total_allocs;           /* 8 bytes - COLD */
    /* ... other stats ... */
    /* = stays in separate cache line */
};
```

**Result:** Hot path fields fit in one 64-byte cache line. Cold statistics
don't pollute cache during normal operations.

### Testing Isolation

**With wrapper, tests can create private managers:**

```c
/* Test: stress test without affecting global state */
static void test_event_manager_stress(void **state)
{
    /* Create private manager for this test */
    EVENT_MANAGER *test_mgr = calloc(1, sizeof(EVENT_MANAGER));
    test_mgr->heap_events = calloc(128, sizeof(UNIT_EVENT *));
    test_mgr->heap_capacity = 128;
    pthread_mutex_init(&test_mgr->lock, NULL);
    
    /* Swap in test manager */
    EVENT_MANAGER *saved = sim_event_manager;
    sim_event_manager = test_mgr;
    
    /* Run test... */
    for (int i = 0; i < 10000; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        sim_event_free(evt);
    }
    
    /* Restore global manager */
    sim_event_manager = saved;
    
    /* Clean up test manager */
    free(test_mgr->heap_events);
    pthread_mutex_destroy(&test_mgr->lock);
    free(test_mgr);
}
```

**Without wrapper:** Tests affect global state, can't run in parallel.

### Future: Per-Core Managers (Phase 4+)

```c
/* Lock-free scaling: one manager per CPU core */
static __thread EVENT_MANAGER *thread_local_manager = NULL;

UNIT_EVENT *sim_event_alloc(void)
{
    /* Fast path: use thread-local manager (no lock) */
    if (thread_local_manager != NULL && 
        thread_local_manager->free_head != NULL) {
        EVENT_FREELIST_NODE *node = thread_local_manager->free_head;
        thread_local_manager->free_head = node->next;
        /* ... no mutex! */
        return &node->event;
    }
    
    /* Slow path: fall back to global manager */
    return sim_event_alloc_global();
}
```

**Wrapper enables scaling** from simple single mutex (Phase 1) to per-core
managers (Phase 4) without changing external API.

## Memory Overhead

### Per-Event Overhead

```c
/* Without free list: */
sizeof(UNIT_EVENT) = 32 bytes (estimated)
malloc overhead    = 16 bytes (typical)
Total per event    = 48 bytes

/* With free list: */
sizeof(EVENT_FREELIST_NODE) = 40 bytes (event + next pointer)
malloc overhead             = 16 bytes
Total per event             = 56 bytes

Additional overhead: 8 bytes per event (17% increase)
```

**Tradeoff:** 17% memory increase for 80× speed improvement during bursts.

### Free List Capacity

```c
/* Conservative: 64 events × 56 bytes = 3.5 KB */
#define EVENT_FREELIST_MAX 64

/* Aggressive: 512 events × 56 bytes = 28 KB */
#define EVENT_FREELIST_MAX 512
```

Modern systems: 28 KB is negligible. Even embedded targets can afford 3.5 KB.

## Testing Strategy

### Unit Tests (add to test_sim_events.c)

```c
/* Test: Free list reuses events */
static void test_freelist_reuses_events(void **state)
{
    UNIT_EVENT *events[10];
    
    /* Allocate 10 events */
    for (size_t i = 0; i < 10; i++) {
        events[i] = sim_event_alloc();
        assert_non_null(events[i]);
        events[i]->fire_time = i * 100;
    }
    
    /* Free all */
    for (size_t i = 0; i < 10; i++)
        sim_event_free(events[i]);
    
    /* Allocate again - should reuse (no malloc) */
    for (size_t i = 0; i < 10; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        assert_non_null(evt);
        
        /* Verify zero-initialized (no stale data) */
        assert_int_equal(evt->fire_time, 0);
        assert_null(evt->unit);
        
        sim_event_free(evt);
    }
}

/* Test: Free list respects cap */
static void test_freelist_respects_cap(void **state)
{
    UNIT_EVENT *events[EVENT_FREELIST_MAX + 10];
    
    /* Allocate more than cap */
    for (size_t i = 0; i < EVENT_FREELIST_MAX + 10; i++)
        events[i] = sim_event_alloc();
    
    /* Free all */
    for (size_t i = 0; i < EVENT_FREELIST_MAX + 10; i++)
        sim_event_free(events[i]);
    
    /* Free list should be capped */
    assert_true(sim_event_freelist_count() <= EVENT_FREELIST_MAX);
}

/* Test: Concurrent alloc/free (stress test) */
static void *allocator_thread(void *arg)
{
    for (int i = 0; i < 10000; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        assert_non_null(evt);
        evt->fire_time = i;
        sim_event_free(evt);
    }
    return NULL;
}

static void test_freelist_concurrent_stress(void **state)
{
    pthread_t threads[8];
    
    for (int i = 0; i < 8; i++)
        pthread_create(&threads[i], NULL, allocator_thread, NULL);
    
    for (int i = 0; i < 8; i++)
        pthread_join(threads[i], NULL);
    
    /* No crashes = success */
}
```

### Performance Benchmark

```c
void benchmark_event_allocation(void)
{
    const int NUM_ITERATIONS = 100000;
    struct timespec start, end;
    
    /* Warm up free list */
    for (int i = 0; i < 100; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        sim_event_free(evt);
    }
    
    /* Benchmark alloc/free cycle */
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        UNIT_EVENT *evt = sim_event_alloc();
        evt->fire_time = i;
        sim_event_free(evt);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    double ns_per_op = (elapsed * 1e9) / NUM_ITERATIONS;
    
    printf("Event alloc/free: %.1f ns/op\n", ns_per_op);
    printf("  Malloc fallbacks: %lu (%.2f%%)\n",
           global_freelist.malloc_slow,
           100.0 * global_freelist.malloc_slow / NUM_ITERATIONS);
}
```

**Target:** <100 ns/op for alloc+free cycle from free list.

## Implementation Phases

### Phase 1b (Add to Phase 1)

1. Add free list to `sim_events.c`
2. Replace malloc/free with sim_event_alloc/free
3. Add unit tests
4. Benchmark to verify improvement

**Compatible with Phase 1** - no API changes, just optimization.

### Phase 4 (If Needed)

If profiling shows mutex contention:
1. Implement lock-free free list
2. Stress test with 16+ concurrent threads
3. Verify no ABA problem (use hazard pointers if needed)

## Alternative: slab allocator

For even better performance, use slab allocator pattern:

```c
/* Allocate events in slabs of 64 */
#define SLAB_SIZE 64

typedef struct event_slab {
    EVENT_FREELIST_NODE events[SLAB_SIZE];
    struct event_slab *next;
    uint64_t allocated_mask;  /* bitmap: which events are in use */
} EVENT_SLAB;
```

**Defer to Phase 4** - mutex-based free list is sufficient for Phase 1.

## Summary

**Recommendation: Add free list in Phase 1b (minor addition to Phase 1)**

- Simple mutex-based implementation
- 80× speedup for bursty event workloads
- 17% memory overhead (acceptable)
- Easy to test and tune
- Lock-free upgrade path if profiling demands it

**Statistics command** (`SHOW EVENT STATISTICS`) lets operators monitor
malloc fallback rate and tune EVENT_FREELIST_MAX for their workload.

This optimization pairs perfectly with the min-heap to make event scheduling
fast AND allocation-efficient.
