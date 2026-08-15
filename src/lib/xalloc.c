/* xalloc.c: fatal allocation wrappers */
// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#include "xalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

/* Report an allocation contract violation and abort execution. */
static void xalloc_abort(const char *func, const char *reason)
{
    fprintf(stderr, "%s: %s\n", func, reason);
    abort();
}

/* Allocate memory or abort execution. */
void *xmalloc(size_t size)
{
    void *ptr;

    if (size == 0)
        xalloc_abort("xmalloc", "zero-byte allocation request");
    ptr = malloc(size);
    if (ptr == NULL)
        xalloc_abort("xmalloc", "out of memory");
    return ptr;
}

/* Allocate zero-filled memory or abort execution. */
void *xcalloc(size_t count, size_t size)
{
    void *ptr;

    if ((count == 0) || (size == 0))
        xalloc_abort("xcalloc", "zero-byte allocation request");
    if (count > SIZE_MAX / size)
        xalloc_abort("xcalloc", "allocation size overflow");
    ptr = calloc(count, size);
    if (ptr == NULL)
        xalloc_abort("xcalloc", "out of memory");
    return ptr;
}

/* Resize an allocation or abort execution. */
void *xrealloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (size == 0)
        xalloc_abort("xrealloc", "zero-byte allocation request");
    new_ptr = realloc(ptr, size);
    if (new_ptr == NULL)
        xalloc_abort("xrealloc", "out of memory");
    return new_ptr;
}

/* Duplicate a NUL-terminated string or abort execution. */
char *xstrdup(const char *str)
{
    size_t len;
    char *copy;

    if (str == NULL)
        xalloc_abort("xstrdup", "NULL string");
    len = strlen(str) + 1;
    copy = (char *)xmalloc(len);
    memcpy(copy, str, len);
    return copy;
}

/* Duplicate at most max_len bytes of a NUL-terminated string or abort. */
char *xstrndup(const char *str, size_t max_len)
{
    size_t len;
    char *copy;

    if (str == NULL)
        xalloc_abort("xstrndup", "NULL string");
    len = 0;
    while ((len < max_len) && (str[len] != '\0'))
        ++len;
    copy = (char *)xmalloc(len + 1);
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

/*
 * Allocate isolated memory in a separate address space region.
 * On Unix: anonymous mmap() with MAP_PRIVATE | MAP_ANON
 * On Windows: VirtualAlloc() with MEM_COMMIT | MEM_RESERVE
 * Returns NULL on failure. Memory is zero-filled and page-aligned.
 */
void *xmalloc_isolated(size_t size)
{
    void *ptr;

    if (size == 0)
        return NULL;

#if defined(_WIN32)
    ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (ptr == NULL)
        return NULL;
#else
    /* MAP_ANON is standard BSD (macOS, *BSD); MAP_ANONYMOUS is Linux */
# if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#  define MAP_ANONYMOUS MAP_ANON
# endif
    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
#endif

    return ptr;
}

/*
 * Free memory allocated by xmalloc_isolated().
 * No-op if ptr is NULL.
 */
void xfree_isolated(void *ptr, size_t size)
{
    if (ptr == NULL)
        return;

#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}
