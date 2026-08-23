/* Expression evaluation: EvalResult values, built-in and aggregate-aware
 * function dispatch, arithmetic/comparison/logical operators, LIKE matching
 * and the recursive eval_expr tree walk. Split out of executor.c. */
#include "eval.h"
#include "aggregate.h"
#include "str_util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* ===== Constants ===== */
/* Doubles at or beyond this magnitude cannot be converted to long long
   without undefined behaviour (LLONG_MAX is ~9.22e18). */
static const double LL_CAST_SAFE_BOUND = 9.0e18;

/* Doubles below this magnitude are printed as integers. */
static const double INT_DISPLAY_BOUND = 1e15;

/* ===== EvalResult builders ===== */
EvalResult eval_result_null(void) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = true;
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = NULL;
    return r;
}

EvalResult eval_result_num(double val) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = false;
    r.is_numeric = true;
    r.num_val = val;
    r.str_val = NULL;
    return r;
}

EvalResult eval_result_str(const char *s) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = (s == NULL);
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = s ? s : "";
    return r;
}

EvalResult eval_result_error(const char *msg) {
    EvalResult r;
    r.is_error = true;
    r.error = msg;
    r.is_null = true;
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = NULL;
    return r;
}

bool eval_result_is_error(const EvalResult *r) {
    return r->is_error;
}

bool eval_result_is_true(const EvalResult *r) {
    if (r->is_error) return false;
    if (r->is_null) return false;
    if (r->is_numeric) return r->num_val != 0.0;
    return r->str_val != NULL && r->str_val[0] != '\0';
}

/* A string is numeric-like if it fully parses as a number, using the same
   rule as the column evaluator so literals classify identically to cells.
   Numeric values pass through unchanged. */
bool parse_numeric_str(const EvalResult *r, double *out) {
    if (r->is_numeric) {
        if (out) *out = r->num_val;
        return true;
    }
    if (r->str_val) {
        char *end;
        double v = strtod(r->str_val, &end);
        if (end != r->str_val && *end == '\0') {
            if (out) *out = v;
            return true;
        }
    }
    return false;
}

/* Value comparison. CSV has no declared column types, so a numeric value
   compares numerically against a string that parses as a number ('5' = 5 is
   true, '05' = 5 is true); a string that is not numeric never equals or
   orders below a number (NULL < numeric < text, SQLite storage-class order).
   Two strings compare textually even if both look numeric ('05' = '5' is
   false). This is the single ordering used by WHERE/HAVING predicates, ORDER
   BY (qsort and the top-k heap), DISTINCT aggregates, and MIN/MAX. */
int eval_result_compare(const EvalResult *a, const EvalResult *b) {
    if (a->is_null && b->is_null) return 0;
    if (a->is_null) return -1;
    if (b->is_null) return 1;
    if (a->is_numeric || b->is_numeric) {
        double av, bv;
        bool an = parse_numeric_str(a, &av);
        bool bn = parse_numeric_str(b, &bv);
        if (an && bn) {
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }
        /* Exactly one numeric-like operand: numbers sort before strings. */
        return an ? -1 : 1;
    }
    const char *as = a->str_val ? a->str_val : "";
    const char *bs = b->str_val ? b->str_val : "";
    return strcmp(as, bs);
}

/* Duplicate an EvalResult's display value into the arena with a single
   copy (numeric values are formatted straight into the arena). Returns
   NULL on allocation failure. */
const char* eval_result_dup_to_arena(const EvalResult *r, Arena *arena) {
    if (r->is_error) return r->error ? r->error : "";
    if (r->is_null) return "NULL";
    if (r->is_numeric) {
        char buf[128];
        double v = r->num_val;
        /* The range check must precede the cast: casting an out-of-range
           double to long long is undefined behaviour. */
        if (fabs(v) < LL_CAST_SAFE_BOUND && v == (long long)v && fabs(v) < INT_DISPLAY_BOUND) {
            snprintf(buf, sizeof(buf), "%lld", (long long)v);
        } else {
            snprintf(buf, sizeof(buf), "%.15g", v);
        }
        size_t len = strlen(buf);
        void *mem;
        ArenaResult ar = arena_alloc(arena, len + 1, &mem);
        if (ar != ARENA_OK) return NULL;
        char *out = (char*)mem;
        memcpy(out, buf, len);
        out[len] = '\0';
        return out;
    }
    return arena_strdup(arena, r->str_val ? r->str_val : "");
}

