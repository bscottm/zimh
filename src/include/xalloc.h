/* xalloc.h: fatal allocation wrappers */
// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#ifndef XALLOC_H_
#define XALLOC_H_ 1

#include <stddef.h>

/*
 * Allocate memory or abort execution. A zero-byte request is treated as a
 * caller error and aborts.
 */
void *xmalloc(size_t size);

/*
 * Allocate zero-filled memory or abort execution. A zero-sized element count
 * or element size is treated as a caller error and aborts.
 */
void *xcalloc(size_t count, size_t size);

/*
 * Resize an allocation or abort execution. A zero-byte request is treated as a
 * caller error and aborts.
 */
void *xrealloc(void *ptr, size_t size);

/* Duplicate a NUL-terminated string or abort execution. */
char *xstrdup(const char *str);

/*
 * Duplicate at most max_len bytes of a NUL-terminated string or abort. A
 * max_len of zero is valid and returns an owned empty string.
 */
char *xstrndup(const char *str, size_t max_len);

/*
 * Allocate isolated memory in a separate address space region. On Unix, uses
 * anonymous mmap(); on Windows, uses VirtualAlloc(). This provides stronger
 * isolation than malloc — stray pointers from the simulator cannot corrupt
 * this memory, and guard pages can catch out-of-bounds accesses.
 *
 * Returns NULL on failure. The returned memory is zero-filled and page-aligned.
 */
void *xmalloc_isolated(size_t size);

/*
 * Free memory allocated by xmalloc_isolated(). No-op if ptr is NULL.
 */
void xfree_isolated(void *ptr, size_t size);

#endif
