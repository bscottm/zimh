/* test_sim_events.c: Unit tests for non-intrusive event heap */
// SPDX-FileCopyrightText: 2024 B. Scott Michel
// SPDX-License-Identifier: X11

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_cmocka.h"

#include "scp.h"
#include "sim_events.h"
#include "test_simh_personality.h"
#include "test_support.h"

/* Test fixture */

struct event_heap_fixture {
    UNIT units[10];
    DEVICE device;
};

static int setup_event_heap(void **state)
{
    struct event_heap_fixture *fixture;
    DEVICE *devices[2];

    fixture = calloc(1, sizeof(*fixture));
    assert_non_null(fixture);

    /* Initialize test device with 10 units */
    fixture->device.name = "TST";
    fixture->device.units = fixture->units;
    fixture->device.numunits = 10;

    for (size_t i = 0; i < 10; i++) {
        fixture->units[i].dptr = &fixture->device;
    }

    devices[0] = &fixture->device;
    devices[1] = NULL;
    assert_int_equal(simh_test_install_devices("zimh-event-heap", devices), 0);

    /* Initialize event heap */
    assert_int_equal(sim_event_heap_init(), SCPE_OK);

    *state = fixture;
    return 0;
}

static int teardown_event_heap(void **state)
{
    struct event_heap_fixture *fixture = *state;

    sim_event_heap_cleanup();
    free(fixture);
    *state = NULL;
    return 0;
}

/* Test: Initialization creates empty heap */

static void test_heap_init_creates_empty_heap(void **state)
{
    (void)state; /* unused */

    assert_int_equal(sim_event_heap_count(), 0);
    assert_null(sim_event_heap_peek());
    assert_null(sim_event_heap_pop());
}

/* Test: Insert single event and pop it */

static void test_heap_insert_single_event_and_pop(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *event;
    UNIT_EVENT *popped;

    event = calloc(1, sizeof(UNIT_EVENT));
    assert_non_null(event);

    event->unit = &fixture->units[0];
    event->fire_time = 100;
    event->is_timer_event = false;

    assert_int_equal(sim_event_heap_insert(event), SCPE_OK);
    assert_int_equal(sim_event_heap_count(), 1);

    /* Peek doesn't remove */
    popped = sim_event_heap_peek();
    assert_non_null(popped);
    assert_ptr_equal(popped, event);
    assert_int_equal(sim_event_heap_count(), 1);

    /* Pop removes */
    popped = sim_event_heap_pop();
    assert_non_null(popped);
    assert_ptr_equal(popped, event);
    assert_int_equal(sim_event_heap_count(), 0);
    assert_null(sim_event_heap_peek());

    free(event);
}

/* Test: Insert multiple events, pop in order */

static void test_heap_pops_events_in_fire_time_order(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[5];
    uint64_t fire_times[] = {500, 100, 300, 200, 400};
    uint64_t expected_order[] = {100, 200, 300, 400, 500};

    /* Insert events in random order */
    for (size_t i = 0; i < 5; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        assert_non_null(events[i]);

        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = fire_times[i];
        events[i]->is_timer_event = false;

        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    assert_int_equal(sim_event_heap_count(), 5);

    /* Pop events - should come out in fire_time order */
    for (size_t i = 0; i < 5; i++) {
        UNIT_EVENT *popped = sim_event_heap_pop();
        assert_non_null(popped);
        assert_int_equal(popped->fire_time, expected_order[i]);
        free(popped);
    }

    assert_int_equal(sim_event_heap_count(), 0);
}

/* Test: Heap maintains min-heap property */

static void test_heap_maintains_min_heap_property(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[20];

    /* Insert 20 events with random fire times */
    for (size_t i = 0; i < 20; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        assert_non_null(events[i]);

        events[i]->unit = &fixture->units[i % 10];
        events[i]->fire_time = (uint64_t)(rand() % 10000);
        events[i]->is_timer_event = false;

        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);

        /* Validate heap property after each insertion */
        assert_true(sim_event_heap_validate());
    }

    /* Clean up */
    while (sim_event_heap_count() > 0) {
        UNIT_EVENT *popped = sim_event_heap_pop();
        assert_non_null(popped);
        free(popped);
    }
}

/* Test: Remove event from middle of heap */

static void test_heap_remove_from_middle_maintains_property(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[10];
    uint64_t fire_times[] = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};

    /* Insert 10 events */
    for (size_t i = 0; i < 10; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        assert_non_null(events[i]);

        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = fire_times[i];
        events[i]->is_timer_event = false;

        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    assert_int_equal(sim_event_heap_count(), 10);

    /* Remove event from middle (fire_time = 500) */
    size_t middle_index = events[4]->heap_index;
    assert_int_equal(sim_event_heap_remove_at(middle_index), SCPE_OK);
    free(events[4]);
    events[4] = NULL;

    assert_int_equal(sim_event_heap_count(), 9);
    assert_true(sim_event_heap_validate());

    /* Verify remaining events pop in correct order (skipping 500) */
    uint64_t expected[] = {100, 200, 300, 400, 600, 700, 800, 900, 1000};
    for (size_t i = 0; i < 9; i++) {
        UNIT_EVENT *popped = sim_event_heap_pop();
        assert_non_null(popped);
        assert_int_equal(popped->fire_time, expected[i]);
        free(popped);
    }
}

/* Test: Remove first event (min) */