/* Convert EvalResult to display string */
const char* eval_result_to_string(const EvalResult *r, Arena *arena) {
    if (r->is_error) return r->error ? r->error : "";
    if (r->is_null) return "NULL";
    if (r->is_numeric) return eval_result_dup_to_arena(r, arena);
    return r->str_val ? r->str_val : "";
}



/* ===== LIKE pattern matching ===== */
bool like_match(const char *s, const char *p, bool case_insensitive) {
    /* Iterative greedy matcher: on a mismatch after a '%', retry with the
       '%' matching one more character. Avoids the exponential backtracking
       of a recursive implementation while preserving identical semantics. */
    const char *star_p = NULL;   /* pattern position after the last '%' */
    const char *star_s = NULL;   /* string position the last '%' matched */

    while (*s) {
        if (*p == '_') {
            s++;
            p++;
        } else if (*p == '%') {
            star_p = p++;
            star_s = s;
        } else {
            bool eq = case_insensitive
                          ? (toupper((unsigned char)*s) == toupper((unsigned char)*p))
                          : (*s == *p);
            if (eq) {
                s++;
                p++;
            } else if (star_p) {
                p = star_p + 1;
                s = ++star_s;
            } else {
                return false;
            }
        }
    }

    /* String exhausted: only trailing '%'s may remain in the pattern. */
    while (*p == '%') p++;
    return *p == '\0';
}

/* ===== Argument evaluation helpers ===== */
/* Evaluate args[i] as a display string. On error or NULL returns false and
   sets *out to the result the caller should return unchanged. */
static bool eval_str_arg(EvalCtx *ctx, ExprNode **args, int arg_count, int i,
                         const char **str, EvalResult *out) {
    if (i >= arg_count) { *out = eval_result_null(); return false; }
    EvalResult v = eval_expr(args[i], ctx);
    if (eval_result_is_error(&v) || v.is_null) { *out = v; return false; }
    *str = eval_result_to_string(&v, ctx->tmp);
    return true;
}

/* Like eval_str_arg but requires a numeric value. */
static bool eval_num_arg(EvalCtx *ctx, ExprNode **args, int arg_count, int i,
                         double *val, EvalResult *out) {
    if (i >= arg_count) { *out = eval_result_null(); return false; }
    EvalResult v = eval_expr(args[i], ctx);
    if (eval_result_is_error(&v) || v.is_null) { *out = v; return false; }
    if (!v.is_numeric) { *out = eval_result_null(); return false; }
    *val = v.num_val;
    return true;
}

/* ===== Built-in functions ===== */
bool is_aggregate_name(const char *name) {
    return str_ieq(name, "COUNT") ||
           str_ieq(name, "SUM") ||
           str_ieq(name, "AVG") ||
           str_ieq(name, "MIN") ||
           str_ieq(name, "MAX");
}

typedef EvalResult (*FuncImpl)(EvalCtx *ctx, ExprNode **args, int arg_count);

typedef struct {
    const char *name;
    FuncImpl impl;
} FuncDef;

static EvalResult fn_upper(EvalCtx *ctx, ExprNode **args, int arg_count) {
    const char *s;
    EvalResult out;
    if (!eval_str_arg(ctx, args, arg_count, 0, &s, &out)) return out;
    size_t len = strlen(s);
    char *res = arena_strdup(ctx->tmp, s);
    if (!res) return eval_result_null();
    for (size_t i = 0; i < len; i++) res[i] = (char)toupper((unsigned char)res[i]);
    return eval_result_str(res);
}

