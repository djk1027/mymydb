#ifndef MYMYDB_INDEX_H
#define MYMYDB_INDEX_H

#include "object.h"
#include "pager.h"
#include "table.h"   /* Block, Extent, Column, Value, ColType */

/*
 * A B+tree index object (v2.3). Nodes are blocks in the owning database's file,
 * allocated in extents just like table data. A leaf entry stores the encoded
 * index-column key plus a row locator (data block index + slot); an internal
 * entry stores a separator key + child block. Keys use an order-preserving
 * byte encoding so node comparisons are plain memcmp. Non-unique (no PK).
 */

#define IDX_NO_BLOCK 0xFFFFFFFFu

typedef struct { uint32_t block; uint16_t slot; } RowLoc;

typedef struct Index {
    Object obj;              /* type == OBJ_INDEX */
    uint32_t db_id;
    char name[MAX_NAME];
    uint32_t table_id;       /* owning table's object id */
    char table_name[MAX_NAME];

    int ncols;
    int *cols;               /* indexed columns as table column indices */
    ColType *types;          /* their types (for key encode/decode) */

    uint32_t root;           /* root node block seq, or IDX_NO_BLOCK if empty */

    /* block store (mirrors Table's) */
    Block **blocks;
    int nblocks_used, blockcap;
    Extent *extents;
    int nextents;
    uint32_t capacity_blocks;
    uint32_t block_size;
    Pager *pager;

    struct Index *next;
} Index;

Index *index_new(uint32_t db_id, uint32_t obj_id, const char *name,
                 uint32_t table_id, const char *table_name,
                 const int *cols, const ColType *types, int ncols,
                 Pager *pager, uint32_t block_size);
void index_free(Index *ix);

/* Whether index column i (0..ncols-1) equals table column `table_col`. */
int index_col_pos(const Index *ix, int table_col);

/* Insert / delete the key built from a table row's cells, with locator loc. */
void index_insert(Index *ix, const Value *row_cells, RowLoc loc);
void index_delete(Index *ix, const Value *row_cells, RowLoc loc);

/* Forward scan in key order. begin positions at the first entry whose key is
 * >= the prefix built from `low` using its first `nlow` index columns; low may
 * be NULL to start at the smallest key. next fills loc and, if key_cells is
 * non-NULL, the decoded index-column values (ncols owned Values, caller frees).
 */
typedef struct {
    Index *ix;
    uint32_t leaf;
    int pos;
} IndexCursor;

void index_scan_begin(Index *ix, IndexCursor *c, const Value *low, int nlow);
bool index_scan_next(IndexCursor *c, RowLoc *loc, Value *key_cells);

/* ---- persistence helpers (used by dbfile.c) ----------------------------- */
uint32_t index_phys_block(const Index *ix, int seq);
void index_add_extent(Index *ix, uint32_t start, uint32_t nblocks);
void index_reserve_blocks(Index *ix, int nblocks);
void index_load_block(Index *ix, int seq, const void *image);

#endif /* MYMYDB_INDEX_H */
