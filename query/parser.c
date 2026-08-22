#include "parser.h"
#include "scanner.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define MAX_PARSE_ERRORS 50

/* Initial capacity for growable arrays (error list, expression lists);
   doubled on demand. */
static const int LIST_INITIAL_CAPACITY = 8;

/* ===== Parser state ===== */
typedef struct {
    Scanner scanner;
    Token current;
    Token previous;
    ParseErrorList *errors;
    Arena *arena;
    const char *source;
    /* Non-NULL fallback node returned when arena allocation fails. Sharing a
       single node is safe because parse errors abort execution before any
       tree is evaluated. */
    ExprNode oom_node;
} Parser;

/* ===== Forward declarations ===== */
static ExprNode* parse_expression(Parser *parser);
static ExprNode* parse_arithmetic_primary(Parser *parser);
static ExprNode* parse_case_expression(Parser *parser);
static ExprNode* parse_not_expr(Parser *parser);
static ExprNode* parse_and_expr(Parser *parser);
static ExprNode* parse_search_condition(Parser *parser);
static SelectStmt* parse_select_query(Parser *parser);
static void parse_order_by(Parser *parser, SelectStmt *stmt);
static void parse_limit_offset(Parser *parser, SelectStmt *stmt);
static void parse_group_by(Parser *parser, SelectStmt *stmt);
static void parse_having(Parser *parser, SelectStmt *stmt);
static void sync_after_error(Parser *parser);
static ExprNode* alloc_expr_node(Parser *parser);
static ExprNode* make_error_node(Parser *parser, const char *msg);

/* ===== Init ===== */
static Parser parser_init(const char *source, Arena *arena, ParseErrorList *errors) {
    Parser parser;
    parser.scanner = scanner_init(source);
    parser.current.type = TOKEN_EOF;
    parser.current.lexeme = NULL;
    parser.current.length = 0;
    parser.current.line = 0;
    parser.current.column = 0;
    parser.previous = parser.current;
    parser.errors = errors;
    parser.arena = arena;
    parser.source = source;
    memset(&parser.oom_node, 0, sizeof(parser.oom_node));
    parser.oom_node.type = EXPR_LITERAL_NULL;
    return parser;
}

/* ===== ParseErrorList implementation ===== */
ParseErrorList* parse_error_list_init(Arena *arena) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(ParseErrorList), &mem);
    if (ar != ARENA_OK) return NULL;
    ParseErrorList *list = (ParseErrorList*)mem;
    list->errors = NULL;
    list->error_lines = NULL;
    list->error_columns = NULL;
    list->count = 0;
    list->capacity = 0;
    return list;
}

static void record_error(Parser *parser, const char *msg, int line, int col) {
    if (parser->errors->count >= MAX_PARSE_ERRORS) return;

    /* Suppress cascading errors reported at a location that already has
     * one. Panic-mode recovery can re-hit the same token (e.g. a sync
     * token left as `current` after a failure), and the scanner error
     * path may report a TOKEN_ERROR twice. Keeping the first occurrence
     * preserves the root cause while ensuring each error has a unique
     * position. */
    ParseErrorList *list = parser->errors;
    for (int i = 0; i < list->count; i++) {
        if (list->error_lines[i] == line && list->error_columns[i] == col) {
            return;
        }
    }
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : LIST_INITIAL_CAPACITY;
        if (new_cap > MAX_PARSE_ERRORS) new_cap = MAX_PARSE_ERRORS;

        const char **new_errors = (const char**)arena_realloc(
            parser->arena, list->errors,
            sizeof(const char*) * (size_t)list->capacity,
            sizeof(const char*) * (size_t)new_cap);
        if (new_errors == NULL) return;

        int *new_lines = (int*)arena_realloc(
            parser->arena, list->error_lines,
            sizeof(int) * (size_t)list->capacity,
            sizeof(int) * (size_t)new_cap);
        if (new_lines == NULL) return;

        int *new_cols = (int*)arena_realloc(
            parser->arena, list->error_columns,
            sizeof(int) * (size_t)list->capacity,
            sizeof(int) * (size_t)new_cap);
        if (new_cols == NULL) return;

        /* Commit only after every array grew, keeping capacity and the
           parallel arrays consistent on partial failure. */
        list->errors = new_errors;
        list->error_lines = new_lines;
        list->error_columns = new_cols;
        list->capacity = new_cap;
    }

    const char *stored = arena_strdup(parser->arena, msg);
    if (stored == NULL) stored = "Out of memory.";
    list->errors[list->count] = stored;
    list->error_lines[list->count] = line;
    list->error_columns[list->count] = col;
    list->count++;
}

