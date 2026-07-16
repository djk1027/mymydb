#include "value.h"

#include <stdlib.h>
#include <string.h>

Value value_int(int64_t v) {
    Value out = {.type = TYPE_INT, .is_null = false};
    out.as.i = v;
    return out;
}

Value value_text(const char *s) {
    Value out = {.type = TYPE_TEXT, .is_null = false};
    out.as.s = s ? strdup(s) : strdup("");
    return out;
}

Value value_text_n(const char *s, size_t n) {
    Value out = {.type = TYPE_TEXT, .is_null = false};
    char *buf = malloc(n + 1);
    if (s && n) memcpy(buf, s, n);
    buf[n] = '\0';
    out.as.s = buf;
    return out;
}

Value value_null(ColType type) {
    Value out = {.type = type, .is_null = true};
    if (type == TYPE_TEXT) out.as.s = NULL;
    else out.as.i = 0;
    return out;
}

Value value_copy(const Value *v) {
    Value out = *v;
    if (v->type == TYPE_TEXT && !v->is_null && v->as.s)
        out.as.s = strdup(v->as.s);
    return out;
}

void value_free(Value *v) {
    if (v && v->type == TYPE_TEXT && v->as.s) {
        free(v->as.s);
        v->as.s = NULL;
    }
}

const char *coltype_name(ColType t) {
    switch (t) {
        case TYPE_INT:  return "INT";
        case TYPE_TEXT: return "TEXT";
    }
    return "?";
}