static EvalResult fn_lower(EvalCtx *ctx, ExprNode **args, int arg_count) {
    const char *s;
    EvalResult out;
    if (!eval_str_arg(ctx, args, arg_count, 0, &s, &out)) return out;
    size_t len = strlen(s);
    char *res = arena_strdup(ctx->tmp, s);
    if (!res) return eval_result_null();
    for (size_t i = 0; i < len; i++) res[i] = (char)tolower((unsigned char)res[i]);
    return eval_result_str(res);
}

static EvalResult fn_length(EvalCtx *ctx, ExprNode **args, int arg_count) {
    const char *s;
    EvalResult out;
    if (!eval_str_arg(ctx, args, arg_count, 0, &s, &out)) return out;
    return eval_result_num((double)strlen(s));
}

static EvalResult fn_trim(EvalCtx *ctx, ExprNode **args, int arg_count) {
    const char *s;
    EvalResult out;
    if (!eval_str_arg(ctx, args, arg_count, 0, &s, &out)) return out;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '\0') return eval_result_str("");
    size_t len = strlen(s);
    const char *end = s + len - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    size_t new_len = (size_t)(end - s + 1);
    char *res;
    ArenaResult ar = arena_alloc(ctx->tmp, new_len + 1, (void**)&res);
    if (ar != ARENA_OK) return eval_result_null();
    memcpy(res, s, new_len);
    res[new_len] = '\0';
    return eval_result_str(res);
}

static EvalResult fn_substr(EvalCtx *ctx, ExprNode **args, int arg_count) {
    if (arg_count < 2) return eval_result_null();
    const char *s;
    EvalResult out;
    if (!eval_str_arg(ctx, args, arg_count, 0, &s, &out)) return out;
    EvalResult pv = eval_expr(args[1], ctx);
    if (eval_result_is_error(&pv)) return pv;
    if (pv.is_null || !pv.is_numeric) return eval_result_null();
    int start = (int)pv.num_val;
    int length = -1;
    if (arg_count >= 3) {
        EvalResult lv = eval_expr(args[2], ctx);
        if (eval_result_is_error(&lv)) return lv;
        if (!lv.is_null && lv.is_numeric) length = (int)lv.num_val;
    }
    size_t slen = strlen(s);
    if (start < 1) start = 1;
    if ((size_t)start > slen) return eval_result_str("");
    size_t offset = (size_t)(start - 1);
    size_t remaining = slen - offset;
    if (length >= 0 && (size_t)length < remaining) remaining = (size_t)length;
    char *res;
    ArenaResult ar = arena_alloc(ctx->tmp, remaining + 1, (void**)&res);
    if (ar != ARENA_OK) return eval_result_null();
    memcpy(res, s + offset, remaining);
    res[remaining] = '\0';
    return eval_result_str(res);
}

static EvalResult fn_concat(EvalCtx *ctx, ExprNode **args, int arg_count) {
    if (arg_count < 1) return eval_result_null();
    /* Evaluate every argument exactly once, then concatenate. */
    EvalResult *vals;
    ArenaResult ar = arena_alloc(ctx->tmp, sizeof(EvalResult) * (size_t)arg_count,
                                 (void**)&vals);
    if (ar != ARENA_OK) return eval_result_null();
    size_t total = 0;
    for (int i = 0; i < arg_count; i++) {
        vals[i] = eval_expr(args[i], ctx);
        if (eval_result_is_error(&vals[i])) return vals[i];
        if (vals[i].is_null) continue;   /* NULL contributes nothing */
        total += strlen(eval_result_to_string(&vals[i], ctx->tmp));
    }
    char *res;
    ar = arena_alloc(ctx->tmp, total + 1, (void**)&res);
    if (ar != ARENA_OK) return eval_result_null();
    size_t pos = 0;
    for (int i = 0; i < arg_count; i++) {
        if (vals[i].is_null) continue;
        const char *s = eval_result_to_string(&vals[i], ctx->tmp);
        size_t len = strlen(s);
        memcpy(res + pos, s, len);
        pos += len;
    }
    res[pos] = '\0';
    return eval_result_str(res);
}

