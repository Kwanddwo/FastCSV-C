#ifndef QUERY_PARSER_INTERNAL_H
#define QUERY_PARSER_INTERNAL_H

#include "qarena.h"
#include "ast.h"
#include "parser.h"
#include "scanner.h"
#include <stdbool.h>

/* Initial capacity for growable arrays (error list, expression lists);
   doubled on demand. */
#define LIST_INITIAL_CAPACITY 8

/* ===== Parser state ===== */
typedef struct {
    Scanner scanner;
    Token current;
    Token previous;
    ParseErrorList *errors;
    QArena *arena;
    const char *source;
    /* Hard stop flags: oom on arena allocation failure, too_deep on
       expression-nesting beyond MAX_EXPR_DEPTH. While either is set, token
       helpers stop consuming and parse entry points unwind with NULL, so no
       broken subtree can escape into the executor. */
    bool oom;
    bool too_deep;
    /* Current recursive-descent nesting depth (guarded by MAX_EXPR_DEPTH so
       the parser itself cannot smash the C stack on nested parens/unary/
       NOT chains). */
    int depth;
} Parser;

/* ===== Init ===== */
Parser parser_init(const char *source, QArena *arena, ParseErrorList *errors);

/* ===== Token helpers (parser.c) ===== */
void advance(Parser *parser);
bool check(Parser *parser, TokenType type);
bool match(Parser *parser, TokenType type);
void consume(Parser *parser, TokenType type, const char *message);
void error_at_current(Parser *parser, const char *message);
void record_error(Parser *parser, const char *msg, int line, int col);
void sync_after_error(Parser *parser);

/* ===== OOM handling (parser.c) ===== */
/* Record "Out of memory." and stop the parse. */
void parser_oom(Parser *parser, int line, int col);

/* ===== QArena string helpers (parser.c) ===== */
char* copy_lexeme(Parser *parser, const char *lexeme, int length);
char* copy_string_literal(Parser *parser, const char *start, int length);
char* qarena_concat(Parser *parser, const char *a, const char *b);

/* ===== AST node allocators (parser.c) ===== */
ExprNode* alloc_expr_node(Parser *parser);
ExprNode* make_error_node(Parser *parser, const char *msg);

/* ===== Expression parsing (expr.c) ===== */
ExprNode* parse_expression(Parser *parser);
ExprNode* parse_search_condition(Parser *parser);
ExprNode** parse_expr_list(Parser *parser, int *out_count, const char *item_msg);

/* ===== Statement parsing (parser.c) ===== */
SelectStmt* parse_select_query(Parser *parser);

#endif