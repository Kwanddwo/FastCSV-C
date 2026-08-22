#include "query.h"
#include "executor.h"
#include "str_util.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>

/* Scratch arenas owned by query_execute. The parse arena holds the AST and
   is reclaimed once execution finishes; the temp arena holds per-row
   evaluation scratch and is reset by the executor between records. Neither
   consumes the caller's result arena. */
#define QUERY_PARSE_ARENA_MIN (64 * 1024)
#define QUERY_TMP_ARENA_SIZE (4 * 1024 * 1024)

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

    return result;
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
/* Length of the leading [A-Za-z_][A-Za-z0-9_]* identifier at s. */
static size_t id_len(const char *s) {
    size_t n = 0;
    while (isalnum((unsigned char)s[n]) || s[n] == '_') n++;
    return n;
}

/* Skip a single-quoted string literal starting at *pp (already at the
   opening quote). Advances *pp past the closing quote (or to NUL). */
static void skip_string(const char **pp) {
    const char *p = *pp + 1;
    while (*p && *p != '\'') p++;
    *pp = *p ? p + 1 : p;
}

/* Estimate the bytes a result set can reach so csvql can size its arena
   before running the query. Factors: the source file size (from stat) and
   query shape. A plain projection keeps every field, ORDER BY keeps sort
   keys, GROUP BY/aggregates add group tables and keys, CONCAT produces
   larger fields, DISTINCT adds a dedupe table. Returns a size >= 4MiB. */
size_t query_estimate_result_size(const char *sql) {
    bool has_concat = false, has_order = false, has_group = false,
         has_agg = false, has_distinct = false, has_limit = false;
    size_t limit_val = 0, offset_val = 0;

    if (sql == NULL) sql = "";
    const char *p = sql;
    while (*p) {
        if (*p == '\'') {
            skip_string(&p);
            continue;
        }
        if (isalpha((unsigned char)*p)) {
            size_t n = id_len(p);
            bool bound_ok = (p == sql || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'));
            if (bound_ok) {
                char c_after = p[n];
                if (!(isalnum((unsigned char)c_after) || c_after == '_')) {
                    if (str_nieq(p, "CONCAT", 6)) has_concat = true;
                    else if (str_nieq(p, "DISTINCT", 8)) has_distinct = true;
                    else if (str_nieq(p, "ORDER", 5)) has_order = true;
                    else if (str_nieq(p, "GROUP", 5)) has_group = true;
                    else if (str_nieq(p, "COUNT", 5) || str_nieq(p, "SUM", 3) ||
                             str_nieq(p, "AVG", 3) || str_nieq(p, "MIN", 3) ||
                             str_nieq(p, "MAX", 3)) has_agg = true;
                    else if (str_nieq(p, "LIMIT", 5)) {
                        has_limit = true;
                        const char *q = p + n;
                        while (*q && isspace((unsigned char)*q)) q++;
                        while (*q && isdigit((unsigned char)*q)) {
                            limit_val = limit_val * 10 + (size_t)(*q - '0');
                            q++;
                        }
                    } else if (str_nieq(p, "OFFSET", 6)) {
                        const char *q = p + n;
                        while (*q && isspace((unsigned char)*q)) q++;
                        while (*q && isdigit((unsigned char)*q)) {
                            offset_val = offset_val * 10 + (size_t)(*q - '0');
                            q++;
                        }
                    }
                }
            }
            p += n;
            continue;
        }
        p++;
    }

    /* Extract the table name following FROM and stat the underlying file. */
    size_t base = 0;
    p = sql;
    while (*p) {
        if (*p == '\'') {
            skip_string(&p);
            continue;
        }
        if (isalpha((unsigned char)*p)) {
            size_t n = id_len(p);
            bool bound_ok = (p == sql || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'));
            if (bound_ok && str_nieq(p, "FROM", 4)) {
                const char *q = p + n;
                while (*q && (isspace((unsigned char)*q) || *q == ';')) q++;
                const char *table = NULL;
                size_t table_len = 0;
                if (*q == '\'') {
                    q++;
                    table = q;
                    while (*q && *q != '\'') q++;
                    table_len = (size_t)(q - table);
                } else if (*q) {
                    table = q;
                    while (*q && *q != '\'' && !isspace((unsigned char)*q) && *q != ';') q++;
                    table_len = (size_t)(q - table);
                }
                if (table != NULL && table_len > 0) {
                    char *path = malloc(table_len + 1);
                    if (path != NULL) {
                        memcpy(path, table, table_len);
                        path[table_len] = '\0';
                        struct stat st;
                        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                            base = (size_t)st.st_size;
                        free(path);
                    }
                }
                break;
            }
            p += n;
            continue;
        }
        p++;
    }
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
        est = QUERY_PARSE_ARENA_MIN * 64;
        return est;
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
    if (est < QUERY_PARSE_ARENA_MIN * 64) est = QUERY_PARSE_ARENA_MIN * 64; /* floor 4MiB */
    return est;
}

QueryResult query_execute(CSVConfig *config, const char *sql, Arena *arena) {
    QueryResult result = query_result_init();

    /* Parse into a private scratch arena so the AST and parse-error scratch
       do not consume the caller's result arena. Scaled with the SQL length
       so very large queries cannot fail to allocate their own AST. */
    size_t sql_len = sql ? strlen(sql) : 0;
    size_t parse_size = QUERY_PARSE_ARENA_MIN;
    if (sql_len * 4 > parse_size) parse_size = sql_len * 4;

    Arena parse_arena;
    if (arena_create(&parse_arena, parse_size) != ARENA_OK) {
        result.error = "Out of memory.";
        return result;
    }

    ParseErrorList *parse_errors = parse_error_list_init(&parse_arena);
    if (parse_errors == NULL) {
        result.error = "Out of memory.";
        arena_destroy(&parse_arena);
        return result;
    }

    SelectStmt *stmt = parse_select(sql, &parse_arena, parse_errors);
    if (stmt == NULL || parse_errors->count > 0) {
        if (parse_errors->count > 0) {
            result.parse_errors = copy_parse_errors(parse_errors, arena);
            if (result.parse_errors != NULL) {
                result.error = result.parse_errors->errors[0];
                result.error_line = result.parse_errors->error_lines[0];
                result.error_column = result.parse_errors->error_columns[0];
            } else {
                result.error = "Out of memory.";
            }
        }
        arena_destroy(&parse_arena);
        return result;
    }

    /* Per-row evaluation scratch, reset by the executor between records. */
    Arena tmp_arena;
    if (arena_create(&tmp_arena, QUERY_TMP_ARENA_SIZE) != ARENA_OK) {
        result.error = "Out of memory.";
        arena_destroy(&parse_arena);
        return result;
    }

    QueryResult exec = execute_select(config, stmt, arena, &tmp_arena);
    arena_destroy(&tmp_arena);
    arena_destroy(&parse_arena);
    return exec;
}