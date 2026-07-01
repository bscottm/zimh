// SPDX-FileCopyrightText: The ZIMH Project
// SPDX-License-Identifier: MIT

#ifndef STRING_COMPAT_H_
#define STRING_COMPAT_H_ 1

/*
 * String compatibility routines that may be missing on several hosts.
 * Do not override fortified libc macros here; tests that need to call the
 * shim directly must undefine those macros locally.
 */

#include <stddef.h>

#if !defined(HAVE_STRLCPY) && !defined(strlcpy)
size_t strlcpy(char *dst, const char *src, size_t dsize);
#endif

#if !defined(HAVE_STRLCAT) && !defined(strlcat)
size_t strlcat(char *dst, const char *src, size_t dsize);
#endif

#if !defined(HAVE_STRNLEN) && !defined(strnlen)
#if !defined(_MSC_VER)
size_t strnlen(const char *s, size_t n);
#else
#define strnlen _strnlen_s
#endif
#endif

#if !defined(HAVE_STRDUP) && !defined(strdup)
char *strdup(const char *s);
#endif

#if !defined(HAVE_STRNDUP) && !defined(strndup)
char *strndup(const char *s, size_t n);
#endif

#if !defined(HAVE_STRCASECMP) && !defined(strcasecmp)
#if !defined(_MSC_VER)
int strcasecmp(const char *l, const char *r);
#else
#define strcasecmp _stricmp
#endif
#endif

#if !defined(HAVE_STRNCASECMP) && !defined(strncasecmp)
#if !defined(_MSC_VER)
int strncasecmp(const char *l, const char *r, size_t n);
#else
#define strncasecmp _strnicmp
#endif
#endif

#endif /* STRING_COMPAT_H_ */
