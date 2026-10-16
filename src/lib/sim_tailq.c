// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "sim_threads.h"
#include "sim_tailq.h"

/* Default capacity for queue (power of 2) */
enum { DEFAULT_TAILQ_CAPACITY = 256 };

/* Maximum number of enqueue retries when queue is full */
enum { MAX_ENQUEUE_RETRIES = 4096 };

/* Check if value is power of 2 */
static inline bool is_power_of_2(size_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

sim_tailq_t *sim_tailq_init()
{
    return sim_tailq_init_capacity(DEFAULT_TAILQ_CAPACITY);
}

sim_tailq_t *sim_tailq_init_capacity(size_t capacity)
{
    if (capacity < 2 || !is_power_of_2(capacity))
        return NULL;

    sim_tailq_t *tailq = (sim_tailq_t *)calloc(1, sizeof(sim_tailq_t) + capacity * sizeof(sim_tailq_slot_t));

    if (tailq == NULL)
        return NULL;

    /* Note: sim_atomic_init() sets the interior value to 0. */
    sim_atomic_init(&tailq->prod_head);
    sim_atomic_init(&tailq->prod_tail);
    tailq->prod_size = tailq->cons_size = capacity;
    tailq->prod_mask = tailq->cons_mask = capacity - 1;
    sim_atomic_init(&tailq->cons_head);
    sim_atomic_init(&tailq->cons_tail);

    /* Initialize all slots as empty */
    for (size_t i = 0; i < capacity; i++) {
        tailq->slots[i].item = NULL;
    }

    return tailq;
}

void sim_tailq_destroy(sim_tailq_t *tailq, void (*free_item)(sim_tailq_item_t))
{
    if (tailq == NULL)
        return;

    if (free_item != NULL) {
        for (size_t i = 0; i < tailq->prod_size; i++) {
            if (tailq->slots[i].item)
                free_item(tailq->slots[i].item);
        }
    }

    free(tailq);
}

/* Enqueue a new item.
 *
 * Note: The FreeBSD code says that this is multi-producer safe.
 */
sim_tailq_t *sim_tailq_enqueue(sim_tailq_t *tailq, sim_tailq_item_t item)
{
    if (tailq == NULL)
        return NULL;

    size_t mask = tailq->prod_mask;
    sim_atomic_type_t prod_head, prod_next;
    sim_atomic_type_t cons_tail;
    int retries = 0;

    do {
        /* FreeBSD comment (adapted):
         *
         * tailq->prod_head needs to be read before tailq->cons_tail.
         * 
         * If not then we could perform the dequeue and enqueue
         * between reading cons_tail and reading prod_head. This
         * could give us values where cons_head == prod_tail
         * (after masking).
         *
         * To work around this use a load acquire. This is just to
         * ensure ordering within this thread.
         */
        prod_head = (size_t)sim_atomic_get_explicit(&tailq->prod_head, SIM_ATOMIC_ACQUIRE);
        prod_next = prod_head + 1;
        cons_tail = (size_t)sim_atomic_get_explicit(&tailq->cons_tail, SIM_ATOMIC_ACQUIRE);

        /* Check if the queue is now full. */
        if (cons_tail + tailq->prod_size < prod_next + 1) {
            if (++retries < MAX_ENQUEUE_RETRIES) {
                sim_thread_yield();
                continue;
            } else {
                return NULL;
            }
        }
    } while (!sim_atomic_cas(&tailq->prod_head, &prod_head, prod_next));

    size_t prod_idx = ((size_t) prod_head) & mask;
    tailq->slots[prod_idx].item = item;

    /* FreeBSD comment:
     *
     * If there are other enqueues in progress that preceded us, we
     * need to wait for them to complete
     */
    while (sim_atomic_get(&tailq->prod_tail) != prod_head)
        sim_thread_yield();

    sim_atomic_put_explicit(&tailq->prod_tail, prod_next, SIM_ATOMIC_RELEASE);

    return tailq;
}

/* Single-consumer dequeue */
sim_tailq_item_t sim_tailq_dequeue(sim_tailq_t *tailq)
{
    if (tailq == NULL)
        return NULL;

    size_t mask = tailq->cons_mask;
    sim_atomic_type_t cons_head = sim_atomic_get(&tailq->cons_head);
    sim_atomic_type_t prod_tail = sim_atomic_get_explicit(&tailq->prod_tail, SIM_ATOMIC_ACQUIRE);

    if (cons_head == prod_tail)
        return NULL;

    sim_atomic_type_t cons_next = cons_head + 1;
    sim_atomic_put(&tailq->cons_head, cons_next);

    size_t cons_idx = ((size_t)cons_head) & mask;
    sim_tailq_item_t item = tailq->slots[cons_idx].item;

    sim_atomic_put_explicit(&tailq->cons_tail, cons_next, SIM_ATOMIC_RELEASE);

    return item;
}
