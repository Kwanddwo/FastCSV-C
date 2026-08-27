#ifndef QUERY_EVAL_H
#define QUERY_EVAL_H

#include "qarena.h"
#include "../csv_reader.h"
#include "ast.h"
#include <stdbool.h>
#include <stddef.h>
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
/* Per-row memo of cell text classification (strtod is locale-aware and
   slow): the first classification of a field in a row is cached for every
   subsequent reference in WHERE, sort keys, grouping and projection. The
   memo lives in the temp arena and dies with the row's arena reset. */
typedef struct {
    EvalResult *vals;   /* classified result per field index */
    uint8_t *valid;     /* parallel flags */
    int cap;            /* == record->field_count */
} CellMemo;

typedef struct {
    CSVRecord *record;
    char **headers;
    int header_count;
    QArena *arena;    /* result arena: persistent outputs are copied here */
    QArena *tmp;      /* temp arena: per-row transient evaluation scratch */
    CellMemo *memo;   /* per-row cell classification cache; NULL = none */
    const AggContext *agg;
    int depth;        /* eval_expr recursion depth (stack-overflow guard) */
} EvalCtx;

/* ===== EvalResult builders ===== */
EvalResult eval_result_null(void);
EvalResult eval_result_num(double val);
/* A numeric value whose raw text is preserved for display and string
   functions (e.g. a cell or literal "05"): is_numeric is true, but
   eval_result_to_string yields the original text, not a reformatted number. */
EvalResult eval_result_num_text(double val, const char *raw_text);
EvalResult eval_result_str(const char *s);
EvalResult eval_result_error(const char *msg);
bool eval_result_is_error(const EvalResult *r);
bool eval_result_is_true(const EvalResult *r);

/* Type-aware numeric coercion used by comparisons and DISTINCT hashing:
   returns true when r is numeric or its text parses as a number. */
bool parse_numeric_str(const EvalResult *r, double *out);

/* Total order used by comparisons, MIN/MAX and DISTINCT equality:
   NULL < numeric-like < text. Two values are "equal" when this returns 0. */
int eval_result_compare(const EvalResult *a, const EvalResult *b);

/* QArena-owning copy of a string result; NULL-safe. */
const char* eval_result_dup_to_arena(const EvalResult *r, QArena *arena);

/* Format a value for display (arena-allocated string). */
const char* eval_result_to_string(const EvalResult *r, QArena *arena);

/* ===== LIKE pattern matching ===== */
/* Match s against the pattern p; esc (0 = none) is the ESCAPE character
   making the next pattern character literal. */
bool like_match(const char *s, const char *p, char esc, bool case_insensitive);

/* ===== Built-in functions ===== */
bool is_aggregate_name(const char *name);

/* Aggregate kinds used by the cached per-node dispatch. */
typedef enum {
    AGG_NONE = 0,
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggKind;

/* Classify an aggregate name once (AGG_NONE if not an aggregate). */
AggKind aggregate_kind(const char *name);

/* Resolve a scalar function name once (index into the builtin table, or
   -1 for an unknown function). */
int lookup_function_index(const char *name);

/* True when s fully parses as a number; *out receives the value. */
bool text_parses_numeric(const char *s, double *out);

/* True when a scalar function is volatile (returns a different value on each
   evaluation): such calls must never be constant-folded. */
bool is_volatile_function(const char *name);

/* Evaluate an expression node against a context. */
EvalResult eval_expr(ExprNode *node, EvalCtx *ctx);

/* Build the evaluation context for one row (agg is NULL outside of
   aggregate finalization). */
EvalCtx eval_ctx_for(CSVRecord *record, char **headers, int header_count,
                     QArena *arena, QArena *tmp, CellMemo *memo,
                     const AggContext *agg);

#endif