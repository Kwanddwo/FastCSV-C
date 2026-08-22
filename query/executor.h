#ifndef QUERY_EXECUTOR_H
#define QUERY_EXECUTOR_H

#include "query.h"
#include "ast.h"
#include "../csv_config.h"

/* Largest ORDER BY + LIMIT window handled by the bounded top-k heap. Beyond
   this the executor falls back to materializing and sorting every row, and
   query_estimate_result_size must not cap the arena for it. */
#define QUERY_TOPK_MAX_K (1 << 16)

QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, Arena *arena, Arena *tmp);

#endif
