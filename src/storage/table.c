#include "table.h"

#include <stdlib.h>
#include <string.h>

/*
 * Slotted block layout (identical in spirit to a slotted page, at 8KB):
 *
 *   header : u16 nslots, u16 free_end
 *   slot i : u16 offset, u16 length   (offset == 0 marks a deleted/tombstoned row)
 *   tuple  : null-bitmap, then each non-null cell:
 *              INT  -> 8 bytes (native int64)
 *              TEXT -> u16 length, then that many bytes
 */

#define HDR_SIZE  4
#define SLOT_SIZE 4

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void     wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static uint16_t blk_nslots(const uint8_t *d)   { return rd16(d); }
static uint16_t blk_free_end(const uint8_t *d) { return rd16(d + 2); }

static void blk_init(uint8_t *d, uint32_t block_size) {
    memset(d, 0, block_size);
    wr16(d, 0);                        /* nslots   */
    wr16(d + 2, (uint16_t)block_size); /* free_end */
}

static int bitmap_bytes(int ncols) { return (ncols + 7) / 8; }

static int tuple_size(const Table *t, const Value *cells) {
    long size = bitmap_bytes(t->ncols);
    for (int i = 0; i < t->ncols; i++) {
        if (cells[i].is_null) continue;
        if (cells[i].type == TYPE_INT) size += 8;
        else size += 2 + (long)strlen(cells[i].as.s ? cells[i].as.s : "");
    }
    if (size + SLOT_SIZE > (long)t->block_size - HDR_SIZE) return -1;
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

/* Inserts a serialized tuple; returns the new slot index, or -1 if no room. */
static int block_insert(uint8_t *d, const Table *t, const Value *cells, int size) {
    uint16_t nslots = blk_nslots(d);
    uint16_t free_end = blk_free_end(d);
    uint16_t free_start = HDR_SIZE + nslots * SLOT_SIZE;

    if (free_end - free_start < size + SLOT_SIZE) return -1;

    uint16_t off = free_end - (uint16_t)size;
    serialize_tuple(t, cells, d + off);

    uint8_t *slot = d + HDR_SIZE + nslots * SLOT_SIZE;
    wr16(slot, off);
    wr16(slot + 2, (uint16_t)size);

    wr16(d, (uint16_t)(nslots + 1));
    wr16(d + 2, off);
    return nslots;
}

static void deserialize_tuple(const Table *t, const uint8_t *d, int s, Value *cells) {
    const uint8_t *slot = d + HDR_SIZE + s * SLOT_SIZE;
    const uint8_t *tup = d + rd16(slot);

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

/* ---- block lifecycle ---------------------------------------------------- */

static Block *make_block(Table *t, int seq, bool dirty) {
    if (t->nblocks_used == t->blockcap) {
        t->blockcap = t->blockcap ? t->blockcap * 2 : 8;
        t->blocks = realloc(t->blocks, t->blockcap * sizeof(Block *));
    }
    Block *b = malloc(sizeof(Block));
    b->data = malloc(t->block_size);
    blk_init(b->data, t->block_size);
    b->id = (PageId){.db_id = t->db_id, .obj_id = t->obj.id, .seq = (uint32_t)seq};
    b->dirty = dirty;
    t->blocks[seq] = b;
    t->nblocks_used = seq + 1;
    return b;
}

/* Grows the extent map by one extent (8, 16, 32, ... blocks). */
static void grow_extent(Table *t) {
    uint32_t size = (uint32_t)EXTENT_INIT_BLOCKS << t->nextents;
    uint32_t start = t->pager ? pager_alloc(t->pager, size) : 0;
    t->extents = realloc(t->extents, (t->nextents + 1) * sizeof(Extent));
    t->extents[t->nextents].start_block = start;
    t->extents[t->nextents].nblocks = size;
    t->nextents++;
    t->capacity_blocks += size;
}

/* ---- public ------------------------------------------------------------- */

Table *table_new(uint32_t db_id, uint32_t obj_id, const char *name,
                 const Column *cols, int ncols, Pager *pager,
                 uint32_t block_size) {
    Table *t = calloc(1, sizeof(Table));
    t->obj.id = obj_id;
    t->obj.type = OBJ_TABLE;
    t->db_id = db_id;
    strncpy(t->name, name, MAX_NAME - 1);
    t->ncols = ncols;
    t->cols = calloc(ncols, sizeof(Column));
    memcpy(t->cols, cols, ncols * sizeof(Column));
    t->pager = pager;
    t->block_size = block_size;
    return t;
}

void table_free(Table *t) {
    for (int i = 0; i < t->nblocks_used; i++) {
        free(t->blocks[i]->data);
        free(t->blocks[i]);
    }
    free(t->blocks);
    free(t->extents);
    free(t->cols);
    free(t);
}

int table_col_index(const Table *t, const char *col_name) {
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, col_name) == 0)
            return i;
    return -1;
}

