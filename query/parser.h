#ifndef QUERY_PARSER_H
#define QUERY_PARSER_H

#include "qarena.h"
#include "ast.h"

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
SelectStmt* parse_select(const char *source, QArena *arena, ParseErrorList *errors);

#endif