#include "param.h"

#include <stdlib.h>
#include <string.h>

static int param_index(const ParamStore *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i].name, name) == 0)
            return i;
    return -1;
}

void param_set(ParamStore *s, const char *name, const char *value) {
    int i = param_index(s, name);
    if (i >= 0) {
        free(s->items[i].value);
        s->items[i].value = strdup(value);
        return;
    }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->items = realloc(s->items, s->cap * sizeof(Param));
    }
    s->items[s->n].name = strdup(name);
    s->items[s->n].value = strdup(value);
    s->n++;
}

const char *param_get(const ParamStore *s, const char *name) {
    int i = param_index(s, name);
    return i >= 0 ? s->items[i].value : NULL;
}

void param_copy(ParamStore *dst, const ParamStore *src) {
    for (int i = 0; i < src->n; i++)
        param_set(dst, src->items[i].name, src->items[i].value);
}

void param_free(ParamStore *s) {
    for (int i = 0; i < s->n; i++) {
        free(s->items[i].name);
        free(s->items[i].value);
    }
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}