/* ===== Token helpers ===== */
static void advance(Parser *parser) {
    parser->previous = parser->current;

    for (;;) {
        parser->current = scan_token(&parser->scanner);
        if (parser->current.type != TOKEN_ERROR) break;

        record_error(parser, parser->current.lexeme,
                     parser->current.line, parser->current.column);

        sync_after_error(parser);
        if (parser->current.type != TOKEN_ERROR) break;
    }
}

static bool check(Parser *parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser *parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static void consume(Parser *parser, TokenType type, const char *message) {
    if (parser->current.type == type) {
        advance(parser);
        return;
    }

    record_error(parser, message, parser->current.line, parser->current.column);
    sync_after_error(parser);
}

static void error_at_current(Parser *parser, const char *message) {
    record_error(parser, message, parser->current.line, parser->current.column);
}

/* ===== Error recovery synchronization ===== */
static bool is_sync_token(TokenType type) {
    switch (type) {
        case TOKEN_SELECT:
        case TOKEN_FROM:
        case TOKEN_WHERE:
        case TOKEN_GROUP:
        case TOKEN_ORDER:
        case TOKEN_LIMIT:
        case TOKEN_HAVING:
        case TOKEN_COMMA:
        case TOKEN_RPAREN:
        case TOKEN_AS:
        case TOKEN_BY:
        case TOKEN_SEMICOLON:
        case TOKEN_EOF:
            return true;
        default:
            return false;
    }
}

/* Discard tokens until a synchronization point. The sync token is left as
   the current token so the caller can resume parsing. Each discarded token
   updates `previous`, keeping the token stream consistent. */
static void sync_after_error(Parser *parser) {
    if (parser->previous.type == TOKEN_SEMICOLON) return;

    while (!is_sync_token(parser->current.type)) {
        if (parser->current.type == TOKEN_ERROR) {
            record_error(parser, parser->current.lexeme,
                         parser->current.line, parser->current.column);
        }
        parser->previous = parser->current;
        parser->current = scan_token(&parser->scanner);
    }
}

static ExprNode* make_error_node(Parser *parser, const char *msg) {
    /* The message has already been recorded by the caller; the returned node
       is only a placeholder so parsing can continue. */
    (void)msg;
    return alloc_expr_node(parser);
}

/* ===== Arena string helpers ===== */
static char* copy_lexeme(Parser *parser, const char *lexeme, int length) {
    if (lexeme == NULL) { lexeme = ""; length = 0; }
    if (length < 0) length = 0;
    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, (size_t)length + 1, &mem);
    if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    char *str = (char*)mem;
    memcpy(str, lexeme, (size_t)length);
    str[length] = '\0';
    return str;
}

static char* copy_string_literal(Parser *parser, const char *start, int length) {
    if (start == NULL) { start = ""; length = 0; }
    int content_length = length - 2;
    if (content_length < 0) content_length = 0;
    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, (size_t)content_length + 1, &mem);
    if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    char *str = (char*)mem;
    /* Copy the text between the surrounding quotes, collapsing '' into '. */
    int out = 0;
    for (int i = 1; i + 1 < length; i++) {
        if (start[i] == '\'' && i + 2 < length && start[i + 1] == '\'') {
            str[out++] = '\'';
            i++;
        } else {
            str[out++] = start[i];
        }
    }
    str[out] = '\0';
    return str;
}

static char* arena_concat(Parser *parser, const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, alen + blen + 1, &mem);
    if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    char *str = (char*)mem;
    memcpy(str, a, alen);
    memcpy(str + alen, b, blen);
    str[alen + blen] = '\0';
    return str;
}

