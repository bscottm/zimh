# Phase 2: Wallclock Timer Queue Migration - STATUS

## Current Progress

✅ **Phase 1 Complete** - Queue display fixed
- Display now shows MPSC heap contents
- Committed: 901ec25c

## Phase 2: Wallclock Timer Analysis

### Wallclock Timer Architecture

**File:** `src/runtime/sim_timer.c`

**Key Structures:**
- `sim_wallclock_queue` - Intrusive linked list using `UNIT->a_next`
- `sim_wallclock_entry` - Staging area for new events

**Threading:**
- Producer: Simulator thread (various timer activation calls)
- Consumer: `_timer_thread()` - dedicated timer thread
- Synchronization: `sim_timer_lock` mutex

**Current Flow:**
1. Producer calls timer activation → sets `sim_wallclock_entry`
2. Producer signals `sim_timer_wake` condition
3. Timer thread wakes, inserts `sim_wallclock_entry` into sorted queue
4. Timer thread waits until first event's `a_due_time`
5. Timer thread dequeues and processes event

### Key Differences from Async I/O Queue

| Aspect | Async I/O | Wallclock Timers |
|--------|-----------|------------------|
| **Ordering** | By (sim_time, sequence) | By wallclock time |
| **Time Type** | Simulator instructions | Real wall clock (double) |
| **Insert Pattern** | Unordered stack | Sorted by due_time |
| **Mutex Status** | Was broken | Already safe |
| **Performance** | Lock-free critical | O(n) insertion acceptable |

### Migration Strategy

Given the differences, **Option B** (non-intrusive with mutex) is better:

1. Create separate event structure (no MPSC needed)
2. Replace intrusive `a_next` with allocated structures
3. Keep mutex protection (already works)
4. Maintain sorted insertion (different from async I/O)

### Rationale for NOT Using MPSC Queue

1. **Different ordering needs**: Wallclock time vs simulation time
2. **Single consumer pattern**: Only timer thread consumes
3. **Already thread-safe**: Mutex works fine
4. **Low contention**: Timer events are infrequent
5. **Sorted insertion**: Need O(n) scan for correct position anyway

## Next Implementation

### Create Wallclock Event Structure

```c
typedef struct sim_wallclock_event_s {
    UNIT *unit;
    double a_due_time;          // Wall clock time (seconds since epoch)
    struct sim_wallclock_event_s *next;
} sim_wallclock_event_t;
```

### Replace Queue Operations

- **Allocation**: `malloc(sizeof(sim_wallclock_event_t))`
- **Insertion**: Same sorted insertion logic, but using event->next
- **Extraction**: Same dequeue logic
- **Cleanup**: Free allocated events

### Changes Required

1. **Global queue**: `static sim_wallclock_event_t *sim_wallclock_queue = NULL;`
2. **Staging**: `static sim_wallclock_event_t *sim_wallclock_entry = NULL;`
3. **Insertion**: Update sorted insertion to use new structure
4. **Extraction**: Update dequeue to free events
5. **Cleanup**: Free remaining queue on shutdown

## Files to Modify

- `src/runtime/sim_timer.c` - ~15 locations using `a_next` and `a_due_time`

## Estimated Effort

- Code changes: 1 hour
- Testing: 30 minutes
- **Total**: 1.5 hours

## Risk Assessment

- **Low**: Keeping mutex, minimal logic changes
- **Testable**: Can verify with timer unit tests
- **Reversible**: Changes localized to sim_timer.c

## Status

**READY TO IMPLEMENT** - Simple non-intrusive structure with mutex protection
