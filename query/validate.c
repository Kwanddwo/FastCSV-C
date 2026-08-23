/* Statement validation: expression tree walking, column-reference
 * resolution/caching, aggregate and grouping rules. Split out of executor.c. */
#include "validate.h"
#include "str_util.h"
#include <string.h>
#include <stdio.h>

/* Initial capacity for growable arrays (doubled on demand). */
static const int COL_REFS_INITIAL_CAPACITY = 16; /* column-ref name lists */
static const int SPECS_INITIAL_CAPACITY = 8;   /* aggregate spec descriptors */

/* ===== Column lookup ===== */
int find_column_index(const char *name, char **headers, int header_count) {
    for (int i = 0; i < header_count; i++) {
        if (strcmp(name, headers[i]) == 0) return i;
    }
    /* Try stripping table prefix (text before dot) */
    const char *dot = strchr(name, '.');
    if (dot) {
        for (int i = 0; i < header_count; i++) {
            if (strcmp(dot + 1, headers[i]) == 0) return i;
        }
    }
    return -1;
}

bool expr_walk(ExprNode *node, ExprVisitFn visit, void *ud) {
    if (node == NULL) return true;

    switch (visit(node, ud)) {
        case EXPR_VISIT_ABORT: return false;
        case EXPR_VISIT_PRUNE: return true;
        default: break;
    }

    if (!expr_walk(node->left, visit, ud)) return false;
    if (!expr_walk(node->right, visit, ud)) return false;
    if (!expr_walk(node->mid, visit, ud)) return false;
    for (int i = 0; i < node->arg_count; i++) {
        if (!expr_walk(node->args[i], visit, ud)) return false;
    }
    for (CaseWhen *cw = node->case_whens; cw; cw = cw->next) {
        if (!expr_walk(cw->condition, visit, ud)) return false;
        if (!expr_walk(cw->result, visit, ud)) return false;
    }
    return expr_walk(node->case_else, visit, ud);
}

