#ifndef QUERY_QARENA_H
#define QUERY_QARENA_H

#include <stddef.h>

/* Growable chunk-chaining bump allocator used by the query engine. Unlike the
 * library's fixed-buffer Arena (arena.h), this allocator never fails short of
 * real memory exhaustion: when the active chunk is full it chains another one
 * (geometric growth), or reuses a spare chunk parked by qarena_reset. */
typedef struct QArenaChunk QArenaChunk;

typedef struct {
    QArenaChunk *first;   /* first chunk, kept across resets */
    QArenaChunk *active;  /* chunk currently being bumped into */
    QArenaChunk *spare;   /* chunks parked by qarena_reset, reusable */
} QArena;

typedef enum {
    QARENA_OK = 0,
    QARENA_ERROR_NULL_POINTER,
    QARENA_ERROR_MEMORY_ALLOCATION,
    QARENA_ERROR_OUT_OF_MEMORY,
    QARENA_ERROR_INVALID_SIZE
} QArenaResult;

QArenaResult qarena_create(QArena *arena, size_t size);
void qarena_reset(QArena *arena);
void qarena_destroy(QArena *arena);

QArenaResult qarena_alloc(QArena *arena, size_t size, void **ptr);
char* qarena_strdup(QArena *arena, const char *str);
void* qarena_realloc(QArena *arena, void *ptr, size_t old_size, size_t new_size);

#endif
