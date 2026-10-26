# Event System Redesign: Non-Intrusive Event Tracking

## Overview

The current SIMH/ZIMH event system uses intrusive linked lists embedded 
directly in the `UNIT` structure. This proposal separates event tracking 
into dedicated `UNIT_EVENT` structures organized as a min-heap, reducing 
coupling and improving both maintainability and performance.

## Current Design Issues

### 1. Intrusive Data Structures

The `UNIT` structure contains event-specific fields that violate 
separation of concerns:

```c
struct UNIT {
    UNIT *next;                 /* next active - INTRUSIVE! */
    int32_t time;               /* time out */
    // ... device-specific fields ...
    
#ifdef SIM_ASYNCH_IO
    UNIT *a_next;               /* next asynch active - ALSO INTRUSIVE! */
    int32_t a_event_time;
    double a_due_time;
    double a_due_gtime;
    double a_usec_delay;
    // ... more async event tracking ...
#endif
};
```

**Problems:**
- `UNIT` cannot be in multiple queues without additional pointers
- Event tracking metadata pollutes the device abstraction
- Testing event logic requires full `UNIT` setup
- Async I/O requires duplicated tracking (`a_next`, `a_event_time`)

### 2. Linear Linked List Performance

Event activation (`_sim_activate` in `scp.c:10252`) walks a singly-linked 
list to find insertion position:

```c
t_stat _sim_activate (UNIT *uptr, int32_t event_time)
{
    UNIT *cptr, *prvptr;
    int32_t accum;
    
    prvptr = NULL;
    accum = 0;
    for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
        if (event_time < (accum + cptr->time))
            break;
        accum = accum + cptr->time;
        prvptr = cptr;
    }
    // ... insertion at prvptr ...
}
```

**Performance:** O(n) insertion where n = active events. With many devices 
(disk arrays, terminal multiplexers), this becomes a hot path.

### 3. Delta-Time Encoding Complexity

The queue stores **relative** deltas between adjacent events, requiring 
adjustment on every insertion/cancellation:

```c
uptr->time = event_time - accum;
if (cptr != QUEUE_LIST_END)
    cptr->time = cptr->time - uptr->time;  /* adjust next event */
```

This is clever for minimizing timer register updates but complicates 
debugging and error handling.

## Proposed Design

### 1. Separate Event Structure

```c
/* Non-intrusive event descriptor */
typedef struct unit_event {
    UNIT *unit;              /* backpointer to unit */
    uint64_t fire_time;      /* absolute instruction time */
    double fire_wallclock;   /* absolute wall-clock time (for timers) */
    uint32_t heap_index;     /* position in heap (for O(log n) update) */
    bool is_timer_event;     /* instruction vs wall-clock scheduling */
} UNIT_EVENT;
```

**Key changes:**
- Event is a **separate allocation**, not embedded in `UNIT`
- Stores **absolute** fire time, not relative delta
- `heap_index` enables O(log n) cancellation (see below)
- Backpointer allows event → unit lookup

### 2. Min-Heap Organization

Replace `sim_clock_queue` linked list with a binary min-heap:

```c
typedef struct event_heap {
    UNIT_EVENT **events;     /* array of pointers */
    size_t count;            /* active events */
    size_t capacity;         /* allocated capacity */
    uint64_t now;            /* current instruction time */
} EVENT_HEAP;

static EVENT_HEAP *sim_event_heap;
```

**Complexity improvements:**
- **Insert:** O(log n) instead of O(n)
- **Pop min:** O(log n) (unchanged)
- **Cancel:** O(log n) with `heap_index` tracking
- **Peek min:** O(1) (unchanged)

### 3. Heap Operations

Standard binary heap with parent/child indexing:

