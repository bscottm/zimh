// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <stdbool.h>

#include "sim_defs.h"
#include "sim_tailq.h"
#include "sim_threads.h"

/* Forward declarations */
static inline sim_tailq_elem_t *tailq_alloc_nodes(sim_tailq_t *tailq, size_t count);
static inline sim_tailq_elem_t *advance_head(sim_tailq_t *tailq);
static inline sim_tailq_elem_t *advance_tail(sim_tailq_t *tailq);
static inline sim_tailq_elem_t *tailq_add_node(sim_tailq_t *tailq);

enum { INITIAL_TAILQ_NODES = 17 };

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Initialization, destruction
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

int sim_tailq_init(sim_tailq_t *tailq)
{
    sim_tailq_elem_t *sentinel;

    sim_atomic_init(&tailq->n_elements);
    sim_atomic_init(&tailq->n_allocated);
    sim_atomic_ptr_init(&tailq->head);
    sim_atomic_ptr_init(&tailq->tail);

    /* Allocate initial circular buffer */
    sentinel = tailq_alloc_nodes(tailq, INITIAL_TAILQ_NODES);
    if (sentinel == NULL)
        return 0;

    /* Both head and tail point to sentinel when empty */
    sim_atomic_ptr_put(&tailq->head, sentinel);
    sim_atomic_ptr_put(&tailq->tail, sentinel);

    sim_atomic_put(&tailq->n_elements, 0);
    return 1;
}

