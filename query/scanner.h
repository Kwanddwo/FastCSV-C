#ifndef QUERY_SCANNER_H
#define QUERY_SCANNER_H

#include "token.h"
#include <stdbool.h>

typedef struct {
    const char *start;
    const char *current;
    int line;
    int column;
    /* True after an unterminated string/identifier/comment was reported at
       end of input: the lexer stopped inside a construct, so callers (e.g.
       the REPL's statement splitter) keep reading rather than executing. */
    bool incomplete;
} Scanner;

Scanner scanner_init(const char *source);
Token scan_token(Scanner *scanner);

#endif
