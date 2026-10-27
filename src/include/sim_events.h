/* sim_events.h: Non-intrusive event queue public API */
// SPDX-FileCopyrightText: 2024 B. Scott Michel
// SPDX-License-Identifier: X11

#ifndef SIM_EVENTS_H_
#define SIM_EVENTS_H_

#include "sim_defs.h"

/* Initialize the event heap

   Call once at simulator startup. Allocates the initial heap structure
   with INITIAL_HEAP_CAPACITY slots. Returns SCPE_OK on success, SCPE_MEM
   on allocation failure.
*/
t_stat sim_event_heap_init(void);

/* Event heap cleanup */

/* Clean up the event heap - call at simulator shutdown */
void sim_event_heap_cleanup(void);

/* Event heap operations */

/* Insert an event into the heap - O(log n) complexity */
t_stat sim_event_heap_insert(UNIT_EVENT *event);

/* Remove and return the earliest-firing event - O(log n) complexity
   Returns NULL if heap is empty. Caller takes ownership of returned event. */
UNIT_EVENT *sim_event_heap_pop(void);

/* Peek at the earliest-firing event without removing it - O(1) complexity
   Returns NULL if heap is empty. Heap retains ownership. */
UNIT_EVENT *sim_event_heap_peek(void);

/* Remove an event from an arbitrary position - O(log n) complexity
   Used by sim_cancel() to remove a specific event by heap_index. */
t_stat sim_event_heap_remove_at(size_t index);

/* Query functions */

/* Return the number of events currently in the heap - O(1) complexity */
size_t sim_event_heap_count(void);

/* Find event by unit pointer - O(n) complexity, used for cancellation */
UNIT_EVENT *sim_event_heap_find_by_unit(const UNIT *uptr);

/* Get the current instruction time maintained by the event system */
uint64_t sim_event_heap_now(void);

/* Set the current instruction time (called by sim_process_event) */
void sim_event_heap_set_now(uint64_t time);

/* Debug validation */

/* Validate heap invariant - returns true if heap property holds
   Use in assertions during development: assert(sim_event_heap_validate()); */
bool sim_event_heap_validate(void);

/* Integration layer functions (Phase 4A) */

/* Heap-based replacements for core event functions */
t_stat _sim_activate_heap(UNIT *uptr, int32_t event_time);
t_stat sim_cancel_heap(UNIT *uptr);
t_stat sim_process_event_heap(void);

#endif /* SIM_EVENTS_H_ */