void sim_tailq_destroy(sim_tailq_t *tailq, void (*free_item)(sim_tailq_item_t))
{
    sim_tailq_elem_t *p = (sim_tailq_elem_t *)sim_atomic_ptr_get(&tailq->head);
    sim_tailq_elem_t *sentinel = p;

    if (p == NULL)
        return;

    /* Walk the circular list and free nodes */
    do {
        sim_tailq_elem_t *next = (sim_tailq_elem_t *)sim_atomic_ptr_get(&p->next);

        if (free_item != NULL && p->item != NULL)
            free_item(p->item);

        sim_atomic_destroy(&p->item_status);
        sim_atomic_ptr_destroy(&p->next);

        sim_tailq_elem_t *to_free = p;
        p = next;
        free(to_free);
    } while (p != sentinel);

    sim_atomic_ptr_put(&tailq->head, NULL);
    sim_atomic_ptr_put(&tailq->tail, NULL);

    sim_atomic_destroy(&tailq->n_elements);
    sim_atomic_destroy(&tailq->n_allocated);
    sim_atomic_ptr_destroy(&tailq->head);
    sim_atomic_ptr_destroy(&tailq->tail);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Basic operations
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

sim_tailq_t *sim_tailq_enqueue_xform(sim_tailq_t *tailq, sim_tailq_xform_t xform, void *xform_arg)
{
    sim_tailq_elem_t *current_tail, *tail_next, *head;

    /* SPSC Producer side: Check if queue is full */
    current_tail = (sim_tailq_elem_t *)sim_atomic_ptr_get(&tailq->tail);
    tail_next = (sim_tailq_elem_t *)sim_atomic_ptr_get(&current_tail->next);
    head = (sim_tailq_elem_t *)sim_atomic_ptr_get(&tailq->head);

    if (tail_next == head) {
        /* Queue full, add more nodes */
        if (tailq_add_node(tailq) == NULL) {
            return NULL; /* Allocation failed */
        }
    }

    /* Advance tail to next available slot */
    sim_tailq_elem_t *elem = advance_tail(tailq);

    /* Mark item as BUSY during transformation
     * RELEASE: ensures consumer sees BUSY before tail advances */
    sim_atomic_put_explicit(&elem->item_status, TAILQ_ITEM_BUSY, SIM_ATOMIC_RELEASE);

    /* Transform the item (plain store is safe, protected by BUSY/READY protocol) */
    elem->item = xform(elem->item, xform_arg);

    /* Mark item as READY
     * RELEASE: ensures consumer sees item write before READY */
    sim_atomic_put_explicit(&elem->item_status, TAILQ_ITEM_READY, SIM_ATOMIC_RELEASE);

    return tailq;
}

sim_tailq_item_t sim_tailq_dequeue(sim_tailq_t *tailq)
{
    sim_tailq_item_t item = NULL;
    sim_tailq_elem_t *head, *tail;

    /* SPSC Consumer side: Check if queue is empty
     * ACQUIRE: ensures we see producer's writes */
    head = (sim_tailq_elem_t *)sim_atomic_ptr_get_explicit(&tailq->head, SIM_ATOMIC_ACQUIRE);
    tail = (sim_tailq_elem_t *)sim_atomic_ptr_get_explicit(&tailq->tail, SIM_ATOMIC_ACQUIRE);

    if (head == tail)
        return NULL;

    /* Advance head to dequeue */
    sim_tailq_elem_t *elem = advance_head(tailq);

    /* Spin until item is ready
     * ACQUIRE: ensures we see producer's item write */
    while (sim_atomic_get_explicit(&elem->item_status, SIM_ATOMIC_ACQUIRE) != TAILQ_ITEM_READY) {
        sim_thread_yield();
    }

    /* Retrieve and clear the item */
    item = elem->item;
    elem->item = NULL;

    /* Mark slot as available for reuse
     * RELEASE: ensures producer sees item = NULL before reusing slot */
    sim_atomic_put_explicit(&elem->item_status, TAILQ_ITEM_BUSY, SIM_ATOMIC_RELEASE);

    return item;
}

static inline sim_tailq_item_t identity_item_transform(sim_tailq_item_t ignored, void *new_item)
{
    (void)ignored;
    return (sim_tailq_item_t)new_item;
}

sim_tailq_t *sim_tailq_enqueue(sim_tailq_t *tailq, sim_tailq_item_t item)
{
    return sim_tailq_enqueue_xform(tailq, identity_item_transform, item);
}

/*~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
 * Internal utilities
 *~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=*/

sim_tailq_elem_t *tailq_alloc_nodes(sim_tailq_t *tailq, size_t count)
{
    sim_tailq_elem_t *first = NULL, *last = NULL;
    size_t i;

    for (i = 0; i < count; i++) {
        sim_tailq_elem_t *node = (sim_tailq_elem_t *)malloc(sizeof(sim_tailq_elem_t));

        if (node == NULL) {
            /* Cleanup on allocation failure */
            if (first != NULL) {
                sim_tailq_elem_t *p = first;
                do {
                    sim_tailq_elem_t *next = (sim_tailq_elem_t *)sim_atomic_ptr_get(&p->next);
                    if (next == first)
                        next = NULL;
                    sim_atomic_destroy(&p->item_status);
                    sim_atomic_ptr_destroy(&p->next);
                    free(p);
                    p = next;
                } while (p != NULL);
            }
            return NULL;
        }

        node->item = NULL;
        sim_atomic_init(&node->item_status);
        sim_atomic_put(&node->item_status, TAILQ_ITEM_BUSY); /* New nodes start BUSY */
        sim_atomic_ptr_init(&node->next);

        if (first != NULL) {
            sim_atomic_ptr_put(&last->next, node);
            last = node;
        } else {
            first = last = node;
        }
    }

    /* Make it circular */
    if (last != NULL) {
        sim_atomic_ptr_put(&last->next, first);
    }

    sim_atomic_add(&tailq->n_allocated, (sim_atomic_type_t)count);
    return first;
}

sim_tailq_elem_t *advance_head(sim_tailq_t *tailq)
{
    sim_tailq_elem_t *old_head, *new_head;

    /* SPSC: Only consumer writes head, so no CAS needed
     * ACQUIRE: see producer's writes to the node */
    old_head = (sim_tailq_elem_t *)sim_atomic_ptr_get_explicit(&tailq->head, SIM_ATOMIC_ACQUIRE);
    new_head = (sim_tailq_elem_t *)sim_atomic_ptr_get(&old_head->next);

    /* RELEASE: make our consumption visible to producer */
    sim_atomic_ptr_put_explicit(&tailq->head, new_head, SIM_ATOMIC_RELEASE);

    sim_atomic_dec(&tailq->n_elements);
    return old_head;
}

sim_tailq_elem_t *advance_tail(sim_tailq_t *tailq)
{
    sim_tailq_elem_t *old_tail, *new_tail;

    /* SPSC: Only producer writes tail, so no CAS needed
     * ACQUIRE: see consumer's writes (item = NULL) */
    old_tail = (sim_tailq_elem_t *)sim_atomic_ptr_get_explicit(&tailq->tail, SIM_ATOMIC_ACQUIRE);
    new_tail = (sim_tailq_elem_t *)sim_atomic_ptr_get(&old_tail->next);

    /* RELEASE: make tail advance visible to consumer */
    sim_atomic_ptr_put_explicit(&tailq->tail, new_tail, SIM_ATOMIC_RELEASE);

    sim_atomic_inc(&tailq->n_elements);
    return old_tail;
}

sim_tailq_elem_t *tailq_add_node(sim_tailq_t *tailq)
{
    sim_tailq_elem_t *node, *current_tail, *tail_next;
    void *expected;

    /* Allocate new node */
    node = (sim_tailq_elem_t *)malloc(sizeof(sim_tailq_elem_t));
    if (node == NULL)
        return NULL;

    node->item = NULL;
    sim_atomic_init(&node->item_status);
    sim_atomic_put(&node->item_status, TAILQ_ITEM_BUSY);
    sim_atomic_ptr_init(&node->next);

    /* SPSC: Only producer calls this, but we still need CAS because
     * we're inserting into the ring without moving tail */
    do {
        current_tail = (sim_tailq_elem_t *)sim_atomic_ptr_get(&tailq->tail);
        tail_next = (sim_tailq_elem_t *)sim_atomic_ptr_get(&current_tail->next);

        /* Insert node between current_tail and tail_next */
        sim_atomic_ptr_put(&node->next, tail_next);

        /* Try to update current_tail->next to point to new node */
        expected = tail_next;
        if (sim_atomic_ptr_cas(&current_tail->next, &expected, node)) {
            break; /* Success */
        }

        /* CAS failed - check if tail moved */
        if ((sim_tailq_elem_t *)sim_atomic_ptr_get(&tailq->tail) != current_tail) {
            continue; /* Tail moved, retry */
        }
    } while (1);

    sim_atomic_inc(&tailq->n_allocated);
    return node;
}
