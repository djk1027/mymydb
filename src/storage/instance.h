#ifndef MYMYDB_INSTANCE_H
#define MYMYDB_INSTANCE_H

#include "database.h"
#include "pager.h"

/*
 * The whole server process's state. An instance owns one or more logical
 * databases and, when file-backed, a pager for durable per-block storage.
 * A memory-only instance (pager == NULL) skips all persistence.
 *
 * On-disk layout (file-backed): block 0 is the superblock; a contiguous catalog
 * run stores the serialized object tree; table data lives in extent-allocated
 * blocks. A checkpoint writes dirty blocks + catalog + superblock; opening an
 * existing file reloads the whole tree.
 */
typedef struct {
    Database *databases;
    Database *current;
    uint32_t next_db_id;
    uint32_t block_size;        /* effective block size for this instance */

    ParamStore globals;         /* instance-wide (global) parameters */

    Pager *pager;               /* NULL for a memory-only instance */
    uint32_t catalog_start;     /* physical block where the catalog begins */
    uint32_t catalog_nblocks;   /* catalog length in blocks */
    uint32_t catalog_capacity;  /* blocks reserved for the catalog run */
} Instance;

/* Memory-only instance (default block size) with a default 'main' database. */
Instance *db_new(void);
/* Memory-only instance with a given block size. */
Instance *instance_new_bs(uint32_t block_size);
/* File-backed instance: reloads path if it exists, otherwise creates it with
 * the given block size (the size is ignored when reloading an existing file). */
Instance *instance_open(const char *path, uint32_t block_size);
/* Checkpoints (if file-backed) and frees everything. */
void db_free(Instance *inst);

Database *instance_current(Instance *inst);
Database *instance_find_db(Instance *inst, const char *name);
/* Creates a database (seeded with the global params as defaults); returns NULL
 * if one with that name already exists. */
Database *instance_create_db(Instance *inst, const char *name);
/* Selects the current database; returns false if not found. */
bool instance_use(Instance *inst, const char *name);

/* Flushes dirty blocks, catalog, and superblock to disk (no-op in memory). */
void instance_checkpoint(Instance *inst);

#endif /* MYMYDB_INSTANCE_H */
