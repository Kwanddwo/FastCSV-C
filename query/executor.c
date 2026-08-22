#include "executor.h"
#include "str_util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

/* ===== Evaluation result ===== */
typedef struct {
    bool is_error;         /* evaluation failed; error holds the message */
    const char *error;
    bool is_null;
    bool is_numeric;
    double num_val;
    const char *str_val;   /* owned by arena, non-owning if from CSV field */
} EvalResult;

/* Aggregate substitution context used while finalizing grouped results. */
typedef struct AggSpec AggSpec;
typedef struct AggState AggState;
typedef struct {
    const AggSpec *specs;
    int spec_count;
    const AggState *states;   /* NULL unless resolving aggregate results */
} AggContext;

/* ===== Evaluation context ===== */
typedef struct {
    CSVRecord *record;
    char **headers;
    int header_count;
    Arena *arena;    /* result arena: persistent outputs are copied here */
    Arena *tmp;      /* temp arena: per-row transient evaluation scratch */
    const AggContext *agg;
} EvalCtx;

static int find_spec(const AggSpec *specs, int spec_count, ExprNode *node);
static EvalResult agg_state_value(const AggState *st, const char *name);
static EvalResult eval_expr(ExprNode *node, EvalCtx *ctx);
static uint64_t fnv1a(const void *data, size_t len);
static uint64_t fnv1a_str(const char *s);
static uint64_t hash_distinct_value(const EvalResult *v);

/* ===== Aggregate descriptors ===== */
/* Query-wide descriptor for one aggregate call node */
struct AggSpec {
    ExprNode *node;        /* the EXPR_FUNCTION_CALL node */
    const char *name;      /* COUNT / SUM / AVG / MIN / MAX */
    bool distinct;
};

/* Open-addressing set of distinct values backing COUNT/SUM/AVG(DISTINCT).
   Slot values index the parallel values/hashes arrays; -1 means empty. The
   hashes come from hash_distinct_value, which is consistent with
   eval_result_compare's numeric-coercion equality ('05', 5 and "5.0" all
   collide). Load is kept at <= 0.5, doubling and rehashing on growth. */
typedef struct {
    EvalResult *values;   /* stored distinct values (arena-owned strings) */
    uint64_t   *hashes;   /* cached hash per stored value */
    int        *slots;    /* -1 empty, else index into values/hashes */
    int         cap;      /* power of two */
    int         count;    /* distinct values stored */
} ValueSet;

/* Runtime accumulator state, one instance per (query, group) */
struct AggState {
    bool has_value;        /* saw at least one usable value */
    double sum;            /* SUM / AVG */
    long long count;       /* COUNT result / AVG denominator */
    EvalResult best;       /* MIN / MAX current best (str_val arena-owned) */

    /* DISTINCT support: open-addressing set of already-seen values. Lazily
       initialised on first use so plain aggregates stay allocation-free. */
    ValueSet dset;
};

/* ===== Constants ===== */
/* Doubles at or beyond this magnitude cannot be converted to long long
   without undefined behaviour (LLONG_MAX is ~9.22e18). */
static const double LL_CAST_SAFE_BOUND = 9.0e18;

/* Doubles below this magnitude are printed as integers. */
static const double INT_DISPLAY_BOUND = 1e15;

/* Initial capacities for growable arrays (doubled on demand). */
static const int GROW_INITIAL_CAPACITY = 64;   /* result records / sort keys */
static const int GROUP_INITIAL_CAPACITY = 16;  /* aggregation groups */
static const int COL_REFS_INITIAL_CAPACITY = 16; /* column-ref name lists */
static const int SPECS_INITIAL_CAPACITY = 8;   /* aggregate spec descriptors */
static const int SEEN_INITIAL_CAPACITY = 8;    /* DISTINCT value-set capacity */

/* FNV-1a hash constants (also used by the group-key and record hashes). */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

/* ===== EvalResult builders ===== */
static EvalResult eval_result_null(void) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = true;
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = NULL;
    return r;
}

static EvalResult eval_result_num(double val) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = false;
    r.is_numeric = true;
    r.num_val = val;
    r.str_val = NULL;
    return r;
}

static EvalResult eval_result_str(const char *s) {
    EvalResult r;
    r.is_error = false;
    r.error = NULL;
    r.is_null = (s == NULL);
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = s ? s : "";
    return r;
}

static EvalResult eval_result_error(const char *msg) {
    EvalResult r;
    r.is_error = true;
    r.error = msg;
    r.is_null = true;
    r.is_numeric = false;
    r.num_val = 0.0;
    r.str_val = NULL;
    return r;
}

static bool eval_result_is_error(const EvalResult *r) {
    return r->is_error;
}

static bool eval_result_is_true(const EvalResult *r) {
    if (r->is_error) return false;
    if (r->is_null) return false;
    if (r->is_numeric) return r->num_val != 0.0;
    return r->str_val != NULL && r->str_val[0] != '\0';
}

/* A string is numeric-like if it fully parses as a number, using the same
   rule as the column evaluator so literals classify identically to cells.
   Numeric values pass through unchanged. */
