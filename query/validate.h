#ifndef QUERY_VALIDATE_H
#define QUERY_VALIDATE_H

#include "qarena.h"
#include "ast.h"
#include "aggregate.h"
#include "eval.h"
#include <stdbool.h>

/* ===== Generic expression tree walker ===== */
typedef enum {
    EXPR_VISIT_CONTINUE,   /* visit children next */
    EXPR_VISIT_PRUNE,      /* skip children, keep walking the rest */
    EXPR_VISIT_ABORT,      /* stop the whole traversal */
} ExprVisit;

typedef ExprVisit (*ExprVisitFn)(ExprNode *node, void *ud);

/* Walk the tree; returns false if the traversal was ABORTed. */
bool expr_walk(ExprNode *node, ExprVisitFn visit, void *ud);

/* ===== Expression predicates ===== */
bool expr_contains_aggregate(ExprNode *node);
bool expr_is_constant(ExprNode *node);
bool validate_distinct_usage(ExprNode *node);
bool expr_grouped_valid(ExprNode *node, char **grouped, int grouped_count);

/* ===== Column lookup ===== */
int find_column_index(const char *name, char **headers, int header_count);

/* Validate an expression's column refs, returning an arena-owned error string
   or NULL if valid. Resolves and caches each column index on the node. */
const char* validate_columns(ExprNode *expr, char **headers, int header_count,
                             QArena *arena, const char **bad_col);

/* ===== Statement validation ===== */
const char* validate_stmt(SelectStmt *stmt, char **headers, int header_count,
                          QArena *arena, const char **bad_col);
const char* validate_grouping(SelectStmt *stmt, OutputCol *out_cols, int out_count,
                              char **grouped_cols, int grouped_col_count,
                              bool grouped, bool group_mode, QArena *arena);

/* ===== Aggregate/column collection ===== */
void collect_specs(ExprNode *node, AggSpec **specs, int *spec_count,
                   int *spec_cap, QArena *arena);
void collect_column_refs(ExprNode *node, char ***names, int *count, int *cap,
                         QArena *arena);

/* ===== Generate column ref ExprNode for star expansion ===== */
ExprNode* make_column_ref_node(QArena *arena, const char *name);

#endif