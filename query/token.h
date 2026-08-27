#ifndef QUERY_TOKEN_H
#define QUERY_TOKEN_H

#include <stddef.h>

typedef enum {
    /* Keywords */
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_IN,
    TOKEN_BETWEEN,
    TOKEN_LIKE,
    TOKEN_ILIKE,
    TOKEN_ESCAPE,
    TOKEN_IS,
    TOKEN_DISTINCT,
    TOKEN_AS,
    TOKEN_NULL,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_CASE,
    TOKEN_WHEN,
    TOKEN_THEN,
    TOKEN_ELSE,
    TOKEN_END,
    TOKEN_ORDER,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_NULLS,
    TOKEN_FIRST,
    TOKEN_LAST,
    TOKEN_LIMIT,
    TOKEN_OFFSET,
    TOKEN_GROUP,
    TOKEN_HAVING,

    /* Datetime keywords */
    TOKEN_DATE,
    TOKEN_TIME,
    TOKEN_TIMESTAMP,
    TOKEN_CURRENT_DATE,
    TOKEN_CURRENT_TIME,
    TOKEN_CURRENT_TIMESTAMP,
    TOKEN_LOCALTIME,
    TOKEN_LOCALTIMESTAMP,
    TOKEN_EXTRACT,

    /* Multi-char operators */
    TOKEN_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS_GREATER,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,

    /* Arithmetic / bitwise operators */
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_AMPERSAND,
    TOKEN_PIPE,
    TOKEN_CARET,
    TOKEN_CONCAT,

    /* Punctuation */
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_SEMICOLON,

    /* Literals / identifiers */
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,

    /* Special */
    TOKEN_ERROR,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    const char *lexeme;
    int length;
    void *literal;
    int line;
    int column;
} Token;

#endif