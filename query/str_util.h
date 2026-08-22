#ifndef STR_UTIL_H
#define STR_UTIL_H

/* Portable case-insensitive string comparison.
 *
 * strcasecmp/strncasecmp are POSIX / XSI and are not available everywhere
 * (notably MSVC), so these helpers are implemented with only C99 functions
 * (tolower over unsigned char). Include this header instead of relying on
 * <strings.h> or feature-test macros. */

#include <stddef.h>
#include <ctype.h>

/* Case-insensitive equality of two NUL-terminated strings. */
static inline int str_ieq(const char *a, const char *b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Case-insensitive equality of the first n characters of two strings.
 * Mirrors strncasecmp(a, b, n) == 0. */
static inline int str_nieq(const char *a, const char *b, size_t n) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

#endif