/* ===== AST node allocators ===== */
static ExprNode* alloc_expr_node(Parser *parser) {
    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, sizeof(ExprNode), &mem);
    if (ar != ARENA_OK) {
        /* Fall back to a shared sentinel. Any recorded error prevents
           execution, so the placeholder tree is never evaluated. */
        record_error(parser, "Out of memory.", parser->current.line, parser->current.column);
        return &parser->oom_node;
    }
    ExprNode *node = (ExprNode*)mem;
    node->type = EXPR_LITERAL_NULL;
    node->str_value = NULL;
    node->num_value = 0.0;
    node->col_index = -1;
    node->left = NULL;
    node->right = NULL;
    node->mid = NULL;
    node->arg_count = 0;
    node->args = NULL;
    node->case_whens = NULL;
    node->case_else = NULL;
    node->subquery = NULL;
    return node;
}

static ExprNode* make_leaf(Parser *parser, ExprType type) {
    ExprNode *node = alloc_expr_node(parser);
    node->type = type;
    return node;
}

static ExprNode* make_binary(Parser *parser, ExprType type, ExprNode *left, ExprNode *right) {
    ExprNode *node = alloc_expr_node(parser);
    node->type = type;
    node->left = left;
    node->right = right;
    return node;
}

static ExprNode* make_unary(Parser *parser, ExprType type, ExprNode *operand) {
    ExprNode *node = alloc_expr_node(parser);
    node->type = type;
    node->left = operand;
    return node;
}

/* ===== Expression parsing (precedence climbing) ===== */

/* multiplicative_expr ::= arithmetic_primary { ('*' | '/' | '%') arithmetic_primary } */
static ExprNode* parse_multiplicative_expr(Parser *parser) {
    ExprNode *expr = parse_arithmetic_primary(parser);

    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT)) {
        TokenType op = parser->previous.type;
        ExprNode *right = parse_arithmetic_primary(parser);
        ExprType type;
        if (op == TOKEN_STAR) type = EXPR_MUL;
        else if (op == TOKEN_SLASH) type = EXPR_DIV;
        else type = EXPR_MOD;
        expr = make_binary(parser, type, expr, right);
    }
    return expr;
}

/* additive_expr ::= multiplicative_expr { ('+' | '-') multiplicative_expr } */
static ExprNode* parse_additive_expr(Parser *parser) {
    ExprNode *expr = parse_multiplicative_expr(parser);

    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        TokenType op = parser->previous.type;
        ExprNode *right = parse_multiplicative_expr(parser);
        ExprType type = (op == TOKEN_PLUS) ? EXPR_ADD : EXPR_SUB;
        expr = make_binary(parser, type, expr, right);
    }
    return expr;
}

/* bitwise_expr ::= additive_expr { ('&' | '|' | '^') additive_expr } */
static ExprNode* parse_bitwise_expr(Parser *parser) {
    ExprNode *expr = parse_additive_expr(parser);

    while (match(parser, TOKEN_AMPERSAND) || match(parser, TOKEN_PIPE) || match(parser, TOKEN_CARET)) {
        TokenType op = parser->previous.type;
        ExprNode *right = parse_additive_expr(parser);
        ExprType type;
        if (op == TOKEN_AMPERSAND) type = EXPR_BIT_AND;
        else if (op == TOKEN_PIPE) type = EXPR_BIT_OR;
        else type = EXPR_BIT_XOR;
        expr = make_binary(parser, type, expr, right);
    }
    return expr;
}

/* expression ::= bitwise_expr */
static ExprNode* parse_expression(Parser *parser) {
    return parse_bitwise_expr(parser);
}

/* ===== Primary expression ===== */
static ExprNode* parse_case_expression(Parser *parser);
static SelectStmt* parse_select_query(Parser *parser);

/* Parse a parenthesized, comma-separated list of expressions (the argument
   list of a function call or an IN / NOT IN list). Returns the arena-allocated
   array and sets *out_count, or NULL on OOM (error already recorded). */
