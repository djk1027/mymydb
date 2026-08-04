#include "serialize.h"

#include <stdlib.h>
#include <string.h>

static void buf_need(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    while (b->cap < b->len + extra) b->cap = b->cap ? b->cap * 2 : 256;
    b->data = realloc(b->data, b->cap);
}

void buf_u32(Buf *b, uint32_t v) {
    buf_need(b, 4); memcpy(b->data + b->len, &v, 4); b->len += 4;
}
void buf_u16(Buf *b, uint16_t v) {
    buf_need(b, 2); memcpy(b->data + b->len, &v, 2); b->len += 2;
}
void buf_u8(Buf *b, uint8_t v) {
    buf_need(b, 1); b->data[b->len++] = v;
}
void buf_str(Buf *b, const char *s) {
    uint16_t n = (uint16_t)strlen(s);
    buf_u16(b, n);
    buf_need(b, n); memcpy(b->data + b->len, s, n); b->len += n;
}
void buf_params(Buf *b, const ParamStore *s) {
    buf_u32(b, (uint32_t)s->n);
    for (int i = 0; i < s->n; i++) {
        buf_str(b, s->items[i].name);
        buf_str(b, s->items[i].value);
    }
}
void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

uint32_t rd_u32(Rdr *r) {
    uint32_t v = 0;
    if (r->pos + 4 <= r->len) memcpy(&v, r->data + r->pos, 4);
    r->pos += 4; return v;
}
uint16_t rd_u16(Rdr *r) {
    uint16_t v = 0;
    if (r->pos + 2 <= r->len) memcpy(&v, r->data + r->pos, 2);
    r->pos += 2; return v;
}
uint8_t rd_u8(Rdr *r) {
    uint8_t v = 0;
    if (r->pos + 1 <= r->len) v = r->data[r->pos];
    r->pos += 1; return v;
}
void rd_str(Rdr *r, char *dst, int cap) {
    uint16_t n = rd_u16(r);
    int k = n < cap - 1 ? n : cap - 1;
    if (r->pos + n <= r->len) memcpy(dst, r->data + r->pos, k);
    dst[k] = '\0';
    r->pos += n;
}
char *rd_strdup(Rdr *r) {
    uint16_t n = rd_u16(r);
    char *s = malloc(n + 1);
    if (r->pos + n <= r->len) memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}
void rd_params(Rdr *r, ParamStore *s) {
    uint32_t n = rd_u32(r);
    for (uint32_t i = 0; i < n; i++) {
        char *name = rd_strdup(r);
        char *val = rd_strdup(r);
        param_set(s, name, val);
        free(name);
        free(val);
    }
}
