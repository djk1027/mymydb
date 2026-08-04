#include "dbfile.h"

#include "index.h"
#include "serialize.h"
#include "table.h"

#include <stdlib.h>
#include <string.h>

/* Per-db superblock (block 0), fixed offsets so block size can be read first. */
static const char DB_MAGIC[8] = {'M','Y','M','Y','D','B','2','\0'};
#define SB_HEAD 64

/* ---- catalog (de)serialization ------------------------------------------ */

static void serialize_catalog(Database *db, Buf *b) {
    buf_params(b, &db->params);

    uint32_t ntab = 0;
    for (Table *t = db->tables; t; t = t->next) ntab++;
    buf_u32(b, ntab);

    for (Table *t = db->tables; t; t = t->next) {
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

    uint32_t nidx = 0;
    for (Index *ix = db->indexes; ix; ix = ix->next) nidx++;
    buf_u32(b, nidx);
    for (Index *ix = db->indexes; ix; ix = ix->next) {
        buf_u32(b, ix->obj.id);
        buf_str(b, ix->name);
        buf_u32(b, ix->table_id);
        buf_str(b, ix->table_name);
        buf_u32(b, (uint32_t)ix->ncols);
        for (int i = 0; i < ix->ncols; i++) {
            buf_u32(b, (uint32_t)ix->cols[i]);
            buf_u8(b, (uint8_t)ix->types[i]);
        }
        buf_u32(b, ix->root);
        buf_u32(b, (uint32_t)ix->nblocks_used);
        buf_u32(b, (uint32_t)ix->nextents);
        for (int e = 0; e < ix->nextents; e++) {
            buf_u32(b, ix->extents[e].start_block);
            buf_u32(b, ix->extents[e].nblocks);
        }
    }
}

static void deserialize_catalog(Database *db, const uint8_t *data, size_t len) {
    Rdr r = {.data = data, .len = len, .pos = 0};
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

        Table *t = table_new(db->obj.id, tid, tname, cols, (int)ncols,
                             db->pager, db->block_size);
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
        uint8_t *image = malloc(db->block_size);
        for (uint32_t seq = 0; seq < nblocks_used; seq++) {
            pager_read_block(db->pager, table_phys_block(t, (int)seq), image);
            table_load_block(t, (int)seq, image);
        }
        free(image);
        t->nrows = (int)nrows;
        db_attach_table(db, t);
    }

    uint32_t nidx = rd_u32(&r);
    for (uint32_t ii = 0; ii < nidx; ii++) {
        uint32_t iid = rd_u32(&r);
        char iname[MAX_NAME];
        rd_str(&r, iname, MAX_NAME);
        uint32_t table_id = rd_u32(&r);
        char tname[MAX_NAME];
        rd_str(&r, tname, MAX_NAME);
        uint32_t ncols = rd_u32(&r);

        int *cols = malloc((ncols ? ncols : 1) * sizeof(int));
        ColType *types = malloc((ncols ? ncols : 1) * sizeof(ColType));
        for (uint32_t ci = 0; ci < ncols; ci++) {
            cols[ci] = (int)rd_u32(&r);
            types[ci] = (ColType)rd_u8(&r);
        }
        Index *ix = index_new(db->obj.id, iid, iname, table_id, tname,
                              cols, types, (int)ncols, db->pager, db->block_size);
        free(cols);
        free(types);

        ix->root = rd_u32(&r);
        uint32_t nblocks_used = rd_u32(&r);
        uint32_t nextents = rd_u32(&r);
        for (uint32_t e = 0; e < nextents; e++) {
            uint32_t start = rd_u32(&r);
            uint32_t nblk = rd_u32(&r);
            index_add_extent(ix, start, nblk);
        }
        index_reserve_blocks(ix, (int)nblocks_used);
        uint8_t *image = malloc(db->block_size);
        for (uint32_t seq = 0; seq < nblocks_used; seq++) {
            pager_read_block(db->pager, index_phys_block(ix, (int)seq), image);
            index_load_block(ix, (int)seq, image);
        }
        free(image);
        db_attach_index(db, ix);
    }
}

/* ---- superblock --------------------------------------------------------- */

