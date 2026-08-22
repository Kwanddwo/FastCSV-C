#ifndef QUERY_SCANNER_H
#define QUERY_SCANNER_H

#include "token.h"

typedef struct {
    const char *start;
    const char *current;
    int line;
    int column;
} Scanner;

Scanner scanner_init(const char *source);
Token scan_token(Scanner *scanner);

#endif
