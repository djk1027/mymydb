#ifndef MYMYDB_VALUE_H
#define MYMYDB_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Column / value types supported in v0. */
typedef enum {
    TYPE_INT,
    TYPE_TEXT,
} ColType;

/* A single cell value. Owns its string when type == TYPE_TEXT. */
typedef struct {
    ColType type;
    bool is_null;
    union {
        int64_t i;
        char *s; /* heap-allocated, NUL-terminated */
    } as;
} Value;

Value value_int(int64_t v);
Value value_text(const char *s);          /* copies s (NUL-terminated) */
Value value_text_n(const char *s, size_t n); /* copies n bytes, adds NUL */
Value value_null(ColType type);
Value value_copy(const Value *v);
void value_free(Value *v);

const char *coltype_name(ColType t);

#endif /* MYMYDB_VALUE_H */
