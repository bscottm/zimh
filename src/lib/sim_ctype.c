// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

/* Simplified wrappers around the standard C library ctype functions.
 *
 * NOTE: It's unclear why SIMH needed these wrappers, presumably due to incompatibilities with the standard C
 * library on certain platforms. They are likely not really needed now, but are kept for compatibility until
 * they can be safely removed.
 *
 * The code ensures that the functions only process 7-bit ASCII characters, which may have been the initial
 * intent.
 */

#include <stdbool.h>
#include <ctype.h>

bool sim_isspace(int c)
{
    return ((c < 0) || (c >= 128)) ? false : (isspace(c) != 0);
}

bool sim_islower(int c)
{
    return (c >= 'a') && (c <= 'z');
}

bool sim_isupper(int c)
{
    return (c >= 'A') && (c <= 'Z');
}

int sim_toupper(int c)
{
    return ((c >= 'a') && (c <= 'z')) ? ((c - 'a') + 'A') : c;
}

int sim_tolower(int c)
{
    return ((c >= 'A') && (c <= 'Z')) ? ((c - 'A') + 'a') : c;
}

bool sim_isalpha(int c)
{
    return ((c < 0) || (c >= 128)) ? false : (isalpha(c) != 0);
}

bool sim_isprint(int c)
{
    return ((c < 0) || (c >= 128)) ? false : (isprint(c) != 0);
}

bool sim_isdigit(int c)
{
    return ((c >= '0') && (c <= '9'));
}

bool sim_isgraph(int c)
{
    return ((c < 0) || (c >= 128)) ? false : (isgraph(c) != 0);
}

bool sim_isalnum(int c)
{
    return ((c < 0) || (c >= 128)) ? false : (isalnum(c) != 0);
}
