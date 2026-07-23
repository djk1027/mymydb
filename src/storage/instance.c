#include "instance.h"

#include <stdlib.h>
#include <string.h>

/* ---- superblock (block 0) -----------------------------------------------
 * Fixed-offset header (independent of block size so it can be read first):
 *   [0..7]  magic          [8..11]  block_size     [12..15] next_db_id
 *   [16..19] hw_blocks      [20..23] catalog_start  [24..27] catalog_nblocks
 *   [28..31] catalog_capacity
 */
static const char SB_MAGIC[8] = {'M','Y','M','Y','D','B','2','\0'};
#define SB_HEAD 32

/* ---- little growable byte buffer for catalog (de)serialization ---------- */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

static void buf_need(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    while (b->cap < b->len + extra) b->cap = b->cap ? b->cap * 2 : 256;
    b->data = realloc(b->data, b->cap);
}
static void buf_u32(Buf *b, uint32_t v) {
    buf_need(b, 4); memcpy(b->data + b->len, &v, 4); b->len += 4;
}
static void buf_u16(Buf *b, uint16_t v) {
    buf_need(b, 2); memcpy(b->data + b->len, &v, 2); b->len += 2;
}
static void buf_u8(Buf *b, uint8_t v) {
    buf_need(b, 1); b->data[b->len++] = v;
}
static void buf_str(Buf *b, const char *s) {
    uint16_t n = (uint16_t)strlen(s);
    buf_u16(b, n);
    buf_need(b, n); memcpy(b->data + b->len, s, n); b->len += n;
}

typedef struct { const uint8_t *data; size_t len, pos; } Rdr;

static uint32_t rd_u32(Rdr *r) {
    uint32_t v = 0;
    if (r->pos + 4 <= r->len) memcpy(&v, r->data + r->pos, 4);
    r->pos += 4; return v;
}
static uint16_t rd_u16(Rdr *r) {
    uint16_t v = 0;
    if (r->pos + 2 <= r->len) memcpy(&v, r->data + r->pos, 2);
    r->pos += 2; return v;
}
static uint8_t rd_u8(Rdr *r) {
    uint8_t v = 0;
    if (r->pos + 1 <= r->len) v = r->data[r->pos];
    r->pos += 1; return v;
}
static void rd_str(Rdr *r, char *dst, int cap) {
    uint16_t n = rd_u16(r);
    int k = n < cap - 1 ? n : cap - 1;
    if (r->pos + n <= r->len) memcpy(dst, r->data + r->pos, k);
    dst[k] = '\0';
    r->pos += n;
}
/* Reads a length-prefixed string into a fresh heap buffer (for param values). */
static char *rd_strdup(Rdr *r) {
    uint16_t n = rd_u16(r);
    char *s = malloc(n + 1);
    if (r->pos + n <= r->len) memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}

/* ---- catalog serialization ---------------------------------------------- */

static void buf_params(Buf *b, const ParamStore *s) {
    buf_u32(b, (uint32_t)s->n);
    for (int i = 0; i < s->n; i++) {
        buf_str(b, s->items[i].name);
        buf_str(b, s->items[i].value);
    }
}

static void rd_params(Rdr *r, ParamStore *s) {
    uint32_t n = rd_u32(r);
    for (uint32_t i = 0; i < n; i++) {
        char *name = rd_strdup(r);
        char *val = rd_strdup(r);
        param_set(s, name, val);
        free(name);
        free(val);
    }
}

static void serialize_catalog(Instance *inst, Buf *b) {
    buf_params(b, &inst->globals);

    uint32_t ndb = 0;
    for (Database *d = inst->databases; d; d = d->next) ndb++;
    buf_u32(b, ndb);

    for (Database *d = inst->databases; d; d = d->next) {
        buf_u32(b, d->obj.id);
        buf_str(b, d->name);
        buf_u32(b, d->next_table_id);
        buf_params(b, &d->params);

        uint32_t ntab = 0;
        for (Table *t = d->tables; t; t = t->next) ntab++;
        buf_u32(b, ntab);

        for (Table *t = d->tables; t; t = t->next) {
            buf_u32(b, t->obj.id);
            buf_str(b, t->name);
            buf_u32(b, (uint32_t)t->ncols);
            for (int i = 0; i < t->ncols; i++) {
                buf_str(b, t->cols[i].name);
                buf_u8(b, (uint8_t)t->cols[i].type);
            }
            buf_u32(b, (uint32_t)t->nblocks_used);
            buf_u32(b, (uint32_t)t->nrows);
            buf_u32(b, (uint32_t)t->nextents);
            for (int e = 0; e < t->nextents; e++) {
                buf_u32(b, t->extents[e].start_block);
                buf_u32(b, t->extents[e].nblocks);
            }
        }
    }
}

