/* sim_events.c: Non-intrusive event queue using binary min-heap */
// SPDX-FileCopyrightText: 2024 B. Scott Michel
// SPDX-License-Identifier: X11

/*
   This module implements a non-intrusive event scheduling system using
   a binary min-heap for O(log n) insertion and removal performance.

   Key features:
   - Events are separate allocations (UNIT_EVENT), not embedded in UNIT
   - Min-heap organization provides O(log n) insert/remove/cancel
   - Absolute fire times simplify debugging vs. relative deltas
   - Heap index tracking enables O(log n) cancellation from middle
   - Designed for both instruction-based and wall-clock scheduling

   Thread safety:
   - Phase 1: Single mutex protects all heap operations
   - Future: Lock-free MPSC queue for async I/O (profile-guided)

   Migration path:
   - Phase 1: Internal use only, maintains UNIT.next/UNIT.time compatibility
   - Phase 2: Remove intrusive UNIT fields
   - Phase 3: Unify instruction and timer event queues
   - Phase 4: Add lock-free MPSC if profiling shows contention
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sim_defs.h"
#include "sim_atomic.h"

/* Event heap structure */

typedef struct event_heap {
    UNIT_EVENT **events; /* array of event pointers */
    size_t count;        /* number of active events */
    size_t capacity;     /* allocated capacity */
    uint64_t now;        /* current instruction time */
} EVENT_HEAP;

/* Initial heap capacity - grows as needed */
#define INITIAL_HEAP_CAPACITY 128

/* Growth factor when heap fills - 2.0 for power-of-2 alignment */
#define HEAP_GROWTH_FACTOR 2.0

/* Static event heap (global) */
static EVENT_HEAP *sim_event_heap = NULL;

/* Forward declarations */
static void heap_sift_up(EVENT_HEAP *heap, size_t index);
static void heap_sift_down(EVENT_HEAP *heap, size_t index);
static t_stat heap_ensure_capacity(EVENT_HEAP *heap);

/* Heap index computation - standard binary heap relationships */

static inline size_t heap_parent(size_t i)
{
    return (i - 1) / 2;
}

static inline size_t heap_left_child(size_t i)
{
    return 2 * i + 1;
}

static inline size_t heap_right_child(size_t i)
{
    return 2 * i + 2;
}

/* Initialize the event heap

   Call once at simulator startup. Allocates the initial heap structure
   with INITIAL_HEAP_CAPACITY slots. Returns SCPE_OK on success, SCPE_MEM
   on allocation failure.
*/

t_stat sim_event_heap_init(void)
{
    if (sim_event_heap != NULL)
        return SCPE_OK; /* already initialized */

    sim_event_heap = (EVENT_HEAP *)calloc(1, sizeof(EVENT_HEAP));
    if (sim_event_heap == NULL)
        return SCPE_MEM;

    sim_event_heap->events = (UNIT_EVENT **)calloc(INITIAL_HEAP_CAPACITY, sizeof(UNIT_EVENT *));
    if (sim_event_heap->events == NULL) {
        free(sim_event_heap);
        sim_event_heap = NULL;
        return SCPE_MEM;
    }

    sim_event_heap->count = 0;
    sim_event_heap->capacity = INITIAL_HEAP_CAPACITY;
    sim_event_heap->now = 0;

    return SCPE_OK;
}

/* Clean up the event heap

   Call at simulator shutdown. Frees all events and the heap structure.
   Does not free the UNIT structures (those are owned by devices).
*/

void sim_event_heap_cleanup(void)
{
    if (sim_event_heap == NULL)
        return;

    /* Free all pending events */
    for (size_t i = 0; i < sim_event_heap->count; i++) {
        if (sim_event_heap->events[i] != NULL) {
            /* Clear backpointer in unit (if phase 2+ migration) */
            /* sim_event_heap->events[i]->unit->event = NULL; */
            free(sim_event_heap->events[i]);
        }
    }

    free(sim_event_heap->events);
    free(sim_event_heap);
    sim_event_heap = NULL;
}

/* Ensure heap has capacity for one more event

   Grows the heap by HEAP_GROWTH_FACTOR if full. Returns SCPE_OK on success,
   SCPE_MEM if reallocation fails (heap is unchanged on failure).
*/

