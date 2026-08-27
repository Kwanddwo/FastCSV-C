/* ORDER BY machinery: key comparison, bounded top-k heap, full sort and
 * LIMIT / OFFSET application. Split out of executor.c. */
#include "sort.h"
#include "str_util.h"
#include <stdlib.h>
#include <string.h>
/* ===== ORDER BY helpers ===== */

/* Compare two entries' ORDER BY keys. Negative means a sorts before b.
   Mirrors the ORDER BY clause: per-key eval_result_compare, flipped for DESC.
   Used both by the merge-sort path and the top-k heap so their ordering
   semantics (NULLs, type-aware compare, multi-key) are identical. */
int cmp_keys(const EvalResult *a, const EvalResult *b, int k,
                    const OrderByItem *order_by) {
    for (int j = 0; j < k; j++) {
        bool an = a[j].is_null;
        bool bn = b[j].is_null;
        if (an || bn) {
            if (an && bn) continue;   /* both NULL: equal on this key */
            /* NULL placement is direction-independent: an explicit
               NULLS FIRST/LAST wins over ASC/DESC; unspecified keeps the
               historical default (ASC -> NULLs first, DESC -> NULLs last). */
            int nulls_first;
            if (order_by[j].nulls == 1) nulls_first = 1;
            else if (order_by[j].nulls == 2) nulls_first = 0;
            else nulls_first = order_by[j].asc;
            return an ? (nulls_first ? -1 : 1) : (nulls_first ? 1 : -1);
        }
        int cmp = eval_result_compare(&a[j], &b[j]);
        if (cmp != 0)
            return order_by[j].asc ? cmp : -cmp;
    }
    return 0;
}

/* Comparator over index values with the arrival-order tie-break (the index
   array starts as 0..n-1, i.e. scan order), so equal keys keep their input
   order: deterministic and identical to the top-k path's tie handling. */
static int cmp_idx(const EvalResult *keys, int k, const OrderByItem *ob,
                   int a, int b) {
    int c = cmp_keys(&keys[a * k], &keys[b * k], k, ob);
    if (c != 0) return c;
    return a - b;
}

/* Stable bottom-up-ish mergesort over the index array with an explicit
   scratch buffer: no module statics (the qsort comparator closure problem),
   deterministic, and stability is by construction rather than a
   tie-break hack. */
static void msort_merge(int *a, int a_len, int *b, int b_len, int *out,
                        const EvalResult *keys, int k, const OrderByItem *ob) {
    int i = 0, j = 0, o = 0;
    while (i < a_len && j < b_len) {
        if (cmp_idx(keys, k, ob, a[i], b[j]) <= 0) out[o++] = a[i++];
        else out[o++] = b[j++];
    }
    while (i < a_len) out[o++] = a[i++];
    while (j < b_len) out[o++] = b[j++];
}

static void msort_rec(int *a, int *tmp, int n,
                      const EvalResult *keys, int k, const OrderByItem *ob) {
    if (n <= 1) return;
    int m = n / 2;
    msort_rec(a, tmp, m, keys, k, ob);
    msort_rec(a + m, tmp + m, n - m, keys, k, ob);
    msort_merge(a, m, a + m, n - m, tmp, keys, k, ob);
    for (int i = 0; i < n; i++) a[i] = tmp[i];
}

static void sort_indices(int *order, int n,
                         const EvalResult *keys, int k,
                         const OrderByItem *ob, int *scratch) {
    if (n <= 1) return;
    msort_rec(order, scratch, n, keys, k, ob);
}

/* ===== Top-K heap (ORDER BY + LIMIT) ===== */

/* Keeps the best `window` rows in final ORDER BY order using a positional
   binary max-heap: heap[i] is an entry index, and the root (heap[0]) is the
   worst kept entry, i.e. the first candidate for eviction. Entries hold the
   projected output record plus its persisted ORDER BY keys. */
