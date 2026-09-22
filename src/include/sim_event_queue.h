// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * sim_event_queue: Lock-free MPSC (Multiple Producer, Single Consumer) event queue
 *
 * Threading model:
 * - Multiple async I/O threads may call enqueue operations concurrently (producers)
 * - Only the main simulator thread calls dequeue/process operations (consumer)
 *
 * Implementation:
 * - Producers enqueue to a lock-free Treiber stack using CAS operations
 * - Consumer batch-dequeues the entire stack and inserts events into a min-heap
 * - Min-heap is ordered by (event_time, sequence_number) for deterministic processing
 * - Uses C11 atomics with proper memory ordering
 *
 * Benefits over the old intrusive a_next approach:
 * - Proper MPSC semantics with portable memory ordering
 * - Non-intrusive: no pollution of UNIT structure
 * - Deterministic ordering via sequence numbers
 * - Better separation of concerns
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

#if !defined(SIM_EVENT_QUEUE_H_)
#define SIM_EVENT_QUEUE_H_ 1

#include <stdint.h>
#include <stdbool.h>
#include "sim_atomic.h"
#include "sim_atomic_ptr.h"
#include "sim_defs.h"

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Event structure: Wraps a UNIT activation with timing and ordering metadata
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

typedef struct sim_unit_event_s {
    /* The unit to activate */
    UNIT *unit;

    /* Event time in simulator instruction/cycle units */
    int32_t event_time;

    /* Monotonically increasing sequence number for deterministic ordering.
     * When two events have the same event_time, the one with the lower
     * sequence number is processed first. */
    uint64_t sequence;

    /* The activation function to call (sim_activate, sim_activate_abs, etc.) */
    ACTIVATE_API activate_call;

    /* Check completion function (may be NULL) */
    void (*check_completion)(UNIT *);

    /* Next event in the MPSC queue (Treiber stack link) */
    struct sim_unit_event_s *next;
} sim_unit_event_t;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * MPSC Queue Head: Lock-free Treiber stack for producer-side enqueue
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

typedef struct {
    /* Head of the lock-free stack (accessed by producers via CAS) */
    sim_atomic_ptr_t head;

    /* Monotonic sequence counter (accessed by producers via atomic increment) */
    sim_atomic_value_t sequence;
} sim_event_mpsc_queue_t;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Min-Heap: Consumer-side priority queue ordered by (event_time, sequence)
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

typedef struct {
    /* Array of event pointers (binary min-heap) */
    sim_unit_event_t **events;

    /* Current number of events in the heap */
    size_t count;

    /* Allocated capacity of the events array */
    size_t capacity;
} sim_event_heap_t;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * API Functions
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

/* Initialize the MPSC queue (called once at simulator startup) */
void sim_event_queue_init(sim_event_mpsc_queue_t *queue);

/* Destroy the MPSC queue (called at simulator shutdown) */
void sim_event_queue_destroy(sim_event_mpsc_queue_t *queue);

/* Initialize the consumer-side min-heap (called once at simulator startup) */
void sim_event_heap_init(sim_event_heap_t *heap);

/* Destroy the min-heap and free all events (called at simulator shutdown) */
void sim_event_heap_destroy(sim_event_heap_t *heap);

/* Producer: Enqueue a new event (called from async I/O threads)
 *
 * This is lock-free and wait-free. Multiple producers can call this concurrently.
 *
 * Parameters:
 *   queue          - The MPSC queue
 *   unit           - The UNIT to activate
 *   event_time     - Time in simulator instruction/cycle units
 *   activate_call  - Activation function to call
 *   check_completion - Completion check function (may be NULL)
 *
 * Returns: true on success, false on allocation failure
 */
bool sim_event_enqueue(sim_event_mpsc_queue_t *queue,
                        UNIT *unit,
                        int32_t event_time,
                        ACTIVATE_API activate_call,
                        void (*check_completion)(UNIT *));

/* Consumer: Batch-dequeue all pending events and insert into the heap
 *
 * This atomically exchanges the queue head with NULL, retrieves all pending
 * events, and inserts them into the min-heap ordered by (event_time, sequence).
 *
 * MUST be called only from the simulator thread.
 *
 * Parameters:
 *   queue - The MPSC queue to dequeue from
 *   heap  - The min-heap to insert events into
 *
 * Returns: Number of events migrated from queue to heap
 */
int sim_event_migrate_to_heap(sim_event_mpsc_queue_t *queue, sim_event_heap_t *heap);

/* Consumer: Process all events in the heap with event_time <= current_time
 *
 * Extracts events from the min-heap in priority order and activates them.
 * Events with later times remain in the heap.
 *
 * MUST be called only from the simulator thread.
 *
 * Parameters:
 *   heap         - The min-heap to process
 *   current_time - Current simulator time (events at or before this time are processed)
 *
 * Returns: Number of events processed
 */
int sim_event_process_heap(sim_event_heap_t *heap, int32_t current_time);

/* Consumer: Peek at the next event without removing it
 *
 * Returns: Pointer to the next event, or NULL if heap is empty
 */
sim_unit_event_t *sim_event_heap_peek(const sim_event_heap_t *heap);

/* Query: Is the heap empty? */
static inline bool sim_event_heap_empty(const sim_event_heap_t *heap)
{
    return heap->count == 0;
}

/* Query: Number of events in the heap */
static inline size_t sim_event_heap_count(const sim_event_heap_t *heap)
{
    return heap->count;
}

/* Query: Get event at specific index (for display/debugging)
 *
 * Returns: Pointer to event at index, or NULL if index >= count
 * Note: This is for read-only access (display). Do not modify the returned event.
 */
static inline sim_unit_event_t *sim_event_heap_get_at(const sim_event_heap_t *heap, size_t index)
{
    if (index >= heap->count)
        return NULL;
    return heap->events[index];
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Internal heap operations (exposed for testing, not part of public API)
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

/* Insert an event into the heap (maintains min-heap property) */
bool sim_event_heap_insert(sim_event_heap_t *heap, sim_unit_event_t *event);

/* Extract the minimum event from the heap (returns NULL if empty) */
sim_unit_event_t *sim_event_heap_extract_min(sim_event_heap_t *heap);

#endif /* SIM_EVENT_QUEUE_H_ */
