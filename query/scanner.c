#include "scanner.h"
#include "str_util.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static bool is_at_end(Scanner *scanner) {
    return *scanner->current == '\0';
}

static char advance(Scanner *scanner) {
    scanner->current++;
    scanner->column++;
    return scanner->current[-1];
}

static char peek(Scanner *scanner) {
    return *scanner->current;
}

static bool match(Scanner *scanner, char expected) {
    if (is_at_end(scanner)) return false;
    if (*scanner->current != expected) return false;
    advance(scanner);
    return true;
}

static void skip_whitespace(Scanner *scanner) {
    for (;;) {
        char c = peek(scanner);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(scanner);
                break;
            case '\n':
                scanner->line++;
                scanner->column = 0;
                advance(scanner);
                break;
            default:
                return;
        }
    }
}

static Token make_token(Scanner *scanner, TokenType type) {
    Token token;
    token.type = type;
    token.lexeme = scanner->start;
    token.length = (int)(scanner->current - scanner->start);
    token.literal = NULL;
    token.line = scanner->line;
    token.column = scanner->column - token.length;
    return token;
}

static Token error_token(Scanner *scanner, const char *message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.lexeme = message;
    token.length = (int)strlen(message);
    token.literal = NULL;
    token.line = scanner->line;
    token.column = scanner->column;
    return token;
}

static TokenType keyword_type(const char *start, int length) {
    switch (length) {
        case 2:
            if (str_nieq(start, "IN", 2)) return TOKEN_IN;
            if (str_nieq(start, "IS", 2)) return TOKEN_IS;
            if (str_nieq(start, "OR", 2)) return TOKEN_OR;
            if (str_nieq(start, "AS", 2)) return TOKEN_AS;
            if (str_nieq(start, "BY", 2)) return TOKEN_BY;
            break;
        case 3:
            if (str_nieq(start, "AND", 3)) return TOKEN_AND;
            if (str_nieq(start, "NOT", 3)) return TOKEN_NOT;
            if (str_nieq(start, "END", 3)) return TOKEN_END;
            if (str_nieq(start, "ASC", 3)) return TOKEN_ASC;
            break;
        case 4:
            if (str_nieq(start, "FROM", 4)) return TOKEN_FROM;
            if (str_nieq(start, "LIKE", 4)) return TOKEN_LIKE;
            if (str_nieq(start, "NULL", 4)) return TOKEN_NULL;
            if (str_nieq(start, "TRUE", 4)) return TOKEN_TRUE;
            if (str_nieq(start, "CASE", 4)) return TOKEN_CASE;
            if (str_nieq(start, "WHEN", 4)) return TOKEN_WHEN;
            if (str_nieq(start, "THEN", 4)) return TOKEN_THEN;
            if (str_nieq(start, "ELSE", 4)) return TOKEN_ELSE;
            if (str_nieq(start, "DESC", 4)) return TOKEN_DESC;
            if (str_nieq(start, "LAST", 4)) return TOKEN_LAST;
            if (str_nieq(start, "DATE", 4)) return TOKEN_DATE;
            if (str_nieq(start, "TIME", 4)) return TOKEN_TIME;
            break;
        case 5:
            if (str_nieq(start, "WHERE", 5)) return TOKEN_WHERE;
            if (str_nieq(start, "FALSE", 5)) return TOKEN_FALSE;
            if (str_nieq(start, "ILIKE", 5)) return TOKEN_ILIKE;
            if (str_nieq(start, "ORDER", 5)) return TOKEN_ORDER;
            if (str_nieq(start, "LIMIT", 5)) return TOKEN_LIMIT;
            if (str_nieq(start, "GROUP", 5)) return TOKEN_GROUP;
            if (str_nieq(start, "FIRST", 5)) return TOKEN_FIRST;
            if (str_nieq(start, "NULLS", 5)) return TOKEN_NULLS;
            break;
        case 6:
            if (str_nieq(start, "SELECT", 6)) return TOKEN_SELECT;
            if (str_nieq(start, "OFFSET", 6)) return TOKEN_OFFSET;
            if (str_nieq(start, "HAVING", 6)) return TOKEN_HAVING;
            break;
        case 7:
            if (str_nieq(start, "BETWEEN", 7)) return TOKEN_BETWEEN;
            if (str_nieq(start, "EXTRACT", 7)) return TOKEN_EXTRACT;
            break;
        case 8:
            if (str_nieq(start, "DISTINCT", 8)) return TOKEN_DISTINCT;
            break;
        case 9:
            if (str_nieq(start, "LOCALTIME", 9)) return TOKEN_LOCALTIME;
            if (str_nieq(start, "TIMESTAMP", 9)) return TOKEN_TIMESTAMP;
            break;
        case 12:
            if (str_nieq(start, "CURRENT_DATE", 12)) return TOKEN_CURRENT_DATE;
            if (str_nieq(start, "CURRENT_TIME", 12)) return TOKEN_CURRENT_TIME;
            break;
        case 14:
            if (str_nieq(start, "LOCALTIMESTAMP", 14)) return TOKEN_LOCALTIMESTAMP;
            break;
        case 17:
            if (str_nieq(start, "CURRENT_TIMESTAMP", 17)) return TOKEN_CURRENT_TIMESTAMP;
            break;
    }
    return TOKEN_IDENTIFIER;
}

