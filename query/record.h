#ifndef QUERY_RECORD_H
#define QUERY_RECORD_H

#include "../csv_reader.h"
#include "qarena.h"
#include <stdint.h>

/* Contract: a CSVRecord's field pointers are never NULL — empty and absent
   fields are stored as "". hash_record and records_equal therefore treat a
   NULL pointer (defensive) identically to "": the projection layer renders
   both as an empty field, so a distinct NULL bucket could never occur and
   would only make hashing inconsistent with equality. */

/* Hash a record's contents (used by the DISTINCT dedupe machinery). */
uint64_t hash_record(const CSVRecord *r);

/* Field-by-field equality of two records. */
bool records_equal(const CSVRecord *a, const CSVRecord *b);

/* Deep-copy a record into the arena (fields become arena-owned strings). */
CSVRecord copy_record(CSVRecord *src, QArena *arena);

#endif