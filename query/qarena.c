#include "qarena.h"
#include <stdlib.h>
#include <string.h>

#define QARENA_ALIGN 8

struct QArenaChunk {
    struct QArenaChunk *next;
    size_t size;   /* payload capacity in bytes */
    size_t used;   /* payload bytes currently allocated */
};

static size_t align_up(size_t n) {
    return (n + QARENA_ALIGN - 1) & ~((size_t)QARENA_ALIGN - 1);
}

static char* chunk_payload(QArenaChunk *chunk) {
    return (char*)(chunk + 1);
}

/* Append a new chunk (or reuse a fitting spare) as the active chunk.
 * Growth is geometric (double the previous chunk size, never below the
 * request) so repeated small allocations don't fragment memory. */
static QArenaResult chain_new_chunk(QArena *qa, size_t wanted) {
    size_t want = align_up(wanted);
    size_t grow = want;
    if (qa->active) {
        size_t prev = qa->active->size;
        if (prev <= (size_t)-1 / 2) {
            size_t doubled = prev * 2;
            if (doubled > grow) grow = doubled;
        }
    }

    QArenaChunk *prev_spare = NULL;
    for (QArenaChunk *c = qa->spare; c; c = c->next) {
        if (c->size >= want) {
            if (prev_spare) prev_spare->next = c->next;
            else qa->spare = c->next;
            c->used = 0;
            c->next = NULL;
            if (qa->active) qa->active->next = c;
            else qa->first = c;
            qa->active = c;
            return QARENA_OK;
        }
        prev_spare = c;
    }

    QArenaChunk *c = (QArenaChunk*)malloc(sizeof(QArenaChunk) + grow);
    if (c == NULL) return QARENA_ERROR_MEMORY_ALLOCATION;
    c->next = NULL;
    c->size = grow;
    c->used = 0;
    if (qa->active) qa->active->next = c;
    else qa->first = c;
    qa->active = c;
    return QARENA_OK;
}

QArenaResult qarena_create(QArena *qa, size_t size) {
    if (!qa) return QARENA_ERROR_NULL_POINTER;
    if (size == 0) return QARENA_ERROR_INVALID_SIZE;

    QArenaChunk *chunk = (QArenaChunk*)malloc(sizeof(QArenaChunk) + size);
    if (chunk == NULL) return QARENA_ERROR_MEMORY_ALLOCATION;
    chunk->next = NULL;
    chunk->size = size;
    chunk->used = 0;

    qa->first = chunk;
    qa->active = chunk;
    qa->spare = NULL;
    return QARENA_OK;
}

void qarena_reset(QArena *qa) {
    if (!qa || !qa->first) return;

    /* Keep the first chunk (its data stays valid); park every other chunk
       on the spare list for reuse by later growth. */
    QArenaChunk *c = qa->first->next;
    qa->first->next = NULL;
    qa->first->used = 0;
    while (c) {
        QArenaChunk *next = c->next;
        c->used = 0;
        c->next = qa->spare;
        qa->spare = c;
        c = next;
    }
    qa->active = qa->first;
}

void qarena_destroy(QArena *qa) {
    if (!qa) return;

    QArenaChunk *c = qa->first;
    while (c) {
        QArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    c = qa->spare;
    while (c) {
        QArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    memset(qa, 0, sizeof(QArena));
}

QArenaResult qarena_alloc(QArena *qa, size_t size, void **ptr) {
    if (!qa || !ptr) return QARENA_ERROR_NULL_POINTER;
    if (size == 0) return QARENA_ERROR_INVALID_SIZE;
    if (!qa->first || !qa->active) return QARENA_ERROR_NULL_POINTER;

    QArenaChunk *chunk = qa->active;
    char *cur = chunk_payload(chunk) + chunk->used;
    size_t aligned = align_up(size);

    if (cur + aligned > chunk_payload(chunk) + chunk->size) {
        QArenaResult ar = chain_new_chunk(qa, aligned);
        if (ar != QARENA_OK) {
            *ptr = NULL;
            return ar;
        }
        chunk = qa->active;
        cur = chunk_payload(chunk);
    }

    *ptr = cur;
    chunk->used += aligned;

    return QARENA_OK;
}

char* qarena_strdup(QArena *qa, const char *str) {
    if (!qa || !str) return NULL;

    size_t len = strlen(str);
    void *ptr;
    QArenaResult result = qarena_alloc(qa, len + 1, &ptr);
    if (result != QARENA_OK) return NULL;

    char *copy = (char*)ptr;
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

/* Reallocation in a bump arena is always a copy: the caller updates its
 * pointer, and the old region is simply abandoned (never touched later by
 * the allocator). This matches the library arena contract: a NULL `ptr` is
 * an allocation of new_size, shrinking is a no-op returning `ptr`. */
void* qarena_realloc(QArena *qa, void *ptr, size_t old_size, size_t new_size) {
    if (!qa) return NULL;
    if (new_size == 0) return NULL;

    if (!ptr) {
        void *new_ptr;
        QArenaResult result = qarena_alloc(qa, new_size, &new_ptr);
        return (result == QARENA_OK) ? new_ptr : NULL;
    }

    if (new_size <= old_size) {
        return ptr;
    }

    void *new_ptr;
    QArenaResult result = qarena_alloc(qa, new_size, &new_ptr);
    if (result != QARENA_OK) return NULL;

    if (old_size > 0) {
        memcpy(new_ptr, ptr, old_size);
    }

    return new_ptr;
}