```c
/* Heap property: parent fires before children */
static inline size_t heap_parent(size_t i) { return (i - 1) / 2; }
static inline size_t heap_left(size_t i)   { return 2 * i + 1; }
static inline size_t heap_right(size_t i)  { return 2 * i + 2; }

/* Maintain heap invariant while tracking indices */
static void heap_sift_up(EVENT_HEAP *heap, size_t i)
{
    while (i > 0) {
        size_t parent = heap_parent(i);
        if (heap->events[i]->fire_time >= heap->events[parent]->fire_time)
            break;
        
        /* Swap and update indices */
        UNIT_EVENT *tmp = heap->events[i];
        heap->events[i] = heap->events[parent];
        heap->events[parent] = tmp;
        
        heap->events[i]->heap_index = i;
        heap->events[parent]->heap_index = parent;
        
        i = parent;
    }
}

static void heap_sift_down(EVENT_HEAP *heap, size_t i)
{
    while (true) {
        size_t smallest = i;
        size_t left = heap_left(i);
        size_t right = heap_right(i);
        
        if (left < heap->count && 
            heap->events[left]->fire_time < heap->events[smallest]->fire_time)
            smallest = left;
        
        if (right < heap->count && 
            heap->events[right]->fire_time < heap->events[smallest]->fire_time)
            smallest = right;
        
        if (smallest == i)
            break;
        
        /* Swap and update indices */
        UNIT_EVENT *tmp = heap->events[i];
        heap->events[i] = heap->events[smallest];
        heap->events[smallest] = tmp;
        
        heap->events[i]->heap_index = i;
        heap->events[smallest]->heap_index = smallest;
        
        i = smallest;
    }
}
```

### 4. Public API (Compatible with Current)

```c
/* Activation - now allocates UNIT_EVENT internally */
t_stat sim_activate(UNIT *uptr, int32_t interval)
{
    if (uptr->event != NULL)
        return SCPE_OK;  /* already scheduled */
    
    UNIT_EVENT *evt = malloc(sizeof(UNIT_EVENT));
    evt->unit = uptr;
    evt->fire_time = sim_event_heap->now + interval;
    evt->is_timer_event = false;
    
    heap_insert(sim_event_heap, evt);
    uptr->event = evt;  /* link back to unit */
    
    return SCPE_OK;
}

/* Cancellation - O(log n) with heap_index */
t_stat sim_cancel(UNIT *uptr)
{
    if (uptr->event == NULL)
        return SCPE_OK;  /* not scheduled */
    
    UNIT_EVENT *evt = uptr->event;
    size_t idx = evt->heap_index;
    
    /* Replace with last element and restore heap property */
    heap_remove_at(sim_event_heap, idx);
    
    free(evt);
    uptr->event = NULL;
    
    return SCPE_OK;
}

/* Event processing - minimal change */
t_stat sim_process_event(void)
{
    if (sim_event_heap->count == 0)
        return SCPE_STOP;
    
    UNIT_EVENT *evt = heap_pop_min(sim_event_heap);
    UNIT *uptr = evt->unit;
    
    sim_event_heap->now = evt->fire_time;
    uptr->event = NULL;
    
    free(evt);
    
    return uptr->action(uptr);
}
```

### 5. Async I/O Integration

The current async system has its own parallel queue (`a_next`, 
`a_event_time`). With non-intrusive events, the **same** heap and event 
structure serve both:

```c
/* Timer activation using same UNIT_EVENT */
t_stat sim_activate_after(UNIT *uptr, double usec_delay)
{
    UNIT_EVENT *evt = malloc(sizeof(UNIT_EVENT));
    evt->unit = uptr;
    evt->fire_wallclock = sim_get_wallclock_time() + usec_delay;
    evt->is_timer_event = true;  /* use wall-clock, not instructions */
    
    /* Same heap, different comparison in sift operations */
    heap_insert(sim_event_heap, evt);
    uptr->event = evt;
    
    return SCPE_OK;
}
```

**Unification opportunity:** A single heap could serve both instruction-
based and timer-based events by choosing the comparison key dynamically. 
Alternatively, maintain two heaps (instruction + timer) with the same 
underlying structure.

## Thread Safety Considerations

You mentioned MPSC (multi-producer, single-consumer) characteristics for 
cross-thread event insertion. Options:

### Option A: Lock-Free MPSC Queue (Front Buffer)