static EvalResult fn_coalesce(EvalCtx *ctx, ExprNode **args, int arg_count) {
    for (int i = 0; i < arg_count; i++) {
        EvalResult v = eval_expr(args[i], ctx);
        if (eval_result_is_error(&v)) return v;
        if (!v.is_null) return v;
    }
    return eval_result_null();
}

static EvalResult fn_abs(EvalCtx *ctx, ExprNode **args, int arg_count) {
    double v;
    EvalResult out;
    if (!eval_num_arg(ctx, args, arg_count, 0, &v, &out)) return out;
    return eval_result_num(fabs(v));
}

static EvalResult fn_round(EvalCtx *ctx, ExprNode **args, int arg_count) {
    double v;
    EvalResult out;
    if (!eval_num_arg(ctx, args, arg_count, 0, &v, &out)) return out;
    int decimals = 0;
    if (arg_count >= 2) {
        EvalResult d = eval_expr(args[1], ctx);
        if (eval_result_is_error(&d)) return d;
        if (!d.is_null && d.is_numeric) decimals = (int)d.num_val;
    }
    double mult = pow(10.0, decimals);
    return eval_result_num(round(v * mult) / mult);
}

static const FuncDef funcs[] = {
    { "UPPER", fn_upper }, { "UCASE", fn_upper },
    { "LOWER", fn_lower }, { "LCASE", fn_lower },
    { "LENGTH", fn_length },
    { "TRIM", fn_trim },
    { "SUBSTR", fn_substr }, { "SUBSTRING", fn_substr },
    { "CONCAT", fn_concat },
    { "COALESCE", fn_coalesce }, { "IFNULL", fn_coalesce },
    { "ABS", fn_abs },
    { "ROUND", fn_round },
};

static EvalResult eval_function(const char *name, ExprNode **args, int arg_count,
                                EvalCtx *ctx) {
    /* Aggregates are resolved by the caller. Reaching here means one was used
       in an invalid context (e.g. WHERE without an aggregate query). */
    if (is_aggregate_name(name)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Aggregate function '%s' is not supported in this context.", name);
        return eval_result_error(arena_strdup(ctx->arena, buf));
    }

    for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
        if (str_ieq(name, funcs[i].name)) {
            return funcs[i].impl(ctx, args, arg_count);
        }
    }

    /* Unknown function */
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Unknown function: %s", name);
        return eval_result_error(arena_strdup(ctx->arena, buf));
    }
}



typedef enum { BIT_AND, BIT_OR, BIT_XOR } BitwiseOp;

static EvalResult eval_bitwise(ExprNode *left, ExprNode *right, EvalCtx *ctx,
                               BitwiseOp op) {
    EvalResult l = eval_expr(left, ctx);
    if (eval_result_is_error(&l)) return l;
    EvalResult r = eval_expr(right, ctx);
    if (eval_result_is_error(&r)) return r;
    if (l.is_null || r.is_null) return eval_result_null();
    if (!l.is_numeric || !r.is_numeric) return eval_result_null();
    /* Casting an out-of-range double to long long is undefined behaviour. */
    if (fabs(l.num_val) >= LL_CAST_SAFE_BOUND || fabs(r.num_val) >= LL_CAST_SAFE_BOUND) return eval_result_null();
    long long a = (long long)l.num_val;
    long long b = (long long)r.num_val;
    long long result;
    switch (op) {
        case BIT_AND: result = a & b; break;
        case BIT_OR:  result = a | b; break;
        default:      result = a ^ b; break;
    }
    return eval_result_num((double)result);
}

typedef enum { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV, ARITH_MOD } ArithOp;

