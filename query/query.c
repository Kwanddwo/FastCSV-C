#include "query.h"
#include "executor.h"
#include "fold.h"
#include "validate.h"
#include "str_util.h"
#include "../arena.h"

#include <string.h>
#include <stdlib.h>

/* Scratch arenas owned by query_execute. The parse arena holds the AST and
   is reclaimed once execution finishes; the temp arena holds per-row
   evaluation scratch and is reset by the executor between records. The
   result arena is created here and returned to the caller inside QueryResult.
   These sizes are only the FIRST chunks: every qarena chains additional
   chunks on demand (geometric growth), so no query-shape guessing is needed.
   The library Arena below backs only the reader's config copy (the reader
   retains it for the whole scan); it is a shallow, bounded library arena. */
#define QUERY_PARSE_ARENA_INIT (64 * 1024)
#define QUERY_TMP_ARENA_INIT    (64 * 1024)
#define QUERY_RESULT_ARENA_INIT (64 * 1024)
#define QUERY_CONFIG_ARENA_INIT (8 * 1024)

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
    memset(&result.result_arena, 0, sizeof(QArena));

    return result;
}

void query_result_destroy(QueryResult *result) {
    if (result == NULL) return;
    qarena_destroy(&result->result_arena);
}

/* Deep-copy a parse-error list out of a scratch arena so the messages
   survive the parse arena's destruction. Returns NULL when there is nothing
   to copy or on allocation failure. */
static ParseErrorList* copy_parse_errors(const ParseErrorList *src, QArena *dst) {
    if (src == NULL || src->count == 0) return NULL;
    void *mem;
    QArenaResult ar = qarena_alloc(dst, sizeof(ParseErrorList), &mem);
    if (ar != QARENA_OK) return NULL;
    ParseErrorList *copy = (ParseErrorList*)mem;
    copy->count = src->count;
    copy->capacity = src->count;

    ar = qarena_alloc(dst, sizeof(const char*) * (size_t)src->count, &mem);
    if (ar != QARENA_OK) return NULL;
    copy->errors = (const char**)mem;
    for (int i = 0; i < src->count; i++) {
        const char *msg = qarena_strdup(dst, src->errors[i]);
        if (msg == NULL) return NULL;
        copy->errors[i] = msg;
    }

    ar = qarena_alloc(dst, sizeof(int) * (size_t)src->count, &mem);
    if (ar != QARENA_OK) return NULL;
    copy->error_lines = (int*)mem;
    memcpy(copy->error_lines, src->error_lines, sizeof(int) * (size_t)src->count);

    ar = qarena_alloc(dst, sizeof(int) * (size_t)src->count, &mem);
    if (ar != QARENA_OK) return NULL;
    copy->error_columns = (int*)mem;
    memcpy(copy->error_columns, src->error_columns, sizeof(int) * (size_t)src->count);
    return copy;
}

QueryResult query_execute(CSVConfig *config, const char *sql) {
    QueryResult result = query_result_init();

    /* Parse into a private scratch arena so the AST and parse-error scratch
       do not consume the result arena. */
    QArena parse_arena;
    if (qarena_create(&parse_arena, QUERY_PARSE_ARENA_INIT) != QARENA_OK) {
        result.error = "Out of memory.";
        return result;
    }

    ParseErrorList *parse_errors = parse_error_list_init(&parse_arena);
    if (parse_errors == NULL) {
        result.error = "Out of memory.";
        qarena_destroy(&parse_arena);
        return result;
    }

    SelectStmt *stmt = parse_select(sql, &parse_arena, parse_errors);
    if (stmt == NULL || parse_errors->count > 0) {
        if (parse_errors->count > 0) {
            if (qarena_create(&result.result_arena, QUERY_RESULT_ARENA_INIT) != QARENA_OK) {
                result.error = "Out of memory.";
                qarena_destroy(&parse_arena);
                return result;
            }
            result.parse_errors = copy_parse_errors(parse_errors, &result.result_arena);
            if (result.parse_errors != NULL) {
                result.error = result.parse_errors->errors[0];
                result.error_line = result.parse_errors->error_lines[0];
                result.error_column = result.parse_errors->error_columns[0];
            } else {
                result.error = "Out of memory.";
            }
        }
        qarena_destroy(&parse_arena);
        return result;
    }

    /* Fold constant subtrees once (allocated in the parse arena, which stays
       alive through execution) so they are never re-evaluated per row. */
    fold_constants(stmt, &parse_arena);

    if (qarena_create(&result.result_arena, QUERY_RESULT_ARENA_INIT) != QARENA_OK) {
        result.error = "Out of memory.";
        qarena_destroy(&parse_arena);
        return result;
    }

    /* Per-row evaluation scratch, reset by the executor between records. */
    QArena tmp_arena;
    if (qarena_create(&tmp_arena, QUERY_TMP_ARENA_INIT) != QARENA_OK) {
        result.error = "Out of memory.";
        qarena_destroy(&parse_arena);
        qarena_destroy(&result.result_arena);
        return result;
    }

    /* Library arena for the reader's CSVConfig copy: csv_config_copy and
       the reader both use the library's fixed-buffer Arena, so their memory
       cannot come from the query arenas. */
    Arena config_arena;
    if (arena_create(&config_arena, QUERY_CONFIG_ARENA_INIT) != ARENA_OK) {
        result.error = "Out of memory.";
        qarena_destroy(&parse_arena);
        qarena_destroy(&result.result_arena);
        qarena_destroy(&tmp_arena);
        return result;
    }

    QueryResult exec = execute_select(config, stmt, &result.result_arena,
                                      &tmp_arena, &config_arena);
    arena_destroy(&config_arena);
    qarena_destroy(&tmp_arena);
    qarena_destroy(&parse_arena);

    /* execute_select returns a fresh struct; hand over ownership of the
       result arena we created for it. */
    exec.result_arena = result.result_arena;
    result.result_arena = (QArena){0};

    return exec;
}
