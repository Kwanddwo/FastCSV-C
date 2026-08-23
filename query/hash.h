#ifndef QUERY_HASH_H
#define QUERY_HASH_H

#include <stddef.h>
#include <stdint.h>

/* FNV-1a hash constants, shared by the value-set, group-key and record
   hashes across the executor modules. */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static inline uint64_t fnv1a(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char*)data;
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static inline uint64_t fnv1a_str(const char *s) {
    return fnv1a(s, s ? strlen(s) : 0);
}

#endif