static EvalResult eval_arith(ExprNode *node, EvalCtx *ctx, ArithOp op) {
    EvalResult l = eval_expr(node->left, ctx);
    if (eval_result_is_error(&l)) return l;
    EvalResult r = eval_expr(node->right, ctx);
    if (eval_result_is_error(&r)) return r;
    if (l.is_null || r.is_null) return eval_result_null();
    if (!l.is_numeric || !r.is_numeric) return eval_result_null();
    switch (op) {
        case ARITH_ADD: return eval_result_num(l.num_val + r.num_val);
        case ARITH_SUB: return eval_result_num(l.num_val - r.num_val);
        case ARITH_MUL: return eval_result_num(l.num_val * r.num_val);
        case ARITH_DIV:
            if (r.num_val == 0.0) return eval_result_null();
            return eval_result_num(l.num_val / r.num_val);
        default: /* ARITH_MOD */
            if (r.num_val == 0.0) return eval_result_null();
            return eval_result_num(fmod(l.num_val, r.num_val));
    }
}

typedef enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE } CmpOp;

static EvalResult eval_cmp(ExprNode *node, EvalCtx *ctx, CmpOp op) {
    EvalResult l = eval_expr(node->left, ctx);
    if (eval_result_is_error(&l)) return l;
    EvalResult r = eval_expr(node->right, ctx);
    if (eval_result_is_error(&r)) return r;
    if (l.is_null || r.is_null) return eval_result_num(0.0);
    int cmp = eval_result_compare(&l, &r);
    bool res;
    switch (op) {
        case CMP_EQ: res = (cmp == 0); break;
        case CMP_NE: res = (cmp != 0); break;
        case CMP_LT: res = (cmp < 0); break;
        case CMP_GT: res = (cmp > 0); break;
        case CMP_LE: res = (cmp <= 0); break;
        default:     res = (cmp >= 0); break;
    }
    return eval_result_num((double)res);
}

typedef enum { LOGIC_AND, LOGIC_OR } LogicOp;

static EvalResult eval_logic(ExprNode *node, EvalCtx *ctx, LogicOp op) {
    EvalResult l = eval_expr(node->left, ctx);
    if (eval_result_is_error(&l)) return l;
    if (op == LOGIC_AND && !eval_result_is_true(&l)) return eval_result_num(0.0);
    if (op == LOGIC_OR && eval_result_is_true(&l)) return eval_result_num(1.0);
    EvalResult r = eval_expr(node->right, ctx);
    if (eval_result_is_error(&r)) return r;
    return eval_result_num((double)eval_result_is_true(&r));
}

static EvalResult eval_unary_num(ExprNode *node, EvalCtx *ctx, bool negate) {
    EvalResult v = eval_expr(node->left, ctx);
    if (eval_result_is_error(&v)) return v;
    if (v.is_null) return eval_result_null();
    if (v.is_numeric) return eval_result_num(negate ? -v.num_val : v.num_val);
    return eval_result_null();
}

static EvalResult eval_like(ExprNode *node, EvalCtx *ctx, bool case_insensitive) {
    EvalResult v = eval_expr(node->left, ctx);
    if (eval_result_is_error(&v)) return v;
    EvalResult p = eval_expr(node->right, ctx);
    if (eval_result_is_error(&p)) return p;
    if (v.is_null || p.is_null) return eval_result_num(0.0);
    const char *val_str = eval_result_to_string(&v, ctx->tmp);
    const char *pat_str = eval_result_to_string(&p, ctx->tmp);
    return eval_result_num((double)like_match(val_str, pat_str, case_insensitive));
}

static EvalResult eval_between(ExprNode *node, EvalCtx *ctx) {
    EvalResult v = eval_expr(node->left, ctx);
    if (eval_result_is_error(&v)) return v;
    if (v.is_null) return eval_result_num(0.0);
    EvalResult s = eval_expr(node->right, ctx);
    if (eval_result_is_error(&s)) return s;
    EvalResult e = eval_expr(node->mid, ctx);
    if (eval_result_is_error(&e)) return e;
    if (s.is_null || e.is_null) return eval_result_num(0.0);
    return eval_result_num((double)(eval_result_compare(&s, &v) <= 0 &&
                                    eval_result_compare(&v, &e) <= 0));
}