static void deserialize_catalog(Instance *inst, const uint8_t *data, size_t len) {
    Rdr r = {.data = data, .len = len, .pos = 0};
    rd_params(&r, &inst->globals);
    uint32_t ndb = rd_u32(&r);

    for (uint32_t di = 0; di < ndb; di++) {
        uint32_t db_id = rd_u32(&r);
        char dbname[MAX_NAME];
        rd_str(&r, dbname, MAX_NAME);
        uint32_t next_table_id = rd_u32(&r);

        Database *db = database_new(db_id, dbname, inst->pager, inst->block_size);
        db->next_table_id = next_table_id;
        rd_params(&r, &db->params);

        uint32_t ntab = rd_u32(&r);
        for (uint32_t ti = 0; ti < ntab; ti++) {
            uint32_t tid = rd_u32(&r);
            char tname[MAX_NAME];
            rd_str(&r, tname, MAX_NAME);
            uint32_t ncols = rd_u32(&r);

            Column *cols = calloc(ncols ? ncols : 1, sizeof(Column));
            for (uint32_t ci = 0; ci < ncols; ci++) {
                rd_str(&r, cols[ci].name, MAX_NAME);
                cols[ci].type = (ColType)rd_u8(&r);
            }

            Table *t = table_new(db_id, tid, tname, cols, (int)ncols,
                                 inst->pager, inst->block_size);
            free(cols);

            uint32_t nblocks_used = rd_u32(&r);
            uint32_t nrows = rd_u32(&r);
            uint32_t nextents = rd_u32(&r);
            for (uint32_t e = 0; e < nextents; e++) {
                uint32_t start = rd_u32(&r);
                uint32_t nblk = rd_u32(&r);
                table_add_extent(t, start, nblk);
            }
            table_reserve_blocks(t, (int)nblocks_used);
            uint8_t *image = malloc(inst->block_size);
            for (uint32_t seq = 0; seq < nblocks_used; seq++) {
                pager_read_block(inst->pager, table_phys_block(t, (int)seq), image);
                table_load_block(t, (int)seq, image);
            }
            free(image);
            t->nrows = (int)nrows;
            db_attach_table(db, t);
        }

        /* Preserve list order across save/load by appending. */
        db->next = NULL;
        if (!inst->databases) {
            inst->databases = db;
        } else {
            Database *last = inst->databases;
            while (last->next) last = last->next;
            last->next = db;
        }
    }
}

/* ---- superblock read/write ---------------------------------------------- */

static void write_superblock(Instance *inst) {
    uint8_t *sb = calloc(1, inst->block_size);
    memcpy(sb, SB_MAGIC, 8);
    memcpy(sb + 8,  &inst->block_size, 4);
    memcpy(sb + 12, &inst->next_db_id, 4);
    memcpy(sb + 16, &inst->pager->hw_blocks, 4);
    memcpy(sb + 20, &inst->catalog_start, 4);
    memcpy(sb + 24, &inst->catalog_nblocks, 4);
    memcpy(sb + 28, &inst->catalog_capacity, 4);
    pager_write_block(inst->pager, 0, sb);
    free(sb);
}

/* Reads the block size out of an existing file's superblock header, or 0 if the
 * file is not a valid database. */
static uint32_t read_block_size(Pager *p) {
    uint8_t head[SB_HEAD];
    if (!pager_read_head(p, head, SB_HEAD)) return 0;
    if (memcmp(head, SB_MAGIC, 8) != 0) return 0;
    uint32_t bs;
    memcpy(&bs, head + 8, 4);
    if (bs < MIN_BLOCK_SIZE || bs > MAX_BLOCK_SIZE) return 0;
    return bs;
}

static void read_superblock(Instance *inst) {
    uint8_t *sb = malloc(inst->block_size);
    pager_read_block(inst->pager, 0, sb);
    memcpy(&inst->next_db_id,       sb + 12, 4);
    memcpy(&inst->pager->hw_blocks, sb + 16, 4);
    memcpy(&inst->catalog_start,    sb + 20, 4);
    memcpy(&inst->catalog_nblocks,  sb + 24, 4);
    memcpy(&inst->catalog_capacity, sb + 28, 4);
    free(sb);
}

/* ---- checkpoint --------------------------------------------------------- */