```c
typedef struct event_heap {
    /* Single-consumer heap (processed by sim thread) */
    UNIT_EVENT **events;
    size_t count;
    
    /* Multi-producer incoming queue */
    struct mpsc_node *incoming_head;  /* atomic */
    struct mpsc_node *incoming_tail;  /* atomic */
    
    pthread_mutex_t heap_lock;  /* only for heap modification */
} EVENT_HEAP;
```

**Pattern:**
1. Producers push to lock-free `incoming_*` queue
2. Consumer (simulator thread) periodically drains queue into heap
3. Heap operations remain single-threaded (no locking)

**Reference:** See `buf_ring.h` (already in repo) for lock-free ring 
buffer patterns.

### Option B: Simple Mutex

```c
typedef struct event_heap {
    UNIT_EVENT **events;
    size_t count;
    pthread_mutex_t lock;  /* held during all heap operations */
} EVENT_HEAP;
```

**Simpler** but may serialize async I/O threads. Profile before choosing.

### Option C: Per-Thread Event Pools

Each async thread maintains a private heap of "future" events. Main 
simulator thread periodically merges thread heaps:

```c
void sim_merge_async_events(void)
{
    for (each async thread) {
        while (thread_heap not empty && top event due) {
            evt = thread_heap_pop();
            main_heap_insert(evt);
        }
    }
}
```

**Most scalable** but complex. Defer unless profiling shows contention.

## Migration Path

### Phase 1: Internal Refactoring (No API Change)
1. Add `UNIT_EVENT` structure
2. Implement min-heap operations
3. Rewrite `_sim_activate` / `sim_cancel` to use heap
4. Keep `UNIT.time` and `UNIT.next` for compatibility (set from event)
5. **Test:** Run full regression suite

### Phase 2: Remove Intrusive Fields
1. Deprecate `UNIT.next` (nullptr it)
2. Remove `UNIT.time` (read from `uptr->event->fire_time - now`)
3. Update async code to use same event structure
4. **Test:** Verify no external code reads `UNIT.time` directly

### Phase 3: Async Unification
1. Merge `sim_asynch_queue` into main event heap
2. Remove `UNIT.a_next`, `UNIT.a_event_time`
3. Single heap with timer/instruction hybrid scheduling

### Phase 4: Thread Safety
1. Profile event insertion under async load
2. Choose synchronization strategy (A/B/C above)
3. Implement and stress-test with `tmxr` devices

## Testing Requirements

Per repository `TESTING.md` methodology:

### Unit Tests
- `tests/unit/src/core/test_event_heap.c`:
  - Heap property maintenance
  - Insertion order vs fire order
  - Cancellation from middle
  - Edge cases (empty, single, full)

### Integration Tests
- `tests/integration/test_event_migration.c`:
  - Compare old vs new queue ordering
  - Stress test: 1000 events, random insertion/cancellation
  - Verify delta-time → absolute-time equivalence

### Performance Tests
- `tests/performance/test_event_throughput.c`:
  - Measure O(n) → O(log n) improvement
  - Profile with 100+ disk units (VAX 8600 config)

### Regression
- Run full `ctest --parallel` suite before merging each phase

## Open Design Questions

1. **Heap resizing policy:** Start at 128 events, grow by 1.5× or 2×?
2. **Timer event interleaving:** Single heap or separate timer heap?
3. **Synchronization:** Benchmark mutex vs lock-free MPSC
4. **Backwards compat:** Should `UNIT.time` become a getter macro?
5. **Validation:** Add heap invariant checking in debug builds?

## Related Work

- `buf_ring.h`: Lock-free MPSC ring buffer (already in repo)
- `projects/EVENT_QUEUE_DELAY_TYPES.md`: Typed delay values
- `projects/AIO_DIO_UNIFICATION.md`: Async I/O patterns

## References

- `src/core/scp.c:10245`: Current `_sim_activate` implementation
- `src/core/sim_defs.h:385`: `UNIT` structure definition
- `src/runtime/sim_timer.c`: Timer coscheduling
- "Introduction to Algorithms" (CLRS): Ch 6, Binary Heaps
