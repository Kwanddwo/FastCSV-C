#ifndef QUERY_EXECUTOR_H
#define QUERY_EXECUTOR_H

#include "query.h"
#include "../arena.h"
#include "eval.h"
#include "ast.h"
#include "../csv_config.h"

/* Largest ORDER BY + LIMIT window handled by the bounded top-k heap. Beyond
   this the executor falls back to materializing and sorting every row. */
#define QUERY_TOPK_MAX_K (1 << 16)

/* ===== Shared executor helpers ===== */

/* Output column descriptor (star-expanded). */
typedef struct {
    ExprNode *expr;
    const char *name;
} OutputCol;

/* Grow a dynamically sized array so it holds at least `needed` elements. */
const char* grow_array(QArena *arena, void **arr, int *cap, int needed,
                       size_t elem_size);

/* Allocate from the arena, setting *error on failure. */
void* alloc_or_error(QArena *arena, size_t size, const char **error);

/* Project one row through the output columns (arena-owned record). */
const char* project_row(const OutputCol *out_cols, int out_count, EvalCtx *ctx,
                        CSVRecord **out);

/* Append a record to the result array, growing it as needed. */
const char* append_result(CSVRecord ***records, int *record_count, int *capacity,
                          CSVRecord *rec, QArena *arena);

/* ===== Entry point ===== */
QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, QArena *arena,
                           QArena *tmp, Arena *config_arena);

#endif