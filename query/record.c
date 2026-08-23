/* Record-level helpers shared by the executor modules: hashing, equality
 * and deep-copying of CSVRecord values. */
#include "record.h"
#include "hash.h"
#include <string.h>

uint64_t hash_record(const CSVRecord *r) {
    uint64_t h = fnv1a(&r->field_count, sizeof(r->field_count));
    for (size_t q = 0; q < r->field_count; q++) {
        h ^= fnv1a_str(r->fields[q] ? r->fields[q] : "");
        h *= FNV_PRIME;
    }
    return h;
}

bool records_equal(const CSVRecord *a, const CSVRecord *b) {
    if (a->field_count != b->field_count) return false;
    for (size_t q = 0; q < a->field_count; q++) {
        const char *as = a->fields[q] ? a->fields[q] : "";
        const char *bs = b->fields[q] ? b->fields[q] : "";
        if (strcmp(as, bs) != 0) return false;
    }
    return true;
}

CSVRecord copy_record(CSVRecord *src, Arena *arena) {
    CSVRecord out;
    out.field_count = src ? src->field_count : 0;
    out.fields = NULL;
    if (src == NULL || out.field_count == 0) return out;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(char*) * out.field_count, &mem);
    if (ar != ARENA_OK) return out;
    out.fields = (char**)mem;
    for (size_t i = 0; i < out.field_count; i++) {
        out.fields[i] = arena_strdup(arena, src->fields[i] ? src->fields[i] : "");
    }
    return out;
}