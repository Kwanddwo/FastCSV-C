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
#include <limits.h>

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
    QARENA_IF_FAIL(qarena_alloc(ctx->arena, sizeof(CSVRecord), &mem), "Out of memory.");
    CSVRecord *proj = (CSVRecord*)mem;

    QARENA_IF_FAIL(qarena_alloc(ctx->arena, sizeof(char*) * (size_t)out_count, &mem), "Out of memory.");
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
        char *s = (char*)eval_result_to_string(&er, ctx->arena);
        if (s == NULL) return "Out of memory.";
        proj->fields[i] = s;
    }
    *out = proj;
    return NULL;
}

/* Append a projected record to the result, growing as needed. */
const char* append_result(CSVRecord ***records, int *record_count, int *capacity,
                                 CSVRecord *proj, QArena *arena) {
    /* Counts are ints throughout the engine: refuse to wrap instead of
       silently corrupting the result. */
    if (*record_count == INT_MAX) {
        return "Result exceeds INT_MAX rows.";
    }
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
        QARENA_IF_FAIL(qarena_alloc(arena, len + 5, &mem), "Out of memory.");
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
    QARENA_IF_FAIL(qarena_alloc(arena, sizeof(OutputCol) * (size_t)count, &mem), "Out of memory.");
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
    QARENA_IF_FAIL(qarena_alloc(arena, sizeof(char*) * (size_t)out_count, &mem), "Out of memory.");
    result->headers = (char**)mem;
    result->header_count = out_count;

    for (int i = 0; i < out_count; i++) {
        result->headers[i] = qarena_strdup(arena, out_cols[i].name);
    }
    return NULL;
}

/* Enforce the grouping rules for the given select items. */

/* ===== Executor phases ===== */

/* Shared state threaded through the executor phases. */
typedef struct {
    CSVConfig *config;
    SelectStmt *stmt;
    QArena *arena;
    QArena *tmp;
    Arena *config_arena;

    CSVReader *reader;
    char **headers;
    int header_count;

    OutputCol *out_cols;
    int out_count;

    bool has_agg;
    bool group_mode;
    bool grouped;

    char **grouped_cols;
    int grouped_col_count;

    AggSpec *specs;
    int spec_count;

    bool distinct_limit_path;
    RecordSet rset;
    bool topk_path;
    TopK topk;
    long long window_ll;

    int k;
    EvalResult *sort_keys;
    int sort_keys_cap;

    int capacity;
    AggGroup **groups;
    int group_count;
    int group_cap;
    GroupTable gtab;
    int group_by_k;
} ExecState;

/* Phase 0-2: open the CSV, validate column references, build output columns
   and resolve ORDER BY positional references against the expanded columns. */
static const char* exec_prepare(ExecState *st) {
    const char *err = open_reader(st->config, st->stmt, st->arena, st->config_arena,
                                  &st->reader, &st->headers, &st->header_count);
    if (err) return err;

    const char *bad_col = NULL;
    err = validate_stmt(st->stmt, st->headers, st->header_count, st->arena, &bad_col);
    if (err) return err;

    err = build_output_cols(st->stmt, st->headers, st->header_count, st->arena,
                            &st->out_cols, &st->out_count);
    if (err) return err;

    for (int j = 0; j < st->stmt->order_by_count; j++) {
        ExprNode *e = st->stmt->order_by[j].expr;
        if (e != NULL && e->type == EXPR_ORDER_ORDINAL) {
            int pos = (int)e->num_value;
            if (pos < 1 || pos > st->out_count) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "SELECT position %d is not in the select list.", pos);
                char *msg = qarena_strdup(st->arena, buf);
                return msg ? msg : "Out of memory.";
            }
            st->stmt->order_by[j].expr = st->out_cols[pos - 1].expr;
        }
    }
    return NULL;
}

/* Phase 3: aggregate detection, grouping validation and aggregate-spec
   collection. */