const char* topk_init(QArena *arena, TopK *tk, int cap, int key_count,
                             const OrderByItem *order_by) {
    tk->cap = cap;
    tk->count = 0;
    tk->key_count = key_count;
    tk->order_by = order_by;
    tk->next_ord = 0;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(CSVRecord*) * (size_t)cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    tk->recs = (CSVRecord**)mem;
    ar = qarena_alloc(arena, sizeof(EvalResult) * (size_t)cap * (size_t)key_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    tk->keys = (EvalResult*)mem;
    ar = qarena_alloc(arena, sizeof(int) * (size_t)cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    tk->heap = (int*)mem;
    ar = qarena_alloc(arena, sizeof(int) * (size_t)cap, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    tk->ord = (int*)mem;
    return NULL;
}

/* Entry comparator with the arrival-ordinal tie-break: earlier arrivals are
   "better" when the ORDER BY keys tie, independent of sort direction. */
static int cmp_entries(const TopK *tk, int a, int b) {
    int c = cmp_keys(&tk->keys[a * tk->key_count], &tk->keys[b * tk->key_count],
                     tk->key_count, tk->order_by);
    if (c != 0) return c;
    if (tk->ord[a] < tk->ord[b]) return -1;
    if (tk->ord[a] > tk->ord[b]) return 1;
    return 0;
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
static void persist_keys(QArena *arena, EvalResult *keys, int key_count) {
    for (int j = 0; j < key_count; j++) {
        if (keys[j].str_val)
            keys[j].str_val = qarena_strdup(arena, keys[j].str_val);
    }
}

/* Sift entry at position `i` toward the root. The root holds the worst entry,
   so a parent may never compare "better" than a child. */
static void topk_sift_up(TopK *tk, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (cmp_entries(tk, tk->heap[parent], tk->heap[i]) >= 0) break;
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
        if (right < tk->count &&
            cmp_entries(tk, tk->heap[left], tk->heap[right]) < 0)
            worst = right;
        if (cmp_entries(tk, tk->heap[i], tk->heap[worst]) >= 0) break;
        int tmp = tk->heap[i];
        tk->heap[i] = tk->heap[worst];
        tk->heap[worst] = tmp;
        i = worst;
    }
}

/* Store a kept entry: copy rec + keys into the next free slot (or over the
   evicted root), persisting string keys, and restore the heap invariant. */
void topk_insert(QArena *arena, TopK *tk, CSVRecord *rec,
                        const EvalResult *keys) {
    int idx;
    if (tk->count < tk->cap) {
        idx = tk->count++;
        tk->heap[idx] = idx;
        tk->ord[idx] = tk->next_ord++;
        memcpy(&tk->keys[idx * tk->key_count], keys,
               sizeof(EvalResult) * (size_t)tk->key_count);
        persist_keys(arena, &tk->keys[idx * tk->key_count], tk->key_count);
        topk_sift_up(tk, idx);
    } else {
        idx = tk->heap[0];   /* worst kept entry, being evicted */
        tk->ord[idx] = tk->next_ord++;
        memcpy(&tk->keys[idx * tk->key_count], keys,
               sizeof(EvalResult) * (size_t)tk->key_count);
        persist_keys(arena, &tk->keys[idx * tk->key_count], tk->key_count);
        topk_sift_down(tk);
    }
    tk->recs[idx] = rec;
}

/* Emit the stored entries in ascending final ORDER BY order into a fresh
   arena-allocated array. */
const char* topk_emit(QArena *arena, TopK *tk, CSVRecord ***out,
                             int *out_count) {
    int n = tk->count;
    *out_count = n;
    if (n == 0) { *out = NULL; return NULL; }
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(CSVRecord*) * (size_t)n, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
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
            er.str_val = qarena_strdup(ctx->arena, er.str_val);
        (*sort_keys)[idx * k + j] = er;
    }
    return NULL;
}
const char* order_records(CSVRecord ***records, int record_count, int k,
                                 const EvalResult *sort_keys, const OrderByItem *order_by,
                                 QArena *arena) {
    if (k <= 0 || record_count <= 1) return NULL;
    void *mem;
    QArenaResult ar = qarena_alloc(arena, sizeof(int) * (size_t)record_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    int *order = (int*)mem;
    for (int i = 0; i < record_count; i++) order[i] = i;

    /* Mergesort scratch (arena-owned). */
    ar = qarena_alloc(arena, sizeof(int) * (size_t)record_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
    int *scratch = (int*)mem;

    sort_indices(order, record_count, sort_keys, k, order_by, scratch);

    ar = qarena_alloc(arena, sizeof(CSVRecord*) * (size_t)record_count, &mem);
    if (ar != QARENA_OK) return "Out of memory.";
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
