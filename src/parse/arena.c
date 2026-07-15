#include "arena.h"

#include <stdlib.h>
#include <string.h>

/* Each user allocation is preceded by this header and chained into a list. */
typedef struct Block {
    struct Block *next;
    size_t size;      /* usable payload size */
    /* payload follows, suitably aligned */
    max_align_t data[]; /* flexible array forces max alignment */
} Block;

struct Arena {
    Block *head;
};

Arena *arena_new(void) {
    Arena *a = calloc(1, sizeof(Arena));
    return a;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    Block *b = a->head;
    while (b) {
        Block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void *arena_alloc(Arena *a, size_t size) {
    Block *b = malloc(sizeof(Block) + size);
    if (!b) { abort(); } /* OOM: acceptable for this toy engine */
    b->size = size;
    b->next = a->head;
    a->head = b;
    return b->data;
}

void *arena_calloc(Arena *a, size_t size) {
    void *p = arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

void *arena_realloc(Arena *a, void *old, size_t oldsize, size_t newsize) {
    void *p = arena_alloc(a, newsize);
    if (old && oldsize)
        memcpy(p, old, oldsize < newsize ? oldsize : newsize);
    return p; /* old block remains tracked and is freed at destroy */
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
