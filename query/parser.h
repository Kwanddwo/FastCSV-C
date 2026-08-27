#ifndef QUERY_PARSER_H
#define QUERY_PARSER_H

#include "qarena.h"
#include "ast.h"
#include <stdbool.h>

/* ===== Parse error list ===== */
typedef struct {
    const char **errors;       /* arena-allocated array of error messages */
    int *error_lines;          /* parallel array: line numbers */
    int *error_columns;        /* parallel array: column numbers */
    int count;
    int capacity;
} ParseErrorList;

ParseErrorList* parse_error_list_init(QArena *arena);

/* ===== Public API ===== */
/* On parse-time memory exhaustion *out_oom is set true and NULL is returned:
   callers must surface "Out of memory." rather than continue with a broken
   (possibly non-NULL) statement. */
SelectStmt* parse_select(const char *source, QArena *arena, ParseErrorList *errors,
                         bool *out_oom);

#endif