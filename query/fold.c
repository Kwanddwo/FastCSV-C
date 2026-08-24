/* Constant folding: evaluates constant subtrees once, after parsing, and
 * replaces them with literal nodes so the per-row evaluation path never
 * re-computes them. Folding is semantics-preserving: only subtrees free of
 * column refs, aggregates, '*' and subqueries are touched (expr_is_constant),
 * and a subtree whose evaluation errors is left in place. */
#include "fold.h"
#include "eval.h"
#include "validate.h"

/* Build a literal AST node from an evaluated value. */
static ExprNode* alloc_literal_node(Arena *arena, const EvalResult *v) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(ExprNode), &mem);
    if (ar != ARENA_OK) return NULL;
    ExprNode *node = (ExprNode*)mem;
    node->col_index = -1;
    node->num_value = 0.0;
    node->str_value = NULL;
    node->left = NULL;
    node->right = NULL;
    node->mid = NULL;
    node->arg_count = 0;
    node->args = NULL;
    node->distinct = false;
    node->case_whens = NULL;
    node->case_else = NULL;
    node->subquery = NULL;

    if (v->is_null) {
        node->type = EXPR_LITERAL_NULL;
    } else if (v->is_numeric) {
        node->type = EXPR_LITERAL_NUMBER;
        node->num_value = v->num_val;
    } else {
        node->type = EXPR_LITERAL_STRING;
        node->str_value = arena_strdup(arena, v->str_val ? v->str_val : "");
    }
    return node;
}

/* Fold one subtree in place, returning the (possibly new) node. */
static ExprNode* fold_node(ExprNode *node, Arena *arena) {
    if (node == NULL) return NULL;

    /* Post-order: fold children first so operators see folded operands. */
    node->left = fold_node(node->left, arena);
    node->right = fold_node(node->right, arena);
    node->mid = fold_node(node->mid, arena);
    for (int i = 0; i < node->arg_count; i++) {
        node->args[i] = fold_node(node->args[i], arena);
    }
    for (CaseWhen *cw = node->case_whens; cw; cw = cw->next) {
        cw->condition = fold_node(cw->condition, arena);
        cw->result = fold_node(cw->result, arena);
    }
    node->case_else = fold_node(node->case_else, arena);

    /* Already a literal: nothing to gain. */
    if (node->type == EXPR_LITERAL_NUMBER ||
        node->type == EXPR_LITERAL_STRING ||
        node->type == EXPR_LITERAL_NULL) {
        return node;
    }
    if (!expr_is_constant(node)) return node;

    /* Evaluate once. A NULL record is safe: constant subtrees contain no
       column refs. A failing evaluation stays unfolded to keep the per-row
       error behavior (message and timing) unchanged. */
    EvalCtx ctx = eval_ctx_for(NULL, NULL, 0, arena, arena, NULL);
    EvalResult v = eval_expr(node, &ctx);
    if (eval_result_is_error(&v)) return node;

    ExprNode *lit = alloc_literal_node(arena, &v);
    return lit ? lit : node;
}

void fold_constants(SelectStmt *stmt, Arena *arena) {
    if (stmt == NULL) return;
    for (int i = 0; i < stmt->item_count; i++) {
        stmt->items[i].expr = fold_node(stmt->items[i].expr, arena);
    }
    stmt->where = fold_node(stmt->where, arena);
    for (int j = 0; j < stmt->group_by_count; j++) {
        stmt->group_by[j] = fold_node(stmt->group_by[j], arena);
    }
    stmt->having = fold_node(stmt->having, arena);
    for (int j = 0; j < stmt->order_by_count; j++) {
        stmt->order_by[j].expr = fold_node(stmt->order_by[j].expr, arena);
    }
}