static EvalResult eval_in(ExprNode *node, EvalCtx *ctx, bool negate) {
    if (node->subquery) {
        return eval_result_error("Subqueries are not supported in expressions.");
    }
    EvalResult lhs = eval_expr(node->left, ctx);
    if (eval_result_is_error(&lhs)) return lhs;
    if (lhs.is_null) return eval_result_num(0.0);
    if (negate) {
        bool found = false;
        for (int i = 0; i < node->arg_count; i++) {
            EvalResult rhs = eval_expr(node->args[i], ctx);
            if (eval_result_is_error(&rhs)) return rhs;
            if (!rhs.is_null && eval_result_compare(&lhs, &rhs) == 0) {
                found = true;
                break;
            }
        }
        return eval_result_num((double)(!found));
    }
    for (int i = 0; i < node->arg_count; i++) {
        EvalResult rhs = eval_expr(node->args[i], ctx);
        if (eval_result_is_error(&rhs)) return rhs;
        if (!rhs.is_null && eval_result_compare(&lhs, &rhs) == 0)
            return eval_result_num(1.0);
    }
    return eval_result_num(0.0);
}

static EvalResult eval_case(ExprNode *node, EvalCtx *ctx) {
    for (CaseWhen *cw = node->case_whens; cw; cw = cw->next) {
        bool match;
        if (node->left) {
            /* Simple CASE: compare left (case operand) with condition */
            EvalResult case_val = eval_expr(node->left, ctx);
            if (eval_result_is_error(&case_val)) return case_val;
            EvalResult when_val = eval_expr(cw->condition, ctx);
            if (eval_result_is_error(&when_val)) return when_val;
            match = (!case_val.is_null && !when_val.is_null &&
                     eval_result_compare(&case_val, &when_val) == 0);
        } else {
            /* Searched CASE: evaluate condition as boolean */
            EvalResult cond = eval_expr(cw->condition, ctx);
            if (eval_result_is_error(&cond)) return cond;
            match = eval_result_is_true(&cond);
        }
        if (match) {
            return eval_expr(cw->result, ctx);
        }
    }
    if (node->case_else) {
        return eval_expr(node->case_else, ctx);
    }
    return eval_result_null();
}

static EvalResult eval_call(ExprNode *node, EvalCtx *ctx) {
    if (node->distinct && !is_aggregate_name(node->str_value)) {
        return eval_result_error("DISTINCT is only allowed with aggregate functions.");
    }
    /* During grouped finalization, resolve aggregate calls from the
       precomputed per-group state instead of evaluating them. */
    if (ctx->agg && is_aggregate_name(node->str_value)) {
        int idx = find_spec(ctx->agg->specs, ctx->agg->spec_count, node);
        if (idx >= 0) {
            return agg_state_value(&ctx->agg->states[idx], node->str_value);
        }
    }
    return eval_function(node->str_value, node->args, node->arg_count, ctx);
}

