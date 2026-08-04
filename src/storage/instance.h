#ifndef MYMYDB_INSTANCE_H
#define MYMYDB_INSTANCE_H

#include "database.h"
#include "param.h"

/*
 * The whole server process's state (v2.2). An instance is rooted at a base
 * path with `bin/` and `data/` subdirectories. Under `data/`:
 *   _master        instance registry: global params, next_db_id, db list
 *   <name>.mdb     one self-contained file per database
 *
 * Blocks are buffered in memory and only written on a checkpoint, which runs on
 * an explicit CHECKPOINT and when the instance is freed (not per mutation).
 */
typedef struct {
    Database *databases;
    Database *current;
    uint32_t next_db_id;
    uint32_t block_size;   /* default block size for newly created databases */

    ParamStore globals;    /* instance-wide (global) parameters */

    char *base_path;       /* e.g. $MYMY/mymydb or $HOME/mymydb */
    char *data_dir;        /* base_path + "/data" */
} Instance;

/* Opens (creating if absent) an instance rooted at base_path. block_size is the
 * default for new databases; existing database files keep their stored size.
 * Returns NULL on I/O error. */
Instance *instance_open(const char *base_path, uint32_t block_size);

/* Checkpoints all databases + the master, then frees everything. */
void db_free(Instance *inst);

Database *instance_current(Instance *inst);
Database *instance_find_db(Instance *inst, const char *name);
/* Creates a database (its own file, params seeded from the globals); returns
 * NULL if one with that name already exists. */
Database *instance_create_db(Instance *inst, const char *name);
/* Selects the current database; returns false if not found. */
bool instance_use(Instance *inst, const char *name);

/* Flushes every database's dirty blocks/catalog and the master registry. */
void instance_checkpoint(Instance *inst);

#endif /* MYMYDB_INSTANCE_H */