static ExprNode** parse_expr_list(Parser *parser, int *out_count, const char *item_msg) {
    int capacity = LIST_INITIAL_CAPACITY;
    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, sizeof(ExprNode*) * (size_t)capacity, &mem);
    if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    ExprNode **items = (ExprNode**)mem;
    int count = 0;

    for (;;) {
        if (count >= capacity) {
            capacity *= 2;
            mem = arena_realloc(parser->arena, items,
                                sizeof(ExprNode*) * (size_t)(capacity / 2),
                                sizeof(ExprNode*) * (size_t)capacity);
            if (mem == NULL) { error_at_current(parser, "Out of memory."); return NULL; }
            items = (ExprNode**)mem;
        }
        items[count++] = parse_expression(parser);
        if (match(parser, TOKEN_RPAREN)) break;
        consume(parser, TOKEN_COMMA, item_msg);
    }

    *out_count = count;
    return items;
}

static ExprNode* parse_function_args(Parser *parser, const char *func_name) {
    ExprNode *node = alloc_expr_node(parser);
    node->type = EXPR_FUNCTION_CALL;
    node->str_value = arena_strdup(parser->arena, func_name);
    node->distinct = match(parser, TOKEN_DISTINCT);

    if (match(parser, TOKEN_RPAREN)) {
        node->arg_count = 0;
        node->args = NULL;
        return node;
    }

    node->args = parse_expr_list(parser, &node->arg_count,
                                 "Expected ',' or ')' after function argument.");
    return node;
}

static ExprNode* parse_arithmetic_primary(Parser *parser) {
    /* Unary plus / minus */
    if (match(parser, TOKEN_PLUS)) {
        ExprNode *operand = parse_arithmetic_primary(parser);
        return make_unary(parser, EXPR_UNARY_PLUS, operand);
    }
    if (match(parser, TOKEN_MINUS)) {
        ExprNode *operand = parse_arithmetic_primary(parser);
        return make_unary(parser, EXPR_UNARY_MINUS, operand);
    }

    /* (expression) or (subquery) */
    if (match(parser, TOKEN_LPAREN)) {
        if (check(parser, TOKEN_SELECT)) {
            /* Scalar subquery */
            SelectStmt *subq = parse_select_query(parser);
            consume(parser, TOKEN_RPAREN, "Expected ')' after subquery.");
            ExprNode *node = alloc_expr_node(parser);
            node->type = EXPR_SUBQUERY;
            node->subquery = subq;
            return node;
        } else {
            ExprNode *expr = parse_expression(parser);
            consume(parser, TOKEN_RPAREN, "Expected ')' after expression.");
            return expr;
        }
    }

    /* CASE expression */
    if (match(parser, TOKEN_CASE)) {
        return parse_case_expression(parser);
    }

    /* Literal: NULL */
    if (match(parser, TOKEN_NULL)) {
        return make_leaf(parser, EXPR_LITERAL_NULL);
    }

    /* Literal: TRUE / FALSE */
    if (match(parser, TOKEN_TRUE)) {
        ExprNode *node = make_leaf(parser, EXPR_LITERAL_NUMBER);
        if (node) node->num_value = 1.0;
        return node;
    }
    if (match(parser, TOKEN_FALSE)) {
        ExprNode *node = make_leaf(parser, EXPR_LITERAL_NUMBER);
        if (node) node->num_value = 0.0;
        return node;
    }

    /* Bare asterisk (e.g. COUNT(*)) */
    if (match(parser, TOKEN_STAR)) {
        return make_leaf(parser, EXPR_STAR);
    }

    /* String literal */
    if (match(parser, TOKEN_STRING)) {
        ExprNode *node = make_leaf(parser, EXPR_LITERAL_STRING);
        node->str_value = copy_string_literal(parser, parser->previous.lexeme, parser->previous.length);
        return node;
    }

    /* Number literal */
    if (match(parser, TOKEN_NUMBER)) {
        ExprNode *node = make_leaf(parser, EXPR_LITERAL_NUMBER);
        node->str_value = copy_lexeme(parser, parser->previous.lexeme, parser->previous.length);
        node->num_value = strtod(node->str_value, NULL);
        return node;
    }

    /* Identifier: column ref, qualified identifier, or function call */
    if (match(parser, TOKEN_IDENTIFIER)) {
        Token ident = parser->previous;
        char *name = copy_lexeme(parser, ident.lexeme, ident.length);

        /* Function call */
        if (match(parser, TOKEN_LPAREN)) {
            return parse_function_args(parser, name);
        }

        /* Qualified identifier: table.column */
        if (match(parser, TOKEN_DOT)) {
            consume(parser, TOKEN_IDENTIFIER, "Expected column name after '.'.");
            char *col = copy_lexeme(parser, parser->previous.lexeme, parser->previous.length);
            char *qualified = arena_concat(parser, name, ".");
            char *full = arena_concat(parser, qualified, col);
            ExprNode *node = make_leaf(parser, EXPR_COLUMN_REF);
            node->str_value = full;
            return node;
        }

        /* Simple column reference */
        ExprNode *node = make_leaf(parser, EXPR_COLUMN_REF);
        node->str_value = name;
        return node;
    }

    error_at_current(parser, "Expected expression.");
    return make_error_node(parser, "Expected expression.");
}