uint32_t table_phys_block(const Table *t, int seq) {
    uint32_t base = 0;
    for (int e = 0; e < t->nextents; e++) {
        if ((uint32_t)seq < base + t->extents[e].nblocks)
            return t->extents[e].start_block + ((uint32_t)seq - base);
        base += t->extents[e].nblocks;
    }
    return 0; /* seq out of range (shouldn't happen) */
}

bool table_append_row_loc(Table *t, const Value *cells,
                          int *out_block, int *out_slot) {
    int size = tuple_size(t, cells);
    if (size < 0) return false;

    if (t->nblocks_used > 0) {
        Block *b = t->blocks[t->nblocks_used - 1];
        int slot = block_insert(b->data, t, cells, size);
        if (slot >= 0) {
            b->dirty = true;
            t->nrows++;
            if (out_block) *out_block = t->nblocks_used - 1;
            if (out_slot) *out_slot = slot;
            return true;
        }
    }

    /* Need a fresh block at seq == nblocks_used; grow the extent map if the
     * currently reserved extents are full. */
    if ((uint32_t)t->nblocks_used == t->capacity_blocks)
        grow_extent(t);

    int idx = t->nblocks_used;
    Block *b = make_block(t, idx, true);
    int slot = block_insert(b->data, t, cells, size); /* fits: size validated */
    t->nrows++;
    if (out_block) *out_block = idx;
    if (out_slot) *out_slot = slot;
    return true;
}

bool table_append_row(Table *t, const Value *cells) {
    return table_append_row_loc(t, cells, NULL, NULL);
}

bool table_read_at(const Table *t, int block_idx, int slot, Value *cells) {
    if (block_idx < 0 || block_idx >= t->nblocks_used) return false;
    const uint8_t *d = t->blocks[block_idx]->data;
    if (slot < 0 || slot >= blk_nslots(d)) return false;
    const uint8_t *s = d + HDR_SIZE + slot * SLOT_SIZE;
    if (rd16(s) == 0) return false; /* tombstoned */
    deserialize_tuple(t, d, slot, cells);
    return true;
}

void table_delete_at(Table *t, int block_idx, int slot) {
    Block *b = t->blocks[block_idx];
    uint8_t *s = b->data + HDR_SIZE + slot * SLOT_SIZE;
    if (rd16(s) == 0) return; /* already deleted */
    wr16(s, 0);               /* tombstone: offset 0 is never a valid tuple */
    b->dirty = true;
    t->nrows--;
}

void table_cursor_init(TableCursor *c, const Table *t) {
    c->t = t;
    c->block = 0;
    c->slot = 0;
    c->cur_block = -1;
    c->cur_slot = -1;
}

bool table_cursor_next(TableCursor *c, Value *cells) {
    const Table *t = c->t;
    while (c->block < t->nblocks_used) {
        const uint8_t *d = t->blocks[c->block]->data;
        if (c->slot < blk_nslots(d)) {
            int s = c->slot++;
            const uint8_t *slot = d + HDR_SIZE + s * SLOT_SIZE;
            if (rd16(slot) == 0) continue; /* tombstoned */
            deserialize_tuple(t, d, s, cells);
            c->cur_block = c->block;
            c->cur_slot = s;
            return true;
        }
        c->block++;
        c->slot = 0;
    }
    return false;
}

/* ---- persistence helpers ------------------------------------------------ */

void table_add_extent(Table *t, uint32_t start_block, uint32_t nblocks) {
    t->extents = realloc(t->extents, (t->nextents + 1) * sizeof(Extent));
    t->extents[t->nextents].start_block = start_block;
    t->extents[t->nextents].nblocks = nblocks;
    t->nextents++;
    t->capacity_blocks += nblocks;
}

void table_reserve_blocks(Table *t, int nblocks) {
    while (t->nblocks_used < nblocks)
        make_block(t, t->nblocks_used, false);
}

void table_load_block(Table *t, int seq, const void *image) {
    memcpy(t->blocks[seq]->data, image, t->block_size);
    t->blocks[seq]->dirty = false;
}