static const char* exec_plan(ExecState *st) {
    st->has_agg = false;
    for (int i = 0; i < st->out_count && !st->has_agg; i++) {
        if (expr_contains_aggregate(st->out_cols[i].expr)) st->has_agg = true;
    }
    if (!st->has_agg && st->stmt->having &&
        expr_contains_aggregate(st->stmt->having)) st->has_agg = true;
    for (int j = 0; j < st->stmt->order_by_count && !st->has_agg; j++) {
        if (expr_contains_aggregate(st->stmt->order_by[j].expr)) st->has_agg = true;
    }
    st->group_mode = st->stmt->group_by_count > 0;
    st->grouped = st->has_agg || st->group_mode;

    int grouped_col_cap = 0;
    if (st->group_mode) {
        for (int j = 0; j < st->stmt->group_by_count; j++) {
            collect_column_refs(st->stmt->group_by[j], &st->grouped_cols,
                                &st->grouped_col_count, &grouped_col_cap, st->arena);
        }
    }

    const char *err = validate_grouping(st->stmt, st->out_cols, st->out_count,
                                        st->grouped_cols, st->grouped_col_count,
                                        st->grouped, st->group_mode, st->arena);
    if (err) return err;

    int spec_cap = 0;
    if (st->grouped) {
        for (int i = 0; i < st->out_count; i++) {
            collect_specs(st->out_cols[i].expr, &st->specs, &st->spec_count, &spec_cap,
                          st->arena);
        }
        if (st->stmt->having) {
            collect_specs(st->stmt->having, &st->specs, &st->spec_count, &spec_cap,
                          st->arena);
        }
        for (int j = 0; j < st->stmt->order_by_count; j++) {
            collect_specs(st->stmt->order_by[j].expr, &st->specs, &st->spec_count,
                          &spec_cap, st->arena);
        }
        /* Validate DISTINCT usage */
        for (int i = 0; i < st->spec_count; i++) {
            ExprNode *n = st->specs[i].node;
            if (n->arg_count > 0 && n->args[0]->type == EXPR_STAR &&
                !str_ieq(n->str_value, "COUNT")) {
                return "'*' is only allowed with COUNT.";
            }
            if (!n->distinct) continue;
            if (n->arg_count != 1) {
                return "DISTINCT takes exactly one argument.";
            }
            if (n->args[0]->type == EXPR_STAR) {
                return "DISTINCT cannot be applied to '*'.";
            }
        }
    }
    return NULL;
}

/* Phase 4 initializes the path selection and grouped/top-k state; it is
   inlined in execute_select (it writes directly into QueryResult). */

/* ===== Main executor ===== */

/* Phase 5: the scan loop. Returns NULL on success or the error message
   (also stored in result->error). Failure unwinds to `fail`. */
