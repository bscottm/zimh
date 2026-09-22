# Phase 3: Remove Intrusive Queue Fields (ABI-Breaking Change)

**Status:** COMPLETE

## Overview

Phase 3 completes the MPSC queue integration by removing the intrusive fields from the UNIT structure that were previously used for async I/O queue management.

## Fields Removed

From `struct UNIT` in `src/core/sim_defs.h`:

```c
UNIT *a_next;              /* next asynch active */
int32_t a_event_time;      /* event time */
ACTIVATE_API a_activate_call; /* activation function pointer */
```

## Deprecated Global Removed

- `sim_asynch_queue` - Old intrusive queue head (no longer used)
  - Removed from `src/lib/sim_aio.c`
  - Removed from `src/include/sim_aio.h`

## Impact

### Binary Compatibility

**This is an ABI-breaking change.** Any external code compiled against the old UNIT structure will need to be recompiled.

### Source Compatibility

All internal code has already been migrated in Phases 1 and 2:
- Phase 1: Async I/O queue → MPSC queue with min-heap
- Phase 2: Wallclock timer queue → non-intrusive event structures

No source code changes are required for simulators or devices that follow the standard async I/O patterns.

## Migration Guide

### For Simulator Developers

**No action required** if your code uses the standard async I/O APIs:
- `sim_activate()` / `sim_activate_abs()` / `sim_activate_after()`
- `sim_cancel()`
- `sim_is_active()`

**Action required** if your code:
- Directly accessed `UNIT->a_next` (none found in codebase)
- Directly accessed `UNIT->a_event_time` (none found in codebase)
- Directly accessed `UNIT->a_activate_call` (none found in codebase)

### For External Module Developers

**Recompilation required** for any external modules that:
- Link against libsimh
- Include `sim_defs.h`
- Reference the UNIT structure

## Benefits

1. **Cleaner abstraction** - UNIT structure no longer polluted with queue implementation details
2. **Non-intrusive design** - Units can be in multiple queues without conflicts
3. **Better encapsulation** - Queue implementation can change without affecting UNIT
4. **Smaller UNIT structure** - Saves 16 bytes per UNIT (on 64-bit systems)
5. **Type safety** - Queue operations now use proper event structures

## What Remains

The following async I/O fields remain in UNIT:
- `a_check_completion` - Completion callback (device-specific)
- `a_is_active` - Active status callback (device-specific)
- `a_polling_now` - Polling state flag (multiplexer support)
- `a_poll_waiter_count` - Polling waiter count (multiplexer support)
- `a_due_time` - Timer due time (wallclock timers)
- `a_due_gtime` - Timer due time in instructions (wallclock timers)
- `a_usec_delay` - Timer delay in microseconds (wallclock timers)

These fields are either:
- Device-specific callbacks that must be in UNIT
- State that belongs to the unit itself, not the queue

## Preserved Compatibility

The synchronous event queue (`sim_clock_queue`) still uses intrusive linking via `UNIT->next`. This is appropriate because:
- Single consumer (main simulator loop)
- No thread safety issues
- Performance-critical path
- Well-established, stable API

Only the async I/O queue needed the MPSC redesign due to:
- Multiple producers (async I/O threads)
- Race conditions with intrusive pointers
- Need for lock-free producer-side operations

## Documentation Updates

- Updated `docs/developers/writing_a_simulator.md` to note field removal
- Marked removed fields with comment indicating removal date

## Files Modified

- `src/core/sim_defs.h` - Removed 3 fields from UNIT structure
- `src/lib/sim_aio.c` - Removed sim_asynch_queue definition and initialization
- `src/include/sim_aio.h` - Removed sim_asynch_queue extern declaration
- `docs/developers/writing_a_simulator.md` - Updated UNIT structure documentation

## Testing

All existing tests pass:
- Async I/O operations work correctly via MPSC queue
- Wallclock timers work correctly via non-intrusive event structures
- Synchronous event queue unchanged and working

## Commits

- Phase 1: 901ec25c - Queue display fix
- Phase 2: 7f320931 - Wallclock timer migration
- Phase 3: (pending) - Remove intrusive fields

## Next Steps

None - MPSC queue integration is complete!

Future considerations:
- Monitor performance in production use
- Consider applying similar patterns to other queue types if needed
- Update any external documentation or tutorials
