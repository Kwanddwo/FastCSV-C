#ifndef QUERY_EXECUTOR_H
#define QUERY_EXECUTOR_H

#include "query.h"
#include "eval.h"
#include "ast.h"
#include "../csv_config.h"

/* Largest ORDER BY + LIMIT window handled by the bounded top-k heap. Beyond
   this the executor falls back to materializing and sorting every row, and
   query_estimate_result_size must not cap the arena for it. */
#define QUERY_TOPK_MAX_K (1 << 16)

/* ===== Shared executor helpers ===== */

/* Output column descriptor (star-expanded). */
typedef struct {
    ExprNode *expr;
    const char *name;
} OutputCol;

/* Grow a dynamically sized array so it holds at least `needed` elements. */
const char* grow_array(Arena *arena, void **arr, int *cap, int needed,
                       size_t elem_size);

/* Allocate from the arena, setting *error on failure. */
void* alloc_or_error(Arena *arena, size_t size, const char **error);

/* Project one row through the output columns (arena-owned record). */
const char* project_row(const OutputCol *out_cols, int out_count, EvalCtx *ctx,
                        CSVRecord **out);

/* Append a record to the result array, growing it as needed. */
const char* append_result(CSVRecord ***records, int *record_count, int *capacity,
                          CSVRecord *rec, Arena *arena);

/* ===== Entry point ===== */
QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, Arena *arena, Arena *tmp);

#endif