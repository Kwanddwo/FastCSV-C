#ifndef QUERY_AST_H
#define QUERY_AST_H

#include <stdbool.h>

/* Forward declaration */
typedef struct SelectStmt SelectStmt;

/* ===== Expression types ===== */
typedef enum {
    /* Literals / identifiers */
    EXPR_LITERAL_NUMBER,
    EXPR_LITERAL_STRING,
    EXPR_LITERAL_NULL,
    EXPR_COLUMN_REF,
    EXPR_STAR,               /* bare asterisk, e.g. COUNT(*) */

    /* Unary arithmetic */
    EXPR_UNARY_PLUS,
    EXPR_UNARY_MINUS,

    /* Binary arithmetic */
    EXPR_ADD,
    EXPR_SUB,
    EXPR_MUL,
    EXPR_DIV,
    EXPR_MOD,

    /* Bitwise */
    EXPR_BIT_AND,
    EXPR_BIT_OR,
    EXPR_BIT_XOR,

    /* String concatenation (SQL-standard ||) */
    EXPR_CONCAT,

    /* Comparison */
    EXPR_EQ,
    EXPR_NE,
    EXPR_LT,
    EXPR_GT,
    EXPR_LE,
    EXPR_GE,

    /* Search condition logical */
    EXPR_AND,
    EXPR_OR,
    EXPR_NOT,

    /* Search condition special */
    EXPR_IN,
    EXPR_NOT_IN,
    EXPR_BETWEEN,
    EXPR_LIKE,
    EXPR_ILIKE,
    EXPR_IS_NULL,
    EXPR_IS_NOT_NULL,

    /* Datetime */
    EXPR_DATETIME_VALUE,   /* CURRENT_DATE etc.; kind in num_value */
    EXPR_DATE_LITERAL,     /* DATE / TIME / TIMESTAMP '...' */
    EXPR_EXTRACT,          /* EXTRACT(field FROM value); field in str_value */

    /* Complex */
    EXPR_FUNCTION_CALL,
    EXPR_CASE,
    EXPR_SUBQUERY,

    /* ORDER BY positional reference to a select item that is '*': the
       executor resolves it against the star-expanded output columns
       (num_value holds the 1-based result-column ordinal). */
    EXPR_ORDER_ORDINAL,
} ExprType;

/* ===== CASE WHEN clause ===== */
typedef struct CaseWhen {
    struct ExprNode *condition;    /* WHEN condition */
    struct ExprNode *result;       /* THEN result */
    struct CaseWhen *next;
} CaseWhen;

/* ===== Expression node ===== */
typedef struct ExprNode {
    ExprType type;

    /* Values */
    char *str_value;         /* string literal, column name, function name */
    double num_value;        /* numeric literal */
    int col_index;           /* resolved header index for EXPR_COLUMN_REF; -1 until resolved */

    /* Children */
    struct ExprNode *left;   /* primary operand */
    struct ExprNode *right;  /* secondary operand (or BETWEEN start) */
    struct ExprNode *mid;    /* BETWEEN end */

    /* Variable-length children (IN list, function args) */
    int arg_count;
    struct ExprNode **args;

    /* Function call flag: DISTINCT modifier (e.g. COUNT(DISTINCT x)) */
    bool distinct;

    /* Resolved once at parse time so the per-row hot path never re-scans
       tables or re-parses text:
       - func_index: index into the scalar function table (-1 = unknown),
       - agg_kind:   aggregate kind (0 = not an aggregate),
       - text_numeric: a string literal whose text fully parses as a number
         (num_value holds the parsed value; str_value keeps the raw text). */
    int func_index;
    int agg_kind;
    bool text_numeric;

    /* CASE */
    struct CaseWhen *case_whens;
    struct ExprNode *case_else;

    /* Subquery */
    SelectStmt *subquery;
} ExprNode;

/* ===== Expression size bound =====
 * Deep trees (chains of additions, nested parens, unary/NOT recursion)
 * would overflow the C stack in the parser, the evaluator and the tree
 * walkers. The engine therefore limits expression depth to 1000 at parse
 * time (SQLite uses the same figure), enforced iteratively so the check
 * itself cannot recurse. All walkers that may see a tree past the cap
 * (defensive, for hand-built trees) also bound their recursion. */
#define MAX_EXPR_DEPTH 1000

/* Enumerate the children of an expression node by index (left, right, mid,
   IN/function arguments, CASE WHEN conditions/results, CASE ELSE), or NULL
   when exhausted. Indexed access keeps walkers' explicit stacks bounded. */
static inline const ExprNode* expr_node_child_at(const ExprNode *n, int i) {
    if (n->left) { if (i == 0) return n->left; i--; }
    if (n->right) { if (i == 0) return n->right; i--; }
    if (n->mid) { if (i == 0) return n->mid; i--; }
    if (i < n->arg_count) return n->args[i];
    i -= n->arg_count;
    for (const CaseWhen *cw = n->case_whens; cw; cw = cw->next) {
        if (i == 0) return cw->condition;
        if (i == 1) return cw->result;
        i -= 2;
    }
    if (n->case_else) { if (i == 0) return n->case_else; }
    return NULL;
}

/* ===== Select item ===== */
typedef struct {
    ExprNode *expr;     /* expression (column ref, literal, complex expr, or * ) */
    char *name;         /* display name: alias, column name, or source text */
    char *alias;        /* explicit AS alias, or NULL */
    char *source_text;  /* raw SQL text of the expression */
} SelectItem;

/* ===== ORDER BY item ===== */
typedef struct {
    ExprNode *expr;
    bool asc;       /* true = ASC (default), false = DESC */
    int nulls;      /* 0 = unspecified, 1 = NULLS FIRST, 2 = NULLS LAST */
} OrderByItem;

/* ===== SELECT statement ===== */
typedef struct SelectStmt {
    SelectItem *items;
    int item_count;
    bool distinct;
    char *table_name;
    ExprNode *where;
    ExprNode **group_by;
    int group_by_count;
    ExprNode *having;
    OrderByItem *order_by;
    int order_by_count;
    int limit;
    int offset;
    bool has_limit;
    bool has_offset;
} SelectStmt;

#endif
