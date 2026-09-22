# MPSC Queue + Min-Heap Prototype for `sim_asynch_queue`

## Overview

This prototype replaces the unsafe intrusive linked list (`sim_asynch_queue`) with a **lock-free MPSC queue + min-heap** design that provides:

1. **Proper MPSC semantics** - Multiple async I/O threads enqueue safely
2. **Portable memory ordering** - Works correctly on weak memory models (ARM, RISC-V)
3. **Non-intrusive design** - Eliminates `UNIT->a_next` pollution
4. **Deterministic ordering** - Events at the same time are ordered by sequence number
5. **Better separation of concerns** - Producer/consumer roles are explicit

## Architecture

### Producer Side (Async I/O Threads)

**Lock-Free Treiber Stack**
- Multiple threads can call `sim_event_enqueue()` concurrently
- Uses CAS (Compare-And-Swap) operations for wait-free enqueue
- Each event gets a monotonically increasing sequence number
- Proper RELEASE memory ordering ensures event data is visible

```c
sim_event_mpsc_queue_t sim_event_queue = {
    .head = atomic pointer,      // Stack head (CAS target)
    .sequence = atomic counter   // Monotonic sequence generator
};
```

### Consumer Side (Main Simulator Thread)

**Min-Heap Priority Queue**
- Only the simulator thread calls dequeue/process operations
- Events ordered by `(event_time, sequence_number)`
- Binary heap provides O(log n) insert/extract
- Events processed in strict priority order

```c
sim_event_heap_t sim_event_heap = {
    .events = array of event pointers,
    .count = current size,
    .capacity = allocated size
};
```

## Data Structures

### Event Wrapper: `sim_unit_event_t`

```c
typedef struct sim_unit_event_s {
    UNIT *unit;                      // The unit to activate
    int32_t event_time;              // Time in simulator units
    uint64_t sequence;               // Deterministic ordering
    ACTIVATE_API activate_call;      // Activation function
    void (*check_completion)(UNIT*); // Optional completion check
    struct sim_unit_event_s *next;   // Treiber stack link
} sim_unit_event_t;
```

## API

### Initialization (called at simulator startup)

```c
void sim_event_queue_init(sim_event_mpsc_queue_t *queue);
void sim_event_heap_init(sim_event_heap_t *heap);
```

### Producer: Enqueue (called from async I/O threads)

```c
bool sim_event_enqueue(sim_event_mpsc_queue_t *queue,
                        UNIT *unit,
                        int32_t event_time,
                        ACTIVATE_API activate_call,
                        void (*check_completion)(UNIT *));
```

- **Thread-safe**: Multiple producers can call concurrently
- **Lock-free**: Uses CAS-based Treiber stack
- **Wait-free progress**: No producer blocks another
- **Returns**: `true` on success, `false` if malloc fails

### Consumer: Migrate Queue to Heap (called from `sim_process_event()`)

```c
int sim_event_migrate_to_heap(sim_event_mpsc_queue_t *queue,
                                sim_event_heap_t *heap);
```

- **Atomically drains** the MPSC queue using exchange
- **Inserts events** into the min-heap by priority
- **Returns**: Number of events migrated

### Consumer: Process Heap Events (called from `sim_process_event()`)

```c
int sim_event_process_heap(sim_event_heap_t *heap, int32_t current_time);
```

- **Extracts** all events with `event_time <= current_time`
- **Activates** units in priority order
- **Returns**: Number of events processed

## Memory Ordering Guarantees

### Enqueue (Producer)

```c
event->sequence = sim_atomic_fetch_add(&queue->sequence, 1);  // SEQ_CST
// ... fill event fields ...
while (!sim_atomic_ptr_cas(&queue->head, &expected, event));  // SEQ_CST
```

- **SEQ_CST** CAS provides:
  - **RELEASE** on success: All event fields visible to consumer
  - **ACQUIRE** on retry: See current head value

### Dequeue (Consumer)

```c
sim_unit_event_t *event = sim_atomic_ptr_exchange(&queue->head, NULL);  // SEQ_CST
```

- **SEQ_CST** exchange provides:
  - **ACQUIRE**: All producer writes to events are visible
  - **RELEASE**: NULL store visible to future producers

## Comparison to Old Implementation

### Old Code (Unsafe on Weak Memory Models)

```c
// Producer (sim_aio_activate)
do {
    q = AIO_QUEUE_VAL;              // CAS-based read
    uptr->a_next = q;                // PLAIN STORE - no ordering!
} while (q != AIO_QUEUE_SET(uptr, q)); // CAS
```

**Problem**: The `uptr->a_next = q` is a plain store with no memory ordering. On ARM/RISC-V, the consumer might see the new head but not the `a_next` value.

### New Code (Portable)

```c
// Producer (sim_event_enqueue)
event->next = (sim_unit_event_t *)expected;  // Local, not visible yet
while (!sim_atomic_ptr_cas(&queue->head, &expected, event));  // Atomic with RELEASE
```

**Correct**: The CAS with RELEASE semantics ensures all writes to `event` (including `event->next`) are visible before the new head becomes visible.

## Integration Points

### 1. `sim_aio.c` Changes

