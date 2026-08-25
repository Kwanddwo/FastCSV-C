#ifndef QUERY_H
#define QUERY_H

#include "../csv_reader.h"
#include "../csv_config.h"
#include "../arena.h"
#include "parser.h"

typedef struct {
    char **headers;
    int header_count;
    CSVRecord **records;
    int record_count;
    const char *error;              /* Runtime/semantic error (fail-fast) */
    int error_line;                 /* 0 for runtime errors (no source location) */
    int error_column;               /* -1 for runtime errors (no source location);
                                       parse errors mirror the first entry of
                                       parse_errors below. */
    ParseErrorList *parse_errors;   /* All parse errors collected */
    Arena result_arena;             /* Owned result arena; destroy via
                                       query_result_destroy. */
    bool out_of_memory;             /* True when error is an arena OOM. */
    size_t result_arena_size;       /* Bytes allocated for the result arena. */
} QueryResult;

QueryResult query_result_init(void);

/* Execute a statement. The result set is materialized in a result arena that
   query_execute creates and owns; size it via query_result_destroy(). The
   arena is sized from the parsed AST and the source file size unless
   arena_size is nonzero, in which case that exact size is used (bypassing
   the estimate). On failure result.error is set and
   out_of_memory/result_arena_size report how big the arena was. */
QueryResult query_execute(CSVConfig *config, const char *sql, size_t arena_size);

/* Release the owned result arena (all headers, records, error copies). */
void query_result_destroy(QueryResult *result);

#endif