#include <ctype.h>

int sim_isspace(int c)
{
    return ((c < 0) || (c >= 128)) ? 0 : isspace(c);
}

int sim_islower(int c)
{
    return (c >= 'a') && (c <= 'z');
}

int sim_isupper(int c)
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

int sim_isalpha(int c)
{
    return ((c < 0) || (c >= 128)) ? 0 : isalpha(c);
}

int sim_isprint(int c)
{
    return ((c < 0) || (c >= 128)) ? 0 : isprint(c);
}

int sim_isdigit(int c)
{
    return ((c >= '0') && (c <= '9'));
}

int sim_isgraph(int c)
{
    return ((c < 0) || (c >= 128)) ? 0 : isgraph(c);
}

int sim_isalnum(int c)
{
    return ((c < 0) || (c >= 128)) ? 0 : isalnum(c);
}
