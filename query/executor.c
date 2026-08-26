/* Main execution flow: opens the CSV, validates the statement, builds the
 * output columns and runs the row scan through the WHERE / GROUP BY / top-k /
 * DISTINCT / ORDER BY / LIMIT paths. The heavy lifting lives in eval.c,
 * aggregate.c, sort.c, dedupe.c and validate.c. */
#include "executor.h"
#include "../arena.h"
#include "eval.h"
#include "aggregate.h"
#include "sort.h"
#include "dedupe.h"
#include "validate.h"
#include "record.h"
#include "str_util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Initial capacities for growable arrays (doubled on demand). */
static const int GROW_INITIAL_CAPACITY = 64;   /* result records / sort keys */
static const int GROUP_INITIAL_CAPACITY = 16;  /* aggregation groups */

/* ===== Growable array helpers ===== */

/* Grow a dynamically sized array so it holds at least `needed` elements. */
const char* grow_array(QArena *arena, void **arr, int *cap, int needed,
                              size_t elem_size) {
    if (needed <= *cap) return NULL;
    int new_cap = *cap ? *cap * 2 : GROW_INITIAL_CAPACITY;
    while (new_cap < needed) new_cap *= 2;
    void *mem = qarena_realloc(arena, *arr, elem_size * (size_t)*cap,
                              elem_size * (size_t)new_cap);
    if (mem == NULL) return "Out of memory.";
    *arr = mem;
    *cap = new_cap;
    return NULL;
}

/* Allocate from the arena, setting *error on failure. */
void* alloc_or_error(QArena *arena, size_t size, const char **error) {
    void *mem;
    QArenaResult ar = qarena_alloc(arena, size, &mem);
    if (ar != QARENA_OK) {
        *error = "Out of memory.";
        return NULL;
    }
    return mem;
}

/* ===== Row processing helpers ===== */

