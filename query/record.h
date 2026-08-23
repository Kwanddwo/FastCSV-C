#ifndef QUERY_RECORD_H
#define QUERY_RECORD_H

#include "../csv_reader.h"
#include "../arena.h"
#include <stdint.h>

/* Hash a record's contents (used by the DISTINCT dedupe machinery). */
uint64_t hash_record(const CSVRecord *r);

/* Field-by-field equality of two records. */
bool records_equal(const CSVRecord *a, const CSVRecord *b);

/* Deep-copy a record into the arena (fields become arena-owned strings). */
CSVRecord copy_record(CSVRecord *src, Arena *arena);

#endif