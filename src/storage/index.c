#include "index.h"

#include <stdlib.h>
#include <string.h>

/*
 * Node block layout (slotted, entries kept sorted by key):
 *   [0]     node type: 0 = leaf, 1 = internal
 *   [2..3]  u16 nslots
 *   [4..5]  u16 free_end (tuple data grows down from here)
 *   [8..11] u32 extra: leaf -> next-leaf seq (IDX_NO_BLOCK if none)
 *                      internal -> leftmost child seq
 *   [12..]  slot dir: nslots x (u16 off, u16 len); then entry data.
 *
 * A leaf key is the encoded index columns followed by the row locator
 * (u32 data-block index, u16 slot) — appending the locator makes every leaf key
 * unique, which avoids the duplicate-key descent problem (equal column values
 * split across leaves). Column-value lookups search by the column prefix.
 * An internal entry is a separator key (a full leaf key) + u32 child block seq.
 */
#define NHDR 12
#define NSLOT 4
#define LOC_BYTES 6   /* row locator at the tail of every leaf key */
#define INT_PAY 4     /* internal payload: child block seq */

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void     wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int  nd_type(const uint8_t *d)      { return d[0]; }
static uint16_t nd_nslots(const uint8_t *d){ return rd16(d + 2); }
static uint16_t nd_free_end(const uint8_t *d){ return rd16(d + 4); }
static uint32_t nd_extra(const uint8_t *d) { return rd32(d + 8); }
static void nd_set_nslots(uint8_t *d, uint16_t v) { wr16(d + 2, v); }
static void nd_set_free_end(uint8_t *d, uint16_t v){ wr16(d + 4, v); }
static void nd_set_extra(uint8_t *d, uint32_t v)  { wr32(d + 8, v); }

static void nd_init(uint8_t *d, uint32_t block_size, int type, uint32_t extra) {
    memset(d, 0, block_size);
    d[0] = (uint8_t)type;
    nd_set_nslots(d, 0);
    nd_set_free_end(d, (uint16_t)block_size);
    nd_set_extra(d, extra);
}

static uint8_t *slot_ptr(uint8_t *d, int i) { return d + NHDR + i * NSLOT; }
static const uint8_t *slot_cptr(const uint8_t *d, int i) { return d + NHDR + i * NSLOT; }
static const uint8_t *entry_ptr(const uint8_t *d, int i) {
    return d + rd16(slot_cptr(d, i));
}
static int entry_len(const uint8_t *d, int i) {
    return rd16(slot_cptr(d, i) + 2);
}
static int paysize(int is_leaf) { return is_leaf ? 0 : INT_PAY; }
static int key_len(const uint8_t *d, int i, int is_leaf) {
    return entry_len(d, i) - paysize(is_leaf);
}

/* memcmp-based key order: shared prefix compared byte-wise, shorter key first. */
static int key_cmp(const uint8_t *a, int alen, const uint8_t *b, int blen) {
    int n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c) return c;
    return (alen > blen) - (alen < blen);
}

