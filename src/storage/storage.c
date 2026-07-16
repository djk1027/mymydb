#include "storage.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Slotted-page storage. Each page is exactly PAGE_SIZE (4KB) bytes:
 *
 *   +-----------------------------------------------------------+
 *   | header (4B) | slot dir -->            ... <-- tuple data  |
 *   +-----------------------------------------------------------+
 *
 * The slot directory grows upward from just after the header; tuple data grows
 * downward from the end of the page. A page is full when the two would meet.
 *
 *   header : u16 nslots, u16 free_end (offset where tuple data begins)
 *   slot i : u16 offset, u16 length   (at HDR_SIZE + i*SLOT_SIZE)
 *   tuple  : null-bitmap, then each non-null cell:
 *              INT  -> 8 bytes (native int64)
 *              TEXT -> u16 length, then that many bytes (no NUL)
 */

struct Page {
    uint8_t data[PAGE_SIZE];
};

#define HDR_SIZE  4
#define SLOT_SIZE 4

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void     wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static uint16_t page_nslots(const Page *pg)  { return rd16(pg->data); }
static uint16_t page_free_end(const Page *pg) { return rd16(pg->data + 2); }

static void page_init(Page *pg) {
    wr16(pg->data, 0);              /* nslots   */
    wr16(pg->data + 2, PAGE_SIZE);  /* free_end */
}

static int bitmap_bytes(int ncols) { return (ncols + 7) / 8; }

/* Serialized size of a row, or -1 if it cannot fit in an (empty) page. */
static int tuple_size(const Table *t, const Value *cells) {
    long size = bitmap_bytes(t->ncols);
    for (int i = 0; i < t->ncols; i++) {
        if (cells[i].is_null) continue;
        if (cells[i].type == TYPE_INT) size += 8;
        else size += 2 + (long)strlen(cells[i].as.s ? cells[i].as.s : "");
    }
    /* One slot plus the tuple must both fit after the header. */
    if (size + SLOT_SIZE > PAGE_SIZE - HDR_SIZE) return -1;
    return (int)size;
}

static void serialize_tuple(const Table *t, const Value *cells, uint8_t *dst) {
    int bm = bitmap_bytes(t->ncols);
    memset(dst, 0, bm);
    uint8_t *p = dst + bm;
    for (int i = 0; i < t->ncols; i++) {
        if (cells[i].is_null) {
            dst[i / 8] |= (uint8_t)(1u << (i % 8));
            continue;
        }
        if (cells[i].type == TYPE_INT) {
            int64_t v = cells[i].as.i;
            memcpy(p, &v, 8);
            p += 8;
        } else {
            const char *s = cells[i].as.s ? cells[i].as.s : "";
            uint16_t len = (uint16_t)strlen(s);
            wr16(p, len);
            p += 2;
            memcpy(p, s, len);
            p += len;
        }
    }
}

/* Attempts to place a row of the given serialized size into pg. */
static bool page_insert(Page *pg, const Table *t, const Value *cells, int size) {
    uint16_t nslots = page_nslots(pg);
    uint16_t free_end = page_free_end(pg);
    uint16_t free_start = HDR_SIZE + nslots * SLOT_SIZE;

    if (free_end - free_start < size + SLOT_SIZE) return false;

    uint16_t off = free_end - (uint16_t)size;
    serialize_tuple(t, cells, pg->data + off);

    uint8_t *slot = pg->data + HDR_SIZE + nslots * SLOT_SIZE;
    wr16(slot, off);
    wr16(slot + 2, (uint16_t)size);

    wr16(pg->data, (uint16_t)(nslots + 1));
    wr16(pg->data + 2, off);
    return true;
}

/* Deserializes slot `s` of `pg` into cells[t->ncols] as owned Values. */
static void deserialize_tuple(const Table *t, const Page *pg, int s, Value *cells) {
    const uint8_t *slot = pg->data + HDR_SIZE + s * SLOT_SIZE;
    uint16_t off = rd16(slot);
    const uint8_t *tup = pg->data + off;

    int bm = bitmap_bytes(t->ncols);
    const uint8_t *p = tup + bm;
    for (int i = 0; i < t->ncols; i++) {
        bool is_null = (tup[i / 8] >> (i % 8)) & 1u;
        if (is_null) {
            cells[i] = value_null(t->cols[i].type);
        } else if (t->cols[i].type == TYPE_INT) {
            int64_t v;
            memcpy(&v, p, 8);
            p += 8;
            cells[i] = value_int(v);
        } else {
            uint16_t len = rd16(p);
            p += 2;
            cells[i] = value_text_n((const char *)p, len);
            p += len;
        }
    }
}

/* ---- database / table lifecycle ----------------------------------------- */

Database *db_new(void) {
    return calloc(1, sizeof(Database));
}

static void table_free(Table *t) {
    for (int i = 0; i < t->npages; i++) free(t->pages[i]);
    free(t->pages);
    free(t->cols);
    free(t);
}

void db_free(Database *db) {
    if (!db) return;
    Table *t = db->tables;
    while (t) {
        Table *next = t->next;
        table_free(t);
        t = next;
    }
    free(db);
}

Table *db_find_table(Database *db, const char *name) {
    for (Table *t = db->tables; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t;
    return NULL;
}

Table *db_create_table(Database *db, const char *name,
                       const Column *cols, int ncols) {
    if (db_find_table(db, name)) return NULL;

    Table *t = calloc(1, sizeof(Table));
    strncpy(t->name, name, MAX_NAME - 1);
    t->ncols = ncols;
    t->cols = calloc(ncols, sizeof(Column));
    memcpy(t->cols, cols, ncols * sizeof(Column));

    t->next = db->tables;
    db->tables = t;
    return t;
}

bool table_append_row(Table *t, const Value *cells) {
    int size = tuple_size(t, cells);
    if (size < 0) return false; /* row exceeds one page */

    /* Try the most recently added page first (append-only workload). */
    if (t->npages > 0 && page_insert(t->pages[t->npages - 1], t, cells, size)) {
        t->nrows++;
        return true;
    }

    if (t->npages == t->pagecap) {
        t->pagecap = t->pagecap ? t->pagecap * 2 : 8;
        t->pages = realloc(t->pages, t->pagecap * sizeof(Page *));
    }
    Page *pg = malloc(sizeof(Page));
    page_init(pg);
    t->pages[t->npages++] = pg;

    /* A fresh page must fit it (size was already validated against page size). */
    page_insert(pg, t, cells, size);
    t->nrows++;
    return true;
}

int table_col_index(const Table *t, const char *col_name) {
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, col_name) == 0)
            return i;
    return -1;
}

/* ---- cursor ------------------------------------------------------------- */

void table_cursor_init(TableCursor *c, const Table *t) {
    c->t = t;
    c->page = 0;
    c->slot = 0;
}

bool table_cursor_next(TableCursor *c, Value *cells) {
    const Table *t = c->t;
    while (c->page < t->npages) {
        const Page *pg = t->pages[c->page];
        if (c->slot < page_nslots(pg)) {
            deserialize_tuple(t, pg, c->slot, cells);
            c->slot++;
            return true;
        }
        c->page++;
        c->slot = 0;
    }
    return false;
}
