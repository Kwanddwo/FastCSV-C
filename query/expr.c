/* Expression parsing: precedence-climbing expression grammar, CASE, and
 * search conditions (NOT / AND / OR, IN / BETWEEN / LIKE). Separated from
 * parser.c, which handles statement-level parsing (SELECT, FROM, WHERE,
 * GROUP BY, HAVING, ORDER BY, LIMIT / OFFSET). */
#include "parser_internal.h"
#include "str_util.h"
#include "date.h"
#include <stdlib.h>
#include <stdio.h>

/* ===== Forward declarations (mutual recursion) ===== */
static ExprNode* parse_arithmetic_primary(Parser *parser);
static ExprNode* parse_case_expression(Parser *parser);
static ExprNode* parse_not_expr(Parser *parser);
static ExprNode* parse_and_expr(Parser *parser);
static ExprNode* parse_primary_condition(Parser *parser);

/* ===== AST node allocators ===== */
static ExprNode* make_leaf(Parser *parser, ExprType type) {
    ExprNode *node = alloc_expr_node(parser);
    if (node) node->type = type;
    return node;
}

static ExprNode* make_binary(Parser *parser, ExprType type, ExprNode *left, ExprNode *right) {
    ExprNode *node = alloc_expr_node(parser);
    if (node) {
        node->type = type;
        node->left = left;
        node->right = right;
    }
    return node;
}

static ExprNode* make_unary(Parser *parser, ExprType type, ExprNode *operand) {
    ExprNode *node = alloc_expr_node(parser);
    if (node) {
        node->type = type;
        node->left = operand;
    }
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

/* bitwise_expr ::= additive_expr { ('&' | '|' | '^' | '||') additive_expr }
   The SQL-standard concatenation operator shares this tier (below additive):
   `a || b + c` is a || (b + c). */
static ExprNode* parse_bitwise_expr(Parser *parser) {
    ExprNode *expr = parse_additive_expr(parser);

    while (match(parser, TOKEN_AMPERSAND) || match(parser, TOKEN_PIPE) ||
           match(parser, TOKEN_CARET) || match(parser, TOKEN_CONCAT)) {
        TokenType op = parser->previous.type;
        ExprNode *right = parse_additive_expr(parser);
        ExprType type;
        if (op == TOKEN_AMPERSAND) type = EXPR_BIT_AND;
        else if (op == TOKEN_PIPE) type = EXPR_BIT_OR;
        else if (op == TOKEN_CARET) type = EXPR_BIT_XOR;
        else type = EXPR_CONCAT;
        expr = make_binary(parser, type, expr, right);
    }
    return expr;
}

/* expression ::= bitwise_expr */
ExprNode* parse_expression(Parser *parser) {
    return parse_bitwise_expr(parser);
}

/* ===== Primary expression ===== */

/* Parse a parenthesized, comma-separated list of expressions (the argument
   list of a function call or an IN / NOT IN list). Returns the arena-allocated
   array and sets *out_count, or NULL on OOM (error already recorded). */
ExprNode** parse_expr_list(Parser *parser, int *out_count, const char *item_msg) {
    int capacity = LIST_INITIAL_CAPACITY;
    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, sizeof(ExprNode*) * (size_t)capacity, &mem);
    if (ar != QARENA_OK) { parser_oom(parser, parser->current.line, parser->current.column); return NULL; }
    ExprNode **items = (ExprNode**)mem;
    int count = 0;

    for (;;) {
        if (count >= capacity) {
            capacity *= 2;
            mem = qarena_realloc(parser->arena, items,
                                sizeof(ExprNode*) * (size_t)(capacity / 2),
                                sizeof(ExprNode*) * (size_t)capacity);
            if (mem == NULL) { parser_oom(parser, parser->current.line, parser->current.column); return NULL; }
            items = (ExprNode**)mem;
        }
        items[count++] = parse_expression(parser);
        /* Fail-stop: under OOM the token helpers stop consuming, so this loop
           must break explicitly or it would spin on a frozen token. */
        if (parser->oom) break;
        if (match(parser, TOKEN_RPAREN)) break;
        consume(parser, TOKEN_COMMA, item_msg);
    }

    *out_count = count;
    return items;
}

