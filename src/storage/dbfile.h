#ifndef MYMYDB_DBFILE_H
#define MYMYDB_DBFILE_H

#include "database.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Per-database file I/O (v2.2). Each database lives in its own file:
 *   block 0        superblock (magic, block_size, db_id, next_table_id,
 *                  hw_blocks, catalog placement)
 *   catalog run    serialized per-db params + table catalog
 *   data blocks    extent-allocated table blocks
 *
 * dbfile_open attaches a pager to db and, for an existing file, reloads its
 * block size, catalog, and data. dbfile_checkpoint flushes dirty blocks +
 * catalog + superblock. Block writes are deferred until a checkpoint.
 */

/* Opens db's file at path, creating it (with default_block_size) if absent.
 * Returns false on I/O error. */
bool dbfile_open(Database *db, const char *path, uint32_t default_block_size);

void dbfile_checkpoint(Database *db);

#endif /* MYMYDB_DBFILE_H */