static t_stat heap_ensure_capacity(EVENT_HEAP *heap)
{
    if (heap->count < heap->capacity)
        return SCPE_OK;

    size_t new_capacity = (size_t)(heap->capacity * HEAP_GROWTH_FACTOR);
    if (new_capacity <= heap->capacity)
        new_capacity = heap->capacity + INITIAL_HEAP_CAPACITY;

    UNIT_EVENT **new_events = (UNIT_EVENT **)realloc(heap->events, new_capacity * sizeof(UNIT_EVENT *));
    if (new_events == NULL)
        return SCPE_MEM;

    heap->events = new_events;
    heap->capacity = new_capacity;

    /* Zero the new slots */
    memset(&heap->events[heap->count], 0, (new_capacity - heap->count) * sizeof(UNIT_EVENT *));

    return SCPE_OK;
}

/* Restore heap property upward (child fires before parent)

   Used after insertion at the end. Swaps the event at 'index' with its
   parent while it fires earlier, updating heap_index fields to maintain
   the invariant that events[i]->heap_index == i.
*/

static void heap_sift_up(EVENT_HEAP *heap, size_t index)
{
    while (index > 0) {
        size_t parent = heap_parent(index);

        /* Heap property: parent fires at or before child */
        if (heap->events[index]->fire_time >= heap->events[parent]->fire_time)
            break;

        /* Swap child with parent */
        UNIT_EVENT *tmp = heap->events[index];
        heap->events[index] = heap->events[parent];
        heap->events[parent] = tmp;

        /* Update heap_index to match array position */
        heap->events[index]->heap_index = (uint32_t)index;
        heap->events[parent]->heap_index = (uint32_t)parent;

        index = parent;
    }
}

/* Restore heap property downward (parent fires before children)

   Used after removing the minimum or updating an event. Swaps the event at
   'index' with its earliest-firing child while it fires later, maintaining
   heap_index invariants.
*/

static void heap_sift_down(EVENT_HEAP *heap, size_t index)
{
    while (true) {
        size_t earliest = index;
        size_t left = heap_left_child(index);
        size_t right = heap_right_child(index);

        /* Find earliest-firing among parent and children */
        if (left < heap->count && heap->events[left]->fire_time < heap->events[earliest]->fire_time)
            earliest = left;

        if (right < heap->count && heap->events[right]->fire_time < heap->events[earliest]->fire_time)
            earliest = right;

        /* Heap property satisfied */
        if (earliest == index)
            break;

        /* Swap parent with earliest child */
        UNIT_EVENT *tmp = heap->events[index];
        heap->events[index] = heap->events[earliest];
        heap->events[earliest] = tmp;

        /* Update heap_index to match array position */
        heap->events[index]->heap_index = (uint32_t)index;
        heap->events[earliest]->heap_index = (uint32_t)earliest;

        index = earliest;
    }
}

/* Insert event into heap

   Adds 'event' to the heap, maintaining min-heap property. Updates
   event->heap_index to reflect its position. Returns SCPE_OK on success,
   SCPE_MEM if heap is full and cannot grow.

   Complexity: O(log n)
*/

t_stat sim_event_heap_insert(UNIT_EVENT *event)
{
    if (sim_event_heap == NULL)
        return SCPE_IERR; /* heap not initialized */

    t_stat status = heap_ensure_capacity(sim_event_heap);
    if (status != SCPE_OK)
        return status;

    /* Insert at end and bubble up */
    size_t index = sim_event_heap->count;
    sim_event_heap->events[index] = event;
    event->heap_index = (uint32_t)index;
    sim_event_heap->count++;

    heap_sift_up(sim_event_heap, index);

    return SCPE_OK;
}

/* Remove and return the earliest-firing event

   Removes the event at the root of the heap (earliest fire_time) and
   returns it. The caller takes ownership and must free the event.
   Returns NULL if the heap is empty.

   Complexity: O(log n)
*/