static ExprNode* parse_function_args(Parser *parser, const char *func_name) {
    ExprNode *node = alloc_expr_node(parser);
    if (node == NULL) return NULL;
    node->type = EXPR_FUNCTION_CALL;
    node->str_value = qarena_strdup(parser->arena, func_name);
    node->distinct = match(parser, TOKEN_DISTINCT);

    if (match(parser, TOKEN_RPAREN)) {
        node->arg_count = 0;
        node->args = NULL;
        return node;
    }

    /* Standard POSITION(substring IN string) syntax: the arguments are
       separated by IN, not a comma. */
    if (str_ieq(func_name, "POSITION")) {
        void *mem;
        QArenaResult ar = qarena_alloc(parser->arena, sizeof(ExprNode*) * 2, &mem);
        if (ar != QARENA_OK) { parser_oom(parser, parser->current.line, parser->current.column); return node; }
        ExprNode **args = (ExprNode**)mem;
        args[0] = parse_expression(parser);
        consume(parser, TOKEN_IN, "Expected 'IN' in POSITION(substring IN string).");
        args[1] = parse_expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after POSITION arguments.");
        node->arg_count = 2;
        node->args = args;
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
            if (node == NULL) return NULL;
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
        if (node == NULL) return NULL;
        node->str_value = copy_lexeme(parser, parser->previous.lexeme, parser->previous.length);
        /* A NULL lexeme means the arena ran out of memory; copy_lexeme already
           flagged the parse and strtod must not see the NULL string. */
        if (node->str_value == NULL) return NULL;
        node->num_value = strtod(node->str_value, NULL);
        return node;
    }

    /* Datetime value functions (standard: CURRENT_DATE, CURRENT_TIME,
       CURRENT_TIMESTAMP, LOCALTIME, LOCALTIMESTAMP). */
    if (parser->current.type == TOKEN_CURRENT_DATE ||
        parser->current.type == TOKEN_CURRENT_TIME ||
        parser->current.type == TOKEN_CURRENT_TIMESTAMP ||
        parser->current.type == TOKEN_LOCALTIME ||
        parser->current.type == TOKEN_LOCALTIMESTAMP) {
        TokenType kw = parser->current.type;
        advance(parser);
        ExprNode *node = make_leaf(parser, EXPR_DATETIME_VALUE);
        if (node == NULL) return NULL;
        switch (kw) {
            case TOKEN_CURRENT_DATE: node->num_value = DT_CURRENT_DATE; break;
            case TOKEN_CURRENT_TIME: node->num_value = DT_CURRENT_TIME; break;
            case TOKEN_CURRENT_TIMESTAMP: node->num_value = DT_CURRENT_TIMESTAMP; break;
            case TOKEN_LOCALTIME: node->num_value = DT_LOCALTIME; break;
            default: node->num_value = DT_LOCALTIMESTAMP; break;
        }
        return node;
    }

    /* Datetime literals: DATE / TIME / TIMESTAMP '...' (standard since
       SQL-92). The literal string is validated here; a malformed value is a
       parse error, not a runtime NULL. */
    if (parser->current.type == TOKEN_DATE ||
        parser->current.type == TOKEN_TIME ||
        parser->current.type == TOKEN_TIMESTAMP) {
        TokenType kw = parser->current.type;
        advance(parser);
        consume(parser, TOKEN_STRING, "Expected string literal after date/time keyword.");
        char *str = copy_string_literal(parser, parser->previous.lexeme,
                                        parser->previous.length);
        bool ok = (kw == TOKEN_DATE) ? is_valid_iso_date(str)
                 : (kw == TOKEN_TIME) ? is_valid_iso_time(str)
                                      : is_valid_iso_timestamp(str);
        if (!ok) {
            record_error(parser, "Invalid date/time literal.",
                         parser->previous.line, parser->previous.column);
        }
        ExprNode *node = make_leaf(parser, EXPR_DATE_LITERAL);
        if (node == NULL) return NULL;
        node->str_value = str;
        return node;
    }

    /* EXTRACT(field FROM expr): the field is a text identifier (YEAR, MONTH,
       DAY, HOUR, MINUTE, SECOND, QUARTER), so those words stay usable as
       column names everywhere else. */
    if (match(parser, TOKEN_EXTRACT)) {
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'EXTRACT'.");
        if (parser->current.type != TOKEN_IDENTIFIER) {
            error_at_current(parser, "Expected a field name (YEAR, MONTH, DAY, ...) in EXTRACT.");
            return make_error_node(parser, "Expected a field name in EXTRACT.");
        }
        Token field_tok = parser->current;
        advance(parser);
        char *field = copy_lexeme(parser, field_tok.lexeme, field_tok.length);
        if (!is_valid_extract_field(field)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Unknown EXTRACT field '%s'.", field);
            record_error(parser, buf, field_tok.line, field_tok.column);
        }
        consume(parser, TOKEN_FROM, "Expected 'FROM' in EXTRACT.");
        ExprNode *value = parse_expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after EXTRACT.");
        ExprNode *node = make_leaf(parser, EXPR_EXTRACT);
        if (node == NULL) return NULL;
        node->str_value = field;
        node->left = value;
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
            char *qualified = qarena_concat(parser, name, ".");
            char *full = qarena_concat(parser, qualified, col);
            ExprNode *node = make_leaf(parser, EXPR_COLUMN_REF);
            if (node == NULL) return NULL;
            node->str_value = full;
            return node;
        }

        /* Simple column reference */
        ExprNode *node = make_leaf(parser, EXPR_COLUMN_REF);
        if (node == NULL) return NULL;
        node->str_value = name;
        return node;
    }

    error_at_current(parser, "Expected expression.");
    return make_error_node(parser, "Expected expression.");
}

/* ===== CASE expression ===== */
static ExprNode* parse_case_expression(Parser *parser) {
    ExprNode *node = alloc_expr_node(parser);
    if (node == NULL) return NULL;
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
        QArenaResult ar = qarena_alloc(parser->arena, sizeof(CaseWhen), (void**)&cw);
        if (ar != QARENA_OK) { parser_oom(parser, parser->current.line, parser->current.column); return NULL; }
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
            if (node == NULL) return NULL;
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
        if (node == NULL) return NULL;
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
        if (node == NULL) return NULL;
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

    /* IS [NOT] NULL — the only NULL test that never yields UNKNOWN */
    if (match(parser, TOKEN_IS)) {
        bool negate = match(parser, TOKEN_NOT);
        consume(parser, TOKEN_NULL, "Expected 'NULL' after 'IS'.");
        ExprNode *node = alloc_expr_node(parser);
        if (node == NULL) return NULL;
        node->type = negate ? EXPR_IS_NOT_NULL : EXPR_IS_NULL;
        node->left = expr;
        return node;
    }

    /* Bare expression (truthy evaluation) */
    return expr;
}

/* parse_search_condition ::= parse_or_expr */
ExprNode* parse_search_condition(Parser *parser) {
    return parse_or_expr(parser);
}