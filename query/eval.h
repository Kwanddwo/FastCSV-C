#ifndef QUERY_EVAL_H
#define QUERY_EVAL_H

#include "../arena.h"
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
typedef struct {
    CSVRecord *record;
    char **headers;
    int header_count;
    Arena *arena;    /* result arena: persistent outputs are copied here */
    Arena *tmp;      /* temp arena: per-row transient evaluation scratch */
    const AggContext *agg;
} EvalCtx;

/* ===== EvalResult builders ===== */
EvalResult eval_result_null(void);
EvalResult eval_result_num(double val);
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

/* Arena-owning copy of a string result; NULL-safe. */
const char* eval_result_dup_to_arena(const EvalResult *r, Arena *arena);

/* Format a value for display (arena-allocated string). */
const char* eval_result_to_string(const EvalResult *r, Arena *arena);

/* ===== LIKE pattern matching ===== */
bool like_match(const char *s, const char *p, bool case_insensitive);

/* ===== Built-in functions ===== */
bool is_aggregate_name(const char *name);

/* Evaluate an expression node against a context. */
EvalResult eval_expr(ExprNode *node, EvalCtx *ctx);

/* Build the evaluation context for one row (agg is NULL outside of
   aggregate finalization). */
EvalCtx eval_ctx_for(CSVRecord *record, char **headers, int header_count,
                     Arena *arena, Arena *tmp, const AggContext *agg);

#endif