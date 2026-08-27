#ifndef QUERY_AGGREGATE_H
#define QUERY_AGGREGATE_H

#include "qarena.h"
#include "ast.h"
#include "eval.h"
#include "executor.h"
#include "record.h"
#include "sort.h"
#include <stdbool.h>
#include <stdint.h>

/* ===== Aggregate descriptors ===== */
/* Query-wide descriptor for one aggregate call node */
struct AggSpec {
    ExprNode *node;        /* the EXPR_FUNCTION_CALL node */
    const char *name;      /* COUNT / SUM / AVG / MIN / MAX */
    AggKind kind;          /* resolved once at spec collection */
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

/* Open-addressing table mapping a group key hash to an index into the
   parallel `groups` array. Slot value -1 means empty. */
typedef struct {
    int *slots;
    int cap;     /* power of two */
    int count;   /* occupied slots */
} GroupTable;

/* Per-group aggregation state */
typedef struct {
    EvalResult *keys;     /* group_by_count key components (arena-owned strings) */
    AggState *states;     /* spec_count accumulator states */
    CSVRecord rep;        /* arena-owned copy of a representative record */
} AggGroup;

/* ===== Grouping ===== */
const char* group_table_init(QArena *arena, GroupTable *t, int cap);
const char* group_table_insert(QArena *arena, GroupTable *t, AggGroup **groups,
                               int gi, int group_count, int k);
int group_table_find(const GroupTable *t, AggGroup **groups,
                     const EvalResult *keys, int k);
AggGroup* agg_group_create(int k, int spec_count, QArena *arena);

/* ===== Aggregate accumulation ===== */
const char* aggregate_row(ExprNode *arg, AggKind kind, bool distinct,
                          AggState *st, EvalCtx *ctx);
EvalResult agg_state_value(const AggState *st, AggKind kind);

/* Find the spec index for a given aggregate call node, or -1. */
int find_spec(const AggSpec *specs, int spec_count, ExprNode *node);

/* Build one output row per group, filtered by HAVING, with ORDER BY keys. */
const char* finalize_groups(QueryResult *result, AggGroup **groups, int group_count,
                            AggSpec *specs, int spec_count, OutputCol *out_cols,
                            int out_count, int k, SelectStmt *stmt, QArena *arena,
                            QArena *tmp,
                            char **headers, int header_count,
                            EvalResult **sort_keys, int *sort_keys_cap,
                            int *capacity);

#endif