#ifndef QUERY_SORT_H
#define QUERY_SORT_H

#include "qarena.h"
#include "../csv_reader.h"
#include "ast.h"
#include "eval.h"
#include "executor.h"
#include <stdbool.h>

/* Compare two entries' ORDER BY keys. Negative means a sorts before b.
   Mirrors the ORDER BY clause: per-key eval_result_compare, flipped for DESC.
   Used both by the qsort path and the top-k heap so their ordering semantics
   (NULLs, type-aware compare, multi-key) are identical. */
int cmp_keys(const EvalResult *a, const EvalResult *b, int k,
             const OrderByItem *order_by);

/* Bounded top-k heap for ORDER BY + LIMIT. Holds at most `cap` projected
   records with their sort keys; the heap root is the worst kept row. */
typedef struct {
    CSVRecord **recs;   /* entry records, size cap */
    EvalResult *keys;   /* entry keys, size cap * key_count */
    int *heap;          /* positional heap over entry indices */
    int cap;            /* == the window size */
    int count;          /* entries stored so far */
    int key_count;
    const OrderByItem *order_by;
} TopK;

const char* topk_init(QArena *arena, TopK *tk, int cap, int key_count,
                      const OrderByItem *order_by);
bool topk_would_keep(const TopK *tk, const EvalResult *keys);
void topk_insert(QArena *arena, TopK *tk, CSVRecord *rec, const EvalResult *keys);
const char* topk_emit(QArena *arena, TopK *tk, CSVRecord ***out, int *out_count);

/* Evaluate ORDER BY keys for one row into the persistent sort-keys array. */
const char* eval_sort_keys(EvalCtx *ctx, const OrderByItem *order_by, int k,
                           EvalResult **sort_keys, int *cap, int idx);

/* Materialize and sort every projected row by the pre-computed keys. */
const char* order_records(CSVRecord ***records, int record_count, int k,
                          const EvalResult *sort_keys, const OrderByItem *order_by,
                          QArena *arena);

/* Apply LIMIT / OFFSET in place. */
void apply_limit_offset(SelectStmt *stmt, CSVRecord **records, int *record_count);

#endif