static bool name_in_list(const char *name, char **names, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

/* ===== Expression predicates ===== */

/* Does the expression tree contain an aggregate function call? */
static ExprVisit contains_aggregate_visit(ExprNode *node, void *ud) {
    (void)ud;
    if (node->type == EXPR_FUNCTION_CALL && is_aggregate_name(node->str_value))
        return EXPR_VISIT_ABORT;
    return EXPR_VISIT_CONTINUE;
}

bool expr_contains_aggregate(ExprNode *node) {
    return !expr_walk(node, contains_aggregate_visit, NULL);
}

/* Is the expression constant (no column refs, aggregates, or subqueries)? */
static ExprVisit is_constant_visit(ExprNode *node, void *ud) {
    (void)ud;
    switch (node->type) {
        case EXPR_LITERAL_NUMBER:
        case EXPR_LITERAL_STRING:
        case EXPR_LITERAL_NULL:
            return EXPR_VISIT_CONTINUE;
        case EXPR_STAR:
        case EXPR_COLUMN_REF:
        case EXPR_SUBQUERY:
            return EXPR_VISIT_ABORT;
        default:
            break;
    }
    if (node->type == EXPR_FUNCTION_CALL && is_aggregate_name(node->str_value))
        return EXPR_VISIT_ABORT;
    return EXPR_VISIT_CONTINUE;
}

bool expr_is_constant(ExprNode *node) {
    return expr_walk(node, is_constant_visit, NULL);
}

/* Reject DISTINCT on non-aggregate functions anywhere in an expression tree */
static ExprVisit validate_distinct_visit(ExprNode *node, void *ud) {
    (void)ud;
    if (node->type == EXPR_FUNCTION_CALL && node->distinct &&
        !is_aggregate_name(node->str_value))
        return EXPR_VISIT_ABORT;
    return EXPR_VISIT_CONTINUE;
}

bool validate_distinct_usage(ExprNode *node) {
    return expr_walk(node, validate_distinct_visit, NULL);
}

/* Validate column references exist */
typedef struct {
    char **headers;
    int header_count;
    const char **err_col;
} ColRefCheckUD;

static ExprVisit has_valid_column_refs_visit(ExprNode *node, void *ud) {
    ColRefCheckUD *u = (ColRefCheckUD*)ud;
    if (node->type == EXPR_COLUMN_REF) {
        int idx = find_column_index(node->str_value, u->headers, u->header_count);
        if (idx < 0) {
            *u->err_col = node->str_value;
            return EXPR_VISIT_ABORT;
        }
        /* Cache the resolved index so the per-row eval path is a direct
           array lookup instead of a header scan. */
        node->col_index = idx;
        return EXPR_VISIT_PRUNE;
    }
    return EXPR_VISIT_CONTINUE;
}

static bool has_valid_column_refs(ExprNode *node, char **headers, int header_count,
                                  const char **err_col) {
    ColRefCheckUD u = { headers, header_count, err_col };
    return expr_walk(node, has_valid_column_refs_visit, &u);
}

/* Validate an expression's column refs, returning an arena-owned error string
   or NULL if valid. */
const char* validate_columns(ExprNode *expr, char **headers, int header_count,
                                    Arena *arena, const char **bad_col) {
    if (!has_valid_column_refs(expr, headers, header_count, bad_col)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Column '%s' not found in CSV headers.", *bad_col);
        char *msg = arena_strdup(arena, buf);
        return msg ? msg : "Column not found in CSV headers.";
    }
    return NULL;
}

/* Collect descriptors for every aggregate call in the tree. */
typedef struct {
    AggSpec **specs;
    int *count;
    int *cap;
    Arena *arena;
} CollectSpecsUD;

static ExprVisit collect_specs_visit(ExprNode *node, void *ud) {
    CollectSpecsUD *u = (CollectSpecsUD*)ud;
    if (node->type == EXPR_FUNCTION_CALL && is_aggregate_name(node->str_value)) {
        if (*u->count >= *u->cap) {
            *u->cap = *u->cap ? *u->cap * 2 : SPECS_INITIAL_CAPACITY;
            void *mem = arena_realloc(u->arena, *u->specs,
                                      sizeof(AggSpec) * (size_t)(*u->cap / 2),
                                      sizeof(AggSpec) * (size_t)*u->cap);
            if (mem == NULL) return EXPR_VISIT_PRUNE;
            *u->specs = (AggSpec*)mem;
        }
        AggSpec *s = &(*u->specs)[(*u->count)++];
        s->node = node;
        s->name = node->str_value;
        s->distinct = node->distinct;
        /* Aggregate arguments may not themselves contain aggregates */
        return EXPR_VISIT_PRUNE;
    }
    return EXPR_VISIT_CONTINUE;
}

void collect_specs(ExprNode *node, AggSpec **specs, int *spec_count,
                          int *spec_cap, Arena *arena) {
    CollectSpecsUD u = { specs, spec_count, spec_cap, arena };
    expr_walk(node, collect_specs_visit, &u);
}

/* Collect the names of all column refs under an expression. */
typedef struct {
    char ***names;
    int *count;
    int *cap;
    Arena *arena;
} CollectRefsUD;

static ExprVisit collect_column_refs_visit(ExprNode *node, void *ud) {
    CollectRefsUD *u = (CollectRefsUD*)ud;
    if (node->type == EXPR_COLUMN_REF && node->str_value) {
        if (*u->count >= *u->cap) {
            *u->cap = *u->cap ? *u->cap * 2 : COL_REFS_INITIAL_CAPACITY;
            void *mem = arena_realloc(u->arena, *u->names,
                                      sizeof(char*) * (size_t)(*u->cap / 2),
                                      sizeof(char*) * (size_t)*u->cap);
            if (mem == NULL) return EXPR_VISIT_PRUNE;
            *u->names = (char**)mem;
        }
        (*u->names)[(*u->count)++] = node->str_value;
        return EXPR_VISIT_PRUNE;
    }
    return EXPR_VISIT_CONTINUE;
}

void collect_column_refs(ExprNode *node, char ***names, int *count, int *cap,
                                Arena *arena) {
    CollectRefsUD u = { names, count, cap, arena };
    expr_walk(node, collect_column_refs_visit, &u);
}

/* True if every non-aggregate column ref in the expression is grouped.
   Column refs inside aggregate arguments are exempt. */
typedef struct {
    char **grouped;
    int grouped_count;
} GroupedUD;

static ExprVisit grouped_valid_visit(ExprNode *node, void *ud) {
    GroupedUD *u = (GroupedUD*)ud;
    if (node->type == EXPR_COLUMN_REF) {
        if (!node->str_value || !name_in_list(node->str_value, u->grouped, u->grouped_count))
            return EXPR_VISIT_ABORT;
        return EXPR_VISIT_PRUNE;
    }
    if (node->type == EXPR_FUNCTION_CALL && is_aggregate_name(node->str_value))
        return EXPR_VISIT_PRUNE;
    return EXPR_VISIT_CONTINUE;
}

bool expr_grouped_valid(ExprNode *node, char **grouped, int grouped_count) {
    GroupedUD u = { grouped, grouped_count };
    return expr_walk(node, grouped_valid_visit, &u);
}

/* ===== Generate column ref ExprNode for star expansion ===== */
ExprNode* make_column_ref_node(Arena *arena, const char *name) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(ExprNode), &mem);
    if (ar != ARENA_OK) return NULL;
    ExprNode *node = (ExprNode*)mem;
    node->type = EXPR_COLUMN_REF;
    node->str_value = arena_strdup(arena, name);
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

