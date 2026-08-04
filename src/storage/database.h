#ifndef MYMYDB_DATABASE_H
#define MYMYDB_DATABASE_H

#include "object.h"
#include "pager.h"
#include "param.h"
#include "table.h"
#include "index.h"

/*
 * A logical database object. v2.2: each database lives in its own data file
 * (data/<name>.mdb) with its own pager, superblock, and catalog. A database
 * carries its own parameter store (seeded from the global defaults at creation)
 * and the runtime block size its tables use.
 */
typedef struct Database {
    Object obj;              /* type == OBJ_DB, id == db id */
    char name[MAX_NAME];
    uint32_t block_size;     /* runtime block size for this db's tables */

    ParamStore params;       /* per-db parameters */

    Table *tables;
    Index *indexes;
    uint32_t next_table_id;   /* shared object-id counter for tables + indexes */

    /* ---- backing file (set by dbfile_open, see dbfile.c) ---- */
    Pager *pager;            /* this database's data file */
    char *path;              /* file path (owned) */
    uint32_t catalog_start;  /* physical block where this db's catalog begins */
    uint32_t catalog_nblocks;
    uint32_t catalog_capacity;

    struct Database *next;
} Database;

Database *database_new(uint32_t id, const char *name, uint32_t block_size);
void database_free(Database *db);

/* Creates a table in db. Returns NULL if a table with that name already
 * exists. cols is copied. */
Table *db_create_table(Database *db, const char *name,
                       const Column *cols, int ncols);
Table *db_find_table(Database *db, const char *name);

/* Reattaches a fully-built table (used when rebuilding from the catalog). */
void db_attach_table(Database *db, Table *t);

/* ---- indexes ------------------------------------------------------------ */

/* Creates an index on t over the given table column indices/types. Returns NULL
 * if an index with that name already exists. */
Index *db_create_index(Database *db, const char *name, Table *t,
                       const int *cols, const ColType *types, int ncols);
Index *db_find_index(Database *db, const char *name);
/* Removes and frees the named index; false if not found. */
bool db_drop_index(Database *db, const char *name);
/* Reattaches a fully-built index (used when rebuilding from the catalog). */
void db_attach_index(Database *db, Index *ix);

#endif /* MYMYDB_DATABASE_H */