/* ===== Expression evaluation ===== */
EvalResult eval_expr(ExprNode *node, EvalCtx *ctx) {
    if (node == NULL) return eval_result_null();

    switch (node->type) {
        /* ===== Literals ===== */
        case EXPR_LITERAL_NUMBER:
            return eval_result_num(node->num_value);

        case EXPR_LITERAL_STRING:
            return eval_result_str(node->str_value ? node->str_value : "");

        case EXPR_LITERAL_NULL:
            return eval_result_null();

        case EXPR_STAR: {
            /* Bare asterisk when used as expression (e.g. COUNT(*)) */
            return eval_result_str("*");
        }

        case EXPR_COLUMN_REF: {
            if (node->str_value == NULL) return eval_result_null();
            int idx = node->col_index;
            if (idx < 0) {
                /* Defense-in-depth: column references are validated up front,
                   so this is unreachable for any parsed expression. */
                char buf[256];
                snprintf(buf, sizeof(buf), "Column '%s' not found in CSV headers.",
                         node->str_value);
                const char *msg = arena_strdup(ctx->arena, buf);
                return eval_result_error(msg ? msg : "Column not found in CSV headers.");
            }
            /* A row may have fewer fields than the header (ragged CSV): absent. */
            if ((size_t)idx >= ctx->record->field_count) return eval_result_null();
            const char *field = ctx->record->fields[idx];
            /* An empty CSV cell evaluates to NULL (absent value). By contrast the
               '' literal is a non-NULL empty string. */
            if (field == NULL || field[0] == '\0') return eval_result_null();

            /* Try to parse as number */
            char *endptr;
            double val = strtod(field, &endptr);
            if (*endptr == '\0' && endptr != field) {
                return eval_result_num(val);
            }
            return eval_result_str(field);
        }

        /* ===== Unary arithmetic ===== */
        case EXPR_UNARY_PLUS:
            return eval_unary_num(node, ctx, false);

        case EXPR_UNARY_MINUS:
            return eval_unary_num(node, ctx, true);

        /* ===== Binary arithmetic ===== */
        case EXPR_ADD:  return eval_arith(node, ctx, ARITH_ADD);
        case EXPR_SUB:  return eval_arith(node, ctx, ARITH_SUB);
        case EXPR_MUL:  return eval_arith(node, ctx, ARITH_MUL);
        case EXPR_DIV:  return eval_arith(node, ctx, ARITH_DIV);
        case EXPR_MOD:  return eval_arith(node, ctx, ARITH_MOD);

        /* ===== Bitwise ===== */
        case EXPR_BIT_AND: return eval_bitwise(node->left, node->right, ctx, BIT_AND);
        case EXPR_BIT_OR:  return eval_bitwise(node->left, node->right, ctx, BIT_OR);
        case EXPR_BIT_XOR: return eval_bitwise(node->left, node->right, ctx, BIT_XOR);

        /* ===== Comparison ===== */
        case EXPR_EQ:  return eval_cmp(node, ctx, CMP_EQ);
        case EXPR_NE:  return eval_cmp(node, ctx, CMP_NE);
        case EXPR_LT:  return eval_cmp(node, ctx, CMP_LT);
        case EXPR_GT:  return eval_cmp(node, ctx, CMP_GT);
        case EXPR_LE:  return eval_cmp(node, ctx, CMP_LE);
        case EXPR_GE:  return eval_cmp(node, ctx, CMP_GE);

        /* ===== Logical ===== */
        case EXPR_AND: return eval_logic(node, ctx, LOGIC_AND);
        case EXPR_OR:  return eval_logic(node, ctx, LOGIC_OR);

        case EXPR_NOT: {
            EvalResult v = eval_expr(node->left, ctx);
            if (eval_result_is_error(&v)) return v;
            return eval_result_num((double)(!eval_result_is_true(&v)));
        }

        /* ===== IN / NOT IN ===== */
        case EXPR_IN:     return eval_in(node, ctx, false);
        case EXPR_NOT_IN: return eval_in(node, ctx, true);

        /* ===== BETWEEN ===== */
        case EXPR_BETWEEN:
            return eval_between(node, ctx);

        /* ===== LIKE / ILIKE ===== */
        case EXPR_LIKE:  return eval_like(node, ctx, false);
        case EXPR_ILIKE: return eval_like(node, ctx, true);

        /* ===== Function call ===== */
        case EXPR_FUNCTION_CALL:
            return eval_call(node, ctx);

        /* ===== CASE ===== */
        case EXPR_CASE:
            return eval_case(node, ctx);

        /* ===== Subquery ===== */
        case EXPR_SUBQUERY:
            return eval_result_error("Subqueries are not supported in expressions.");
    }

    return eval_result_null();
}



/* Build the evaluation context for one row. */
EvalCtx eval_ctx_for(CSVRecord *record, char **headers, int header_count,
                            Arena *arena, Arena *tmp, const AggContext *agg) {
    EvalCtx ctx;
    ctx.record = record;
    ctx.headers = headers;
    ctx.header_count = header_count;
    ctx.arena = arena;
    ctx.tmp = tmp;
    ctx.agg = agg;
    return ctx;
}
