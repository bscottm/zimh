// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "simnetwork/eth_test/eth_test.h"

static ETH_TEST_BACKEND *eth_test_backends = NULL;

enum {
    ETH_TEST_QUEUE_MAX = 256
};

/* Open a named test backend and return the backend handle to sim_ether.c. */
t_stat eth_test_open(const char *devname, ETH_DEV *dev, char *savname, size_t savname_size)
{
    ETH_TEST_BACKEND *test_backend;
    t_stat status = eth_test_get_backend(name, &test_backend);

    if (status != SCPE_OK)
        return status;

    eth_backend_t *backend;

    if ((backend = (eth_backend_t *)calloc(1, sizeof(*backend))) == NULL)
        return SCPE_MEM;

    backend->eth_api = ETH_API_TEST;
    backend->packet_wait = eth_wait_test;
    backend->packet_read = eth_reader_test;
    backend->before_packet_write = NULL;
    backend->write_packet = eth_writer_test;
    backend->after_packet_write = NULL;
    backend->state.test_backend = test_backend;

    dev->backend = backend;

    return SCPE_OK;
}

/*
 * Unit tests drive this backend synchronously. Callers that use these helpers
 * while a simulator is running must serialize access around the helper calls.
 */

/* Return the named test backend, or NULL if it has not been created yet. */
ETH_TEST_BACKEND *eth_test_find_backend(const char *name)
{
    ETH_TEST_BACKEND *backend;

    for (backend = eth_test_backends; backend; backend = backend->next)
        if (!strcmp(backend->name, name))
            return backend;
    return NULL;
}

/* Return an existing named backend, creating its queues on first use. */
t_stat eth_test_get_backend(const char *name, ETH_TEST_BACKEND **backend)
{
    ETH_TEST_BACKEND *new_backend;
    t_stat status;

    if (!name || !*name || !backend)
        return SCPE_ARG;

    *backend = eth_test_find_backend(name);
    if (*backend)
        return SCPE_OK;

    new_backend = (ETH_TEST_BACKEND *)calloc(1, sizeof(*new_backend));
    if (!new_backend)
        return SCPE_MEM;

    new_backend->name = strdup(name);
    if (!new_backend->name) {
        free(new_backend);
        return SCPE_MEM;
    }

    status = ethq_init(&new_backend->rx_to_guest, ETH_TEST_QUEUE_MAX);
    if (status == SCPE_OK)
        status = ethq_init(&new_backend->tx_from_guest, ETH_TEST_QUEUE_MAX);
    if (status != SCPE_OK) {
        ethq_destroy(&new_backend->rx_to_guest);
        ethq_destroy(&new_backend->tx_from_guest);
        free(new_backend->name);
        free(new_backend);
        return status;
    }

    new_backend->next = eth_test_backends;
    eth_test_backends = new_backend;
    *backend = new_backend;
    return SCPE_OK;
}