void instance_checkpoint(Instance *inst) {
    if (!inst->pager) return;
    uint32_t bs = inst->block_size;

    /* 1. flush dirty data blocks to their physical locations */
    for (Database *d = inst->databases; d; d = d->next)
        for (Table *t = d->tables; t; t = t->next)
            for (int seq = 0; seq < t->nblocks_used; seq++) {
                if (!t->blocks[seq]->dirty) continue;
                pager_write_block(inst->pager, table_phys_block(t, seq),
                                  t->blocks[seq]->data);
                t->blocks[seq]->dirty = false;
            }

    /* 2. serialize the catalog and place it (in-place if it still fits) */
    Buf b = {0};
    serialize_catalog(inst, &b);
    uint32_t nblk = (uint32_t)((b.len + bs - 1) / bs);
    if (nblk == 0) nblk = 1;
    if (nblk > inst->catalog_capacity) {
        inst->catalog_start = pager_alloc(inst->pager, nblk);
        inst->catalog_capacity = nblk;
    }
    inst->catalog_nblocks = nblk;

    uint8_t *img = malloc(bs);
    for (uint32_t i = 0; i < nblk; i++) {
        memset(img, 0, bs);
        size_t off = (size_t)i * bs;
        size_t chunk = b.len > off ? b.len - off : 0;
        if (chunk > bs) chunk = bs;
        if (chunk) memcpy(img, b.data + off, chunk);
        pager_write_block(inst->pager, inst->catalog_start + i, img);
    }
    free(img);
    free(b.data);

    /* 3. superblock last, so it always points at a complete catalog */
    write_superblock(inst);
    pager_flush(inst->pager);
}

/* ---- lifecycle ---------------------------------------------------------- */

static Instance *instance_alloc(Pager *pager, uint32_t block_size) {
    Instance *inst = calloc(1, sizeof(Instance));
    inst->pager = pager;
    inst->next_db_id = 1;
    inst->block_size = block_size;
    return inst;
}

/* Seeds the global params with the built-in defaults for this instance. */
static void seed_globals(Instance *inst, const char *data_file) {
    char bs[32];
    snprintf(bs, sizeof bs, "%u", inst->block_size);
    param_set(&inst->globals, "block_size", bs);
    param_set(&inst->globals, "data_file", data_file ? data_file : "");
}

Instance *instance_new_bs(uint32_t block_size) {
    if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE)
        block_size = DEFAULT_BLOCK_SIZE;
    Instance *inst = instance_alloc(NULL, block_size);
    seed_globals(inst, "");
    inst->current = instance_create_db(inst, "main");
    return inst;
}

Instance *db_new(void) {
    return instance_new_bs(DEFAULT_BLOCK_SIZE);
}

Instance *instance_open(const char *path, uint32_t block_size) {
    if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE)
        block_size = DEFAULT_BLOCK_SIZE;

    bool existed = pager_file_exists(path);
    Pager *pager = pager_open(path, block_size);
    if (!pager) return NULL;

    uint32_t stored_bs = existed ? read_block_size(pager) : 0;
    if (stored_bs) { block_size = stored_bs; pager->block_size = stored_bs; }

    Instance *inst = instance_alloc(pager, block_size);

    if (stored_bs) {
        read_superblock(inst);
        uint32_t nblk = inst->catalog_nblocks;
        size_t len = (size_t)nblk * block_size;
        uint8_t *cat = malloc(len ? len : 1);
        for (uint32_t i = 0; i < nblk; i++)
            pager_read_block(pager, inst->catalog_start + i,
                             cat + (size_t)i * block_size);
        deserialize_catalog(inst, cat, len);
        free(cat);
        /* keep the data_file param in step with where we actually opened from */
        param_set(&inst->globals, "data_file", path);
        inst->current = instance_find_db(inst, "main");
        if (!inst->current) inst->current = inst->databases;
    } else {
        /* fresh file: defaults + default database + initial checkpoint */
        seed_globals(inst, path);
        inst->current = instance_create_db(inst, "main");
        instance_checkpoint(inst);
    }
    return inst;
}

void db_free(Instance *inst) {
    if (!inst) return;
    if (inst->pager) {
        instance_checkpoint(inst);
        pager_close(inst->pager);
    }
    Database *d = inst->databases;
    while (d) {
        Database *next = d->next;
        database_free(d);
        d = next;
    }
    param_free(&inst->globals);
    free(inst);
}

/* ---- database management ------------------------------------------------ */

Database *instance_current(Instance *inst) { return inst->current; }

Database *instance_find_db(Instance *inst, const char *name) {
    for (Database *d = inst->databases; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

Database *instance_create_db(Instance *inst, const char *name) {
    if (instance_find_db(inst, name)) return NULL;
    Database *db = database_new(inst->next_db_id++, name, inst->pager,
                                inst->block_size);
    param_copy(&db->params, &inst->globals); /* seed from global defaults */
    db->next = inst->databases;
    inst->databases = db;
    return db;
}

bool instance_use(Instance *inst, const char *name) {
    Database *db = instance_find_db(inst, name);
    if (!db) return false;
    inst->current = db;
    return true;
}
