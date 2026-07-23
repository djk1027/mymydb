#ifndef MYMYDB_TABLE_H
#define MYMYDB_TABLE_H

#include "object.h"
#include "pager.h"
#include "value.h"

#include <stdbool.h>

typedef struct {
    char name[MAX_NAME];
    ColType type;
} Column;

/* A materialized row: ncols Value cells owned by the holder. */
typedef struct {
    Value *cells;
} Row;

/* An in-memory block plus its identity and dirty state. The block image itself
 * is a slotted page (layout private to table.c) of the owning table's runtime
 * block size. */
typedef struct {
    PageId   id;
    bool     dirty;            /* needs writing back at the next checkpoint */
    uint8_t *data;             /* block_size bytes */
} Block;

/* A contiguous run of physical blocks reserved for one table. */
typedef struct {
    uint32_t start_block;      /* first physical block (0 in memory-only mode) */
    uint32_t nblocks;          /* extent size in blocks */
} Extent;

typedef struct Table {
    Object obj;                /* type == OBJ_TABLE, id == table id */
    uint32_t db_id;            /* owning database's id (for PageIds) */
    char name[MAX_NAME];

    Column *cols;
    int ncols;

    Block **blocks;            /* in-memory blocks, indexed by seq */
    int nblocks_used;          /* seq high-water: blocks holding data */
    int blockcap;              /* capacity of the blocks[] array */

    Extent *extents;           /* extent map (logical seq -> physical block) */
    int nextents;
    uint32_t capacity_blocks;  /* total blocks across all extents */

    int nrows;                 /* live (non-deleted) rows */

    uint32_t block_size;       /* runtime block size (bytes) */
    Pager *pager;              /* borrowed; NULL for a memory-only table */
    struct Table *next;
} Table;

/* Creates a table object owned by db_id. cols is copied. pager may be NULL. */
Table *table_new(uint32_t db_id, uint32_t obj_id, const char *name,
                 const Column *cols, int ncols, Pager *pager,
                 uint32_t block_size);
void table_free(Table *t);

int table_col_index(const Table *t, const char *col_name);

/* Physical block index backing logical sequence seq (via the extent map). */
uint32_t table_phys_block(const Table *t, int seq);

/*
 * Serializes a copy of cells (length == t->ncols) into the table's blocks,
 * growing extents as needed. Returns false only if the row cannot fit in one
 * block. The caller retains ownership of cells.
 */
bool table_append_row(Table *t, const Value *cells);

/* Marks the row at (block index, slot) deleted (tombstones its slot). */
void table_delete_at(Table *t, int block_idx, int slot);

/* ---- row cursor --------------------------------------------------------- */
/*
 * Sequential scan skipping deleted rows. After table_cursor_next returns true,
 * cur_block/cur_slot hold the location of the row just materialized (so callers
 * can delete it), and cells[t->ncols] holds owned Values (free with value_free).
 */
typedef struct {
    const Table *t;
    int block;       /* next block to scan */
    int slot;        /* next slot within the current block */
    int cur_block;   /* location of the row last returned */
    int cur_slot;
} TableCursor;

void table_cursor_init(TableCursor *c, const Table *t);
bool table_cursor_next(TableCursor *c, Value *cells);

/* ---- persistence helpers (used by instance.c) --------------------------- */

/* Loads a block image into the table's in-memory blocks[seq] slot. */
void table_load_block(Table *t, int seq, const void *image);
/* Ensures blocks[]/nblocks_used cover `nblocks` sequences (allocates empties). */
void table_reserve_blocks(Table *t, int nblocks);
/* Appends an extent to the map (used when rebuilding from the catalog). */
void table_add_extent(Table *t, uint32_t start_block, uint32_t nblocks);

#endif /* MYMYDB_TABLE_H */
