/* ORDER BY machinery: key comparison, bounded top-k heap, full sort and
 * LIMIT / OFFSET application. Split out of executor.c. */
#include "sort.h"
#include "str_util.h"
#include <stdlib.h>
#include <string.h>
/* ===== ORDER BY helpers ===== */

/* Compare two entries' ORDER BY keys. Negative means a sorts before b.
   Mirrors the ORDER BY clause: per-key eval_result_compare, flipped for DESC.
   Used both by the qsort path and the top-k heap so their ordering semantics
   (NULLs, type-aware compare, multi-key) are identical. */
int cmp_keys(const EvalResult *a, const EvalResult *b, int k,
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
const char* topk_init(Arena *arena, TopK *tk, int cap, int key_count,
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
bool topk_would_keep(const TopK *tk, const EvalResult *keys) {
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
void topk_insert(Arena *arena, TopK *tk, CSVRecord *rec,
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
const char* topk_emit(Arena *arena, TopK *tk, CSVRecord ***out,
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

const char* eval_sort_keys(EvalCtx *ctx, const OrderByItem *order_by, int k,
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
const char* order_records(CSVRecord ***records, int record_count, int k,
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
void apply_limit_offset(SelectStmt *stmt, CSVRecord **records, int *record_count) {
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
