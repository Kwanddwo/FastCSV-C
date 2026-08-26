/* Statement parsing: parser state, error recording/recovery, and the
 * SELECT statement grammar (select list, FROM, WHERE, GROUP BY, HAVING,
 * ORDER BY, LIMIT / OFFSET). Expression parsing lives in expr.c. */
#include "parser.h"
#include "parser_internal.h"
#include "scanner.h"
#include "str_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#define MAX_PARSE_ERRORS 50

/* ===== Init ===== */
Parser parser_init(const char *source, QArena *arena, ParseErrorList *errors) {
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
ParseErrorList* parse_error_list_init(QArena *arena) {
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(ParseErrorList), &mem);
    if (ar != QARENA_OK) return NULL;
    ParseErrorList *list = (ParseErrorList*)mem;
    list->errors = NULL;
    list->error_lines = NULL;
    list->error_columns = NULL;
    list->count = 0;
    list->capacity = 0;
    return list;
}

void record_error(Parser *parser, const char *msg, int line, int col) {
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

        const char **new_errors = (const char**)qarena_realloc(
            parser->arena, list->errors,
            sizeof(const char*) * (size_t)list->capacity,
            sizeof(const char*) * (size_t)new_cap);
        if (new_errors == NULL) return;

        int *new_lines = (int*)qarena_realloc(
            parser->arena, list->error_lines,
            sizeof(int) * (size_t)list->capacity,
            sizeof(int) * (size_t)new_cap);
        if (new_lines == NULL) return;

        int *new_cols = (int*)qarena_realloc(
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

    const char *stored = qarena_strdup(parser->arena, msg);
    if (stored == NULL) stored = "Out of memory.";
    list->errors[list->count] = stored;
    list->error_lines[list->count] = line;
    list->error_columns[list->count] = col;
    list->count++;
}

/* ===== Token helpers ===== */
void advance(Parser *parser) {
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

bool check(Parser *parser, TokenType type) {
    return parser->current.type == type;
}

bool match(Parser *parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

void consume(Parser *parser, TokenType type, const char *message) {
    if (parser->current.type == type) {
        advance(parser);
        return;
    }

    record_error(parser, message, parser->current.line, parser->current.column);
    sync_after_error(parser);
}

void error_at_current(Parser *parser, const char *message) {
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
void sync_after_error(Parser *parser) {
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

ExprNode* make_error_node(Parser *parser, const char *msg) {
    /* The message has already been recorded by the caller; the returned node
       is only a placeholder so parsing can continue. */
    (void)msg;
    return alloc_expr_node(parser);
}

/* ===== QArena string helpers ===== */
char* copy_lexeme(Parser *parser, const char *lexeme, int length) {
    if (lexeme == NULL) { lexeme = ""; length = 0; }
    if (length < 0) length = 0;
    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, (size_t)length + 1, &mem);
    if (ar != QARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    char *str = (char*)mem;
    memcpy(str, lexeme, (size_t)length);
    str[length] = '\0';
    return str;
}

char* copy_string_literal(Parser *parser, const char *start, int length) {
    if (start == NULL) { start = ""; length = 0; }
    int content_length = length - 2;
    if (content_length < 0) content_length = 0;
    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, (size_t)content_length + 1, &mem);
    if (ar != QARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
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

char* qarena_concat(Parser *parser, const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, alen + blen + 1, &mem);
    if (ar != QARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
    char *str = (char*)mem;
    memcpy(str, a, alen);
    memcpy(str + alen, b, blen);
    str[alen + blen] = '\0';
    return str;
}

/* ===== AST node allocators ===== */
ExprNode* alloc_expr_node(Parser *parser) {
    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, sizeof(ExprNode), &mem);
    if (ar != QARENA_OK) {
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
    void *mem = qarena_realloc(parser->arena, *array,
                              elem_size * (size_t)*capacity,
                              elem_size * (size_t)new_cap);
    if (mem == NULL) { error_at_current(parser, "Out of memory."); return false; }
    *array = mem;
    *capacity = new_cap;
    return true;
}

/* Resolve an ORDER BY reference to a select item when possible:
   - a whole-number literal N in [1, item_count] is an ordinal: substitute
     items[N-1].expr (the standard's position reference; nodes are read-only
     during execution, so sharing the pointer is safe);
   - an identifier matching a non-star select item's display name (alias or
     result-column name) substitutes that item's expression.
   Anything else is returned unchanged (validated as a column or constant
   later). A whole-number literal outside the select list is an error. */
static ExprNode* resolve_select_ref(Parser *parser, SelectStmt *stmt, ExprNode *node) {
    if (node->type == EXPR_LITERAL_NUMBER) {
        double v = node->num_value;
        if (v >= -1e9 && v <= 1e9 && v == (double)(int)v) {
            int pos = (int)v;
            if (pos < 1 || pos > stmt->item_count) {
                char buf[64];
                snprintf(buf, sizeof(buf), "SELECT position %d is not in the select list.", pos);
                record_error(parser, buf, parser->previous.line, parser->previous.column);
            } else if (stmt->items[pos - 1].expr->type != EXPR_STAR) {
                return stmt->items[pos - 1].expr;
            }
            /* A valid position holding '*' is left as a constant key: the
               star-expanded output columns are only known after expansion. */
        }
    } else if (node->type == EXPR_COLUMN_REF && node->str_value != NULL) {
        for (int i = 0; i < stmt->item_count; i++) {
            if (stmt->items[i].expr->type == EXPR_STAR) continue;
            if (stmt->items[i].name != NULL &&
                str_ieq(stmt->items[i].name, node->str_value)) {
                return stmt->items[i].expr;
            }
        }
    }
    return node;
}

static void parse_order_by(Parser *parser, SelectStmt *stmt) {
    if (!match(parser, TOKEN_ORDER)) return;

    consume(parser, TOKEN_BY, "Expected 'BY' after 'ORDER'.");

    OrderByItem *items = NULL;
    int capacity = 0;
    int count = 0;

    for (;;) {
        ExprNode *expr = parse_expression(parser);
        expr = resolve_select_ref(parser, stmt, expr);

        bool asc = true;
        if (match(parser, TOKEN_ASC)) { asc = true; }
        else if (match(parser, TOKEN_DESC)) { asc = false; }

        int nulls = 0;
        if (match(parser, TOKEN_NULLS)) {
            if (match(parser, TOKEN_FIRST)) {
                nulls = 1;
            } else if (match(parser, TOKEN_LAST)) {
                nulls = 2;
            } else {
                error_at_current(parser, "Expected 'FIRST' or 'LAST' after 'NULLS'.");
            }
        }

        if (!ensure_capacity(parser, (void**)&items, &capacity, count,
                             sizeof(OrderByItem))) {
            return;
        }

        items[count].expr = expr;
        items[count].asc = asc;
        items[count].nulls = nulls;
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
SelectStmt* parse_select_query(Parser *parser) {
    consume(parser, TOKEN_SELECT, "Expected 'SELECT'.");

    bool distinct = false;
    if (match(parser, TOKEN_DISTINCT)) {
        distinct = true;
    }

    void *mem;
    QArenaResult ar = qarena_alloc(parser->arena, sizeof(SelectStmt), &mem);
    if (ar != QARENA_OK) { error_at_current(parser, "Out of memory."); return NULL; }
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
SelectStmt* parse_select(const char *source, QArena *arena, ParseErrorList *errors) {
    Parser parser = parser_init(source, arena, errors);

    advance(&parser);

    SelectStmt *stmt = parse_select_query(&parser);

    if (!check(&parser, TOKEN_EOF)) {
        error_at_current(&parser, "Unexpected token after SELECT statement.");
    }

    return stmt;
}