static void write_superblock(Database *db) {
    uint8_t *sb = calloc(1, db->block_size);
    memcpy(sb, DB_MAGIC, 8);
    memcpy(sb + 8,  &db->block_size, 4);
    memcpy(sb + 12, &db->obj.id, 4);
    memcpy(sb + 16, &db->next_table_id, 4);
    memcpy(sb + 20, &db->pager->hw_blocks, 4);
    memcpy(sb + 24, &db->catalog_start, 4);
    memcpy(sb + 28, &db->catalog_nblocks, 4);
    memcpy(sb + 32, &db->catalog_capacity, 4);
    pager_write_block(db->pager, 0, sb);
    free(sb);
}

/* Reads block size from an existing file's superblock, or 0 if invalid. */
static uint32_t read_block_size(Pager *p) {
    uint8_t head[SB_HEAD];
    if (!pager_read_head(p, head, SB_HEAD)) return 0;
    if (memcmp(head, DB_MAGIC, 8) != 0) return 0;
    uint32_t bs;
    memcpy(&bs, head + 8, 4);
    if (bs < MIN_BLOCK_SIZE || bs > MAX_BLOCK_SIZE) return 0;
    return bs;
}

static void read_superblock(Database *db) {
    uint8_t *sb = malloc(db->block_size);
    pager_read_block(db->pager, 0, sb);
    memcpy(&db->obj.id,          sb + 12, 4);
    memcpy(&db->next_table_id,   sb + 16, 4);
    memcpy(&db->pager->hw_blocks, sb + 20, 4);
    memcpy(&db->catalog_start,    sb + 24, 4);
    memcpy(&db->catalog_nblocks,  sb + 28, 4);
    memcpy(&db->catalog_capacity, sb + 32, 4);
    free(sb);
}

/* ---- public ------------------------------------------------------------- */

void dbfile_checkpoint(Database *db) {
    uint32_t bs = db->block_size;

    /* 1. flush dirty data + index blocks */
    for (Table *t = db->tables; t; t = t->next)
        for (int seq = 0; seq < t->nblocks_used; seq++) {
            if (!t->blocks[seq]->dirty) continue;
            pager_write_block(db->pager, table_phys_block(t, seq),
                              t->blocks[seq]->data);
            t->blocks[seq]->dirty = false;
        }
    for (Index *ix = db->indexes; ix; ix = ix->next)
        for (int seq = 0; seq < ix->nblocks_used; seq++) {
            if (!ix->blocks[seq]->dirty) continue;
            pager_write_block(db->pager, index_phys_block(ix, seq),
                              ix->blocks[seq]->data);
            ix->blocks[seq]->dirty = false;
        }

    /* 2. serialize + place the catalog (in place if it still fits) */
    Buf b = {0};
    serialize_catalog(db, &b);
    uint32_t nblk = (uint32_t)((b.len + bs - 1) / bs);
    if (nblk == 0) nblk = 1;
    if (nblk > db->catalog_capacity) {
        db->catalog_start = pager_alloc(db->pager, nblk);
        db->catalog_capacity = nblk;
    }
    db->catalog_nblocks = nblk;

    uint8_t *img = malloc(bs);
    for (uint32_t i = 0; i < nblk; i++) {
        memset(img, 0, bs);
        size_t off = (size_t)i * bs;
        size_t chunk = b.len > off ? b.len - off : 0;
        if (chunk > bs) chunk = bs;
        if (chunk) memcpy(img, b.data + off, chunk);
        pager_write_block(db->pager, db->catalog_start + i, img);
    }
    free(img);
    buf_free(&b);

    /* 3. superblock last */
    write_superblock(db);
    pager_flush(db->pager);
}

bool dbfile_open(Database *db, const char *path, uint32_t default_block_size) {
    db->path = strdup(path);
    bool existed = pager_file_exists(path);

    Pager *pager = pager_open(path, default_block_size);
    if (!pager) return false;
    db->pager = pager;

    uint32_t stored_bs = existed ? read_block_size(pager) : 0;
    if (stored_bs) {
        pager->block_size = stored_bs;
        db->block_size = stored_bs;
        read_superblock(db);

        uint32_t nblk = db->catalog_nblocks;
        size_t len = (size_t)nblk * db->block_size;
        uint8_t *cat = malloc(len ? len : 1);
        for (uint32_t i = 0; i < nblk; i++)
            pager_read_block(pager, db->catalog_start + i,
                             cat + (size_t)i * db->block_size);
        deserialize_catalog(db, cat, len);
        free(cat);
    } else {
        /* fresh (or invalid) file: write an initial superblock + empty catalog */
        db->block_size = default_block_size;
        dbfile_checkpoint(db);
    }
    return true;
}
