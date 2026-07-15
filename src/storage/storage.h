#ifndef MYMYDB_STORAGE_H
#define MYMYDB_STORAGE_H

#include "value.h"

#define MAX_NAME 64

typedef struct {
    char name[MAX_NAME];
    ColType type;
} Column;

/* A row owns ncols Value cells (ncols == owning table's ncols). */
typedef struct {
    Value *cells;
} Row;

typedef struct Table {
    char name[MAX_NAME];
    Column *cols;
    int ncols;

    Row *rows;
    int nrows;
    int cap;

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

/* Appends a row. cells[] ownership is transferred to the table on success. */
void table_append_row(Table *t, Value *cells);

/* Column index by name, or -1 if not found. */
int table_col_index(const Table *t, const char *col_name);

#endif /* MYMYDB_STORAGE_H */
