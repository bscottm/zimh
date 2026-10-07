// SPDX-FileCopyrightText: The ZIMH Project
// SPDX-License-Identifier: MIT

/* ctype function shims that only operate on ASCII characters (0 <= c < 128). Considering that the emulators
 * themselves deal principally in ASCII, this makes sense. Otherwise, there could be issues that locale-
 * specific behavior might introduce.
 *
 * These function could be static inlines. Maybe.
 */

#include <stdbool.h>
#include <ctype.h>

bool sim_isspace(int c)
{
    return (c >= 0 && c < 128 && isspace(c) != 0);
}

bool sim_islower(int c)
{
    return (c >= 'a') && (c <= 'z');
}

bool sim_isupper(int c)
{
    return (c >= 'A') && (c <= 'Z');
}

bool sim_isalpha(int c)
{
    return (c >= 0 && c < 128 && isalpha(c) != 0);
}

bool sim_isprint(int c)
{
    return (c >= 0 && c < 128 && isprint(c) != 0);
}

bool sim_isdigit(int c)
{
    return (c >= '0') && (c <= '9');
}

bool sim_isgraph(int c)
{
    return (c >= 0 && c < 128 && isgraph(c) != 0);
}

bool sim_isalnum(int c)
{
    return (c >= 0 && c < 128 && isalnum(c) != 0);
}

int sim_toupper(int c)
{
    return ((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
}

int sim_tolower(int c)
{
    return (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}
