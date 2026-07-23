#ifndef MYMYDB_DATABASE_H
#define MYMYDB_DATABASE_H

#include "object.h"
#include "pager.h"
#include "param.h"
#include "table.h"

/*
 * A logical database object. An instance holds one or more of these; each owns
 * a set of tables and allocates table object ids from its own counter. Each
 * database carries its own parameter store (seeded from the global defaults at
 * creation) and the runtime block size its tables use.
 */
typedef struct Database {
    Object obj;              /* type == OBJ_DB, id == db id */
    char name[MAX_NAME];
    Pager *pager;            /* borrowed from the instance; NULL in memory mode */
    uint32_t block_size;     /* runtime block size for this db's tables */

    ParamStore params;       /* per-db parameters */

    Table *tables;
    uint32_t next_table_id;

    struct Database *next;
} Database;

Database *database_new(uint32_t id, const char *name, Pager *pager,
                       uint32_t block_size);
void database_free(Database *db);

/* Creates a table in db. Returns NULL if a table with that name already
 * exists. cols is copied. */
Table *db_create_table(Database *db, const char *name,
                       const Column *cols, int ncols);
Table *db_find_table(Database *db, const char *name);

/* Reattaches a fully-built table (used when rebuilding from the catalog). */
void db_attach_table(Database *db, Table *t);

#endif /* MYMYDB_DATABASE_H */
