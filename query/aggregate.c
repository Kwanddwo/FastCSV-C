/* Aggregation: accumulator states, DISTINCT value sets, the group hash
 * table and per-group finalization. Split out of executor.c. */
#include "aggregate.h"
#include "hash.h"
#include "str_util.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Initial capacity for the DISTINCT value set (doubled on demand). */
static const int SEEN_INITIAL_CAPACITY = 8;    /* DISTINCT value-set capacity */

/* ===== Aggregate accumulation ===== */
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
static const char* value_set_ensure(QArena *arena, ValueSet *s) {
    if (s->slots != NULL) return NULL;
    s->cap = SEEN_INITIAL_CAPACITY;
    s->count = 0;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)s->cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    s->slots = (int*)mem;
    for (int i = 0; i < s->cap; i++) s->slots[i] = -1;
    ar = qarena_alloc(arena, sizeof(uint64_t) * (size_t)s->cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    s->hashes = (uint64_t*)mem;
    ar = qarena_alloc(arena, sizeof(EvalResult) * (size_t)s->cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    s->values = (EvalResult*)mem;
    return NULL;
}

/* Double the set capacity and rehash every stored value. */
static const char* value_set_grow(QArena *arena, ValueSet *s) {
    int new_cap = s->cap * 2;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    int *slots = (int*)mem;
    for (int i = 0; i < new_cap; i++) slots[i] = -1;
    for (int i = 0; i < s->count; i++) {
        size_t slot = (size_t)(s->hashes[i] & (uint64_t)(new_cap - 1));
        while (slots[slot] != -1) slot = (slot + 1) & (uint64_t)(new_cap - 1);
        slots[slot] = i;
    }
    uint64_t *new_hashes = (uint64_t*)qarena_realloc(
        arena, s->hashes, sizeof(uint64_t) * (size_t)s->cap,
        sizeof(uint64_t) * (size_t)new_cap);
    if (new_hashes == NULL) return "Out of memory.";
    EvalResult *new_values = (EvalResult*)qarena_realloc(
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
static const char* agg_seen_add(AggState *st, const EvalResult *v, QArena *arena,
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
        const char *dup = qarena_strdup(arena, copy.str_val);
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

/* Install v as the MIN/MAX best value (string copies are arena-owned). A
   numeric value that carries its raw text (from a CSV cell or literal) has
   its text duplicated too: the raw pointer may be reader-owned and must not
   dangle once the scan moves on. */
static void agg_best_set(AggState *st, const EvalResult *v, QArena *arena) {
    if (v->is_numeric) {
        st->best = *v;
        if (v->str_val) st->best.str_val = qarena_strdup(arena, v->str_val);
    } else {
        st->best.is_null = false;
        st->best.is_numeric = false;
        st->best.num_val = 0.0;
        st->best.str_val = qarena_strdup(arena, v->str_val ? v->str_val : "");
    }
}

/* Accumulate one record into the state. Returns an error string on failure. */
const char* aggregate_row(ExprNode *arg, const char *name, bool distinct,
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
EvalResult agg_state_value(const AggState *st, const char *name) {
    if (str_ieq(name, "COUNT")) return eval_result_num((double)st->count);
    if (!st->has_value) return eval_result_null();
    if (str_ieq(name, "SUM")) return eval_result_num(st->sum);
    if (str_ieq(name, "AVG")) return eval_result_num(st->sum / (double)st->count);
    return st->best;   /* MIN / MAX; str_val is arena-owned */
}

/* ===== Grouping ===== */

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

/* ===== Group hash table ===== */

const char* group_table_init(QArena *arena, GroupTable *t, int cap) {
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    t->slots = (int*)mem;
    t->cap = cap;
    t->count = 0;
    for (int i = 0; i < cap; i++) t->slots[i] = -1;
    return NULL;
}

/* Rehash every group into a table twice as large. */
static const char* group_table_grow(QArena *arena, GroupTable *t, AggGroup **groups,
                                    int group_count, int k) {
    int new_cap = t->cap * 2;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)new_cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
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
int group_table_find(const GroupTable *t, AggGroup **groups,
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
const char* group_table_insert(QArena *arena, GroupTable *t, AggGroup **groups,
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
AggGroup* agg_group_create(int k, int spec_count, QArena *arena) {
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(AggGroup), &mem);
    if (ar != QARENA_OK) return NULL;
    AggGroup *g = (AggGroup*)mem;
    g->keys = NULL;
    if (k > 0) {
        ar = qarena_alloc(arena, sizeof(EvalResult) * (size_t)k, &mem);
        if (ar != QARENA_OK) return NULL;
        g->keys = (EvalResult*)mem;
    }
    g->states = NULL;
    if (spec_count > 0) {
        ar = qarena_alloc(arena, sizeof(AggState) * (size_t)spec_count, &mem);
        if (ar != QARENA_OK) return NULL;
        g->states = (AggState*)mem;
        for (int i = 0; i < spec_count; i++) {
            agg_state_init(&g->states[i]);
        }
    }
g->rep.fields = NULL;
    g->rep.field_count = 0;
    return g;
}

const char* finalize_groups(QueryResult *result, AggGroup **groups, int group_count,
                                   AggSpec *specs, int spec_count, OutputCol *out_cols,
                                   int out_count, int k, SelectStmt *stmt, QArena *arena,
                                   QArena *tmp,
                                   char **headers, int header_count,
                                   EvalResult **sort_keys, int *sort_keys_cap,
                                   int *capacity) {
    for (int gi = 0; gi < group_count; gi++) {
        AggGroup *g = groups[gi];
        qarena_reset(tmp);

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

/* Find the spec index for a given aggregate call node, or -1. */
int find_spec(const AggSpec *specs, int spec_count, ExprNode *node) {
    for (int i = 0; i < spec_count; i++) {
        if (specs[i].node == node) return i;
    }
    return -1;
}