static const char* exec_scan(ExecState *st, QueryResult *result) {
    const char *err = NULL;
    CSVRecord *record;
    while ((record = csv_reader_next_record(st->reader)) != NULL) {
        qarena_reset(st->tmp);

        CellMemo memo;
        memo.cap = (int)record->field_count;
        memo.valid = NULL;
        memo.vals = NULL;
        if (record->field_count > 0) {
            void *m;
            QArenaResult ar = qarena_alloc(st->tmp, sizeof(EvalResult) * record->field_count,
                                           &m);
            if (ar != QARENA_OK) { result->error = "Out of memory."; goto fail; }
            memo.vals = (EvalResult*)m;
            ar = qarena_alloc(st->tmp, record->field_count, &m);
            if (ar != QARENA_OK) { result->error = "Out of memory."; goto fail; }
            memset(m, 0, record->field_count);
            memo.valid = (uint8_t*)m;
        }

        if (st->stmt->where) {
            EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
            EvalResult where_val = eval_expr(st->stmt->where, &ctx);
            if (eval_result_is_error(&where_val)) {
                result->error = where_val.error;
                goto fail;
            }
            if (!eval_result_is_true(&where_val)) continue;
        }

        if (st->grouped) {
            EvalResult *keys = NULL;
            if (st->group_by_k > 0) {
                void *keys_mem;
                QArenaResult ar = qarena_alloc(st->tmp, sizeof(EvalResult) * (size_t)st->group_by_k,
                                               &keys_mem);
                if (ar != QARENA_OK) { result->error = "Out of memory."; goto fail; }
                keys = (EvalResult*)keys_mem;
                for (int j = 0; j < st->group_by_k; j++) {
                    EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
                    EvalResult er = eval_expr(st->stmt->group_by[j], &ctx);
                    if (eval_result_is_error(&er)) {
                        result->error = er.error;
                        goto fail;
                    }
                    keys[j] = er;
                }
            }

            int gi = group_table_find(&st->gtab, st->groups, keys, st->group_by_k);
            AggGroup *g = gi >= 0 ? st->groups[gi] : NULL;
            if (g == NULL) {
                err = grow_array(st->arena, (void**)&st->groups, &st->group_cap,
                                 st->group_count + 1, sizeof(AggGroup*));
                if (err) { result->error = err; goto fail; }
                g = agg_group_create(st->group_by_k, st->spec_count, st->arena);
                if (g == NULL) { result->error = "Out of memory."; goto fail; }
                if (st->group_by_k > 0) {
                    for (int j = 0; j < st->group_by_k; j++) {
                        g->keys[j] = keys[j];
                        if (keys[j].str_val)
                            g->keys[j].str_val = qarena_strdup(st->arena, keys[j].str_val);
                    }
                }
                if (st->group_count == INT_MAX) {
                    result->error = "Result exceeds INT_MAX groups.";
                    goto fail;
                }
                st->groups[st->group_count] = g;
                err = group_table_insert(st->arena, &st->gtab, st->groups, st->group_count,
                                         st->group_count + 1, st->group_by_k);
                if (err) { result->error = err; goto fail; }
                st->group_count++;
            }
            if (g->rep.field_count == 0) g->rep = copy_record(record, st->arena);

            for (int i = 0; i < st->spec_count; i++) {
                EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
                const char *aerr = aggregate_row(
                    st->specs[i].node->arg_count > 0 ? st->specs[i].node->args[0] : NULL,
                    st->specs[i].kind, st->specs[i].distinct, &g->states[i], &ctx);
                if (aerr) { result->error = aerr; goto fail; }
            }
            continue;
        }

        if (st->topk_path) {
            EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
            EvalResult *keys;
            QArenaResult ar = qarena_alloc(st->tmp, sizeof(EvalResult) * (size_t)st->k,
                                           (void**)&keys);
            if (ar != QARENA_OK) { result->error = "Out of memory."; goto fail; }
            for (int j = 0; j < st->k; j++) {
                EvalResult er = eval_expr(st->stmt->order_by[j].expr, &ctx);
                if (eval_result_is_error(&er)) {
                    result->error = er.error;
                    goto fail;
                }
                keys[j] = er;
            }
            if (!topk_would_keep(&st->topk, keys)) continue;

            CSVRecord *proj = NULL;
            err = project_row(st->out_cols, st->out_count, &ctx, &proj);
            if (err) { result->error = err; goto fail; }
            topk_insert(st->arena, &st->topk, proj, keys);
            continue;
        }

        if (st->k > 0) {
            EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
            err = eval_sort_keys(&ctx, st->stmt->order_by, st->k, &st->sort_keys,
                                 &st->sort_keys_cap, result->record_count);
            if (err) { result->error = err; goto fail; }
        }

        CSVRecord *proj = NULL;
        EvalCtx ctx = eval_ctx_for(record, st->headers, st->header_count, st->arena, st->tmp, &memo, NULL);
        err = project_row(st->out_cols, st->out_count, &ctx, &proj);
        if (err) { result->error = err; goto fail; }

        if (st->distinct_limit_path) {
            if (!record_set_add(&st->rset, st->arena, result->records, proj, &err)) {
                if (err) { result->error = err; goto fail; }
                continue;
            }
        }

        err = append_result(&result->records, &result->record_count, &st->capacity, proj, st->arena);
        if (err) { result->error = err; goto fail; }

        if (!st->grouped && st->stmt->has_limit && st->stmt->order_by_count == 0) {
            long long target = (st->stmt->has_offset && st->stmt->offset > 0) ? st->stmt->offset : 0;
            target += st->stmt->limit;
            if (target < 0) target = 0;
            if (result->record_count >= target) break;
        }
    }


fail:
    return result->error;
}

QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, QArena *arena,
                           QArena *tmp, Arena *config_arena) {
    QueryResult result = query_result_init();

    ExecState st = {0};
    st.config = config;
    st.stmt = stmt;
    st.arena = arena;
    st.tmp = tmp;
    st.config_arena = config_arena;

    /* Set result headers early (needs out_cols from prepare). */
    void *mem;
    const char *err = exec_prepare(&st);
    if (err) { result.error = err; goto cleanup; }

    err = exec_plan(&st);
    if (err) { result.error = err; goto cleanup; }

    err = set_result_headers(&result, st.out_cols, st.out_count, arena);
    if (err) { result.error = err; goto cleanup; }

    /* --- begin scan-state initialization (inlined for clarity) --- */
    int capacity = GROW_INITIAL_CAPACITY;
    mem = alloc_or_error(arena, sizeof(CSVRecord*) * (size_t)capacity, &err);
    if (mem == NULL) { result.error = err; goto cleanup; }
    result.records = (CSVRecord**)mem;
    result.record_count = 0;
    st.capacity = capacity;

    st.distinct_limit_path = stmt->distinct && stmt->has_limit &&
                             stmt->order_by_count == 0 && !st.grouped;
    if (st.distinct_limit_path) {
        err = record_set_init(arena, &st.rset);
        if (err) { result.error = err; goto cleanup; }
    }

    st.k = stmt->order_by_count;

    st.window_ll = (stmt->has_offset && stmt->offset > 0) ? stmt->offset : 0;
    st.window_ll += stmt->limit;
    st.topk_path = st.k > 0 && stmt->has_limit && !st.grouped && !stmt->distinct &&
                   st.window_ll >= 0 && st.window_ll <= QUERY_TOPK_MAX_K;
    if (st.topk_path && st.window_ll > 0) {
        err = topk_init(arena, &st.topk, (int)st.window_ll, st.k, stmt->order_by);
        if (err) { result.error = err; goto cleanup; }
    }
    if (st.topk_path && st.window_ll <= 0) {
        result.record_count = 0;
        goto cleanup;
    }

    st.group_by_k = stmt->group_by_count;
    st.group_cap = 0;
    if (st.grouped) {
        st.group_cap = GROUP_INITIAL_CAPACITY;
        mem = alloc_or_error(arena, sizeof(AggGroup*) * (size_t)st.group_cap, &err);
        if (mem == NULL) { result.error = err; goto cleanup; }
        st.groups = (AggGroup**)mem;

        err = group_table_init(arena, &st.gtab, GROUP_INITIAL_CAPACITY * 2);
        if (err) { result.error = err; goto cleanup; }

        if (!st.group_mode) {
            st.groups[st.group_count] = agg_group_create(0, st.spec_count, arena);
            if (st.groups[st.group_count] == NULL) {
                result.error = "Out of memory.";
                goto cleanup;
            }
            err = group_table_insert(arena, &st.gtab, st.groups, 0, 1, 0);
            if (err) { result.error = err; goto cleanup; }
            st.group_count = 1;
        }
    }
    /* --- end scan-state initialization --- */

    /* Phase 5: scan (extracted above). */
    err = exec_scan(&st, &result);
    if (err) goto cleanup;

    /* Phase 5.5-8: finalize groups, DISTINCT, ORDER BY, LIMIT/OFFSET. */
    if (st.grouped) {
        err = finalize_groups(&result, st.groups, st.group_count, st.specs, st.spec_count,
                              st.out_cols, st.out_count, st.k, stmt, arena, tmp,
                              st.headers, st.header_count, &st.sort_keys, &st.sort_keys_cap,
                              &st.capacity);
        if (err) { result.error = err; goto cleanup; }
    }

    if (stmt->distinct && !st.distinct_limit_path) {
        err = dedupe_records(&result.records, &result.record_count, st.k, st.sort_keys, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    if (st.topk_path) {
        err = topk_emit(arena, &st.topk, &result.records, &result.record_count);
        if (err) { result.error = err; goto cleanup; }
    } else {
        err = order_records(&result.records, result.record_count, st.k, st.sort_keys,
                            stmt->order_by, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    apply_limit_offset(stmt, result.records, &result.record_count);

cleanup:
    csv_reader_free(st.reader);
    return result;

}