static void test_heap_remove_first_event(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[5];
    uint64_t fire_times[] = {100, 200, 300, 400, 500};

    for (size_t i = 0; i < 5; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = fire_times[i];
        events[i]->is_timer_event = false;
        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    /* Remove first (fire_time = 100) */
    assert_int_equal(events[0]->heap_index, 0);
    assert_int_equal(sim_event_heap_remove_at(0), SCPE_OK);
    free(events[0]);

    assert_int_equal(sim_event_heap_count(), 4);
    assert_true(sim_event_heap_validate());

    /* Next minimum should be 200 */
    UNIT_EVENT *next = sim_event_heap_peek();
    assert_non_null(next);
    assert_int_equal(next->fire_time, 200);

    /* Clean up */
    while (sim_event_heap_count() > 0)
        free(sim_event_heap_pop());
}

/* Test: Remove last event */

static void test_heap_remove_last_event(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[5];
    uint64_t fire_times[] = {100, 200, 300, 400, 500};

    for (size_t i = 0; i < 5; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = fire_times[i];
        events[i]->is_timer_event = false;
        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    /* Find and remove the event with fire_time = 500 */
    size_t last_index = events[4]->heap_index;
    assert_int_equal(sim_event_heap_remove_at(last_index), SCPE_OK);
    free(events[4]);

    assert_int_equal(sim_event_heap_count(), 4);
    assert_true(sim_event_heap_validate());

    /* Clean up */
    while (sim_event_heap_count() > 0)
        free(sim_event_heap_pop());
}

/* Test: Heap index tracking */

static void test_heap_maintains_correct_indices(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[10];

    /* Insert events */
    for (size_t i = 0; i < 10; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = (uint64_t)(i * 100);
        events[i]->is_timer_event = false;
        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    /* Verify all events have valid heap_index */
    for (size_t i = 0; i < 10; i++) {
        uint32_t idx = events[i]->heap_index;
        assert_true(idx < sim_event_heap_count());
    }

    /* Validate heap structure */
    assert_true(sim_event_heap_validate());

    /* Clean up */
    while (sim_event_heap_count() > 0)
        free(sim_event_heap_pop());
}

/* Test: Insert many events to trigger growth */

static void test_heap_grows_when_capacity_exceeded(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT **events;
    size_t num_events = 200; /* More than INITIAL_HEAP_CAPACITY (128) */

    events = calloc(num_events, sizeof(UNIT_EVENT *));
    assert_non_null(events);

    /* Insert 200 events */
    for (size_t i = 0; i < num_events; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        assert_non_null(events[i]);

        events[i]->unit = &fixture->units[i % 10];
        events[i]->fire_time = (uint64_t)(rand() % 100000);
        events[i]->is_timer_event = false;

        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    assert_int_equal(sim_event_heap_count(), num_events);
    assert_true(sim_event_heap_validate());

    /* Pop all events and verify ordering */
    uint64_t last_time = 0;
    for (size_t i = 0; i < num_events; i++) {
        UNIT_EVENT *popped = sim_event_heap_pop();
        assert_non_null(popped);
        assert_true(popped->fire_time >= last_time);
        last_time = popped->fire_time;
        free(popped);
    }

    assert_int_equal(sim_event_heap_count(), 0);
    free(events);
}

/* Test: Now time tracking */

static void test_heap_tracks_current_time(void **state)
{
    (void)state; /* unused */

    assert_int_equal(sim_event_heap_now(), 0);

    sim_event_heap_set_now(12345);
    assert_int_equal(sim_event_heap_now(), 12345);

    sim_event_heap_set_now(67890);
    assert_int_equal(sim_event_heap_now(), 67890);
}

/* Test: Empty heap operations */

static void test_empty_heap_operations_are_safe(void **state)
{
    (void)state; /* unused */

    /* Clean heap first */
    while (sim_event_heap_count() > 0)
        free(sim_event_heap_pop());

    assert_int_equal(sim_event_heap_count(), 0);
    assert_null(sim_event_heap_peek());
    assert_null(sim_event_heap_pop());

    /* Remove from invalid index should fail gracefully */
    assert_int_not_equal(sim_event_heap_remove_at(0), SCPE_OK);
    assert_int_not_equal(sim_event_heap_remove_at(100), SCPE_OK);
}

/* Test: Duplicate fire times */

static void test_heap_handles_duplicate_fire_times(void **state)
{
    struct event_heap_fixture *fixture = *state;
    UNIT_EVENT *events[5];

    /* Insert 5 events with same fire time */
    for (size_t i = 0; i < 5; i++) {
        events[i] = calloc(1, sizeof(UNIT_EVENT));
        events[i]->unit = &fixture->units[i];
        events[i]->fire_time = 1000; /* all the same */
        events[i]->is_timer_event = false;
        assert_int_equal(sim_event_heap_insert(events[i]), SCPE_OK);
    }

    assert_int_equal(sim_event_heap_count(), 5);
    assert_true(sim_event_heap_validate());

    /* Pop all - order doesn't matter since times are equal */
    for (size_t i = 0; i < 5; i++) {
        UNIT_EVENT *popped = sim_event_heap_pop();
        assert_non_null(popped);
        assert_int_equal(popped->fire_time, 1000);
        free(popped);
    }
}

/* Test suite */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_heap_init_creates_empty_heap,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_insert_single_event_and_pop,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_pops_events_in_fire_time_order,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_maintains_min_heap_property,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_remove_from_middle_maintains_property,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_remove_first_event,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_remove_last_event,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_maintains_correct_indices,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_grows_when_capacity_exceeded,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_tracks_current_time,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_empty_heap_operations_are_safe,
                                       setup_event_heap, teardown_event_heap),
        cmocka_unit_test_setup_teardown(test_heap_handles_duplicate_fire_times,
                                       setup_event_heap, teardown_event_heap),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