const char* validate_stmt(SelectStmt *stmt, char **headers, int header_count,
                                 Arena *arena, const char **bad_col) {
    for (int i = 0; i < stmt->item_count; i++) {
        const char *err = validate_columns(stmt->items[i].expr, headers,
                                           header_count, arena, bad_col);
        if (err) return err;
    }
    if (stmt->where) {
        const char *err = validate_columns(stmt->where, headers, header_count,
                                           arena, bad_col);
        if (err) return err;
    }
    for (int j = 0; j < stmt->group_by_count; j++) {
        const char *err = validate_columns(stmt->group_by[j], headers,
                                           header_count, arena, bad_col);
        if (err) return err;
    }
    if (stmt->having) {
        const char *err = validate_columns(stmt->having, headers, header_count,
                                           arena, bad_col);
        if (err) return err;
    }
    for (int j = 0; j < stmt->order_by_count; j++) {
        const char *err = validate_columns(stmt->order_by[j].expr, headers,
                                           header_count, arena, bad_col);
        if (err) return err;
    }

    for (int i = 0; i < stmt->item_count; i++) {
        if (!validate_distinct_usage(stmt->items[i].expr))
            return "DISTINCT is only allowed with aggregate functions.";
    }
    if (stmt->where && !validate_distinct_usage(stmt->where))
        return "DISTINCT is only allowed with aggregate functions.";
    for (int j = 0; j < stmt->order_by_count; j++) {
        if (!validate_distinct_usage(stmt->order_by[j].expr))
            return "DISTINCT is only allowed with aggregate functions.";
    }

    /* Aggregates in WHERE are not supported */
    if (stmt->where && expr_contains_aggregate(stmt->where))
        return "Aggregate functions are not supported in WHERE.";
    return NULL;
}
const char* validate_grouping(SelectStmt *stmt, OutputCol *out_cols, int out_count,
                                     char **grouped_cols, int grouped_col_count,
                                     bool grouped, bool group_mode, Arena *arena) {
    if (grouped) {
        if (group_mode) {
            /* Standard SQL: every non-aggregate column ref must be grouped */
            for (int i = 0; i < out_count; i++) {
                if (!expr_grouped_valid(out_cols[i].expr, grouped_cols, grouped_col_count))
                    return "Column must appear in the GROUP BY clause or be used in an aggregate function.";
            }
            if (stmt->having &&
                !expr_grouped_valid(stmt->having, grouped_cols, grouped_col_count))
                return "Column in HAVING must appear in the GROUP BY clause or be used in an aggregate function.";
            for (int j = 0; j < stmt->order_by_count; j++) {
                if (!expr_grouped_valid(stmt->order_by[j].expr, grouped_cols, grouped_col_count))
                    return "Column in ORDER BY must appear in the GROUP BY clause or be used in an aggregate function.";
            }
        } else {
            /* Aggregates without GROUP BY: each item must contain an aggregate
               or be a constant expression */
            for (int i = 0; i < out_count; i++) {
                if (!expr_contains_aggregate(out_cols[i].expr) &&
                    !expr_is_constant(out_cols[i].expr)) {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                             "Non-aggregate expression '%s' in aggregate query "
                             "requires GROUP BY (not supported).",
                             out_cols[i].name ? out_cols[i].name : "");
                    char *msg = arena_strdup(arena, buf);
                    return msg ? msg : "Non-aggregate expression in aggregate query.";
                }
            }
        }
    } else {
        for (int j = 0; j < stmt->order_by_count; j++) {
            if (expr_contains_aggregate(stmt->order_by[j].expr))
                return "Aggregate functions are not allowed in ORDER BY without an aggregate query.";
        }
        if (stmt->having)
            return "HAVING without GROUP BY requires an aggregate function.";
    }
    return NULL;
}
