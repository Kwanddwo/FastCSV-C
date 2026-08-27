#ifndef QUERY_H
#define QUERY_H

/* Thread-safety contract: the query engine is single-user. A statement
   executes in per-process module state (parser arenas, evaluation memo,
   sort scratch, PRNG state), so concurrent query_execute calls from
   multiple threads are NOT supported — serialize them externally. This is
   safe for the csvql CLI and single-threaded hosts.
   Count limits: result rows, groups and headers are ints; exceeding
   INT_MAX rows/groups fails cleanly ("Result exceeds INT_MAX rows."). */

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
    QArena result_arena;             /* Owned result arena; destroy via
                                       query_result_destroy. */
} QueryResult;

QueryResult query_result_init(void);

/* Execute a statement. The result set is materialized in a result arena that
 * query_execute creates and owns; size it via query_result_destroy(). All
 * arenas grow on demand, so a statement needs no manual sizing. On failure
 * result.error is set. */
QueryResult query_execute(CSVConfig *config, const char *sql);

/* Release the owned result arena (all headers, records, error copies). */
void query_result_destroy(QueryResult *result);

#endif
