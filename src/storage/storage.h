#ifndef MYMYDB_STORAGE_H
#define MYMYDB_STORAGE_H

#include "value.h"

#define MAX_NAME 64

/* v1.2: rows are stored in fixed-size 4KB slotted pages (see storage.c). */
#define PAGE_SIZE 4096

typedef struct {
    char name[MAX_NAME];
    ColType type;
} Column;

/* A materialized row: ncols Value cells owned by the holder.
 * Tables no longer store Rows directly (rows live serialized inside pages);
 * this type is what a cursor deserializes into and what the executor keeps. */
typedef struct {
    Value *cells;
} Row;

/* One 4KB page. Layout is private to storage.c (a slotted page). */
typedef struct Page Page;

typedef struct Table {
    char name[MAX_NAME];
    Column *cols;
    int ncols;

    Page **pages;   /* array of 4KB pages holding serialized rows */
    int npages;
    int pagecap;
    int nrows;      /* total rows across all pages */

    struct Table *next;
} Table;

/* The whole in-memory database: just a linked list of tables. */
typedef struct {
    Table *tables;
} Database;

Database *db_new(void);
void db_free(Database *db);

/* Returns NULL if a table with that name already exists. cols is copied. */
Table *db_create_table(Database *db, const char *name,
                       const Column *cols, int ncols);
Table *db_find_table(Database *db, const char *name);

/*
 * Serializes a copy of cells (length == t->ncols) into the table's pages.
 * The caller retains ownership of cells. Returns false only if the row is too
 * large to fit in a single 4KB page.
 */
bool table_append_row(Table *t, const Value *cells);

/* Column index by name, or -1 if not found. */
int table_col_index(const Table *t, const char *col_name);

/* ---- row cursor --------------------------------------------------------- */
/*
 * Sequential scan over a table's pages. table_cursor_next materializes the
 * next row into cells[t->ncols] as owned Values (caller frees each with
 * value_free) and returns true; it returns false when the scan is exhausted.
 */
typedef struct {
    const Table *t;
    int page;
    int slot;
} TableCursor;

void table_cursor_init(TableCursor *c, const Table *t);
bool table_cursor_next(TableCursor *c, Value *cells);

#endif /* MYMYDB_STORAGE_H */
