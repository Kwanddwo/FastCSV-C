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
} QueryResult;

QueryResult query_result_init(void);
QueryResult query_execute(CSVConfig *config, const char *sql, Arena *arena);

/* Estimate the bytes a result set can reach for a statement, so callers can
   size their arena before running it. Never below 4MiB. */
size_t query_estimate_result_size(const char *sql);

#endif