/* ===== CASE expression ===== */
static ExprNode* parse_case_expression(Parser *parser) {
    ExprNode *node = alloc_expr_node(parser);
    node->type = EXPR_CASE;

    /* If next token is not WHEN, it's a simple CASE: CASE expr WHEN ... */
    if (!check(parser, TOKEN_WHEN)) {
        node->left = parse_expression(parser);
    }

    consume(parser, TOKEN_WHEN, "Expected 'WHEN' after 'CASE'.");

    CaseWhen *head = NULL;
    CaseWhen *tail = NULL;

    do {
        CaseWhen *cw;
        ArenaResult ar = arena_alloc(parser->arena, sizeof(CaseWhen), (void**)&cw);
        if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return make_error_node(parser, "Out of memory."); }
        cw->condition = NULL;
        cw->result = NULL;
        cw->next = NULL;

        if (node->left) {
            /* Simple CASE: WHEN expr THEN result */
            cw->condition = parse_expression(parser);
        } else {
            /* Searched CASE: WHEN search_condition THEN result */
            cw->condition = parse_search_condition(parser);
        }

        consume(parser, TOKEN_THEN, "Expected 'THEN' after WHEN condition.");

        cw->result = parse_expression(parser);

        if (!head) head = cw;
        else tail->next = cw;
        tail = cw;
    } while (match(parser, TOKEN_WHEN));

    node->case_whens = head;

    if (match(parser, TOKEN_ELSE)) {
        node->case_else = parse_expression(parser);
    }

    consume(parser, TOKEN_END, "Expected 'END' after CASE.");

    return node;
}

/* ===== Search condition parsing ===== */

/* Forward declaration for recursion */
static ExprNode* parse_primary_condition(Parser *parser);

/* parse_or_expr ::= parse_and_expr { OR parse_and_expr } */
static ExprNode* parse_or_expr(Parser *parser) {
    ExprNode *expr = parse_and_expr(parser);

    while (match(parser, TOKEN_OR)) {
        ExprNode *right = parse_and_expr(parser);
        expr = make_binary(parser, EXPR_OR, expr, right);
    }
    return expr;
}

/* parse_and_expr ::= parse_not_expr { AND parse_not_expr } */
static ExprNode* parse_and_expr(Parser *parser) {
    ExprNode *expr = parse_not_expr(parser);

    while (match(parser, TOKEN_AND)) {
        ExprNode *right = parse_not_expr(parser);
        expr = make_binary(parser, EXPR_AND, expr, right);
    }
    return expr;
}

/* parse_not_expr ::= NOT parse_not_expr | parse_primary_condition */
static ExprNode* parse_not_expr(Parser *parser) {
    if (match(parser, TOKEN_NOT)) {
        ExprNode *operand = parse_not_expr(parser);
        return make_unary(parser, EXPR_NOT, operand);
    }

    return parse_primary_condition(parser);
}

/* parse_primary_condition ::=
 *     '(' search_condition ')'
 *   | expression comparison_operator expression
 *   | expression NOT IN (...)
 *   | expression IN (...)
 *   | expression BETWEEN expression AND expression
 *   | expression LIKE expression
 *   | expression ILIKE expression
 *   | expression    (bare expression, truthy)
 */
