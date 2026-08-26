/* DISTINCT row deduplication: the incremental record set and the
 * post-scan dedupe pass. Split out of executor.c. */
#include "dedupe.h"
#include "record.h"
#include "hash.h"
#include <string.h>

/* Round a size up to the next power of two (hash-table sizing). */
static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
/* ===== Post-processing ===== */

/* Remove duplicate rows in place, keeping first occurrences.
   Expected O(n): each record is hashed once into a precomputed array and
   resolved through an open-addressing table sized for load <= 0.5. */
const char* dedupe_records(CSVRecord ***records, int *record_count, int k,
                                  EvalResult *sort_keys, QArena *arena) {
    int n = *record_count;
    if (n <= 1) return NULL;

    size_t cap = next_pow2((size_t)n * 2);
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(uint64_t) * (size_t)n, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    uint64_t *hashes = (uint64_t*)mem;
    ar = qarena_alloc(arena, sizeof(int) * cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    int *table = (int*)mem;
    for (size_t i = 0; i < cap; i++) table[i] = -1;

    CSVRecord **src = *records;
    for (int i = 0; i < n; i++) {
        hashes[i] = hash_record(src[i]);
    }

    int write_idx = 0;
    for (int i = 0; i < n; i++) {
        uint64_t h = hashes[i];
        size_t slot = (size_t)(h & (uint64_t)(cap - 1));
        bool duplicate = false;
        while (table[slot] != -1) {
            int j = table[slot];
            if (hashes[j] == h && records_equal(src[j], src[i])) {
                duplicate = true;
                break;
            }
            slot = (slot + 1) & (cap - 1);
        }
        if (duplicate) continue;
        src[write_idx] = src[i];
        hashes[write_idx] = h;
        if (sort_keys && k > 0)
            memmove(&sort_keys[write_idx * k], &sort_keys[i * k],
                    (size_t)k * sizeof(EvalResult));
        table[slot] = write_idx;
        write_idx++;
    }
    *record_count = write_idx;
    return NULL;
}

/* ===== Record dedupe set (incremental) ===== */

/* Open-addressing set of distinct records used to dedupe incrementally while
   scanning. Slot values are record indices into the caller's parallel records
   array; hashes holds each distinct record's hash. Kept at load <= 0.5,
   doubling and rehashing on growth. */

const char* record_set_init(QArena *arena, RecordSet *set) {
    set->cap = 32;
    set->count = 0;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)set->cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    set->slots = (int*)mem;
    for (int i = 0; i < set->cap; i++) set->slots[i] = -1;
    ar = qarena_alloc(arena, sizeof(uint64_t) * (size_t)set->cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    set->hashes = (uint64_t*)mem;
    return NULL;
}

static const char* record_set_grow(QArena *arena, RecordSet *set) {
    int new_cap = set->cap * 2;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    int *slots = (int*)mem;
    for (int i = 0; i < new_cap; i++) slots[i] = -1;
    for (int i = 0; i < set->count; i++) {
        uint64_t h = set->hashes[i];
        size_t slot = (size_t)(h & (uint64_t)(new_cap - 1));
        while (slots[slot] != -1) slot = (slot + 1) & (uint64_t)(new_cap - 1);
        slots[slot] = i;
    }
    uint64_t *new_hashes = (uint64_t*)qarena_realloc(
        arena, set->hashes, sizeof(uint64_t) * (size_t)set->cap,
        sizeof(uint64_t) * (size_t)new_cap);
    if (new_hashes == NULL) return "Out of memory.";
    set->hashes = new_hashes;
    set->slots = slots;
    set->cap = new_cap;
    return NULL;
}

/* Add rec to the set if it is not a duplicate of an already-stored record.
   Returns true when stored (at records[count]); false when it was a duplicate
   (with *err unchanged). On allocation failure returns false with *err set. */
bool record_set_add(RecordSet *set, QArena *arena, CSVRecord **records,
                           CSVRecord *rec, const char **err) {
    *err = NULL;
    uint64_t h = hash_record(rec);
    size_t slot = (size_t)(h & (uint64_t)(set->cap - 1));
    while (set->slots[slot] != -1) {
        int j = set->slots[slot];
        if (set->hashes[j] == h && records_equal(records[j], rec))
            return false;
        slot = (slot + 1) & (uint64_t)(set->cap - 1);
    }
    if ((set->count + 1) * 2 > set->cap) {
        const char *e = record_set_grow(arena, set);
        if (e) { *err = e; return false; }
        slot = (size_t)(h & (uint64_t)(set->cap - 1));
        while (set->slots[slot] != -1) slot = (slot + 1) & (uint64_t)(set->cap - 1);
    }
    records[set->count] = rec;
    set->hashes[set->count] = h;
    set->slots[slot] = set->count;
    set->count++;
    return true;
}

/* Sort result rows by the pre-computed ORDER BY keys. */