static bool parse_numeric_str(const EvalResult *r, double *out) {
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
static int eval_result_compare(const EvalResult *a, const EvalResult *b) {
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
static const char* eval_result_dup_to_arena(const EvalResult *r, Arena *arena) {
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
static const char* eval_result_to_string(const EvalResult *r, Arena *arena) {
    if (r->is_error) return r->error ? r->error : "";
    if (r->is_null) return "NULL";
    if (r->is_numeric) return eval_result_dup_to_arena(r, arena);
    return r->str_val ? r->str_val : "";
}

/* ===== LIKE pattern matching ===== */
static bool like_match(const char *s, const char *p, bool case_insensitive) {
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
static bool is_aggregate_name(const char *name) {
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

/* ===== Column lookup ===== */
static int find_column_index(const char *name, char **headers, int header_count) {
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

/* ===== Operator evaluation ===== */
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
static EvalResult eval_expr(ExprNode *node, EvalCtx *ctx) {
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

/* ===== Generic expression tree walker ===== */
/* Visits every node (except subqueries, which are validated separately).
   The visit function returns PRUNE to skip a node's children, or ABORT to
   stop the whole traversal. expr_walk returns false if ABORTed. */
typedef enum { EXPR_VISIT_CONTINUE, EXPR_VISIT_PRUNE, EXPR_VISIT_ABORT } ExprVisit;
typedef ExprVisit (*ExprVisitFn)(ExprNode *node, void *ud);

static bool expr_walk(ExprNode *node, ExprVisitFn visit, void *ud) {
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

static bool expr_contains_aggregate(ExprNode *node) {
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

static bool expr_is_constant(ExprNode *node) {
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

static bool validate_distinct_usage(ExprNode *node) {
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
static const char* validate_columns(ExprNode *expr, char **headers, int header_count,
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

static void collect_specs(ExprNode *node, AggSpec **specs, int *spec_count,
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

static void collect_column_refs(ExprNode *node, char ***names, int *count, int *cap,
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

static bool expr_grouped_valid(ExprNode *node, char **grouped, int grouped_count) {
    GroupedUD u = { grouped, grouped_count };
    return expr_walk(node, grouped_valid_visit, &u);
}

/* ===== Generate column ref ExprNode for star expansion ===== */
static ExprNode* make_column_ref_node(Arena *arena, const char *name) {
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

/* ===== ORDER BY helpers ===== */

/* Compare two entries' ORDER BY keys. Negative means a sorts before b.
   Mirrors the ORDER BY clause: per-key eval_result_compare, flipped for DESC.
   Used both by the qsort path and the top-k heap so their ordering semantics
   (NULLs, type-aware compare, multi-key) are identical. */
static int cmp_keys(const EvalResult *a, const EvalResult *b, int k,
                    const OrderByItem *order_by) {
    for (int j = 0; j < k; j++) {
        int cmp = eval_result_compare(&a[j], &b[j]);
        if (cmp != 0)
            return order_by[j].asc ? cmp : -cmp;
    }
    return 0;
}

/* Module-static context for the qsort comparator (qsort has no closure). */
static const EvalResult *sort_ctx_keys;
static int sort_ctx_k;
static const OrderByItem *sort_ctx_order_by;

static int compare_indices(const void *pa, const void *pb) {
    int a = *(const int*)pa;
    int b = *(const int*)pb;
    return cmp_keys(&sort_ctx_keys[a * sort_ctx_k], &sort_ctx_keys[b * sort_ctx_k],
                    sort_ctx_k, sort_ctx_order_by);
}

static void sort_indices(int *order, int n,
                          const EvalResult *keys, int k,
                          const OrderByItem *ob) {
    if (n <= 1) return;
    sort_ctx_keys = keys;
    sort_ctx_k = k;
    sort_ctx_order_by = ob;
    qsort(order, (size_t)n, sizeof(int), compare_indices);
}

/* ===== Top-K heap (ORDER BY + LIMIT) ===== */

/* Keeps the best `window` rows in final ORDER BY order using a positional
   binary max-heap: heap[i] is an entry index, and the root (heap[0]) is the
   worst kept entry, i.e. the first candidate for eviction. Entries hold the
   projected output record plus its persisted ORDER BY keys. */
typedef struct {
    CSVRecord **recs;   /* entry records, size cap */
    EvalResult *keys;   /* entry keys, size cap * key_count */
    int *heap;          /* positional heap over entry indices */
    int cap;            /* == the window size */
    int count;          /* entries stored so far */
    int key_count;
    const OrderByItem *order_by;
} TopK;

static const char* topk_init(Arena *arena, TopK *tk, int cap, int key_count,
                             const OrderByItem *order_by) {
    tk->cap = cap;
    tk->count = 0;
    tk->key_count = key_count;
    tk->order_by = order_by;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(CSVRecord*) * (size_t)cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    tk->recs = (CSVRecord**)mem;
    ar = arena_alloc(arena, sizeof(EvalResult) * (size_t)cap * (size_t)key_count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    tk->keys = (EvalResult*)mem;
    ar = arena_alloc(arena, sizeof(int) * (size_t)cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    tk->heap = (int*)mem;
    return NULL;
}

/* Would a row with these keys enter the top-k? */
static bool topk_would_keep(const TopK *tk, const EvalResult *keys) {
    if (tk->count < tk->cap) return true;
    int root = tk->heap[0];
    return cmp_keys(keys, &tk->keys[root * tk->key_count], tk->key_count,
                    tk->order_by) < 0;
}

/* Copy string key components into the arena so entries survive row-scoped
   temp arena resets. */
static void persist_keys(Arena *arena, EvalResult *keys, int key_count) {
    for (int j = 0; j < key_count; j++) {
        if (!keys[j].is_numeric && keys[j].str_val)
            keys[j].str_val = arena_strdup(arena, keys[j].str_val);
    }
}

/* Sift entry at position `i` toward the root. The root holds the worst entry,
   so a parent may never compare "better" than a child. */
static void topk_sift_up(TopK *tk, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        const EvalResult *pk = &tk->keys[tk->heap[parent] * tk->key_count];
        const EvalResult *ik = &tk->keys[tk->heap[i] * tk->key_count];
        if (cmp_keys(pk, ik, tk->key_count, tk->order_by) >= 0) break;
        int tmp = tk->heap[parent];
        tk->heap[parent] = tk->heap[i];
        tk->heap[i] = tmp;
        i = parent;
    }
}

/* Sift the root down to restore the heap invariant. */
static void topk_sift_down(TopK *tk) {
    int i = 0;
    for (;;) {
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left >= tk->count) break;
        int worst = left;
        if (right < tk->count) {
            const EvalResult *lk = &tk->keys[tk->heap[left] * tk->key_count];
            const EvalResult *rk = &tk->keys[tk->heap[right] * tk->key_count];
            if (cmp_keys(lk, rk, tk->key_count, tk->order_by) < 0) worst = right;
        }
        const EvalResult *ik = &tk->keys[tk->heap[i] * tk->key_count];
        const EvalResult *wk = &tk->keys[tk->heap[worst] * tk->key_count];
        if (cmp_keys(ik, wk, tk->key_count, tk->order_by) >= 0) break;
        int tmp = tk->heap[i];
        tk->heap[i] = tk->heap[worst];
        tk->heap[worst] = tmp;
        i = worst;
    }
}

/* Store a kept entry: copy rec + keys into the next free slot (or over the
   evicted root), persisting string keys, and restore the heap invariant. */
static void topk_insert(Arena *arena, TopK *tk, CSVRecord *rec,
                        const EvalResult *keys) {
    int idx;
    if (tk->count < tk->cap) {
        idx = tk->count++;
        tk->heap[idx] = idx;
        memcpy(&tk->keys[idx * tk->key_count], keys,
               sizeof(EvalResult) * (size_t)tk->key_count);
        persist_keys(arena, &tk->keys[idx * tk->key_count], tk->key_count);
        topk_sift_up(tk, idx);
    } else {
        idx = tk->heap[0];   /* worst kept entry, being evicted */
        memcpy(&tk->keys[idx * tk->key_count], keys,
               sizeof(EvalResult) * (size_t)tk->key_count);
        persist_keys(arena, &tk->keys[idx * tk->key_count], tk->key_count);
        topk_sift_down(tk);
    }
    tk->recs[idx] = rec;
}

/* Emit the stored entries in ascending final ORDER BY order into a fresh
   arena-allocated array. */
static const char* topk_emit(Arena *arena, TopK *tk, CSVRecord ***out,
                             int *out_count) {
    int n = tk->count;
    *out_count = n;
    if (n == 0) { *out = NULL; return NULL; }
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(CSVRecord*) * (size_t)n, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    CSVRecord **result = (CSVRecord**)mem;
    /* Each root-pop yields the worst of the remaining entries, so it belongs
       at the back of the ascending output. */
    for (int i = 0; i < n; i++) {
        int root = tk->heap[0];
        result[n - 1 - i] = tk->recs[root];
        tk->count--;
        if (tk->count > 0) {
            tk->heap[0] = tk->heap[tk->count];
            topk_sift_down(tk);
        }
    }
    *out = result;
    return NULL;
}

/* ===== Aggregate accumulation ===== */

/* Hash of a value, consistent with eval_result_compare's equality: NULL gets
   a constant bucket; anything parseable as a number hashes as its numeric
   value (so '05', 5 and "5.0" collide); anything else as its text. A NaN
   value falls into its own constant bucket, a deliberate divergence from
   eval_result_compare, which treats NaN-numeric as equal to any numeric. */
static uint64_t hash_distinct_value(const EvalResult *v) {
    if (v->is_null) return FNV_OFFSET ^ 0x9e3779b97f4a7c15ULL;
    double num;
    if (parse_numeric_str(v, &num)) {
        if (num != num) num = 0.0;   /* NaN: constant bucket (never equal) */
        num = num + 0.0;             /* canonicalize -0.0 to +0.0 */
        uint64_t bits;
        memcpy(&bits, &num, sizeof(bits));
        return fnv1a(&bits, sizeof(bits));
    }
    return fnv1a_str(v->str_val ? v->str_val : "");
}

/* Lazily allocate the distinct value set on first use. */
static const char* value_set_ensure(Arena *arena, ValueSet *s) {
    if (s->slots != NULL) return NULL;
    s->cap = SEEN_INITIAL_CAPACITY;
    s->count = 0;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)s->cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    s->slots = (int*)mem;
    for (int i = 0; i < s->cap; i++) s->slots[i] = -1;
    ar = arena_alloc(arena, sizeof(uint64_t) * (size_t)s->cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    s->hashes = (uint64_t*)mem;
    ar = arena_alloc(arena, sizeof(EvalResult) * (size_t)s->cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    s->values = (EvalResult*)mem;
    return NULL;
}

/* Double the set capacity and rehash every stored value. */
static const char* value_set_grow(Arena *arena, ValueSet *s) {
    int new_cap = s->cap * 2;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    int *slots = (int*)mem;
    for (int i = 0; i < new_cap; i++) slots[i] = -1;
    for (int i = 0; i < s->count; i++) {
        size_t slot = (size_t)(s->hashes[i] & (uint64_t)(new_cap - 1));
        while (slots[slot] != -1) slot = (slot + 1) & (uint64_t)(new_cap - 1);
        slots[slot] = i;
    }
    uint64_t *new_hashes = (uint64_t*)arena_realloc(
        arena, s->hashes, sizeof(uint64_t) * (size_t)s->cap,
        sizeof(uint64_t) * (size_t)new_cap);
    if (new_hashes == NULL) return "Out of memory.";
    EvalResult *new_values = (EvalResult*)arena_realloc(
        arena, s->values, sizeof(EvalResult) * (size_t)s->cap,
        sizeof(EvalResult) * (size_t)new_cap);
    if (new_values == NULL) return "Out of memory.";
    s->slots = slots;
    s->hashes = new_hashes;
    s->values = new_values;
    s->cap = new_cap;
    return NULL;
}

/* Register v as a distinct value in st's set. Sets *is_new = true when v was
   not present before (the caller counts it), false for a duplicate. Returns
   NULL on success, or an error string on allocation failure. */
static const char* agg_seen_add(AggState *st, const EvalResult *v, Arena *arena,
                                bool *is_new) {
    const char *err = value_set_ensure(arena, &st->dset);
    if (err) return err;
    ValueSet *s = &st->dset;
    uint64_t h = hash_distinct_value(v);
    size_t slot = (size_t)(h & (uint64_t)(s->cap - 1));
    while (s->slots[slot] != -1) {
        int j = s->slots[slot];
        if (s->hashes[j] == h && eval_result_compare(&s->values[j], v) == 0) {
            *is_new = false;
            return NULL;
        }
        slot = (slot + 1) & (uint64_t)(s->cap - 1);
    }
    if ((s->count + 1) * 2 > s->cap) {
        err = value_set_grow(arena, s);
        if (err) return err;
        slot = (size_t)(h & (uint64_t)(s->cap - 1));
        while (s->slots[slot] != -1) slot = (slot + 1) & (uint64_t)(s->cap - 1);
    }
    EvalResult copy = *v;
    if (!copy.is_numeric && copy.str_val) {
        const char *dup = arena_strdup(arena, copy.str_val);
        if (dup == NULL) return "Out of memory.";
        copy.str_val = dup;
    }
    s->values[s->count] = copy;
    s->hashes[s->count] = h;
    s->slots[slot] = s->count;
    s->count++;
    *is_new = true;
    return NULL;
}

/* Install v as the MIN/MAX best value (string copies are arena-owned). */
static void agg_best_set(AggState *st, const EvalResult *v, Arena *arena) {
    if (v->is_numeric) {
        st->best = *v;
    } else {
        st->best.is_null = false;
        st->best.is_numeric = false;
        st->best.num_val = 0.0;
        st->best.str_val = arena_strdup(arena, v->str_val ? v->str_val : "");
    }
}

/* Accumulate one record into the state. Returns an error string on failure. */
static const char* aggregate_row(ExprNode *arg, const char *name, bool distinct,
                                 AggState *st, EvalCtx *ctx) {
    if (str_ieq(name, "COUNT")) {
        if (arg == NULL || arg->type == EXPR_STAR) {
            st->count++;
            st->has_value = true;
            return NULL;
        }
        EvalResult v = eval_expr(arg, ctx);
        if (eval_result_is_error(&v)) return v.error;
        if (v.is_null) return NULL;
        if (distinct) {
            bool is_new = false;
            const char *aerr = agg_seen_add(st, &v, ctx->arena, &is_new);
            if (aerr) return aerr;
            if (!is_new) return NULL;
        }
        st->count++;
        st->has_value = true;
        return NULL;
    }

    if (arg == NULL) return NULL;
    EvalResult v = eval_expr(arg, ctx);
    if (eval_result_is_error(&v)) return v.error;
    if (v.is_null) return NULL;

    if (str_ieq(name, "SUM") || str_ieq(name, "AVG")) {
        if (!v.is_numeric) return NULL;
        if (distinct) {
            bool is_new = false;
            const char *aerr = agg_seen_add(st, &v, ctx->arena, &is_new);
            if (aerr) return aerr;
            if (!is_new) return NULL;
        }
        st->sum += v.num_val;
        st->count++;
        st->has_value = true;
        return NULL;
    }

    /* MIN / MAX */
    if (!st->has_value) {
        agg_best_set(st, &v, ctx->arena);
        st->has_value = true;
        return NULL;
    }
    int cmp = eval_result_compare(&v, &st->best);
    if ((str_ieq(name, "MIN") && cmp < 0) ||
        (str_ieq(name, "MAX") && cmp > 0)) {
        agg_best_set(st, &v, ctx->arena);
    }
    return NULL;
}

/* Compute the finalized value of an accumulator. */
static EvalResult agg_state_value(const AggState *st, const char *name) {
    if (str_ieq(name, "COUNT")) return eval_result_num((double)st->count);
    if (!st->has_value) return eval_result_null();
    if (str_ieq(name, "SUM")) return eval_result_num(st->sum);
    if (str_ieq(name, "AVG")) return eval_result_num(st->sum / (double)st->count);
    return st->best;   /* MIN / MAX; str_val is arena-owned */
}

/* ===== Grouping ===== */

/* Per-group aggregation state */
typedef struct {
    EvalResult *keys;     /* group_by_count key components (arena-owned strings) */
    AggState *states;     /* spec_count accumulator states */
    CSVRecord rep;        /* arena-owned copy of a representative record */
} AggGroup;

static void agg_state_init(AggState *s) {
    s->has_value = false;
    s->sum = 0.0;
    s->count = 0;
    s->best = eval_result_null();
    memset(&s->dset, 0, sizeof(s->dset));
}

/* Type-aware key comparison, consistent with hash_eval_result. NULL keys
   match only NULL; numeric matches numeric; string matches string; mixed
   types never match. This is deliberately stricter than eval_result_compare,
   which coerces a parseable string against a numeric value: GROUP BY keys are
   classified per raw cell (deterministic), so keys can never mix classes, and
   keeping classification-strict identity makes the hash sound. */
static bool group_key_equals(const EvalResult *a, const EvalResult *b) {
    if (a->is_null || b->is_null) return a->is_null && b->is_null;
    if (a->is_numeric && b->is_numeric) return a->num_val == b->num_val;
    if (a->is_numeric || b->is_numeric) return false;
    const char *as = a->str_val ? a->str_val : "";
    const char *bs = b->str_val ? b->str_val : "";
    return strcmp(as, bs) == 0;
}

static bool group_keys_equal(const EvalResult *a, const EvalResult *b, int k) {
    for (int i = 0; i < k; i++) {
        if (!group_key_equals(&a[i], &b[i])) return false;
    }
    return true;
}

/* ===== Hash helpers ===== */

static uint64_t fnv1a(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char*)data;
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t fnv1a_str(const char *s) {
    return fnv1a(s, strlen(s));
}

static uint64_t hash_eval_result(const EvalResult *er) {
    if (er->is_null) return FNV_OFFSET ^ 0x9e3779b97f4a7c15ULL;
    if (er->is_numeric) {
        double v = er->num_val;
        if (v != v) v = 0.0;      /* NaN: constant bucket (never equal) */
        v = v + 0.0;              /* canonicalize -0.0 to +0.0 */
        uint64_t bits;
        memcpy(&bits, &v, sizeof(bits));
        return fnv1a(&bits, sizeof(bits));
    }
    return fnv1a_str(er->str_val ? er->str_val : "");
}

static uint64_t hash_group_keys(const EvalResult *keys, int k) {
    uint64_t h = FNV_OFFSET;
    for (int i = 0; i < k; i++) {
        h ^= hash_eval_result(&keys[i]);
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t hash_record(const CSVRecord *r) {
    uint64_t h = fnv1a(&r->field_count, sizeof(r->field_count));
    for (size_t q = 0; q < r->field_count; q++) {
        h ^= fnv1a_str(r->fields[q] ? r->fields[q] : "");
        h *= FNV_PRIME;
    }
    return h;
}

static bool records_equal(const CSVRecord *a, const CSVRecord *b) {
    if (a->field_count != b->field_count) return false;
    for (size_t q = 0; q < a->field_count; q++) {
        const char *as = a->fields[q] ? a->fields[q] : "";
        const char *bs = b->fields[q] ? b->fields[q] : "";
        if (strcmp(as, bs) != 0) return false;
    }
    return true;
}

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* ===== Group hash table ===== */

/* Open-addressing table mapping a group key hash to an index into the
   parallel `groups` array. Slot value -1 means empty. */
typedef struct {
    int *slots;
    int cap;     /* power of two */
    int count;   /* occupied slots */
} GroupTable;

static const char* group_table_init(Arena *arena, GroupTable *t, int cap) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    t->slots = (int*)mem;
    t->cap = cap;
    t->count = 0;
    for (int i = 0; i < cap; i++) t->slots[i] = -1;
    return NULL;
}

/* Rehash every group into a table twice as large. */
static const char* group_table_grow(Arena *arena, GroupTable *t, AggGroup **groups,
                                    int group_count, int k) {
    int new_cap = t->cap * 2;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    int *slots = (int*)mem;
    for (int i = 0; i < new_cap; i++) slots[i] = -1;
    for (int gi = 0; gi < group_count; gi++) {
        uint64_t h = hash_group_keys(groups[gi]->keys, k) & (uint64_t)(new_cap - 1);
        while (slots[h] != -1) h = (h + 1) & (uint64_t)(new_cap - 1);
        slots[h] = gi;
    }
    t->slots = slots;
    t->cap = new_cap;
    return NULL;
}

/* Find the group index matching `keys`, or -1. */
static int group_table_find(const GroupTable *t, AggGroup **groups,
                            const EvalResult *keys, int k) {
    uint64_t h = hash_group_keys(keys, k) & (uint64_t)(t->cap - 1);
    for (int i = 0; i < t->cap; i++) {
        int gi = t->slots[h];
        if (gi == -1) return -1;
        if (group_keys_equal(groups[gi]->keys, keys, k)) return gi;
        h = (h + 1) & (uint64_t)(t->cap - 1);
    }
    return -1;
}

/* Insert group `gi` (keys already stored in groups[gi]) keeping load <= 0.5. */
static const char* group_table_insert(Arena *arena, GroupTable *t, AggGroup **groups,
                                      int gi, int group_count, int k) {
    if ((t->count + 1) * 2 > t->cap) {
        const char *err = group_table_grow(arena, t, groups, group_count, k);
        if (err) return err;
    }
    uint64_t h = hash_group_keys(groups[gi]->keys, k) & (uint64_t)(t->cap - 1);
    while (t->slots[h] != -1) h = (h + 1) & (uint64_t)(t->cap - 1);
    t->slots[h] = gi;
    t->count++;
    return NULL;
}

/* Create a group with k key slots and spec_count accumulator states. */
static AggGroup* agg_group_create(int k, int spec_count, Arena *arena) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(AggGroup), &mem);
    if (ar != ARENA_OK) return NULL;
    AggGroup *g = (AggGroup*)mem;
    g->keys = NULL;
    if (k > 0) {
        ar = arena_alloc(arena, sizeof(EvalResult) * (size_t)k, &mem);
        if (ar != ARENA_OK) return NULL;
        g->keys = (EvalResult*)mem;
    }
    g->states = NULL;
    if (spec_count > 0) {
        ar = arena_alloc(arena, sizeof(AggState) * (size_t)spec_count, &mem);
        if (ar != ARENA_OK) return NULL;
        g->states = (AggState*)mem;
        for (int i = 0; i < spec_count; i++) {
            agg_state_init(&g->states[i]);
        }
    }
    g->rep.fields = NULL;
    g->rep.field_count = 0;
    return g;
}

/* Deep-copy a record's fields into the query arena. */
static CSVRecord copy_record(CSVRecord *src, Arena *arena) {
    CSVRecord out;
    out.field_count = src ? src->field_count : 0;
    out.fields = NULL;
    if (src == NULL || out.field_count == 0) return out;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(char*) * out.field_count, &mem);
    if (ar != ARENA_OK) return out;
    out.fields = (char**)mem;
    for (size_t i = 0; i < out.field_count; i++) {
        out.fields[i] = arena_strdup(arena, src->fields[i] ? src->fields[i] : "");
    }
    return out;
}

/* Find the spec index for a given aggregate call node, or -1. */
static int find_spec(const AggSpec *specs, int spec_count, ExprNode *node) {
    for (int i = 0; i < spec_count; i++) {
        if (specs[i].node == node) return i;
    }
    return -1;
}

/* ===== Output column descriptor (star-expanded) ===== */
typedef struct {
    ExprNode *expr;
    const char *name;
} OutputCol;

/* ===== Growable array helpers ===== */

/* Grow a dynamically sized array so it holds at least `needed` elements. */
static const char* grow_array(Arena *arena, void **arr, int *cap, int needed,
                              size_t elem_size) {
    if (needed <= *cap) return NULL;
    int new_cap = *cap ? *cap * 2 : GROW_INITIAL_CAPACITY;
    while (new_cap < needed) new_cap *= 2;
    void *mem = arena_realloc(arena, *arr, elem_size * (size_t)*cap,
                              elem_size * (size_t)new_cap);
    if (mem == NULL) return "Out of memory.";
    *arr = mem;
    *cap = new_cap;
    return NULL;
}

/* Allocate from the arena, setting *error on failure. */
static void* alloc_or_error(Arena *arena, size_t size, const char **error) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, size, &mem);
    if (ar != ARENA_OK) {
        *error = "Out of memory.";
        return NULL;
    }
    return mem;
}

/* ===== Row processing helpers ===== */

static EvalCtx eval_ctx_for(CSVRecord *record, char **headers, int header_count,
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

/* Evaluate the ORDER BY keys for row `idx` and append them. */
static const char* eval_sort_keys(EvalCtx *ctx, const OrderByItem *order_by, int k,
                                  EvalResult **sort_keys, int *cap, int idx) {
    int needed = (idx + 1) * k;
    const char *err = grow_array(ctx->arena, (void**)sort_keys, cap, needed,
                                 sizeof(EvalResult));
    if (err) return err;
    for (int j = 0; j < k; j++) {
        EvalResult er = eval_expr(order_by[j].expr, ctx);
        if (eval_result_is_error(&er)) return er.error;
        if (!er.is_numeric && er.str_val)
            er.str_val = arena_strdup(ctx->arena, er.str_val);
        (*sort_keys)[idx * k + j] = er;
    }
    return NULL;
}

/* Project one input record into an output record of out_count fields. */
static const char* project_row(const OutputCol *out_cols, int out_count,
                               EvalCtx *ctx, CSVRecord **out) {
    void *mem;
    ArenaResult ar = arena_alloc(ctx->arena, sizeof(CSVRecord), &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    CSVRecord *proj = (CSVRecord*)mem;

    ar = arena_alloc(ctx->arena, sizeof(char*) * (size_t)out_count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    proj->fields = (char**)mem;
    proj->field_count = (size_t)out_count;

    for (int i = 0; i < out_count; i++) {
        EvalResult er = eval_expr(out_cols[i].expr, ctx);
        if (eval_result_is_error(&er)) return er.error;
        char *s = (char*)eval_result_dup_to_arena(&er, ctx->arena);
        if (s == NULL) return "Out of memory.";
        proj->fields[i] = s;
    }
    *out = proj;
    return NULL;
}

/* Append a projected record to the result, growing as needed. */
static const char* append_result(CSVRecord ***records, int *record_count, int *capacity,
                                 CSVRecord *proj, Arena *arena) {
    const char *err = grow_array(arena, (void**)records, capacity, *record_count + 1,
                                 sizeof(CSVRecord*));
    if (err) return err;
    (*records)[(*record_count)++] = proj;
    return NULL;
}

/* ===== Post-processing ===== */

/* Remove duplicate rows in place, keeping first occurrences.
   Expected O(n): each record is hashed once into a precomputed array and
   resolved through an open-addressing table sized for load <= 0.5. */
static const char* dedupe_records(CSVRecord ***records, int *record_count, int k,
                                  EvalResult *sort_keys, Arena *arena) {
    int n = *record_count;
    if (n <= 1) return NULL;

    size_t cap = next_pow2((size_t)n * 2);
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(uint64_t) * (size_t)n, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    uint64_t *hashes = (uint64_t*)mem;
    ar = arena_alloc(arena, sizeof(int) * cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    int *table = (int*)mem;
    for (size_t i = 0; i < cap; i++) table[i] = -1;

    CSVRecord **src = *records;
    for (int i = 0; i < n; i++) {
        hashes[i] = hash_record(src[i]);
    }

    int write_idx = 0;
    for (int i = 0; i < n; i++) {
        uint64_t h = hashes[i];
        size_t slot = (size_t)(h & (uint64_t)(cap - 1));
        bool duplicate = false;
        while (table[slot] != -1) {
            int j = table[slot];
            if (hashes[j] == h && records_equal(src[j], src[i])) {
                duplicate = true;
                break;
            }
            slot = (slot + 1) & (cap - 1);
        }
        if (duplicate) continue;
        src[write_idx] = src[i];
        hashes[write_idx] = h;
        if (sort_keys && k > 0)
            memmove(&sort_keys[write_idx * k], &sort_keys[i * k],
                    (size_t)k * sizeof(EvalResult));
        table[slot] = write_idx;
        write_idx++;
    }
    *record_count = write_idx;
    return NULL;
}

/* ===== Record dedupe set (incremental) ===== */

/* Open-addressing set of distinct records used to dedupe incrementally while
   scanning. Slot values are record indices into the caller's parallel records
   array; hashes holds each distinct record's hash. Kept at load <= 0.5,
   doubling and rehashing on growth. */
typedef struct {
    uint64_t *hashes;
    int *slots;
    int cap;     /* power of two */
    int count;   /* distinct records stored */
} RecordSet;

static const char* record_set_init(Arena *arena, RecordSet *set) {
    set->cap = 32;
    set->count = 0;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)set->cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    set->slots = (int*)mem;
    for (int i = 0; i < set->cap; i++) set->slots[i] = -1;
    ar = arena_alloc(arena, sizeof(uint64_t) * (size_t)set->cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    set->hashes = (uint64_t*)mem;
    return NULL;
}

static const char* record_set_grow(Arena *arena, RecordSet *set) {
    int new_cap = set->cap * 2;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    int *slots = (int*)mem;
    for (int i = 0; i < new_cap; i++) slots[i] = -1;
    for (int i = 0; i < set->count; i++) {
        uint64_t h = set->hashes[i];
        size_t slot = (size_t)(h & (uint64_t)(new_cap - 1));
        while (slots[slot] != -1) slot = (slot + 1) & (uint64_t)(new_cap - 1);
        slots[slot] = i;
    }
    uint64_t *new_hashes = (uint64_t*)arena_realloc(
        arena, set->hashes, sizeof(uint64_t) * (size_t)set->cap,
        sizeof(uint64_t) * (size_t)new_cap);
    if (new_hashes == NULL) return "Out of memory.";
    set->hashes = new_hashes;
    set->slots = slots;
    set->cap = new_cap;
    return NULL;
}

/* Add rec to the set if it is not a duplicate of an already-stored record.
   Returns true when stored (at records[count]); false when it was a duplicate
   (with *err unchanged). On allocation failure returns false with *err set. */
static bool record_set_add(RecordSet *set, Arena *arena, CSVRecord **records,
                           CSVRecord *rec, const char **err) {
    *err = NULL;
    uint64_t h = hash_record(rec);
    size_t slot = (size_t)(h & (uint64_t)(set->cap - 1));
    while (set->slots[slot] != -1) {
        int j = set->slots[slot];
        if (set->hashes[j] == h && records_equal(records[j], rec))
            return false;
        slot = (slot + 1) & (uint64_t)(set->cap - 1);
    }
    if ((set->count + 1) * 2 > set->cap) {
        const char *e = record_set_grow(arena, set);
        if (e) { *err = e; return false; }
        slot = (size_t)(h & (uint64_t)(set->cap - 1));
        while (set->slots[slot] != -1) slot = (slot + 1) & (uint64_t)(set->cap - 1);
    }
    records[set->count] = rec;
    set->hashes[set->count] = h;
    set->slots[slot] = set->count;
    set->count++;
    return true;
}

/* Sort result rows by the pre-computed ORDER BY keys. */
static const char* order_records(CSVRecord ***records, int record_count, int k,
                                 const EvalResult *sort_keys, const OrderByItem *order_by,
                                 Arena *arena) {
    if (k <= 0 || record_count <= 1) return NULL;
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(int) * (size_t)record_count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    int *order = (int*)mem;
    for (int i = 0; i < record_count; i++) order[i] = i;

    sort_indices(order, record_count, sort_keys, k, order_by);

    ar = arena_alloc(arena, sizeof(CSVRecord*) * (size_t)record_count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    CSVRecord **gathered = (CSVRecord**)mem;
    for (int i = 0; i < record_count; i++)
        gathered[i] = (*records)[order[i]];
    *records = gathered;
    return NULL;
}

/* Apply LIMIT / OFFSET in place. */
static void apply_limit_offset(SelectStmt *stmt, CSVRecord **records, int *record_count) {
    if (!stmt->has_limit && !stmt->has_offset) return;
    long long start = stmt->has_offset ? stmt->offset : 0;
    long long cnt = stmt->has_limit ? stmt->limit : *record_count;
    if (start < 0) start = 0;
    if (start > *record_count) start = *record_count;
    if (start + cnt > *record_count) cnt = *record_count - start;
    if (cnt < 0) cnt = 0;
    /* Both bounds are clamped to record_count, so the int casts are safe. */
    int istart = (int)start;
    int icnt = (int)cnt;
    for (int i = 0; i < icnt; i++)
        records[i] = records[istart + i];
    *record_count = icnt;
}

/* ===== Statement validation ===== */

/* Does the path already carry a CSV extension? Used to decide whether the
   "<name>.csv" fallback may be tried: a name like "data.tsv" or "STUDENTS.CSV"
   is treated as an explicit file and never gets ".csv" appended. */
static bool has_csv_extension(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;   /* ".csv" plus at least one char */
    if (name[len - 4] != '.') return false;
    return (name[len - 3] == 'c' || name[len - 3] == 'C') &&
           (name[len - 2] == 's' || name[len - 2] == 'S') &&
           (name[len - 1] == 'v' || name[len - 1] == 'V');
}

static const char* open_reader(CSVConfig *config, SelectStmt *stmt, Arena *arena,
                               CSVReader **out_reader, char ***out_headers,
                               int *out_header_count) {
    if (stmt->table_name == NULL || stmt->table_name[0] == '\0') {
        return "No table specified in FROM clause.";
    }

    CSVConfig *cfg_copy = csv_config_copy(arena, config);
    if (cfg_copy == NULL) return "Out of memory.";

    /* Exact path first; on a miss, retry with ".csv" appended when the name
       has no extension (FROM "students" -> students.csv). This keeps every
       existing explicit path (data.tsv, my data.csv) resolving identically,
       and a real no-extension file still wins over its ".csv" sibling. */
    csv_config_set_path(cfg_copy, stmt->table_name);
    CSVReader *reader = csv_reader_init_standalone(cfg_copy);
    if (reader == NULL && !has_csv_extension(stmt->table_name)) {
        size_t len = strlen(stmt->table_name);
        void *mem;
        ArenaResult ar = arena_alloc(arena, len + 5, &mem);
        if (ar != ARENA_OK) return "Out of memory.";
        char *candidate = (char*)mem;
        memcpy(candidate, stmt->table_name, len);
        memcpy(candidate + len, ".csv", 5);
        csv_config_set_path(cfg_copy, candidate);
        reader = csv_reader_init_standalone(cfg_copy);
    }

    if (reader == NULL) {
        char buf[256];
        if (has_csv_extension(stmt->table_name)) {
            snprintf(buf, sizeof(buf), "Failed to open '%s'.", stmt->table_name);
        } else {
            snprintf(buf, sizeof(buf), "Failed to open '%s' (also tried '%s.csv').",
                     stmt->table_name, stmt->table_name);
        }
        char *msg = arena_strdup(arena, buf);
        return msg ? msg : "Failed to open file.";
    }

    int header_count = 0;
    char **headers = csv_reader_get_headers(reader, &header_count);
    if (headers == NULL || header_count == 0) {
        *out_reader = reader;
        return "CSV file has no headers.";
    }

    *out_reader = reader;
    *out_headers = headers;
    *out_header_count = header_count;
    return NULL;
}

/* Validate column references, DISTINCT usage, and WHERE aggregates. */
static const char* validate_stmt(SelectStmt *stmt, char **headers, int header_count,
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

/* Build the output column descriptors, expanding '*' to one per header. */
static const char* build_output_cols(SelectStmt *stmt, char **headers, int header_count,
                                     Arena *arena, OutputCol **out_cols, int *out_count) {
    int count = 0;
    for (int i = 0; i < stmt->item_count; i++) {
        if (stmt->items[i].expr->type == EXPR_STAR) {
            count += header_count;
        } else {
            count++;
        }
    }

    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(OutputCol) * (size_t)count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    OutputCol *cols = (OutputCol*)mem;
    int idx = 0;

    for (int i = 0; i < stmt->item_count; i++) {
        SelectItem *item = &stmt->items[i];
        if (item->expr->type == EXPR_STAR) {
            for (int j = 0; j < header_count; j++) {
                cols[idx].name = headers[j];
                cols[idx].expr = make_column_ref_node(arena, headers[j]);
                if (cols[idx].expr == NULL) return "Out of memory.";
                cols[idx].expr->col_index = j;
                idx++;
            }
        } else {
            cols[idx].name = item->name ? item->name : "";
            cols[idx].expr = item->expr;
            idx++;
        }
    }

    *out_cols = cols;
    *out_count = count;
    return NULL;
}

static const char* set_result_headers(QueryResult *result, OutputCol *out_cols,
                                      int out_count, Arena *arena) {
    void *mem;
    ArenaResult ar = arena_alloc(arena, sizeof(char*) * (size_t)out_count, &mem);
    if (ar != ARENA_OK) return "Out of memory.";
    result->headers = (char**)mem;
    result->header_count = out_count;

    for (int i = 0; i < out_count; i++) {
        result->headers[i] = arena_strdup(arena, out_cols[i].name);
    }
    return NULL;
}

/* Enforce the grouping rules for the given select items. */
static const char* validate_grouping(SelectStmt *stmt, OutputCol *out_cols, int out_count,
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

/* Build one output row per group, filtered by HAVING. */
static const char* finalize_groups(QueryResult *result, AggGroup **groups, int group_count,
                                   AggSpec *specs, int spec_count, OutputCol *out_cols,
                                   int out_count, int k, SelectStmt *stmt, Arena *arena,
                                   Arena *tmp,
                                   char **headers, int header_count,
                                   EvalResult **sort_keys, int *sort_keys_cap,
                                   int *capacity) {
    for (int gi = 0; gi < group_count; gi++) {
        AggGroup *g = groups[gi];
        arena_reset(tmp);

        AggContext agg_ctx;
        agg_ctx.specs = specs;
        agg_ctx.spec_count = spec_count;
        agg_ctx.states = g->states;

        EvalCtx ctx = eval_ctx_for(&g->rep, headers, header_count, arena, tmp, &agg_ctx);

        /* HAVING filter */
        if (stmt->having) {
            EvalResult hv = eval_expr(stmt->having, &ctx);
            if (eval_result_is_error(&hv)) return hv.error;
            if (!eval_result_is_true(&hv)) continue;
        }

        /* Build projected output record */
        CSVRecord *proj = NULL;
        const char *err = project_row(out_cols, out_count, &ctx, &proj);
        if (err) return err;

        /* ORDER BY keys evaluated per group */
        if (k > 0) {
            err = eval_sort_keys(&ctx, stmt->order_by, k, sort_keys, sort_keys_cap,
                                 result->record_count);
            if (err) return err;
        }

        /* Append to result */
        err = append_result(&result->records, &result->record_count, capacity, proj, arena);
        if (err) return err;
    }
    return NULL;
}

/* ===== Main executor ===== */
QueryResult execute_select(CSVConfig *config, SelectStmt *stmt, Arena *arena, Arena *tmp) {
    QueryResult result = query_result_init();

    /* 0. Validate FROM and open file */
    CSVReader *reader = NULL;
    char **headers = NULL;
    int header_count = 0;
    const char *err = open_reader(config, stmt, arena, &reader, &headers, &header_count);
    if (err) { result.error = err; goto cleanup; }

    /* 1. Get CSV headers / 2. Validate column references */
    const char *bad_col = NULL;
    err = validate_stmt(stmt, headers, header_count, arena, &bad_col);
    if (err) { result.error = err; goto cleanup; }

    /* 3. Build output columns from select items */
    OutputCol *out_cols = NULL;
    int out_count = 0;
    err = build_output_cols(stmt, headers, header_count, arena, &out_cols, &out_count);
    if (err) { result.error = err; goto cleanup; }

    /* 3.5 Aggregate / GROUP BY detection */
    // TODO: Do all of these have to run if one has already found an aggregate?
    bool has_agg = false;
    for (int i = 0; i < out_count; i++) {
        if (expr_contains_aggregate(out_cols[i].expr)) { has_agg = true; break; }
    }
    if (stmt->having && expr_contains_aggregate(stmt->having)) has_agg = true;
    for (int j = 0; j < stmt->order_by_count; j++) {
        if (expr_contains_aggregate(stmt->order_by[j].expr)) { has_agg = true; break; }
    }
    bool group_mode = stmt->group_by_count > 0;
    bool grouped = has_agg || group_mode;

    /* Collect grouped column names (for GROUP BY validation) */
    char **grouped_cols = NULL;
    int grouped_col_count = 0;
    int grouped_col_cap = 0;
    if (group_mode) {
        for (int j = 0; j < stmt->group_by_count; j++) {
            collect_column_refs(stmt->group_by[j], &grouped_cols, &grouped_col_count,
                                &grouped_col_cap, arena);
        }
    }

    err = validate_grouping(stmt, out_cols, out_count, grouped_cols, grouped_col_count,
                            grouped, group_mode, arena);
    if (err) { result.error = err; goto cleanup; }

    /* Collect aggregate specs from select items, HAVING, and ORDER BY */
    AggSpec *specs = NULL;
    int spec_count = 0;
    int spec_cap = 0;
    if (grouped) {
        for (int i = 0; i < out_count; i++) {
            collect_specs(out_cols[i].expr, &specs, &spec_count, &spec_cap, arena);
        }
        if (stmt->having) {
            collect_specs(stmt->having, &specs, &spec_count, &spec_cap, arena);
        }
        for (int j = 0; j < stmt->order_by_count; j++) {
            collect_specs(stmt->order_by[j].expr, &specs, &spec_count, &spec_cap, arena);
        }
        /* Validate DISTINCT usage */
        for (int i = 0; i < spec_count; i++) {
            ExprNode *n = specs[i].node;
            if (!n->distinct) continue;
            if (n->arg_count != 1) {
                result.error = "DISTINCT takes exactly one argument.";
                goto cleanup;
            }
            if (n->args[0]->type == EXPR_STAR) {
                result.error = "DISTINCT cannot be applied to '*'.";
                goto cleanup;
            }
        }
    }

    /* 4. Set result headers */
    err = set_result_headers(&result, out_cols, out_count, arena);
    if (err) { result.error = err; goto cleanup; }

    /* 5. Iterate records */
    int capacity = GROW_INITIAL_CAPACITY;
    void *mem = alloc_or_error(arena, sizeof(CSVRecord*) * (size_t)capacity, &err);
    if (mem == NULL) { result.error = err; goto cleanup; }
    result.records = (CSVRecord**)mem;
    result.record_count = 0;

    /* DISTINCT + LIMIT without ORDER BY or grouping: dedupe incrementally so
       reading can stop once the requested number of distinct rows is found. */
    bool distinct_limit_path = stmt->distinct && stmt->has_limit &&
                               stmt->order_by_count == 0 && !grouped;
    RecordSet rset;
    if (distinct_limit_path) {
        err = record_set_init(arena, &rset);
        if (err) { result.error = err; goto cleanup; }
    }

    /* Parallel sort-keys array (order_by_count entries per row) */
    int k = stmt->order_by_count;
    EvalResult *sort_keys = NULL;
    int sort_keys_cap = 0;

    /* ORDER BY + LIMIT without grouping or DISTINCT: keep only the top
       `window` rows in a bounded heap instead of materializing and sorting
       every row. The window includes OFFSET rows, which are read and discarded
       by apply_limit_offset. Above QUERY_TOPK_MAX_K the bounded heap would
       degenerate, so the query falls back to the full-sort path. */
    long long window_ll = (stmt->has_offset && stmt->offset > 0) ? stmt->offset : 0;
    window_ll += stmt->limit;
    bool topk_path = k > 0 && stmt->has_limit && !grouped && !stmt->distinct &&
                     window_ll >= 0 && window_ll <= QUERY_TOPK_MAX_K;
    TopK topk;
    if (topk_path && window_ll > 0) {
        err = topk_init(arena, &topk, (int)window_ll, k, stmt->order_by);
        if (err) { result.error = err; goto cleanup; }
    }

    /* ORDER BY + LIMIT 0: the window is empty, so no row can match; skip the
       scan entirely. (The topk struct is left uninitialized on purpose.) */
    if (topk_path && window_ll <= 0) {
        result.record_count = 0;
        goto cleanup;
    }

    /* Grouped execution state */
    int group_by_k = stmt->group_by_count;
    AggGroup **groups = NULL;
    int group_count = 0;
    int group_cap = 0;
    GroupTable gtab = {NULL, 0, 0};
    if (grouped) {
        group_cap = GROUP_INITIAL_CAPACITY;
        mem = alloc_or_error(arena, sizeof(AggGroup*) * (size_t)group_cap, &err);
        if (mem == NULL) { result.error = err; goto cleanup; }
        groups = (AggGroup**)mem;

        err = group_table_init(arena, &gtab, GROUP_INITIAL_CAPACITY * 2);
        if (err) { result.error = err; goto cleanup; }

        /* Aggregates without GROUP BY use a single implicit group so that
           empty input still produces one output row. */
        if (!group_mode) {
            groups[group_count] = agg_group_create(0, spec_count, arena);
            if (groups[group_count] == NULL) { result.error = "Out of memory."; goto cleanup; }
            err = group_table_insert(arena, &gtab, groups, 0, 1, 0);
            if (err) { result.error = err; goto cleanup; }
            group_count = 1;
        }
    }

    CSVRecord *record;
    while ((record = csv_reader_next_record(reader)) != NULL) {
        arena_reset(tmp);

        /* Evaluate WHERE */
        if (stmt->where) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            EvalResult where_val = eval_expr(stmt->where, &ctx);
            if (eval_result_is_error(&where_val)) {
                result.error = where_val.error;
                goto cleanup;
            }
            if (!eval_result_is_true(&where_val)) continue;
        }

        /* Grouped mode: accumulate into per-group states, no per-row output */
        if (grouped) {
            EvalResult *keys = NULL;
            if (group_by_k > 0) {
                void *keys_mem;
                ArenaResult ar = arena_alloc(tmp, sizeof(EvalResult) * (size_t)group_by_k,
                                             &keys_mem);
                if (ar != ARENA_OK) { result.error = "Out of memory."; goto cleanup; }
                keys = (EvalResult*)keys_mem;
                for (int j = 0; j < group_by_k; j++) {
                    EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
                    EvalResult er = eval_expr(stmt->group_by[j], &ctx);
                    if (eval_result_is_error(&er)) {
                        result.error = er.error;
                        goto cleanup;
                    }
                    if (!er.is_numeric && er.str_val)
                        er.str_val = arena_strdup(tmp, er.str_val);
                    keys[j] = er;
                }
            }

            int gi = group_table_find(&gtab, groups, keys, group_by_k);
            AggGroup *g = gi >= 0 ? groups[gi] : NULL;
            if (g == NULL) {
                err = grow_array(arena, (void**)&groups, &group_cap, group_count + 1,
                                 sizeof(AggGroup*));
                if (err) { result.error = err; goto cleanup; }
                g = agg_group_create(group_by_k, spec_count, arena);
                if (g == NULL) { result.error = "Out of memory."; goto cleanup; }
                if (group_by_k > 0) {
                    for (int j = 0; j < group_by_k; j++) {
                        g->keys[j] = keys[j];
                        if (!keys[j].is_numeric && keys[j].str_val)
                            g->keys[j].str_val = arena_strdup(arena, keys[j].str_val);
                    }
                }
                groups[group_count] = g;
                err = group_table_insert(arena, &gtab, groups, group_count,
                                         group_count + 1, group_by_k);
                if (err) { result.error = err; goto cleanup; }
                group_count++;
            }
            if (g->rep.field_count == 0) g->rep = copy_record(record, arena);

            for (int i = 0; i < spec_count; i++) {
                EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
                const char *aerr = aggregate_row(
                    specs[i].node->arg_count > 0 ? specs[i].node->args[0] : NULL,
                    specs[i].name, specs[i].distinct, &g->states[i], &ctx);
                if (aerr) { result.error = aerr; goto cleanup; }
            }
            continue;
        }

        /* Top-K path: evaluate the ORDER BY keys, keep only the best `window`
           rows, and project a row only when it survives. Non-kept rows cost
           only the key evaluation. */
        if (topk_path) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            EvalResult *keys;
            ArenaResult ar = arena_alloc(tmp, sizeof(EvalResult) * (size_t)k,
                                         (void**)&keys);
            if (ar != ARENA_OK) { result.error = "Out of memory."; goto cleanup; }
            for (int j = 0; j < k; j++) {
                EvalResult er = eval_expr(stmt->order_by[j].expr, &ctx);
                if (eval_result_is_error(&er)) {
                    result.error = er.error;
                    goto cleanup;
                }
                keys[j] = er;
            }
            if (!topk_would_keep(&topk, keys)) continue;

            CSVRecord *proj = NULL;
            err = project_row(out_cols, out_count, &ctx, &proj);
            if (err) { result.error = err; goto cleanup; }
            topk_insert(arena, &topk, proj, keys);
            continue;
        }

        /* Pre-compute ORDER BY keys on the original record */
        if (k > 0) {
            EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
            err = eval_sort_keys(&ctx, stmt->order_by, k, &sort_keys, &sort_keys_cap,
                                 result.record_count);
            if (err) { result.error = err; goto cleanup; }
        }

        /* Allocate and append projected record */
        CSVRecord *proj = NULL;
        EvalCtx ctx = eval_ctx_for(record, headers, header_count, arena, tmp, NULL);
        err = project_row(out_cols, out_count, &ctx, &proj);
        if (err) { result.error = err; goto cleanup; }

        /* DISTINCT + LIMIT: dedupe incrementally so duplicates never enter the
           result, and stop reading once the window of distinct rows is found.
           Duplicate rows are dropped without growing the result. */
        if (distinct_limit_path) {
            if (!record_set_add(&rset, arena, result.records, proj, &err)) {
                if (err) { result.error = err; goto cleanup; }
                continue;
            }
        }

        err = append_result(&result.records, &result.record_count, &capacity, proj, arena);
        if (err) { result.error = err; goto cleanup; }

        /* LIMIT without ORDER BY can stop reading once the requested window
           is materialized. Skipped for ORDER BY (needs every row to sort) and
           grouped queries (output is built after the scan). With DISTINCT the
           window counts distinct rows, which the incremental dedupe tracks. */
        if (!grouped && stmt->has_limit && stmt->order_by_count == 0) {
            long long target = (stmt->has_offset && stmt->offset > 0) ? stmt->offset : 0;
            target += stmt->limit;
            if (target < 0) target = 0;
            if (result.record_count >= target) break;
        }
    }

    /* 5.5 Grouped mode: build one output row per group, filtered by HAVING */
    if (grouped) {
        err = finalize_groups(&result, groups, group_count, specs, spec_count, out_cols,
                              out_count, k, stmt, arena, tmp, headers, header_count,
                              &sort_keys, &sort_keys_cap, &capacity);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 6. Handle DISTINCT (skipped when the incremental LIMIT path already
       deduped records while scanning). */
    if (stmt->distinct && !distinct_limit_path) {
        err = dedupe_records(&result.records, &result.record_count, k, sort_keys, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 7. ORDER BY: either emit the top-k heap (already in final order) or
       materialize and sort every projected row. */
    if (topk_path) {
        err = topk_emit(arena, &topk, &result.records, &result.record_count);
        if (err) { result.error = err; goto cleanup; }
    } else {
        err = order_records(&result.records, result.record_count, k, sort_keys,
                            stmt->order_by, arena);
        if (err) { result.error = err; goto cleanup; }
    }

    /* 8. LIMIT / OFFSET */
    apply_limit_offset(stmt, result.records, &result.record_count);

cleanup:
    csv_reader_free(reader);
    return result;
}