static ExprNode* parse_primary_condition(Parser *parser) {
    /* Parenthesized search condition */
    if (match(parser, TOKEN_LPAREN)) {
        ExprNode *expr = parse_search_condition(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after condition.");
        return expr;
    }

    /* It starts with an expression. Parse it, then check for comparison tail. */
    ExprNode *expr = parse_expression(parser);

    /* Comparison operators */
    if (match(parser, TOKEN_EQUAL)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_EQ, expr, right);
    }
    if (match(parser, TOKEN_BANG_EQUAL) || match(parser, TOKEN_LESS_GREATER)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_NE, expr, right);
    }
    if (match(parser, TOKEN_LESS)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_LT, expr, right);
    }
    if (match(parser, TOKEN_LESS_EQUAL)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_LE, expr, right);
    }
    if (match(parser, TOKEN_GREATER)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_GT, expr, right);
    }
    if (match(parser, TOKEN_GREATER_EQUAL)) {
        ExprNode *right = parse_expression(parser);
        return make_binary(parser, EXPR_GE, expr, right);
    }

    /* NOT IN */
    if (match(parser, TOKEN_NOT)) {
        if (match(parser, TOKEN_IN)) {
            consume(parser, TOKEN_LPAREN, "Expected '(' after 'NOT IN'.");

            ExprNode *node = alloc_expr_node(parser);
            node->type = EXPR_NOT_IN;
            node->left = expr;

            if (check(parser, TOKEN_SELECT)) {
                node->subquery = parse_select_query(parser);
                consume(parser, TOKEN_RPAREN, "Expected ')' after subquery.");
            } else {
                node->args = parse_expr_list(parser, &node->arg_count,
                                             "Expected ',' or ')' after IN list item.");
            }
            return node;
        }
        /* A bare NOT after an expression is invalid; report it instead of
           silently dropping the negation. */
        error_at_current(parser, "Expected 'IN' after 'NOT'.");
    }

    /* IN */
    if (match(parser, TOKEN_IN)) {
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'IN'.");

        ExprNode *node = alloc_expr_node(parser);
        node->type = EXPR_IN;
        node->left = expr;

        if (check(parser, TOKEN_SELECT)) {
            node->subquery = parse_select_query(parser);
            consume(parser, TOKEN_RPAREN, "Expected ')' after subquery.");
        } else {
            node->args = parse_expr_list(parser, &node->arg_count,
                                         "Expected ',' or ')' after IN list item.");
        }
        return node;
    }

    /* BETWEEN */
    if (match(parser, TOKEN_BETWEEN)) {
        ExprNode *start = parse_expression(parser);
        consume(parser, TOKEN_AND, "Expected 'AND' after BETWEEN value.");
        ExprNode *end = parse_expression(parser);

        ExprNode *node = alloc_expr_node(parser);
        node->type = EXPR_BETWEEN;
        node->left = expr;
        node->right = start;
        node->mid = end;
        return node;
    }

    /* LIKE */
    if (match(parser, TOKEN_LIKE)) {
        ExprNode *pattern = parse_expression(parser);
        return make_binary(parser, EXPR_LIKE, expr, pattern);
    }

    /* ILIKE */
    if (match(parser, TOKEN_ILIKE)) {
        ExprNode *pattern = parse_expression(parser);
        return make_binary(parser, EXPR_ILIKE, expr, pattern);
    }

    /* Bare expression (truthy evaluation) */
    return expr;
}

/* parse_search_condition ::= parse_or_expr */
static ExprNode* parse_search_condition(Parser *parser) {
    return parse_or_expr(parser);
}

/* ===== WHERE clause ===== */
static ExprNode* parse_where(Parser *parser) {
    if (!match(parser, TOKEN_WHERE)) return NULL;

    ExprNode *expr = parse_search_condition(parser);
    return expr;
}

/* ===== ORDER BY clause ===== */
/* Ensure an arena-backed array has room for one more element, doubling
   capacity as needed (starting from a NULL array). Returns false on OOM,
   recording the error so callers can bail out. */
static bool ensure_capacity(Parser *parser, void **array, int *capacity,
                            int count, size_t elem_size) {
    if (count < *capacity) return true;
    int new_cap = *capacity ? *capacity * 2 : LIST_INITIAL_CAPACITY;
    void *mem = arena_realloc(parser->arena, *array,
                              elem_size * (size_t)*capacity,
                              elem_size * (size_t)new_cap);
    if (mem == NULL) { error_at_current(parser, "Out of memory."); return false; }
    *array = mem;
    *capacity = new_cap;
    return true;
}