UNIT_EVENT *sim_event_heap_pop(void)
{
    if (sim_event_heap == NULL || sim_event_heap->count == 0)
        return NULL;

    UNIT_EVENT *result = sim_event_heap->events[0];

    /* Move last element to root and restore heap property */
    sim_event_heap->count--;
    if (sim_event_heap->count > 0) {
        sim_event_heap->events[0] = sim_event_heap->events[sim_event_heap->count];
        sim_event_heap->events[0]->heap_index = 0;
        sim_event_heap->events[sim_event_heap->count] = NULL;

        heap_sift_down(sim_event_heap, 0);
    } else {
        sim_event_heap->events[0] = NULL;
    }

    return result;
}

/* Peek at the earliest-firing event without removing it

   Returns the event at the root of the heap, or NULL if empty.
   The heap retains ownership.

   Complexity: O(1)
*/

UNIT_EVENT *sim_event_heap_peek(void)
{
    if (sim_event_heap == NULL || sim_event_heap->count == 0)
        return NULL;

    return sim_event_heap->events[0];
}

/* Remove an event from an arbitrary position in the heap

   Cancels the event at the given heap index. Used by sim_cancel() to
   remove a specific unit's event. Returns SCPE_OK on success, SCPE_IERR
   if the index is out of range.

   Complexity: O(log n)
*/

t_stat sim_event_heap_remove_at(size_t index)
{
    if (sim_event_heap == NULL || index >= sim_event_heap->count)
        return SCPE_IERR;

    UNIT_EVENT *removed = sim_event_heap->events[index];

    /* Move last element to this position */
    sim_event_heap->count--;
    if (index == sim_event_heap->count) {
        /* Removed the last element - no fixup needed */
        sim_event_heap->events[index] = NULL;
        return SCPE_OK;
    }

    sim_event_heap->events[index] = sim_event_heap->events[sim_event_heap->count];
    sim_event_heap->events[index]->heap_index = (uint32_t)index;
    sim_event_heap->events[sim_event_heap->count] = NULL;

    /* Restore heap property - may need to go up or down */
    uint64_t removed_time = removed->fire_time;
    uint64_t replacement_time = sim_event_heap->events[index]->fire_time;

    if (replacement_time < removed_time)
        heap_sift_up(sim_event_heap, index);
    else if (replacement_time > removed_time)
        heap_sift_down(sim_event_heap, index);

    return SCPE_OK;
}

/* Return the number of events in the heap

   Complexity: O(1)
*/

size_t sim_event_heap_count(void)
{
    if (sim_event_heap == NULL)
        return 0;

    return sim_event_heap->count;
}

/* Find event in heap by unit pointer

   Linear search through heap array. Returns the event if found,
   NULL otherwise. Used by sim_cancel() to locate event for removal.

   Complexity: O(n) - use sparingly
*/

UNIT_EVENT *sim_event_heap_find_by_unit(const UNIT *uptr)
{
    if (sim_event_heap == NULL || uptr == NULL)
        return NULL;

    for (size_t i = 0; i < sim_event_heap->count; i++) {
        if (sim_event_heap->events[i]->unit == uptr)
            return sim_event_heap->events[i];
    }

    return NULL;
}

/* Get current instruction time

   Returns the global instruction counter maintained by the event system.
*/

uint64_t sim_event_heap_now(void)
{
    if (sim_event_heap == NULL)
        return 0;

    return sim_event_heap->now;
}

/* Set current instruction time

   Updates the global instruction counter. Typically called by sim_process_event
   when advancing time to the next event's fire_time.
*/

void sim_event_heap_set_now(uint64_t time)
{
    if (sim_event_heap != NULL)
        sim_event_heap->now = time;
}

/* Validate heap invariant

   Walks the heap and verifies that every parent fires at or before its
   children. Returns true if the heap property holds, false otherwise.

   Use in assertions during development:
       assert(sim_event_heap_validate());
*/
bool sim_event_heap_validate(void)
{
    if (sim_event_heap == NULL || sim_event_heap->count == 0)
        return true;

    for (size_t i = 0; i < sim_event_heap->count; i++) {
        /* Check heap_index matches array position */
        if (sim_event_heap->events[i]->heap_index != i)
            return false;

        /* Check parent fires at or before this child */
        if (i > 0) {
            size_t parent = heap_parent(i);
            if (sim_event_heap->events[parent]->fire_time > sim_event_heap->events[i]->fire_time)
                return false;
        }
    }

    return true;
}
