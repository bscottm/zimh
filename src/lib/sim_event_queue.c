// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "sim_defs.h"
#include "sim_event_queue.h"
#include "xalloc.h"

/* External reference to async I/O latency (defined in sim_aio.c) */
extern int32_t sim_asynch_inst_latency;

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Constants
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

/* Initial heap capacity (will grow as needed) */
#define INITIAL_HEAP_CAPACITY 16

/* Heap growth factor (multiply capacity by this when full) */
#define HEAP_GROWTH_FACTOR 2

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Helper: Event comparison for heap ordering
 *
 * Returns: true if event a has higher priority than event b (should be processed first)
 *          Ordered by (event_time, sequence) - lower values have higher priority
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

static inline bool event_has_higher_priority(const sim_unit_event_t *a, const sim_unit_event_t *b)
{
    /* First compare by event_time */
    if (a->event_time != b->event_time)
        return a->event_time < b->event_time;

    /* If times are equal, compare by sequence (deterministic ordering) */
    return a->sequence < b->sequence;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * MPSC Queue Operations
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

void sim_event_queue_init(sim_event_mpsc_queue_t *queue)
{
    sim_atomic_ptr_init(&queue->head);
    sim_atomic_init(&queue->sequence);
}

void sim_event_queue_destroy(sim_event_mpsc_queue_t *queue)
{
    /* Drain any remaining events and free them */
    sim_unit_event_t *event = sim_atomic_ptr_exchange(&queue->head, NULL);
    while (event != NULL) {
        sim_unit_event_t *next = event->next;
        free(event);
        event = next;
    }

    sim_atomic_ptr_destroy(&queue->head);
    sim_atomic_destroy(&queue->sequence);
}

bool sim_event_enqueue(sim_event_mpsc_queue_t *queue,
                        UNIT *unit,
                        int32_t event_time,
                        ACTIVATE_API activate_call,
                        void (*check_completion)(UNIT *))
{
    /* Allocate the event structure */
    sim_unit_event_t *event = malloc(sizeof(sim_unit_event_t));
    if (event == NULL) {
        return false;
    }

    /* Fill in the event fields */
    event->unit = unit;
    event->event_time = event_time;
    event->activate_call = activate_call;
    event->check_completion = check_completion;

    /* Atomically grab the next sequence number */
    event->sequence = sim_atomic_fetch_add(&queue->sequence, 1);

    /* Lock-free Treiber stack push using CAS
     *
     * Standard Treiber stack algorithm:
     * 1. Read current head (with ACQUIRE ordering)
     * 2. Set new node's next pointer to current head
     * 3. Try to CAS head from current to new
     * 4. If CAS fails (head changed), retry from step 1
     *
     * Memory ordering: The sim_atomic_ptr_cas uses SEQ_CST ordering, which provides:
     * - RELEASE semantics on success: All writes to the event structure are visible
     *   before the new head pointer becomes visible to the consumer
     * - ACQUIRE semantics: We see the current head value and any writes that happened-before it
     */
    void *expected;
    do {
        expected = sim_atomic_ptr_get(&queue->head);
        event->next = (sim_unit_event_t *)expected;
    } while (!sim_atomic_ptr_cas(&queue->head, &expected, event));

    return true;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * Min-Heap Operations
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

void sim_event_heap_init(sim_event_heap_t *heap)
{
    heap->events = NULL;
    heap->count = 0;
    heap->capacity = 0;
}

void sim_event_heap_destroy(sim_event_heap_t *heap)
{
    /* Free all remaining events */
    for (size_t i = 0; i < heap->count; i++) {
        free(heap->events[i]);
    }

    /* Free the array itself */
    if (heap->events != NULL) {
        free(heap->events);
        heap->events = NULL;
    }

    heap->count = 0;
    heap->capacity = 0;
}

/* Helper: Ensure heap has capacity for at least one more element */
static bool heap_ensure_capacity(sim_event_heap_t *heap)
{
    if (heap->count < heap->capacity) {
        return true; /* Already have space */
    }

    /* Calculate new capacity */
    size_t new_capacity;
    if (heap->capacity == 0) {
        new_capacity = INITIAL_HEAP_CAPACITY;
    } else {
        new_capacity = heap->capacity * HEAP_GROWTH_FACTOR;
    }

    /* Reallocate the array */
    sim_unit_event_t **new_events = xrealloc(heap->events, new_capacity * sizeof(sim_unit_event_t *));
    if (new_events == NULL) {
        return false;
    }

    heap->events = new_events;
    heap->capacity = new_capacity;
    return true;
}

/* Helper: Swap two events in the heap */
static inline void heap_swap(sim_event_heap_t *heap, size_t i, size_t j)
{
    sim_unit_event_t *tmp = heap->events[i];
    heap->events[i] = heap->events[j];
    heap->events[j] = tmp;
}

/* Helper: Bubble up an element to maintain min-heap property */
static void heap_bubble_up(sim_event_heap_t *heap, size_t index)
{
    while (index > 0) {
        size_t parent = (index - 1) / 2;

        /* If parent has higher priority (lower value), we're done */
        if (event_has_higher_priority(heap->events[parent], heap->events[index])) {
            break;
        }

        /* Swap with parent and continue */
        heap_swap(heap, index, parent);
        index = parent;
    }
}

/* Helper: Bubble down an element to maintain min-heap property */
static void heap_bubble_down(sim_event_heap_t *heap, size_t index)
{
    while (true) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;

        /* Find the smallest among parent, left child, and right child */
        if (left < heap->count && event_has_higher_priority(heap->events[left], heap->events[smallest])) {
            smallest = left;
        }
        if (right < heap->count && event_has_higher_priority(heap->events[right], heap->events[smallest])) {
            smallest = right;
        }

        /* If parent is smallest, we're done */
        if (smallest == index) {
            break;
        }

        /* Swap with smallest child and continue */
        heap_swap(heap, index, smallest);
        index = smallest;
    }
}

bool sim_event_heap_insert(sim_event_heap_t *heap, sim_unit_event_t *event)
{
    /* Ensure we have capacity */
    if (!heap_ensure_capacity(heap)) {
        return false;
    }

    /* Insert at the end and bubble up */
    heap->events[heap->count] = event;
    heap_bubble_up(heap, heap->count);
    heap->count++;

    return true;
}

sim_unit_event_t *sim_event_heap_peek(const sim_event_heap_t *heap)
{
    if (heap->count == 0) {
        return NULL;
    }
    return heap->events[0];
}

sim_unit_event_t *sim_event_heap_extract_min(sim_event_heap_t *heap)
{
    if (heap->count == 0) {
        return NULL;
    }

    /* Save the minimum element */
    sim_unit_event_t *min = heap->events[0];

    /* Move the last element to the root and bubble down */
    heap->count--;
    if (heap->count > 0) {
        heap->events[0] = heap->events[heap->count];
        heap_bubble_down(heap, 0);
    }

    return min;
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=
 * High-Level Operations
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

int sim_event_migrate_to_heap(sim_event_mpsc_queue_t *queue, sim_event_heap_t *heap)
{
    int migrated = 0;

    /* Atomically exchange the queue head with NULL
     *
     * Memory ordering: sim_atomic_ptr_exchange uses SEQ_CST ordering, which provides:
     * - ACQUIRE semantics: All writes to events by producers are visible to us
     * - RELEASE semantics: The NULL store is visible to future producers
     */
    sim_unit_event_t *event = sim_atomic_ptr_exchange(&queue->head, NULL);

    /* Walk the chain and insert each event into the heap */
    while (event != NULL) {
        sim_unit_event_t *next = event->next;
        event->next = NULL; /* Hygiene */

        /* Insert into heap */
        if (!sim_event_heap_insert(heap, event)) {
            /* Out of memory - this is fatal for the simulator */
            sim_printf("FATAL: Out of memory while migrating async events to heap\n");
            abort();
        }

        migrated++;
        event = next;
    }

    return migrated;
}

int sim_event_process_heap(sim_event_heap_t *heap, int32_t current_time)
{
    int processed = 0;

    /* Process all events with event_time <= current_time */
    while (true) {
        /* Peek at the next event */
        sim_unit_event_t *event = sim_event_heap_peek(heap);
        if (event == NULL) {
            break; /* Heap is empty */
        }

        /* If the event is in the future, we're done */
        if (event->event_time > current_time) {
            break;
        }

        /* Extract and process this event */
        event = sim_event_heap_extract_min(heap);
        assert(event != NULL); /* We just peeked at it */

        /* Adjust event time (apply latency compensation like the old code did) */
        int32_t adjusted_time = event->event_time;
        if (event->activate_call != &sim_activate_notbefore) {
            adjusted_time -= ((sim_asynch_inst_latency + 1) / 2);
            if (adjusted_time < 0) {
                adjusted_time = 0;
            }
        }

        /* Activate the unit */
        sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev,
                  "Processing async event for %s (seq %llu) after %d %s\n",
                  sim_uname(event->unit), (unsigned long long)event->sequence,
                  adjusted_time, sim_vm_interval_units);

        event->activate_call(event->unit, adjusted_time);

        /* Call completion check if provided */
        if (event->check_completion != NULL) {
            sim_debug(SIM_DBG_AIO_QUEUE, &sim_scp_dev,
                      "Calling completion check for async event on %s\n",
                      sim_uname(event->unit));
            event->check_completion(event->unit);
        }

        /* Free the event */
        free(event);
        processed++;
    }

    return processed;
}
