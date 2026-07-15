#include "ast.h"

void stmt_free(Stmt *s) {
    if (!s) return;
    /* The Stmt and everything it points to live in this arena, so a single
     * destroy releases the entire AST. Read the handle before freeing, since
     * s itself is allocated inside the arena. */
    Arena *a = s->arena;
    arena_destroy(a);
}