/* First entry index whose key is >= (key,klen); returns nslots if none. */
static int nd_lower_bound(const uint8_t *d, int is_leaf,
                          const uint8_t *key, int klen) {
    int lo = 0, hi = nd_nslots(d);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *ek = entry_ptr(d, mid);
        if (key_cmp(ek, key_len(d, mid, is_leaf), key, klen) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Inserts (key + payload) keeping slots sorted; false if the node is full. */
static bool nd_insert(uint8_t *d, uint32_t block_size, int is_leaf,
                      const uint8_t *key, int klen,
                      const uint8_t *pay, int paylen) {
    uint16_t nslots = nd_nslots(d);
    uint16_t free_end = nd_free_end(d);
    uint16_t free_start = NHDR + nslots * NSLOT;
    int elen = klen + paylen;
    if (free_end - free_start < elen + NSLOT) return false;
    (void)block_size;

    int pos = nd_lower_bound(d, is_leaf, key, klen);
    uint16_t off = free_end - (uint16_t)elen;
    memcpy(d + off, key, klen);
    if (paylen) memcpy(d + off + klen, pay, paylen);

    /* open a slot at pos */
    memmove(slot_ptr(d, pos + 1), slot_ptr(d, pos), (nslots - pos) * NSLOT);
    wr16(slot_ptr(d, pos), off);
    wr16(slot_ptr(d, pos) + 2, (uint16_t)elen);
    nd_set_nslots(d, nslots + 1);
    nd_set_free_end(d, off);
    return true;
}

/* Appends a raw entry (key+payload already concatenated) without sorting; used
 * to rebuild a node from an already-sorted entry list. */
static void nd_append_raw(uint8_t *d, const uint8_t *entry, int elen) {
    uint16_t nslots = nd_nslots(d);
    uint16_t free_end = nd_free_end(d);
    uint16_t off = free_end - (uint16_t)elen;
    memcpy(d + off, entry, elen);
    wr16(slot_ptr(d, nslots), off);
    wr16(slot_ptr(d, nslots) + 2, (uint16_t)elen);
    nd_set_nslots(d, nslots + 1);
    nd_set_free_end(d, off);
}

static void nd_remove(uint8_t *d, int pos) {
    uint16_t nslots = nd_nslots(d);
    memmove(slot_ptr(d, pos), slot_ptr(d, pos + 1), (nslots - pos - 1) * NSLOT);
    nd_set_nslots(d, nslots - 1);
}

/* ---- block store (mirrors table.c) -------------------------------------- */

static Block *ix_make_block(Index *ix, int seq, bool dirty, int type,
                            uint32_t extra) {
    if (ix->nblocks_used == ix->blockcap) {
        ix->blockcap = ix->blockcap ? ix->blockcap * 2 : 8;
        ix->blocks = realloc(ix->blocks, ix->blockcap * sizeof(Block *));
    }
    Block *b = malloc(sizeof(Block));
    b->data = malloc(ix->block_size);
    nd_init(b->data, ix->block_size, type, extra);
    b->id = (PageId){.db_id = ix->db_id, .obj_id = ix->obj.id, .seq = (uint32_t)seq};
    b->dirty = dirty;
    ix->blocks[seq] = b;
    ix->nblocks_used = seq + 1;
    return b;
}

static void ix_grow_extent(Index *ix) {
    uint32_t size = (uint32_t)EXTENT_INIT_BLOCKS << ix->nextents;
    uint32_t start = ix->pager ? pager_alloc(ix->pager, size) : 0;
    ix->extents = realloc(ix->extents, (ix->nextents + 1) * sizeof(Extent));
    ix->extents[ix->nextents].start_block = start;
    ix->extents[ix->nextents].nblocks = size;
    ix->nextents++;
    ix->capacity_blocks += size;
}

/* Allocates a fresh node block of the given type, returns its seq. */
static uint32_t ix_new_node(Index *ix, int type, uint32_t extra) {
    if ((uint32_t)ix->nblocks_used == ix->capacity_blocks)
        ix_grow_extent(ix);
    int seq = ix->nblocks_used;
    ix_make_block(ix, seq, true, type, extra);
    return (uint32_t)seq;
}

static uint8_t *ix_node(Index *ix, uint32_t seq) { return ix->blocks[seq]->data; }
static void ix_dirty(Index *ix, uint32_t seq) { ix->blocks[seq]->dirty = true; }

uint32_t index_phys_block(const Index *ix, int seq) {
    uint32_t base = 0;
    for (int e = 0; e < ix->nextents; e++) {
        if ((uint32_t)seq < base + ix->extents[e].nblocks)
            return ix->extents[e].start_block + ((uint32_t)seq - base);
        base += ix->extents[e].nblocks;
    }
    return 0;
}

/* ---- key encode / decode ------------------------------------------------ */

/* Order-preserving encoding: per column a presence byte (0 null, 1 present),
 * then INT as sign-flipped big-endian 8 bytes, or TEXT bytes + 0x00. Encoding
 * only the first `ncol` columns yields a prefix. Returns malloc'd bytes. */
static uint8_t *encode_key(const Index *ix, const Value *row_cells, int ncol,
                           int *out_len) {
    size_t cap = 16, len = 0;
    uint8_t *buf = malloc(cap);
    for (int i = 0; i < ncol; i++) {
        const Value *v = &row_cells[ix->cols[i]];
        size_t need = 1 + 9 + (v->type == TYPE_TEXT && v->as.s ? strlen(v->as.s) : 0);
        if (len + need > cap) { while (cap < len + need) cap *= 2; buf = realloc(buf, cap); }
        if (v->is_null) { buf[len++] = 0; continue; }
        buf[len++] = 1;
        if (ix->types[i] == TYPE_INT) {
            uint64_t u = (uint64_t)v->as.i ^ 0x8000000000000000ULL;
            for (int b = 7; b >= 0; b--) buf[len++] = (uint8_t)(u >> (b * 8));
        } else {
            const char *s = v->as.s ? v->as.s : "";
            size_t n = strlen(s);
            memcpy(buf + len, s, n); len += n;
            buf[len++] = 0;
        }
    }
    *out_len = (int)len;
    return buf;
}

/* Decodes a full key back into ncols index-column Values (owned). */
static void decode_key(const Index *ix, const uint8_t *key, int klen,
                       Value *out) {
    const uint8_t *p = key, *end = key + klen;
    for (int i = 0; i < ix->ncols; i++) {
        if (p >= end || *p == 0) {
            if (p < end) p++;
            out[i] = value_null(ix->types[i]);
            continue;
        }
        p++; /* presence */
        if (ix->types[i] == TYPE_INT) {
            uint64_t u = 0;
            for (int b = 0; b < 8 && p < end; b++) u = (u << 8) | *p++;
            out[i] = value_int((int64_t)(u ^ 0x8000000000000000ULL));
        } else {
            const uint8_t *q = p;
            while (q < end && *q != 0) q++;
            out[i] = value_text_n((const char *)p, (int)(q - p));
            p = (q < end) ? q + 1 : end;
        }
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

Index *index_new(uint32_t db_id, uint32_t obj_id, const char *name,
                 uint32_t table_id, const char *table_name,
                 const int *cols, const ColType *types, int ncols,
                 Pager *pager, uint32_t block_size) {
    Index *ix = calloc(1, sizeof(Index));
    ix->obj.id = obj_id;
    ix->obj.type = OBJ_INDEX;
    ix->db_id = db_id;
    strncpy(ix->name, name, MAX_NAME - 1);
    ix->table_id = table_id;
    strncpy(ix->table_name, table_name, MAX_NAME - 1);
    ix->ncols = ncols;
    ix->cols = malloc(ncols * sizeof(int));
    ix->types = malloc(ncols * sizeof(ColType));
    memcpy(ix->cols, cols, ncols * sizeof(int));
    memcpy(ix->types, types, ncols * sizeof(ColType));
    ix->root = IDX_NO_BLOCK;
    ix->pager = pager;
    ix->block_size = block_size;
    return ix;
}

void index_free(Index *ix) {
    for (int i = 0; i < ix->nblocks_used; i++) {
        free(ix->blocks[i]->data);
        free(ix->blocks[i]);
    }
    free(ix->blocks);
    free(ix->extents);
    free(ix->cols);
    free(ix->types);
    free(ix);
}

int index_col_pos(const Index *ix, int table_col) {
    for (int i = 0; i < ix->ncols; i++)
        if (ix->cols[i] == table_col) return i;
    return -1;
}

/* ---- descend ------------------------------------------------------------ */

static uint32_t descend_to_leaf(Index *ix, const uint8_t *key, int klen) {
    uint32_t seq = ix->root;
    while (nd_type(ix_node(ix, seq)) == 1) {
        uint8_t *d = ix_node(ix, seq);
        uint32_t child = nd_extra(d); /* leftmost */
        int n = nd_nslots(d);
        for (int i = 0; i < n; i++) {
            const uint8_t *ek = entry_ptr(d, i);
            int eklen = key_len(d, i, 0);
            if (key_cmp(key, klen, ek, eklen) >= 0)
                child = rd32(ek + eklen); /* child of this separator */
            else
                break;
        }
        seq = child;
    }
    return seq;
}

/* ---- insert with split -------------------------------------------------- */

/* Collects all entries of node `seq` plus a new (key,pay) into sorted arrays.
 * Caller frees `ents`/`elens`. Returns the total count. */
static int gather(Index *ix, uint32_t seq, int is_leaf,
                  const uint8_t *key, int klen, const uint8_t *pay, int paylen,
                  uint8_t ***ents, int **elens) {
    uint8_t *d = ix_node(ix, seq);
    int n = nd_nslots(d);
    int pos = nd_lower_bound(d, is_leaf, key, klen);
    uint8_t **e = malloc((n + 1) * sizeof(uint8_t *));
    int *L = malloc((n + 1) * sizeof(int));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i == pos) {
            int el = klen + paylen;
            e[k] = malloc(el); memcpy(e[k], key, klen);
            if (paylen) memcpy(e[k] + klen, pay, paylen);
            L[k] = el; k++;
        }
        int el = entry_len(d, i);
        e[k] = malloc(el); memcpy(e[k], entry_ptr(d, i), el);
        L[k] = el; k++;
    }
    if (pos == n) {
        int el = klen + paylen;
        e[k] = malloc(el); memcpy(e[k], key, klen);
        if (paylen) memcpy(e[k] + klen, pay, paylen);
        L[k] = el; k++;
    }
    *ents = e; *elens = L;
    return k;
}

/* Recursive insert of a full leaf key (already including the row locator). On
 * split, returns true and outputs the promoted separator (malloc'd) + right
 * sibling seq. */
static bool node_insert(Index *ix, uint32_t seq, const uint8_t *key, int klen,
                        uint8_t **sep, int *seplen, uint32_t *rseq) {
    uint8_t *d = ix_node(ix, seq);
    int is_leaf = (nd_type(d) == 0);

    if (is_leaf) {
        if (nd_insert(d, ix->block_size, 1, key, klen, NULL, 0)) {
            ix_dirty(ix, seq);
            return false;
        }
        /* split leaf */
        uint8_t **e; int *L;
        int cnt = gather(ix, seq, 1, key, klen, NULL, 0, &e, &L);
        int half = (cnt + 1) / 2;
        uint32_t rightseq = ix_new_node(ix, 0, nd_extra(ix_node(ix, seq)));
        d = ix_node(ix, seq); /* may have been realloc'd */
        uint8_t *rd = ix_node(ix, rightseq);
        nd_init(d, ix->block_size, 0, rightseq);         /* left.next = right */
        for (int i = 0; i < half; i++) nd_append_raw(d, e[i], L[i]);
        for (int i = half; i < cnt; i++) nd_append_raw(rd, e[i], L[i]);
        /* separator = the (unique) full first key of the right leaf */
        int sk = L[half];
        *sep = malloc(sk); memcpy(*sep, e[half], sk); *seplen = sk;
        *rseq = rightseq;
        for (int i = 0; i < cnt; i++) free(e[i]);
        free(e); free(L);
        ix_dirty(ix, seq); ix_dirty(ix, rightseq);
        return true;
    }

    /* internal: descend to the right child */
    uint32_t child = nd_extra(d);
    int n = nd_nslots(d);
    for (int i = 0; i < n; i++) {
        const uint8_t *ek = entry_ptr(d, i);
        int eklen = key_len(d, i, 0);
        if (key_cmp(key, klen, ek, eklen) >= 0) child = rd32(ek + eklen);
        else break;
    }

    uint8_t *csep; int cseplen; uint32_t cright;
    if (!node_insert(ix, child, key, klen, &csep, &cseplen, &cright))
        return false;

    /* insert (csep -> cright) into this internal node */
    d = ix_node(ix, seq);
    uint8_t cpay[INT_PAY];
    wr32(cpay, cright);
    if (nd_insert(d, ix->block_size, 0, csep, cseplen, cpay, INT_PAY)) {
        free(csep);
        ix_dirty(ix, seq);
        return false;
    }
    /* split internal: promote the middle separator */
    uint8_t **e; int *L;
    int cnt = gather(ix, seq, 0, csep, cseplen, cpay, INT_PAY, &e, &L);
    free(csep);
    int mid = cnt / 2;
    uint32_t leftmost = nd_extra(ix_node(ix, seq));
    uint32_t rightseq = ix_new_node(ix, 1, 0);
    d = ix_node(ix, seq);
    uint8_t *rd = ix_node(ix, rightseq);
    nd_init(d, ix->block_size, 1, leftmost);
    for (int i = 0; i < mid; i++) nd_append_raw(d, e[i], L[i]);
    /* middle entry: its key is promoted, its child becomes right's leftmost */
    int midklen = L[mid] - INT_PAY;
    uint32_t midchild = rd32(e[mid] + midklen);
    nd_set_extra(rd, midchild);
    for (int i = mid + 1; i < cnt; i++) nd_append_raw(rd, e[i], L[i]);
    *sep = malloc(midklen); memcpy(*sep, e[mid], midklen); *seplen = midklen;
    *rseq = rightseq;
    for (int i = 0; i < cnt; i++) free(e[i]);
    free(e); free(L);
    ix_dirty(ix, seq); ix_dirty(ix, rightseq);
    return true;
}

/* Builds a full, unique leaf key: encoded columns followed by the locator. */
static uint8_t *encode_entry(const Index *ix, const Value *cells, RowLoc loc,
                             int *outlen) {
    int klen;
    uint8_t *k = encode_key(ix, cells, ix->ncols, &klen);
    k = realloc(k, klen + LOC_BYTES);
    wr32(k + klen, loc.block);
    wr16(k + klen + 4, loc.slot);
    *outlen = klen + LOC_BYTES;
    return k;
}

void index_insert(Index *ix, const Value *row_cells, RowLoc loc) {
    int klen;
    uint8_t *key = encode_entry(ix, row_cells, loc, &klen);

    if (ix->root == IDX_NO_BLOCK) {
        ix->root = ix_new_node(ix, 0, IDX_NO_BLOCK);
        nd_insert(ix_node(ix, ix->root), ix->block_size, 1, key, klen, NULL, 0);
        ix_dirty(ix, ix->root);
        free(key);
        return;
    }

    uint8_t *sep; int seplen; uint32_t rseq;
    if (node_insert(ix, ix->root, key, klen, &sep, &seplen, &rseq)) {
        uint32_t oldroot = ix->root;
        uint32_t newroot = ix_new_node(ix, 1, oldroot); /* leftmost = old root */
        uint8_t cpay[INT_PAY];
        wr32(cpay, rseq);
        nd_insert(ix_node(ix, newroot), ix->block_size, 0, sep, seplen, cpay, INT_PAY);
        ix_dirty(ix, newroot);
        ix->root = newroot;
        free(sep);
    }
    free(key);
}

/* ---- delete ------------------------------------------------------------- */

void index_delete(Index *ix, const Value *row_cells, RowLoc loc) {
    if (ix->root == IDX_NO_BLOCK) return;
    int klen;
    uint8_t *key = encode_entry(ix, row_cells, loc, &klen); /* exact unique key */

    uint32_t leaf = descend_to_leaf(ix, key, klen);
    uint8_t *d = ix_node(ix, leaf);
    int i = nd_lower_bound(d, 1, key, klen);
    if (i < nd_nslots(d) &&
        key_cmp(entry_ptr(d, i), key_len(d, i, 1), key, klen) == 0) {
        nd_remove(d, i);
        ix_dirty(ix, leaf);
    }
    free(key);
}

/* ---- scan --------------------------------------------------------------- */

void index_scan_begin(Index *ix, IndexCursor *c, const Value *low, int nlow) {
    c->ix = ix;
    c->leaf = IDX_NO_BLOCK;
    c->pos = 0;
    if (ix->root == IDX_NO_BLOCK) return;

    if (!low) {
        /* leftmost leaf */
        uint32_t seq = ix->root;
        while (nd_type(ix_node(ix, seq)) == 1) seq = nd_extra(ix_node(ix, seq));
        c->leaf = seq;
        c->pos = 0;
        return;
    }
    int klen;
    uint8_t *key = encode_key(ix, low, nlow, &klen);
    uint32_t leaf = descend_to_leaf(ix, key, klen);
    c->leaf = leaf;
    c->pos = nd_lower_bound(ix_node(ix, leaf), 1, key, klen);
    free(key);
}

bool index_scan_next(IndexCursor *c, RowLoc *loc, Value *key_cells) {
    Index *ix = c->ix;
    while (c->leaf != IDX_NO_BLOCK) {
        uint8_t *d = ix_node(ix, c->leaf);
        if (c->pos < nd_nslots(d)) {
            int i = c->pos++;
            const uint8_t *ek = entry_ptr(d, i);
            int eklen = key_len(d, i, 1);   /* full key incl. locator tail */
            loc->block = rd32(ek + eklen - LOC_BYTES);
            loc->slot = rd16(ek + eklen - LOC_BYTES + 4);
            if (key_cells) decode_key(ix, ek, eklen, key_cells);
            return true;
        }
        c->leaf = nd_extra(d);
        c->pos = 0;
    }
    return false;
}

/* ---- persistence helpers ------------------------------------------------ */

void index_add_extent(Index *ix, uint32_t start, uint32_t nblocks) {
    ix->extents = realloc(ix->extents, (ix->nextents + 1) * sizeof(Extent));
    ix->extents[ix->nextents].start_block = start;
    ix->extents[ix->nextents].nblocks = nblocks;
    ix->nextents++;
    ix->capacity_blocks += nblocks;
}

void index_reserve_blocks(Index *ix, int nblocks) {
    while (ix->nblocks_used < nblocks)
        ix_make_block(ix, ix->nblocks_used, false, 0, IDX_NO_BLOCK);
}

void index_load_block(Index *ix, int seq, const void *image) {
    memcpy(ix->blocks[seq]->data, image, ix->block_size);
    ix->blocks[seq]->dirty = false;
}