const char* project_row(const OutputCol *out_cols, int out_count,
                               EvalCtx *ctx, CSVRecord **out) {
    void *mem;
    QArenaResult ar = qarena_alloc(ctx->arena, sizeof(CSVRecord), &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    CSVRecord *proj = (CSVRecord*)mem;

    ar = qarena_alloc(ctx->arena, sizeof(char*) * (size_t)out_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    proj->fields = (char**)mem;
    proj->field_count = (size_t)out_count;

    for (int i = 0; i < out_count; i++) {
        /* A bare column reference projects its raw cell text verbatim. The
           field value and the display value are distinct: cells like '05',
           '3.50' or '1e3' must round-trip unchanged, and an empty cell is an
           empty field, not the literal text 'NULL'. Typing happens only when
           a computation actually needs a value (arithmetic, comparisons,
           aggregates), which routes through eval_expr below. */
        ExprNode *expr = out_cols[i].expr;
        if (expr->type == EXPR_COLUMN_REF && expr->col_index >= 0) {
            const char *raw = "";
            if ((size_t)expr->col_index < ctx->record->field_count &&
                ctx->record->fields[expr->col_index] != NULL)
                raw = ctx->record->fields[expr->col_index];
            char *s = qarena_strdup(ctx->arena, raw);
            if (s == NULL) return "Out of memory.";
            proj->fields[i] = s;
            continue;
        }

        EvalResult er = eval_expr(expr, ctx);
        if (eval_result_is_error(&er)) return er.error;
        char *s = (char*)eval_result_dup_to_arena(&er, ctx->arena);
        if (s == NULL) return "Out of memory.";
        proj->fields[i] = s;
    }
    *out = proj;
    return NULL;
}

/* Append a projected record to the result, growing as needed. */
const char* append_result(CSVRecord ***records, int *record_count, int *capacity,
                                 CSVRecord *proj, QArena *arena) {
    const char *err = grow_array(arena, (void**)records, capacity, *record_count + 1,
                                 sizeof(CSVRecord*));
    if (err) return err;
    (*records)[(*record_count)++] = proj;
    return NULL;
}


/* ===== Statement validation ===== */

/* Does the path already carry a CSV extension? Used to decide whether the
   "<name>.csv" fallback may be tried: a name like "data.tsv" or "STUDENTS.CSV"
   is treated as an explicit file and never gets ".csv" appended. */
static bool has_csv_extension(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;   /* ".csv" plus at least one char */
    if (name[len - 4] != '.') return false;
    return (name[len - 3] == 'c' || name[len - 3] == 'C') &&
           (name[len - 2] == 's' || name[len - 2] == 'S') &&
           (name[len - 1] == 'v' || name[len - 1] == 'V');
}

static const char* open_reader(CSVConfig *config, SelectStmt *stmt, QArena *arena,
                               Arena *config_arena,
                               CSVReader **out_reader, char ***out_headers,
                               int *out_header_count) {
    if (stmt->table_name == NULL || stmt->table_name[0] == '\0') {
        return "No table specified in FROM clause.";
    }

    /* The reader keeps this config (and its path string) for the whole scan,
       so it must live in the library's own arena, not the query arena. */
    CSVConfig *cfg_copy = csv_config_copy(config_arena, config);
    if (cfg_copy == NULL) return "Out of memory.";

    /* Exact path first; on a miss, retry with ".csv" appended when the name
       has no extension (FROM "students" -> students.csv). This keeps every
       existing explicit path (data.tsv, my data.csv) resolving identically,
       and a real no-extension file still wins over its ".csv" sibling. */
    csv_config_set_path(cfg_copy, stmt->table_name);
    CSVReader *reader = csv_reader_init_standalone(cfg_copy);
    if (reader == NULL && !has_csv_extension(stmt->table_name)) {
        size_t len = strlen(stmt->table_name);
        void *mem;
        QArenaResult ar = qarena_alloc(arena, len + 5, &mem);
        if (ar != QARENA_OK) return "Out of memory.";
        char *candidate = (char*)mem;
        memcpy(candidate, stmt->table_name, len);
        memcpy(candidate + len, ".csv", 5);
        csv_config_set_path(cfg_copy, candidate);
        reader = csv_reader_init_standalone(cfg_copy);
    }

    if (reader == NULL) {
        char buf[256];
        if (has_csv_extension(stmt->table_name)) {
            snprintf(buf, sizeof(buf), "Failed to open '%s'.", stmt->table_name);
        } else {
            snprintf(buf, sizeof(buf), "Failed to open '%s' (also tried '%s.csv').",
                     stmt->table_name, stmt->table_name);
        }
        char *msg = qarena_strdup(arena, buf);
        return msg ? msg : "Failed to open file.";
    }

    int header_count = 0;
    char **headers = csv_reader_get_headers(reader, &header_count);
    if (headers == NULL || header_count == 0) {
        *out_reader = reader;
        return "CSV file has no headers.";
    }

    *out_reader = reader;
    *out_headers = headers;
    *out_header_count = header_count;
    return NULL;
}

/* Validate column references, DISTINCT usage, and WHERE aggregates. */

/* Build the output column descriptors, expanding '*' to one per header. */
static const char* build_output_cols(SelectStmt *stmt, char **headers, int header_count,
                                     QArena *arena, OutputCol **out_cols, int *out_count) {
    int count = 0;
    for (int i = 0; i < stmt->item_count; i++) {
        if (stmt->items[i].expr->type == EXPR_STAR) {
            count += header_count;
        } else {
            count++;
        }
    }

    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(OutputCol) * (size_t)count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    OutputCol *cols = (OutputCol*)mem;
    int idx = 0;

    for (int i = 0; i < stmt->item_count; i++) {
        SelectItem *item = &stmt->items[i];
        if (item->expr->type == EXPR_STAR) {
            for (int j = 0; j < header_count; j++) {
                cols[idx].name = headers[j];
                cols[idx].expr = make_column_ref_node(arena, headers[j]);
                if (cols[idx].expr == NULL) return "Out of memory.";
                cols[idx].expr->col_index = j;
                idx++;
            }
        } else {
            cols[idx].name = item->name ? item->name : "";
            cols[idx].expr = item->expr;
            idx++;
        }
    }

    *out_cols = cols;
    *out_count = count;
    return NULL;
}

static const char* set_result_headers(QueryResult *result, OutputCol *out_cols,
                                      int out_count, QArena *arena) {
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(char*) * (size_t)out_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    result->headers = (char**)mem;
    result->header_count = out_count;

    for (int i = 0; i < out_count; i++) {
        result->headers[i] = qarena_strdup(arena, out_cols[i].name);
    }
    return NULL;
}

/* Enforce the grouping rules for the given select items. */

/* ===== Main executor ===== */

QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, QArena *arena,
                           QArena *tmp, Arena *config_arena) {
    QueryResult result = query_result_init();

    /* 0. Validate FROM and open file */
    CSVReader *reader = NULL;
    char **headers = NULL;
    int header_count = 0;
    const char *err = open_reader(config, stmt, arena, config_arena, &reader, &headers,
                                  &header_count);
    if (err) { result.error = err; goto cleanup; }

    /* 1. Get CSV headers / 2. Validate column references */
    const char *bad_col = NULL;
    err = validate_stmt(stmt, headers, header_count, arena, &bad_col);
    if (err) { result.error = err; goto cleanup; }

    /* 3. Build output columns from select items */
    OutputCol *out_cols = NULL;
    int out_count = 0;
    err = build_output_cols(stmt, headers, header_count, arena, &out_cols, &out_count);
    if (err) { result.error = err; goto cleanup; }

    /* 3.5 Aggregate / GROUP BY detection */
    // TODO: Do all of these have to run if one has already found an aggregate?
    bool has_agg = false;
    for (int i = 0; i < out_count; i++) {
        if (expr_contains_aggregate(out_cols[i].expr)) { has_agg = true; break; }
    }
    if (stmt->having && expr_contains_aggregate(stmt->having)) has_agg = true;
    for (int j = 0; j < stmt->order_by_count; j++) {
        if (expr_contains_aggregate(stmt->order_by[j].expr)) { has_agg = true; break; }
    }
    bool group_mode = stmt->group_by_count > 0;
    bool grouped = has_agg || group_mode;

    /* Collect grouped column names (for GROUP BY validation) */
    char **grouped_cols = NULL;
    int grouped_col_count = 0;
    int grouped_col_cap = 0;
    if (group_mode) {
        for (int j = 0; j < stmt->group_by_count; j++) {
            collect_column_refs(stmt->group_by[j], &grouped_cols, &grouped_col_count,
                                &grouped_col_cap, arena);
        }
    }

    err = validate_grouping(stmt, out_cols, out_count, grouped_cols, grouped_col_count,
                            grouped, group_mode, arena);
    if (err) { result.error = err; goto cleanup; }

    /* Collect aggregate specs from select items, HAVING, and ORDER BY */
    AggSpec *specs = NULL;
    int spec_count = 0;
    int spec_cap = 0;
    if (grouped) {
        for (int i = 0; i < out_count; i++) {
            collect_specs(out_cols[i].expr, &specs, &spec_count, &spec_cap, arena);
        }
        if (stmt->having) {
            collect_specs(stmt->having, &specs, &spec_count, &spec_cap, arena);
        }
        for (int j = 0; j < stmt->order_by_count; j++) {
            collect_specs(stmt->order_by[j].expr, &specs, &spec_count, &spec_cap, arena);
        }
        /* Validate DISTINCT usage */
        for (int i = 0; i < spec_count; i++) {
            ExprNode *n = specs[i].node;
            if (!n->distinct) continue;
            if (n->arg_count != 1) {
                result.error = "DISTINCT takes exactly one argument.";
                goto cleanup;
            }
            if (n->args[0]->type == EXPR_STAR) {
                result.error = "DISTINCT cannot be applied to '*'.";
                goto cleanup;
            }
        }
    }

    /* 4. Set result headers */
    err = set_result_headers(&result, out_cols, out_count, arena);
    if (err) { result.error = err; goto cleanup; }

    /* 5. Iterate records */
    int capacity = GROW_INITIAL_CAPACITY;
    void *mem = alloc_or_error(arena, sizeof(CSVRecord*) * (size_t)capacity, &err);
    if (mem == NULL) { result.error = err; goto cleanup; }
    result.records = (CSVRecord**)mem;
    result.record_count = 0;

    /* DISTINCT + LIMIT without ORDER BY or grouping: dedupe incrementally so
       reading can stop once the requested number of distinct rows is found. */
    bool distinct_limit_path = stmt->distinct && stmt->has_limit &&
                               stmt->order_by_count == 0 && !grouped;
    RecordSet rset;
    if (distinct_limit_path) {
        err = record_set_init(arena, &rset);
        if (err) { result.error = err; goto cleanup; }
    }

    /* Parallel sort-keys array (order_by_count entries per row) */
    int k = stmt->order_by_count;
    EvalResult *sort_keys = NULL;
    int sort_keys_cap = 0;

    /* ORDER BY + LIMIT without grouping or DISTINCT: keep only the top
       `window` rows in a bounded heap instead of materializing and sorting
       every row. The window includes OFFSET rows, which are read and discarded
       by apply_limit_offset. Above QUERY_TOPK_MAX_K the bounded heap would
       degenerate, so the query falls back to the full-sort path. */
    long long window_ll = (stmt->has_offset && stmt->offset > 0) ? stmt->offset : 0;
    window_ll += stmt->limit;
    bool topk_path = k > 0 && stmt->has_limit && !grouped && !stmt->distinct &&
                     window_ll >= 0 && window_ll <= QUERY_TOPK_MAX_K;
    TopK topk;
    if (topk_path && window_ll > 0) {
        err = topk_init(arena, &topk, (int)window_ll, k, stmt->order_by);
        if (err) { result.error = err; goto cleanup; }
    }

    /* ORDER BY + LIMIT 0: the window is empty, so no row can match; skip the
       scan entirely. (The topk struct is left uninitialized on purpose.) */
    if (topk_path && window_ll <= 0) {
        result.record_count = 0;
        goto cleanup;
    }

    /* Grouped execution state */
    int group_by_k = stmt->group_by_count;
    AggGroup **groups = NULL;
    int group_count = 0;
    int group_cap = 0;
    GroupTable gtab = {NULL, 0, 0};
    if (grouped) {
        group_cap = GROUP_INITIAL_CAPACITY;
        mem = alloc_or_error(arena, sizeof(AggGroup*) * (size_t)group_cap, &err);
        if (mem == NULL) { result.error = err; goto cleanup; }
        groups = (AggGroup**)mem;

        err = group_table_init(arena, &gtab, GROUP_INITIAL_CAPACITY * 2);
        if (err) { result.error = err; goto cleanup; }

        /* Aggregates without GROUP BY use a single implicit group so that
           empty input still produces one output row. */
        if (!group_mode) {
            groups[group_count] = agg_group_create(0, spec_count, arena);
            if (groups[group_count] == NULL) { result.error = "Out of memory."; goto cleanup; }
            err = group_table_insert(arena, &gtab, groups, 0, 1, 0);
            if (err) { result.error = err; goto cleanup; }
            group_count = 1;
        }
    }

    CSVRecord *record;
    while ((record = csv_reader_next_record(reader)) != NULL) {
        qarena_reset(tmp);

        /* Evaluate WHERE */
        if (stmt->where) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            EvalResult where_val = eval_expr(stmt->where, &ctx);
            if (eval_result_is_error(&where_val)) {
                result.error = where_val.error;
                goto cleanup;
            }
            if (!eval_result_is_true(&where_val)) continue;
        }

        /* Grouped mode: accumulate into per-group states, no per-row output */
        if (grouped) {
            EvalResult *keys = NULL;
            if (group_by_k > 0) {
                void *keys_mem;
                QArenaResult ar = qarena_alloc(tmp, sizeof(EvalResult) * (size_t)group_by_k,
                                             &keys_mem);
                if (ar != QARENA_OK) { result.error = "Out of memory."; goto cleanup; }
                keys = (EvalResult*)keys_mem;
                for (int j = 0; j < group_by_k; j++) {
                    EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
                    EvalResult er = eval_expr(stmt->group_by[j], &ctx);
                    if (eval_result_is_error(&er)) {
                        result.error = er.error;
                        goto cleanup;
                    }
                    /* No copy needed: the key string (reader-owned field or
                       tmp-arena value) stays valid for this whole iteration,
                       and the transient key is only hashed/compared here.
                       New groups persist their own arena copy below. */
                    keys[j] = er;
                }
            }

            int gi = group_table_find(&gtab, groups, keys, group_by_k);
            AggGroup *g = gi >= 0 ? groups[gi] : NULL;
            if (g == NULL) {
                err = grow_array(arena, (void**)&groups, &group_cap, group_count + 1,
                                 sizeof(AggGroup*));
                if (err) { result.error = err; goto cleanup; }
                g = agg_group_create(group_by_k, spec_count, arena);
                if (g == NULL) { result.error = "Out of memory."; goto cleanup; }
                if (group_by_k > 0) {
                    for (int j = 0; j < group_by_k; j++) {
                        g->keys[j] = keys[j];
                        if (!keys[j].is_numeric && keys[j].str_val)
                            g->keys[j].str_val = qarena_strdup(arena, keys[j].str_val);
                    }
                }
                groups[group_count] = g;
                err = group_table_insert(arena, &gtab, groups, group_count,
                                         group_count + 1, group_by_k);
                if (err) { result.error = err; goto cleanup; }
                group_count++;
            }
            if (g->rep.field_count == 0) g->rep = copy_record(record, arena);

            for (int i = 0; i < spec_count; i++) {
                EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
                const char *aerr = aggregate_row(
                    specs[i].node->arg_count > 0 ? specs[i].node->args[0] : NULL,
                    specs[i].name, specs[i].distinct, &g->states[i], &ctx);
                if (aerr) { result.error = aerr; goto cleanup; }
            }
            continue;
        }

        /* Top-K path: evaluate the ORDER BY keys, keep only the best `window`
           rows, and project a row only when it survives. Non-kept rows cost
           only the key evaluation. */
        if (topk_path) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            EvalResult *keys;
            QArenaResult ar = qarena_alloc(tmp, sizeof(EvalResult) * (size_t)k,
                                         (void**)&keys);
            if (ar != QARENA_OK) { result.error = "Out of memory."; goto cleanup; }
            for (int j = 0; j < k; j++) {
                EvalResult er = eval_expr(stmt->order_by[j].expr, &ctx);
                if (eval_result_is_error(&er)) {
                    result.error = er.error;
                    goto cleanup;
                }
                keys[j] = er;
            }
            if (!topk_would_keep(&topk, keys)) continue;

            CSVRecord *proj = NULL;
            err = project_row(out_cols, out_count, &ctx, &proj);
            if (err) { result.error = err; goto cleanup; }
            topk_insert(arena, &topk, proj, keys);
            continue;
        }

        /* Pre-compute ORDER BY keys on the original record */
        if (k > 0) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            err = eval_sort_keys(&ctx, stmt->order_by, k, &sort_keys, &sort_keys_cap,
                                 result.record_count);
            if (err) { result.error = err; goto cleanup; }
        }

        /* Allocate and append projected record */
        CSVRecord *proj = NULL;
        EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
        err = project_row(out_cols, out_count, &ctx, &proj);
        if (err) { result.error = err; goto cleanup; }

        /* DISTINCT + LIMIT: dedupe incrementally so duplicates never enter the
           result, and stop reading once the window of distinct rows is found.
           Duplicate rows are dropped without growing the result. */
        if (distinct_limit_path) {
            if (!record_set_add(&rset, arena, result.records, proj, &err)) {
                if (err) { result.error = err; goto cleanup; }
                continue;
            }
        }

        err = append_result(&result.records, &result.record_count, &capacity, proj, arena);
        if (err) { result.error = err; goto cleanup; }

        /* LIMIT without ORDER BY can stop reading once the requested window
           is materialized. Skipped for ORDER BY (needs every row to sort) and
           grouped queries (output is built after the scan). With DISTINCT the
           window counts distinct rows, which the incremental dedupe tracks. */
        if (!grouped && stmt->has_limit && stmt->order_by_count == 0) {
            long long target = (stmt->has_offset && stmt->offset > 0) ? stmt->offset : 0;
            target += stmt->limit;
            if (target < 0) target = 0;
            if (result.record_count >= target) break;
        }
    }

    /* 5.5 Grouped mode: build one output row per group, filtered by HAVING */
    if (grouped) {
        err = finalize_groups(&result, groups, group_count, specs, spec_count, out_cols,
                              out_count, k, stmt, arena, tmp, headers, header_count,
                              &sort_keys, &sort_keys_cap, &capacity);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 6. Handle DISTINCT (skipped when the incremental LIMIT path already
       deduped records while scanning). */
    if (stmt->distinct && !distinct_limit_path) {
        err = dedupe_records(&result.records, &result.record_count, k, sort_keys, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 7. ORDER BY: either emit the top-k heap (already in final order) or
       materialize and sort every projected row. */
    if (topk_path) {
        err = topk_emit(arena, &topk, &result.records, &result.record_count);
        if (err) { result.error = err; goto cleanup; }
    } else {
        err = order_records(&result.records, result.record_count, k, sort_keys,
                            stmt->order_by, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 8. LIMIT / OFFSET */
    apply_limit_offset(stmt, result.records, &result.record_count);

cleanup:
    csv_reader_free(reader);
    return result;

}
