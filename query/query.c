#include "query.h"
#include "executor.h"
#include "fold.h"
#include "validate.h"
#include "str_util.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Scratch arenas owned by query_execute. The parse arena holds the AST and
   is reclaimed once execution finishes; the temp arena holds per-row
   evaluation scratch and is reset by the executor between records. The
   result arena is created here too (sized by the estimator unless the caller
   overrides) and returned to the caller inside QueryResult. */
#define QUERY_PARSE_ARENA_MIN (64 * 1024)
#define QUERY_TMP_ARENA_SIZE (4 * 1024 * 1024)
#define QUERY_RESULT_ARENA_MIN (4 * 1024 * 1024)

QueryResult query_result_init() {
    QueryResult result;
    result.headers = NULL;
    result.header_count = 0;
    result.records = NULL;
    result.record_count = 0;
    result.error = NULL;
    result.error_line = 0;
    result.error_column = -1;
    result.parse_errors = NULL;
    memset(&result.result_arena, 0, sizeof(Arena));
    result.out_of_memory = false;
    result.result_arena_size = 0;

    return result;
}

void query_result_destroy(QueryResult *result) {
    if (result == NULL) return;
    arena_destroy(&result->result_arena);
    result->result_arena_size = 0;
}

/* Deep-copy a parse-error list out of a scratch arena so the messages
   survive the parse arena's destruction. Returns NULL when there is nothing
   to copy or on allocation failure. */
static ParseErrorList* copy_parse_errors(const ParseErrorList *src, Arena *dst) {
    if (src == NULL || src->count == 0) return NULL;
    void *mem;
    ArenaResult ar = arena_alloc(dst, sizeof(ParseErrorList), &mem);
    if (ar != ARENA_OK) return NULL;
    ParseErrorList *copy = (ParseErrorList*)mem;
    copy->count = src->count;
    copy->capacity = src->count;

    ar = arena_alloc(dst, sizeof(const char*) * (size_t)src->count, &mem);
    if (ar != ARENA_OK) return NULL;
    copy->errors = (const char**)mem;
    for (int i = 0; i < src->count; i++) {
        const char *msg = arena_strdup(dst, src->errors[i]);
        if (msg == NULL) return NULL;
        copy->errors[i] = msg;
    }

    ar = arena_alloc(dst, sizeof(int) * (size_t)src->count, &mem);
    if (ar != ARENA_OK) return NULL;
    copy->error_lines = (int*)mem;
    memcpy(copy->error_lines, src->error_lines, sizeof(int) * (size_t)src->count);

    ar = arena_alloc(dst, sizeof(int) * (size_t)src->count, &mem);
    if (ar != ARENA_OK) return NULL;
    copy->error_columns = (int*)mem;
    memcpy(copy->error_columns, src->error_columns, sizeof(int) * (size_t)src->count);
    return copy;
}

/* ===== Result-set size estimation ===== */

/* True when the expression tree contains a CONCAT call. Runs after constant
   folding, so a CONCAT of literals has already been replaced by a literal
   node and no longer counts -- only per-row CONCATs emit larger fields. */
static bool expr_has_concat(const ExprNode *node) {
    if (node == NULL) return false;
    if (node->type == EXPR_FUNCTION_CALL && str_nieq(node->str_value, "CONCAT", 6))
        return true;
    if (expr_has_concat(node->left) || expr_has_concat(node->right) ||
        expr_has_concat(node->mid)) return true;
    if (node->type == EXPR_CASE) {
        for (const CaseWhen *w = node->case_whens; w; w = w->next) {
            if (expr_has_concat(w->condition) || expr_has_concat(w->result)) return true;
        }
        if (expr_has_concat(node->case_else)) return true;
    }
    for (int i = 0; i < node->arg_count; i++) {
        if (expr_has_concat(node->args[i])) return true;
    }
    return false;
}

/* Mirror the executor's open_reader: an extension-less name falls back to
   "<name>.csv". Returns the file size, or 0 when it cannot be determined. */
