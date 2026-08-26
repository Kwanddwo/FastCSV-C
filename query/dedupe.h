#ifndef QUERY_DEDUPE_H
#define QUERY_DEDUPE_H

#include "qarena.h"
#include "../csv_reader.h"
#include "eval.h"
#include <stdbool.h>
#include <stdint.h>

/* Open-addressing set of distinct records used to dedupe incrementally while
   scanning. Slot values are record indices into the caller's parallel records
   array; hashes holds each distinct record's hash. Kept at load <= 0.5,
   doubling and rehashing on growth. */
typedef struct {
    uint64_t *hashes;
    int *slots;
    int cap;     /* power of two */
    int count;   /* distinct records stored */
} RecordSet;

const char* record_set_init(QArena *arena, RecordSet *set);

/* Add rec to the set if it is not a duplicate of an already-stored record.
   Returns true when stored (at records[count]); false when it was a duplicate
   (with *err unchanged). On allocation failure returns false with *err set. */
bool record_set_add(RecordSet *set, QArena *arena, CSVRecord **records,
                    CSVRecord *rec, const char **err);

/* Remove duplicate rows in place, keeping first occurrences.
   Expected O(n): each record is hashed once into a precomputed array and
   resolved through an open-addressing table sized for load <= 0.5. */
const char* dedupe_records(CSVRecord ***records, int *record_count, int k,
                           EvalResult *sort_keys, QArena *arena);

#endif