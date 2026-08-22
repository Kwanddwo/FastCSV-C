#ifndef QUERY_TOKEN_H
#define QUERY_TOKEN_H

#include "token_type.h"
#include <stddef.h>

typedef struct {
    TokenType type;
    const char *lexeme;
    int length;
    void *literal;
    int line;
    int column;
} Token;

#endif