```c
// Global state
static sim_event_mpsc_queue_t sim_event_queue;
static sim_event_heap_t sim_event_heap;

// Initialize in aio_init()
sim_event_queue_init(&sim_event_queue);
sim_event_heap_init(&sim_event_heap);

// Clean up in aio_cleanup()
sim_event_heap_destroy(&sim_event_heap);
sim_event_queue_destroy(&sim_event_queue);

// Replace sim_aio_activate() body
sim_event_enqueue(&sim_event_queue, uptr, event_time, caller, uptr->a_check_completion);

// Update sim_aio_update_queue()
int migrated = sim_event_migrate_to_heap(&sim_event_queue, &sim_event_heap);
```

### 2. `sim_process_event()` Changes (in `scp.c`)

```c
t_stat sim_process_event(void)
{
    // ... existing code ...
    AIO_UPDATE_QUEUE;  // Migrates events from MPSC queue to heap
    
    // NEW: Process heap events with time <= current simulator time
    if (aio_enabled_and_active()) {
        int processed = sim_aio_process_heap(sim_gtime());
        if (processed > 0) {
            // Events were processed, may need to update sim_interval
        }
    }
    
    // ... rest of existing code ...
}
```

## Transition Strategy

### Phase 1: Dual Mode (Current Prototype State)

- **Both systems run in parallel**
- Old intrusive queue still exists (`UNIT->a_next`)
- New MPSC queue + heap processes events side-by-side
- Allows testing without breaking existing code

### Phase 2: Feature Parity

- Verify all event types work with new queue
- Add comprehensive unit tests
- Performance benchmarking

### Phase 3: Deprecation

- Switch `sim_aio_activate()` to only use new queue
- Mark `UNIT->a_next` fields as deprecated
- Update any code checking `a_next` for "is queued" status

### Phase 4: Removal

- Remove old queue code paths
- Remove `UNIT->a_next` field
- Simplify `AIO_IS_ACTIVE` macro
- Remove compatibility shims

## Testing

### Unit Tests Needed

1. **MPSC Queue Tests**
   - Single producer, single consumer
   - Multiple producers, single consumer
   - Sequence number monotonicity
   - Memory ordering verification (TSan)

2. **Min-Heap Tests**
   - Insert/extract ordering
   - Heap property maintenance
   - Growth/reallocation
   - Edge cases (empty, single element)

3. **Integration Tests**
   - Full enqueue → migrate → process cycle
   - Stress test with many concurrent producers
   - Timing accuracy (events fire at correct times)

### Verification Tools

- **ThreadSanitizer (TSan)**: Detect data races
- **AddressSanitizer (ASan)**: Detect memory errors
- **Valgrind**: Memory leak detection

## Performance Considerations

### Heap Operations

- **Insert**: O(log n) - amortized fast due to sparse events
- **Extract-min**: O(log n)
- **Growth**: O(n) when realloc needed (rare)

### Queue Operations

- **Enqueue**: O(1) wait-free
- **Batch dequeue**: O(n) where n = pending events

### Typical Workload

Async I/O events are **sparse** (milliseconds to seconds apart), so:
- Heap rarely exceeds 10-20 events
- O(log 20) ≈ 4 comparisons per operation
- Negligible overhead compared to I/O latency

## Known Issues / TODO

1. ✅ **Add `sim_atomic_fetch_add`** to `sim_atomic.h`
2. ✅ **Include `sim_platform.h`** in `sim_atomic.h` for Windows LONG type
3. ⚠️ **Remove dual-mode** once testing is complete
4. ⚠️ **Eliminate `UNIT->a_next`** field
5. ⚠️ **Update `AIO_IS_ACTIVE` macro** to not check `a_next`
6. ⚠️ **Add unit tests** for queue and heap
7. ⚠️ **Integration testing** with real simulators
8. ⚠️ **Performance benchmarks** vs old implementation

## Files Modified/Added

### New Files
- `src/include/sim_event_queue.h` - Event queue API
- `src/lib/sim_event_queue.c` - Event queue implementation
- `MPSC_QUEUE_PROTOTYPE.md` - This document

### Modified Files
- `src/include/sim_atomic.h` - Added `sim_atomic_fetch_add()`
- `src/include/sim_aio.h` - Added `sim_aio_process_heap()` declaration
- `src/lib/sim_aio.c` - Integrated new queue, dual-mode support

### Files to Modify (Phase 3+)
- `src/core/sim_defs.h` - Remove `UNIT->a_next` field
- `src/core/scp.c` - Call `sim_aio_process_heap()` in `sim_process_event()`
- `src/include/sim_aio.h` - Update `is_unit_aio_active()` macro

## References

### Lock-Free Algorithms
- Treiber Stack: "Systems Programming: Coping with Parallelism" (IBM, 1986)
- Memory Ordering: C11 atomics specification (ISO/IEC 9899:2011)
- MPSC Queues: "The Art of Multiprocessor Programming" (Herlihy & Shavit)

### Existing Code
- `src/lib/sim_tailq.c` - Similar SPSC queue implementation (Ethernet)
- `src/include/sim_atomic_ptr.h` - Pointer CAS operations

---

**Prototype Status**: ✅ Compiles, ready for testing  
**Author**: Claude Sonnet 4.5  
**Date**: 2026-09-14