static void parse_order_by(Parser *parser, SelectStmt *stmt) {
    if (!match(parser, TOKEN_ORDER)) return;

    consume(parser, TOKEN_BY, "Expected 'BY' after 'ORDER'.");

    OrderByItem *items = NULL;
    int capacity = 0;
    int count = 0;

    for (;;) {
        ExprNode *expr = parse_expression(parser);

        bool asc = true;
        if (match(parser, TOKEN_ASC)) { asc = true; }
        else if (match(parser, TOKEN_DESC)) { asc = false; }

        if (!ensure_capacity(parser, (void**)&items, &capacity, count,
                             sizeof(OrderByItem))) {
            return;
        }

        items[count].expr = expr;
        items[count].asc = asc;
        count++;

        if (!match(parser, TOKEN_COMMA)) break;
    }

    stmt->order_by = items;
    stmt->order_by_count = count;
}

/* ===== LIMIT / OFFSET clause ===== */
/* Parse the integer value of the previously-consumed NUMBER token. The raw
   lexeme points into the source (not NUL-terminated), so it is copied first.
   The value is clamped to [0, INT_MAX]. */
static int parse_count_value(Parser *parser) {
    char *lex = copy_lexeme(parser, parser->previous.lexeme, parser->previous.length);
    if (lex == NULL) return 0;
    long value = strtol(lex, NULL, 10);
    if (value < 0) value = 0;
    if (value > INT_MAX) value = INT_MAX;
    return (int)value;
}

static void parse_limit_offset(Parser *parser, SelectStmt *stmt) {
    if (match(parser, TOKEN_LIMIT)) {
        consume(parser, TOKEN_NUMBER, "Expected number after 'LIMIT'.");
        stmt->limit = parse_count_value(parser);
        stmt->has_limit = true;

        if (match(parser, TOKEN_OFFSET)) {
            consume(parser, TOKEN_NUMBER, "Expected number after 'OFFSET'.");
            stmt->offset = parse_count_value(parser);
            stmt->has_offset = true;
        }
        return;
    }

    if (match(parser, TOKEN_OFFSET)) {
        consume(parser, TOKEN_NUMBER, "Expected number after 'OFFSET'.");
        stmt->offset = parse_count_value(parser);
        stmt->has_offset = true;

        if (match(parser, TOKEN_LIMIT)) {
            consume(parser, TOKEN_NUMBER, "Expected number after 'LIMIT'.");
            stmt->limit = parse_count_value(parser);
            stmt->has_limit = true;
        }
    }
}

/* ===== GROUP BY clause ===== */
static void parse_group_by(Parser *parser, SelectStmt *stmt) {
    if (!match(parser, TOKEN_GROUP)) return;

    consume(parser, TOKEN_BY, "Expected 'BY' after 'GROUP'.");

    ExprNode **exprs = NULL;
    int capacity = 0;
    int count = 0;

    for (;;) {
        ExprNode *expr = parse_expression(parser);

        if (!ensure_capacity(parser, (void**)&exprs, &capacity, count,
                             sizeof(ExprNode*))) {
            return;
        }
        exprs[count++] = expr;

        if (!match(parser, TOKEN_COMMA)) break;
    }

    stmt->group_by = exprs;
    stmt->group_by_count = count;
}

/* ===== HAVING clause ===== */
static void parse_having(Parser *parser, SelectStmt *stmt) {
    if (!match(parser, TOKEN_HAVING)) return;

    stmt->having = parse_search_condition(parser);
}