static size_t stat_table_size(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;

    struct stat st;
    if (stat(name, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        return (size_t)st.st_size;

    size_t len = strlen(name);
    bool has_csv_ext = len >= 5 && name[len - 4] == '.' &&
                       (name[len - 3] == 'c' || name[len - 3] == 'C') &&
                       (name[len - 2] == 's' || name[len - 2] == 'S') &&
                       (name[len - 1] == 'v' || name[len - 1] == 'V');
    if (has_csv_ext) return 0;

    char *candidate = malloc(len + 5);
    if (candidate == NULL) return 0;
    memcpy(candidate, name, len);
    memcpy(candidate + len, ".csv", 5);
    size_t size = 0;
    if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        size = (size_t)st.st_size;
    free(candidate);
    return size;
}

/* Estimate the bytes a result set can reach so query_execute can size its
   result arena before running the statement. All query-shape facts come
   from the parsed AST (exact LIMIT/OFFSET values, ORDER BY key count, group
   count, DISTINCT, aggregates, CONCAT); the source file size (from stat) is
   the only proxy for row count and field sizes. A plain projection keeps
   every field, ORDER BY keeps sort keys, GROUP BY/aggregates add group
   tables and keys, CONCAT produces larger fields, DISTINCT adds a dedupe
   table. Returns a size >= QUERY_RESULT_ARENA_MIN. */
static size_t estimate_result_size(const SelectStmt *stmt) {
    bool has_concat = false;
    bool has_order = stmt->order_by_count > 0;
    bool has_group = stmt->group_by_count > 0;
    bool has_agg = false;
    bool has_distinct = stmt->distinct;
    bool has_limit = stmt->has_limit;
    size_t limit_val = stmt->has_limit ? (size_t)stmt->limit : 0;
    size_t offset_val = stmt->has_offset ? (size_t)stmt->offset : 0;

    for (int i = 0; i < stmt->item_count; i++) {
        if (expr_has_concat(stmt->items[i].expr)) has_concat = true;
        if (expr_contains_aggregate(stmt->items[i].expr)) has_agg = true;
    }
    if (stmt->where && expr_contains_aggregate(stmt->where)) has_agg = true;
    if (stmt->having && expr_contains_aggregate(stmt->having)) has_agg = true;
    for (int j = 0; j < stmt->order_by_count; j++) {
        if (expr_has_concat(stmt->order_by[j].expr)) has_concat = true;
        if (expr_contains_aggregate(stmt->order_by[j].expr)) has_agg = true;
    }
    for (int j = 0; j < stmt->group_by_count; j++) {
        if (expr_has_concat(stmt->group_by[j])) has_concat = true;
    }

    size_t base = stat_table_size(stmt->table_name);
    if (base == 0) base = 1024 * 1024; /* unknown file: assume 1MiB */

    size_t est = base * 7 / 2; /* ~3.5x: per-record overhead + field data */
    if (has_order) est += base * 2;
    if (has_group || has_agg) est += base * 4; /* group table + per-group states */
    if (has_distinct) est += base;
    if (has_concat) est += base;
    est += 1024 * 1024; /* overhead: headers, arrays, group tables */
    /* LIMIT 0 without GROUP BY or aggregates: the executor stops before
       materializing anything (ORDER BY short-circuits, DISTINCT stops at the
       empty window, plain scans break on the first record), so only the floor
       is needed. */
    if (has_limit && limit_val == 0 && !has_group && !has_agg) {
        return QUERY_RESULT_ARENA_MIN;
    }
    /* LIMIT without GROUP BY or aggregates stops reading once the window is
       materialized (DISTINCT rows are deduped incrementally, so the window of
       distinct rows bounds the result). ORDER BY + LIMIT keeps only the top
       `window` rows in a bounded heap, so it can also be capped -- provided
       the executor's heap is used (window within QUERY_TOPK_MAX_K); a larger
       window falls back to full materialization and must not be capped. The
       window includes OFFSET rows, which are read but discarded. */
    if (has_limit && !has_group && !has_agg && !has_distinct) {
        size_t window = limit_val + offset_val;
        if (window > 0 && (!has_order || window <= QUERY_TOPK_MAX_K)) {
            size_t per_row = has_order ? 4096 : 1024;
            size_t cap = window * per_row + (has_order ? 1024 * 1024 : 1024);
            if (cap < est) est = cap;
        }
    }
    if (est < QUERY_RESULT_ARENA_MIN) est = QUERY_RESULT_ARENA_MIN; /* floor 4MiB */
    return est;
}

QueryResult query_execute(CSVConfig *config, const char *sql, size_t arena_size) {
    QueryResult result = query_result_init();

    /* Parse into a private scratch arena so the AST and parse-error scratch
       do not consume the result arena. Scaled with the SQL length so very
       large queries cannot fail to allocate their own AST. */
    size_t sql_len = sql ? strlen(sql) : 0;
    size_t parse_size = QUERY_PARSE_ARENA_MIN;
    if (sql_len * 4 > parse_size) parse_size = sql_len * 4;

    Arena parse_arena;
    if (arena_create(&parse_arena, parse_size) != ARENA_OK) {
        result.error = "Out of memory.";
        result.out_of_memory = true;
        return result;
    }

    ParseErrorList *parse_errors = parse_error_list_init(&parse_arena);
    if (parse_errors == NULL) {
        result.error = "Out of memory.";
        result.out_of_memory = true;
        arena_destroy(&parse_arena);
        return result;
    }

    SelectStmt *stmt = parse_select(sql, &parse_arena, parse_errors);
    if (stmt == NULL || parse_errors->count > 0) {
        if (parse_errors->count > 0) {
            /* Parse errors only need an arena to hold the copied messages. */
            size_t err_size = arena_size > 0 ? arena_size : QUERY_RESULT_ARENA_MIN;
            if (arena_create(&result.result_arena, err_size) != ARENA_OK) {
                result.error = "Out of memory.";
                result.out_of_memory = true;
                result.result_arena_size = err_size;
                arena_destroy(&parse_arena);
                return result;
            }
            result.result_arena_size = err_size;
            result.parse_errors = copy_parse_errors(parse_errors, &result.result_arena);
            if (result.parse_errors != NULL) {
                result.error = result.parse_errors->errors[0];
                result.error_line = result.parse_errors->error_lines[0];
                result.error_column = result.parse_errors->error_columns[0];
            } else {
                result.error = "Out of memory.";
                result.out_of_memory = true;
            }
        }
        arena_destroy(&parse_arena);
        return result;
    }

    /* Fold constant subtrees once (allocated in the parse arena, which stays
       alive through execution) so they are never re-evaluated per row. */
    fold_constants(stmt, &parse_arena);

    /* Result arena: sized from the parsed AST (all query-shape facts) and
       the source file size, unless the caller overrides. An exact override
       (e.g. from CSVQL_QUERY_ARENA_SIZE) bypasses the estimate so a
       mis-estimated query can be re-run at a fixed size. */
    size_t need = arena_size > 0 ? arena_size : estimate_result_size(stmt);
    if (arena_create(&result.result_arena, need) != ARENA_OK) {
        result.error = "Out of memory.";
        result.out_of_memory = true;
        result.result_arena_size = need;
        arena_destroy(&parse_arena);
        return result;
    }
    result.result_arena_size = need;

    /* Per-row evaluation scratch, reset by the executor between records. */
    Arena tmp_arena;
    if (arena_create(&tmp_arena, QUERY_TMP_ARENA_SIZE) != ARENA_OK) {
        result.error = "Out of memory.";
        result.out_of_memory = true;
        arena_destroy(&parse_arena);
        arena_destroy(&result.result_arena);
        return result;
    }

    QueryResult exec = execute_select(config, stmt, &result.result_arena, &tmp_arena);
    arena_destroy(&tmp_arena);
    arena_destroy(&parse_arena);

    /* Surface arena exhaustion with the size the user needs to see, so a
       retry can size the arena explicitly (or via the env override). */
    if (exec.error != NULL && strcmp(exec.error, "Out of memory.") == 0) {
        exec.out_of_memory = true;
        exec.result_arena_size = need;
    }
    return exec;
}