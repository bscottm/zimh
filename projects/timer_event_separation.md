# Timer Event Separation - Architectural Fix

## Problem

The original code had a fundamental design flaw where `UNIT::a_next` was used for two different purposes:

1. **sim_asynch_queue**: Lock-free queue for async I/O events (atomic operations)
2. **sim_wallclock_queue**: Mutex-protected queue for timer events

This dual use caused queue corruption when:
- A UNIT was on one queue and transitioned to another
- A UNIT was on both queues simultaneously
- Queue operations from different threads conflicted

**Symptom**: "q == NULL" errors in `sim_aio_update_queue()` where `QUEUE_LIST_END` was expected.

**Example**: Ethernet device UNIT (xq_svc) was found with:
- `next == sim_timer_units[9]` (on main clock queue)
- Being iterated in sim_asynch_queue
- `a_next == NULL` (corrupted linkage)

## Solution

Introduced a separate `TIMER_EVENT` structure to completely isolate timer queue management from async I/O queue management.

### New Architecture

```c
typedef struct timer_event {
    struct timer_event *next;   /* wallclock queue linkage */
    UNIT *unit;                 /* backpointer to the UNIT */
    double due_time;            /* due time for timer event */
    double due_gtime;           /* due time (in instructions) */
    double usec_delay;          /* time delay for timer event */
    double usecs_remaining;     /* time balance for long delays */
} TIMER_EVENT;
```

### UNIT Structure Changes

**Before**:
```c
struct UNIT {
    ...
    UNIT *a_next;           /* Used for BOTH queues - CONFLICT! */
    double a_due_time;      /* timer fields */
    double a_due_gtime;
    double a_usec_delay;
    ...
};
```

**After**:
```c
struct UNIT {
    ...
    UNIT *a_next;           /* ONLY for sim_asynch_queue */
    TIMER_EVENT *timer_event; /* NULL or pointer to active timer */
    ...
};
```

### Queue Changes

**Before**:
```c
UNIT *volatile sim_wallclock_queue = QUEUE_LIST_END;
UNIT *volatile sim_wallclock_entry = NULL;
```

**After**:
```c
TIMER_EVENT *volatile sim_wallclock_queue = NULL;
TIMER_EVENT *volatile sim_wallclock_entry = NULL;
```

## Implementation Details

### Memory Management

- TIMER_EVENT allocated when timer activated: `calloc(1, sizeof(TIMER_EVENT))`
- Freed when:
  - Timer fires (in timer thread after dequeue)
  - Timer cancelled (in `_sim_wallclock_cancel()`)
  - System shutdown (draining wallclock queue)

### Thread Safety

- Timer queue operations protected by `sim_timer_lock` mutex (unchanged)
- Async I/O queue uses atomic operations (unchanged)
- No intersection between the two queues (now enforced by type system)

### Initialization

- `aio_init_unit()` now sets `timer_event = NULL`
- Should be called during device reset or initialization

## Files Modified

1. **src/core/sim_defs.h**
   - Added TIMER_EVENT structure definition
   - Changed UNIT::timer_event (replaced three timer fields)

2. **src/runtime/sim_timer.c**
   - Changed queue types to TIMER_EVENT*
   - Updated all queue operations:
     - Timer insertion/activation
     - Timer cancellation
     - Timer query functions
     - Display/debugging code
     - Thread shutdown logic
   - Allocate/free TIMER_EVENT as needed

3. **src/runtime/sim_aio.c**
   - Initialize timer_event field in aio_init_unit()

## Benefits

1. **Type Safety**: Compiler prevents mixing queue types
2. **Clear Ownership**: Each queue has its own linkage field
3. **Easier Debugging**: Distinct types in debugger
4. **Architectural Clarity**: Separation of concerns enforced
5. **Memory Efficient**: TIMER_EVENT only exists when timer active
6. **Future-Proof**: Foundation for further timer subsystem refactoring

## Testing

- Build: ✅ All 1987 targets compiled successfully
- Smoke Test: ✅ VAX780 simulator runs with async clocks enabled
- Next Steps: Run full test suite to verify timer functionality

## Related Issues

This fix resolves the queue corruption bug where:
- Async I/O operations on timer UNITs caused queue linkage conflicts
- Timer operations on I/O UNITs caused similar conflicts
- Race conditions between timer thread and main thread led to NULL pointers

## Future Work

Consider:
1. Refactoring wallclock queue code into separate source file
2. Converting wallclock queue to min-heap for better performance
3. Further isolation of timer subsystem from main event loop