/* ===== Select list ===== */
static SelectItem* parse_select_list(Parser *parser, int *out_count) {
    SelectItem *items = NULL;
    int capacity = 0;
    int count = 0;

    for (;;) {
        /* Record expression source start position */
        const char *src_start = parser->current.lexeme;

        ExprNode *expr = parse_expression(parser);

        /* Determine expression source text end */
        const char *src_end;
        if (parser->previous.lexeme != NULL) {
            src_end = parser->previous.lexeme + parser->previous.length;
        } else {
            src_end = src_start;
        }
        char *source_text = copy_lexeme(parser, src_start, (int)(src_end - src_start));

        /* Optional alias: AS identifier, or bare identifier */
        char *alias = NULL;
        if (match(parser, TOKEN_AS)) {
            consume(parser, TOKEN_IDENTIFIER, "Expected alias name after 'AS'.");
            alias = copy_lexeme(parser, parser->previous.lexeme, parser->previous.length);
        } else if (check(parser, TOKEN_IDENTIFIER)) {
            Token bare = parser->current;
            advance(parser);
            alias = copy_lexeme(parser, bare.lexeme, bare.length);
        }
        
        if (expr->type == EXPR_STAR && alias != NULL) {
            record_error(
                parser, 
                "Alias is not allowed on '*'.",
                parser->previous.line, 
                parser->previous.column
            );
        }

        /* Determine display name */
        char *name;
        if (alias) {
            name = alias;
        } else if (expr->type == EXPR_COLUMN_REF) {
            name = expr->str_value;
        } else if (expr->type == EXPR_STAR) {
            name = NULL;  /* expanded in executor */
        } else {
            name = source_text;
        }

        if (!ensure_capacity(parser, (void**)&items, &capacity, count,
                             sizeof(SelectItem))) {
            return NULL;
        }

        items[count].expr = expr;
        items[count].name = name;
        items[count].alias = alias;
        items[count].source_text = source_text;
        count++;

        if (!match(parser, TOKEN_COMMA)) break;
    }

    *out_count = count;
    return items;
}

SelectStmt* select_stmt_init(void* mem, bool distinct) {
    SelectStmt* stmt = (SelectStmt*)mem;
    stmt->items = NULL;
    stmt->item_count = 0;
    stmt->distinct = distinct;
    stmt->table_name = NULL;
    stmt->where = NULL;
    stmt->group_by = NULL;
    stmt->group_by_count = 0;
    stmt->having = NULL;
    stmt->order_by = NULL;
    stmt->order_by_count = 0;
    stmt->limit = 0;
    stmt->offset = 0;
    stmt->has_limit = false;
    stmt->has_offset = false;

    return stmt;
}

/* ===== SELECT query ===== */
static SelectStmt* parse_select_query(Parser *parser) {
    consume(parser, TOKEN_SELECT, "Expected 'SELECT'.");

    bool distinct = false;
    if (match(parser, TOKEN_DISTINCT)) {
        distinct = true;
    }

    void *mem;
    ArenaResult ar = arena_alloc(parser->arena, sizeof(SelectStmt), &mem);
    if (ar != ARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    SelectStmt *stmt = select_stmt_init(mem, distinct);

    stmt->items = parse_select_list(parser, &stmt->item_count);

    consume(parser, TOKEN_FROM, "Expected 'FROM' after column list.");

    /* Table name: a single-quoted path (legacy style), a double-quoted
       identifier ("my data.csv", SQL-standard quoted identifier), or a bare
       identifier (students). The executor resolves the path against the
       filesystem and also tries appending ".csv" when none is given. */
    if (parser->current.type != TOKEN_STRING && parser->current.type != TOKEN_IDENTIFIER) {
        record_error(parser, "Expected table name after 'FROM'.",
                     parser->current.line, parser->current.column);
        sync_after_error(parser);
        stmt->table_name = NULL;
    } else {
        advance(parser);
        if (parser->previous.type == TOKEN_STRING) {
            stmt->table_name = copy_string_literal(parser, parser->previous.lexeme,
                                                   parser->previous.length);
        } else {
            stmt->table_name = copy_lexeme(parser, parser->previous.lexeme,
                                           parser->previous.length);
        }
    }

    stmt->where = parse_where(parser);

    parse_group_by(parser, stmt);

    parse_having(parser, stmt);

    parse_order_by(parser, stmt);

    parse_limit_offset(parser, stmt);

    match(parser, TOKEN_SEMICOLON);

    return stmt;
}

/* ===== Public API ===== */
SelectStmt* parse_select(const char *source, Arena *arena, ParseErrorList *errors) {
    Parser parser = parser_init(source, arena, errors);

    advance(&parser);

    SelectStmt *stmt = parse_select_query(&parser);

    if (!check(&parser, TOKEN_EOF)) {
        error_at_current(&parser, "Unexpected token after SELECT statement.");
    }

    return stmt;
}
