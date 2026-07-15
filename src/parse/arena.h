#ifndef MYMYDB_ARENA_H
#define MYMYDB_ARENA_H

#include <stddef.h>

/*
 * A simple region allocator. Every allocation is tracked; arena_destroy frees
 * all of them at once. This lets the parser bail out of a half-built AST (via
 * longjmp) without leaking, and lets a finished AST be freed with a single call.
 *
 * Each allocation is an independent malloc chained into a list, so pointers
 * stay stable (never moved) after they are handed out.
 */
typedef struct Arena Arena;

Arena *arena_new(void);
void arena_destroy(Arena *a);

void *arena_alloc(Arena *a, size_t size);   /* uninitialized */
void *arena_calloc(Arena *a, size_t size);  /* zeroed */

/* Grow an earlier arena allocation; contents up to min(old,new) are preserved.
 * The old block stays tracked (freed at destroy); pass old==NULL to allocate. */
void *arena_realloc(Arena *a, void *old, size_t oldsize, size_t newsize);

/* Copy the first n bytes of s into an arena block and NUL-terminate. */
char *arena_strndup(Arena *a, const char *s, size_t n);

#endif /* MYMYDB_ARENA_H */
