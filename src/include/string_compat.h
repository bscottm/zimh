// SPDX-FileCopyrightText: The ZIMH Project
// SPDX-License-Identifier: MIT

#ifndef STRING_COMPAT_H_
#    define STRING_COMPAT_H_ 1

/*
 * String compatibility routines that may be missing on several hosts.
 * Do not override fortified libc macros here; tests that need to call the
 * shim directly must undefine those macros locally.
 */

#    include <stddef.h>
#    include <stdbool.h>
#    include <ctype.h>

#    if !defined(HAVE_STRLCPY) && !defined(strlcpy)
size_t strlcpy(char *dst, const char *src, size_t dsize);
#    endif

#    if !defined(HAVE_STRLCAT) && !defined(strlcat)
size_t strlcat(char *dst, const char *src, size_t dsize);
#    endif

#    if !defined(HAVE_STRNLEN) && !defined(strnlen)
#        if !defined(_MSC_VER)
size_t strnlen(const char *s, size_t n);
#        else
#            define strnlen _strnlen_s
#        endif
#    endif

#    if !defined(HAVE_STRDUP) && !defined(strdup)
char *strdup(const char *s);
#    endif

#    if !defined(HAVE_STRNDUP) && !defined(strndup)
char *strndup(const char *s, size_t n);
#    endif

#    if !defined(HAVE_STRCASECMP) && !defined(strcasecmp)
#        if !defined(_MSC_VER)
int strcasecmp(const char *l, const char *r);
#        else
#            define strcasecmp _stricmp
#        endif
#    endif

#    if !defined(HAVE_STRNCASECMP) && !defined(strncasecmp)
#        if !defined(_MSC_VER)
int strncasecmp(const char *l, const char *r, size_t n);
#        else
#            define strncasecmp _strnicmp
#        endif
#    endif

bool sim_isspace(int c);
#    ifdef isspace
#        undef isspace
#    endif
#    define isspace(chr) sim_isspace(chr)

bool sim_islower(int c);
#    ifdef islower
#        undef islower
#    endif
#    define islower(chr) sim_islower(chr)

bool sim_isupper(int c);
#    ifdef isupper
#        undef isupper
#    endif
#    define isupper(chr) sim_isupper(chr)

bool sim_isalpha(int c);
#    ifdef isalpha
#        undef isalpha
#    endif
#    define isalpha(chr) sim_isalpha(chr)

bool sim_isprint(int c);
#    ifdef isprint
#        undef isprint
#    endif
#    define isprint(chr) sim_isprint(chr)

bool sim_isdigit(int c);
#    ifdef isdigit
#        undef isdigit
#    endif
#    define isdigit(chr) sim_isdigit(chr)

bool sim_isgraph(int c);
#    ifdef isgraph
#        undef isgraph
#    endif
#    define isgraph(chr) sim_isgraph(chr)

bool sim_isalnum(int c);
#    ifdef isalnum
#        undef isalnum
#    endif
#    define isalnum(chr) sim_isalnum(chr)

int sim_toupper(int c);
int sim_tolower(int c);

#    ifdef toupper
#        undef toupper
#    endif
#    define toupper(chr) sim_toupper(chr)
#    ifdef tolower
#        undef tolower
#    endif
#    define tolower(chr) sim_tolower(chr)

#endif /* STRING_COMPAT_H_ */