static Token scan_identifier(Scanner *scanner) {
    while (isalnum((unsigned char)peek(scanner)) || peek(scanner) == '_') {
        advance(scanner);
    }

    int length = (int)(scanner->current - scanner->start);
    TokenType type = keyword_type(scanner->start, length);
    return make_token(scanner, type);
}

static Token scan_number(Scanner *scanner) {
    while (isdigit((unsigned char)peek(scanner))) {
        advance(scanner);
    }

    /* A fractional part is scanned only when a digit actually follows the
       dot, so the probe at current[1] never reads past the NUL terminator. */
    if (peek(scanner) == '.' && scanner->current[1] != '\0' &&
        isdigit((unsigned char)scanner->current[1])) {
        advance(scanner);
        while (isdigit((unsigned char)peek(scanner))) {
            advance(scanner);
        }
    }

    return make_token(scanner, TOKEN_NUMBER);
}

static Token scan_string(Scanner *scanner) {
    for (;;) {
        if (is_at_end(scanner)) {
            return error_token(scanner, "Unterminated string.");
        }
        char c = peek(scanner);
        if (c == '\'') {
            /* '' is an escaped quote; only a lone quote closes the string. */
            if (scanner->current[1] == '\'') {
                advance(scanner);
                advance(scanner);
                continue;
            }
            break;
        }
        if (c == '\n') {
            scanner->line++;
            scanner->column = 0;
        }
        advance(scanner);
    }

    advance(scanner);
    return make_token(scanner, TOKEN_STRING);
}

/* Double-quoted identifier ("col name"): the SQL-standard quoted-identifier
   delimiter, used for column names and table paths with spaces or keyword
   names. Returns a TOKEN_IDENTIFIER whose lexeme is the content between the
   quotes (the opening quote was already consumed). Unlike strings there is
   no escaped-quote form: a doubled quote terminates the identifier and the
   trailing text fails to parse, which is preferable to silently misreading
   a header. */
static Token scan_quoted_identifier(Scanner *scanner) {
    scanner->start = scanner->current;   /* exclude the opening quote */
    for (;;) {
        if (is_at_end(scanner)) {
            return error_token(scanner, "Unterminated identifier.");
        }
        char c = peek(scanner);
        if (c == '"') break;
        if (c == '\n') {
            scanner->line++;
            scanner->column = 0;
        }
        advance(scanner);
    }
    int length = (int)(scanner->current - scanner->start);
    advance(scanner);   /* consume the closing quote */
    Token token;
    token.type = TOKEN_IDENTIFIER;
    token.lexeme = scanner->start;
    token.length = length;
    token.literal = NULL;
    token.line = scanner->line;
    token.column = scanner->column - length;
    return token;
}

Token scan_token(Scanner *scanner) {
    skip_whitespace(scanner);
    scanner->start = scanner->current;

    if (is_at_end(scanner)) return make_token(scanner, TOKEN_EOF);

    char c = advance(scanner);

    if (isalpha((unsigned char)c) || c == '_') return scan_identifier(scanner);
    if (c == '\'') return scan_string(scanner);
    if (c == '"') return scan_quoted_identifier(scanner);
    if (isdigit((unsigned char)c)) return scan_number(scanner);

    switch (c) {
        case '=': return make_token(scanner, TOKEN_EQUAL);
        case '!':
            if (match(scanner, '=')) return make_token(scanner, TOKEN_BANG_EQUAL);
            return error_token(scanner, "Unexpected character.");
        case '<':
            if (match(scanner, '=')) return make_token(scanner, TOKEN_LESS_EQUAL);
            if (match(scanner, '>')) return make_token(scanner, TOKEN_LESS_GREATER);
            return make_token(scanner, TOKEN_LESS);
        case '>':
            if (match(scanner, '=')) return make_token(scanner, TOKEN_GREATER_EQUAL);
            return make_token(scanner, TOKEN_GREATER);
        case '+': return make_token(scanner, TOKEN_PLUS);
        case '-': return make_token(scanner, TOKEN_MINUS);
        case '*': return make_token(scanner, TOKEN_STAR);
        case '/': return make_token(scanner, TOKEN_SLASH);
        case '%': return make_token(scanner, TOKEN_PERCENT);
        case '&': return make_token(scanner, TOKEN_AMPERSAND);
        case '|': return make_token(scanner, TOKEN_PIPE);
        case '^': return make_token(scanner, TOKEN_CARET);
        case '(': return make_token(scanner, TOKEN_LPAREN);
        case ')': return make_token(scanner, TOKEN_RPAREN);
        case ',': return make_token(scanner, TOKEN_COMMA);
        case '.': return make_token(scanner, TOKEN_DOT);
        case ';': return make_token(scanner, TOKEN_SEMICOLON);
    }

    return error_token(scanner, "Unexpected character.");
}

Scanner scanner_init(const char *source) {
    Scanner scanner;
    scanner.start = source;
    scanner.current = source;
    scanner.line = 1;
    scanner.column = 0;
    return scanner;
}
